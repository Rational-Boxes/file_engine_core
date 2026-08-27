# Proposal: authenticate the internal services that call gRPC

**Status:** Draft / research — for review
**Branch:** `security/grpc-service-auth` (file_engine_core)
**Author:** follows the trust-boundary work in `PROPOSAL_accountability_record.md` §5.4.9 (2026-08-26)
**Scope (cross-repo):** `file_engine_core` (interceptor + config); every gRPC caller — `http_bridge`, `webdav_bridge`, `cmis`, `convert_search_ai`, `mcp`, `discussion`, `folder_actions`, `difference`, `share`, `bcf`, `audit_service`, the CLI, and the Python/JS SDKs; `docker_unified` and `scripts/Ansible` for distribution

> The core currently cannot tell **which** internal service is calling it. Every
> RPC arrives with an end-user identity and nothing about its origin, so the
> platform can record *who* did something but not *through which door*.
>
> This proposes a per-service bearer secret carried in gRPC call metadata,
> validated centrally, resolved to a service identity, and recorded. It is a
> significant internal refactor: it touches every caller and every deployment.

---

## 1. Why

Three things follow from giving the core a verified caller identity, in
increasing order of value:

1. **Reject unknown callers.** A connection that cannot present a valid service
   secret is refused before reaching a handler. Today any process that can open a
   socket to `:50051` is fully trusted.
2. **Attribute every operation to its door.** Knowing an action came via WebDAV,
   CMIS, the REST bridge or an AI agent is materially different information when
   investigating an incident — the same user acting through the MCP door is not
   the same event as acting through a browser.
3. **Enforce least privilege between services** (§6). The `cmis` service has no
   business invoking erasure; `convert_search_ai` has no business managing roles.
   Today nothing stops either.

The third is where this stops being hygiene and starts being a control:
restrictions that are currently *conventions in a design document* become
*mechanisms in the core*.

---

## 2. Current state

| Finding | Evidence |
|---|---|
| No service identity exists at any layer | `AuthenticationContext` carries `user`, `roles`, `tenant`, `claims`, `source_addr` — all about the end user |
| **`source_iface` was designed for exactly this and carries nothing** | `audit_entry.h:53` documents it as `grpc\|rest\|webdav\|mcp`, but all four emit sites hardcode `"grpc"` (`grpc_service.cpp:87`, `:129`, `:171`, `:191`) |
| The channel is unauthenticated and unencrypted | `InsecureServerCredentials()` (`server.cpp:257`) |
| A server interceptor is already registered | `RpcLifetimeInterceptorFactory` (`server.cpp:274-278`) — the hook point exists and is proven |
| Per-RPC context already reaches the audit path | `thread_local GRPCFileService::t_audit_source_` carries the bridge-forwarded client IP (`grpc_service.cpp:29`) |

The last two matter for cost: **both mechanisms this needs are already in the
codebase**, doing analogous jobs. This is not new machinery, it is a second user
of existing machinery.

The `source_iface` finding is the sharpest. The schema, the column
(`database.cpp:136`) and the documented intent all exist; the value is a
constant. Every audit record ever written asserts `"grpc"`, which is the one
thing already implied by the fact that it reached the core.

---

## 3. Design

### 3.1 Transport — call metadata, not the message

The secret rides in gRPC **call metadata** (`authorization: Bearer <secret>`, or
a dedicated `x-fe-service-token`), never in a protobuf field.

Reasons: it is a transport concern rather than a request concern; it can be
enforced in one place; and adding it to `AuthenticationContext` would put a
credential inside a message that is already logged and passed around, which is
how secrets end up in logs.

### 3.2 Enforcement — one interceptor, not 41 handlers

Validation happens in a **server interceptor**, alongside the existing
`RpcLifetimeInterceptorFactory`. The core has 41 RPCs; any per-method check will
eventually miss one, and the miss will be silent. A rejected call never reaches a
handler.

The interceptor:

1. Reads the token from metadata.
2. Resolves it to a **service identity** (`http_bridge`, `cmis`, `audit_service`, …).
3. Rejects with `UNAUTHENTICATED` when absent or unknown.
4. Stashes the identity in a `thread_local`, mirroring `t_audit_source_`.

The audit path then reads that instead of hardcoding — `source_iface` finally
carries what it was designed to carry, with no change to any handler.

### 3.3 The source:secret map

A single shared secret authenticates but does not attribute — every caller looks
identical, and benefits 2 and 3 above evaporate. So the core holds a
**source-to-secret map**, and the secret a caller presents is what identifies it:

```
http_bridge    → <secret>
webdav_bridge  → <secret>
cmis           → <secret>
csai           → <secret>
mcp            → <secret>
audit_service  → <secret>
…
```

Authentication and attribution are then the same operation: a valid secret both
proves the caller is legitimate and names it. There is no separate "who are you?"
claim to be forged, because the caller never asserts an identity — it
demonstrates one.

**Stored hashed, never plaintext.** The map holds
`HMAC-SHA256(secret, pepper)`, following the pattern `ldap_manager` already uses
for user service credentials, with constant-time comparison. A read of the core's
config or database yields no usable token.

#### Lookup direction

Resolving a presented secret back to a source can go two ways, and the choice is
worth making deliberately:

| | Mechanism | Assessment |
|---|---|---|
| **Scan** | Compare the presented secret's hash against every entry | Simple, and trivial at ~13 services. But it is O(N) per RPC on the hot path, and every miss costs a full sweep |
| **Prefixed token** ✅ **chosen** | Token carries the `service_id`; look up by it, then verify the secret half | O(1), and mirrors the platform's existing `fesk_`/`fesks_` shape where a public key id accompanies a secret |

**Decided: the prefixed form.** Constant-time lookup, no per-call sweep on the
hot path, and it matches a credential shape already in use in-house. The trade is
that a stolen token reveals which service it belongs to — not secret information,
and knowable from the traffic anyway.

#### Token format

```
fesvc_<service_id>.<secret>
└──┬─┘ └────┬────┘ └──┬──┘
   │        │         └─ 256 bits, URL-safe base64
   │        └─ indexes the map: http_bridge, cmis, audit_service, …
   └─ scannable prefix
```

Three details, each with a reason:

- **`fesvc_` prefix.** Not decoration — the platform already uses distinctive
  prefixes (`fesk_`, `fesks_`) specifically *"so leaked-credential scanners can
  flag exposure"*. A token pasted into a ticket, committed to a repository or
  echoed into a log becomes greppable by gitleaks and equivalents. This one marks
  an internal **svc** credential, distinct from the user-facing ones.
- **`.` as the separator, not `_`.** Service ids already contain underscores —
  `http_bridge`, `webdav_bridge`, `folder_actions` — so an underscore separator
  is ambiguous to parse. Dots do not appear in service ids, so splitting on the
  first `.` after the prefix is unambiguous.
- **256-bit secret half**, URL-safe base64, matching `token_urlsafe(32)` as
  already used for the user service credentials.

Verification: strip the prefix, split on the first `.`, index the map by
`service_id`, then **constant-time compare** the HMAC of the secret half. Only
the lookup is indexed; the comparison never short-circuits.

> **Do not let an unknown service id fail faster than a bad secret.** If a
> nonexistent id returns immediately while a real one runs a full comparison, the
> timing difference enumerates valid service names. Perform a dummy comparison on
> the miss path so both cost the same. The value of that enumeration is admittedly
> low — service names are guessable — but the mitigation is a few lines, and
> "cheap enough to just do" is the right threshold for this kind of leak.

### 3.4 Rotation must overlap

Each service accepts **a list** of valid secrets, not one. Rotation is then: add
the new secret to the core, roll each service onto it, remove the old. Without
overlap, rotation requires restarting every service simultaneously — which means
in practice it never happens, and a credential that is never rotated is the
problem this is meant to reduce.

---

## 4. What this buys, and what it does not

Being precise here matters, because the failure mode is believing the port is now
safe to expose.

**It does:**

- Refuse callers that cannot present a valid secret.
- Attribute every RPC, and every audit and accountability record, to a door.
- Provide the substrate for inter-service least privilege (§6).

**It does not make the end-user identity trustworthy.** A compromised bridge
still asserts any `user` and any `roles`, including `system_admin`. This
authenticates the *service*, not the *person* — it narrows the trust model from
"anything that can reach the port" to "the services we issued secrets to", which
is a real narrowing and not a replacement.

**It does not make the port safe to expose publicly.** The channel is
`InsecureServerCredentials()`, so a bearer token crosses the wire in plaintext.
Against a passive network observer, the token is harvested on first use and the
control is worth nothing. So:

> **A bearer token over an unencrypted channel is only meaningful on a network
> that is already private.** As defence-in-depth *inside* the trusted network —
> catching a stray process, a misconfigured container, a forgotten debug script —
> it is genuinely valuable. As protection for a publicly reachable port it is
> not, and should not be described as such.

Making the port genuinely exposable requires TLS, and at that point **mTLS
subsumes the token entirely**: a client certificate authenticates the service
cryptographically, cannot be replayed off the wire, and gives the same identity
for attribution. The honest sequencing is in §7.

---

## 5. Configuration

| Setting | Where | Meaning |
|---|---|---|
| `FILEENGINE_SERVICE_TOKEN` | each calling service | the secret it presents |
| `FILEENGINE_SERVICE_TOKENS` | core | the source:secret map (§3.3) as `name:hash` pairs, or a path to a file of them |
| `FILEENGINE_SERVICE_AUTH_REQUIRED` | core | default **`true`** |
| `FILEENGINE_SERVICE_TOKEN_PEPPER` | core | HMAC pepper for the stored hashes |

Distribution: compose `.env` for the container stack, Ansible Vault for
deployments, per the existing secret-handling convention.

`FILEENGINE_SERVICE_AUTH_REQUIRED` defaults to **required**, consistent with the
loopback-bind decision: the safe configuration is the default, and getting it
wrong fails loudly at startup rather than silently leaving the door open. A
development escape hatch exists but must be set deliberately and should log a
warning on every start, not once.

**No default token ships.** A shipped default is a backdoor with a changelog
entry announcing it.

---

## 6. Capability gating — the payoff

Once the core knows the caller, it can bound what that caller may do. **Each
service identity is granted a set of API capabilities, and anything outside that
set is refused** — a second authorization axis (*may this service perform this
operation?*) in front of the existing one (*may this user perform it on this
resource?*). Both must pass.

It is deliberately coarse — method-level, not resource-level — because its job is
blast-radius containment, not access control.

### 6.1 Capabilities, not method lists

Enumerating 41 RPCs per service across ~13 services is 500-odd cells that will
drift and be got wrong. Instead each RPC belongs to exactly one **capability**,
and services are granted capabilities:

| Capability | Covers |
|---|---|
| `read` | `Stat`, `Exists`, `ListDirectory`, `GetFile`, `GetVersion`, `ListVersions`, `GetMetadata*`, `CheckPermission`, `GetEffectivePermissions` |
| `write` | `Touch`, `MakeDirectory`, `PutFile`, `StreamFileUpload`, `Rename`, `Move`, `Copy`, `SetMetadata`, `DeleteMetadata` |
| `delete` | `RemoveFile`, `RemoveDirectory`, `UndeleteFile`, `ListDirectoryWithDeleted` |
| `restore` | `RestoreToVersion` |
| `acl` | `GrantPermission`, `RevokePermission`, `GetResourceAcls` |
| `roles` | `CreateRole`, `DeleteRole`, `AssignUserToRole`, `RemoveUserFromRole`, `Get*ForRole/User`, `GetAllRoles`, `ListClaims` |
| `admin` | `GetStorageUsage`, `TriggerSync` |
| `destroy` | `PurgeOldVersions`, and erasure when it lands |
| `accountability` | `ListAccountabilityRecords` |

Nine sets instead of five hundred cells, and each maps to a section the proto is
already organised into.

**Default deny, including for new RPCs.** An RPC that belongs to no capability is
callable by nobody. So adding a method and forgetting to classify it fails
loudly at first use rather than silently defaulting to open — the opposite of an
allowlist that grants by omission.

#### Every grant is an explicit list. There is no blanket allow.

No network-facing service is granted "all". The map holds an enumerated set of
capabilities for every identity, and `*`, `all`, or an implicit "unlisted means
permitted" is not representable for any of them. There is exactly one reserved
exception, `cli`, which pays for it with a transport constraint instead — see
below.

Three reasons, the last of which is the one that matters:

- **"Everything" cannot be reviewed.** A grant reading `all` conveys nothing to
  a reader; nine named capabilities can be checked against what the service
  actually does.
- **"Everything" grows silently.** Split `destroy` into `purge` and `erase`
  later, and a blanket-allow identity acquires the new one without anyone
  deciding. An enumerated list must be amended, which forces the decision into
  the open.
- **"Everything" re-opens the fail-closed guarantee.** The default-deny property
  above works because an unclassified RPC reaches nobody. A blanket allow
  implemented as "permit all methods" bypasses classification entirely, so the
  one identity most likely to hold it — the admin tool — becomes the one place a
  forgotten RPC is reachable. The blanket silently undoes the protection.

This is the same principle already applied one layer down. In
`PROPOSAL_accountability_record.md` §5.4.9, `kAllPermissions` returning every bit
for `tenant_admin` is what defeated the bespoke `ERASE` permission; the fix was
to stop having a blanket. The service map adopts the rule from the start rather
than having to retract it later.

#### The one exception: `cli`, constrained by transport instead

`cli` is a **reserved identity holding every capability** — and it is the only
one. The justification is not that the CLI is trusted, but that its access is
bounded by something stronger than a capability list: **it may connect only over
loopback.**

An administrator on the box already has full system access. They can read the
PostgreSQL tables, the storage tree, the config and the secrets directly, without
going near the CLI. Restricting what the CLI may ask the core to do would not
withhold anything from them — it would only make the supported tool weaker than
the unsupported paths sitting next to it, which is how operators end up doing
delicate work with `psql` instead. **A control that controls nothing is worse
than no control**, because it invites the belief that a boundary exists there.

The real boundary is host access, so that is where the constraint is placed.

Two things keep this from re-opening what §6.1 closes:

**1. "Every capability" means every *classified* capability — never a
classification bypass.** The distinction is load-bearing:

| Reading | Effect on an unclassified RPC |
|---|---|
| `cli` permits any method | **Reachable.** Re-opens the fail-closed hole exactly as described above |
| `cli` holds all nine capabilities | **Still denied.** An RPC belonging to no capability belongs to none of `cli`'s either |

The second is the intended reading. `cli` is granted the full set, not exempted
from the mechanism, so a newly added and unclassified RPC remains unreachable by
everyone — which was the property worth having.

**2. Loopback is enforced, not assumed.** The bind default is now `127.0.0.1`
(`security/grpc-loopback-default`), but containers widen it to `0.0.0.0`, so the
bind alone does not constrain `cli` there. The interceptor therefore checks the
peer address directly: **a call presenting the `cli` identity from a non-loopback
peer is refused**, whatever the server is bound to.

This is a real control rather than a declared one — the peer address of an
established TCP connection is not something the caller can assert, unlike a
header. And the CLI still presents its token: loopback alone would let *any*
local process act as `cli`, so the most powerful identity carries the most
conditions, not the fewest.

The two conditions are not redundant — **each blocks a population the other does
not**, and together they land on exactly the intended boundary:

| Constraint | Excludes |
|---|---|
| Loopback-only | Everyone off the host |
| The secret, in a non-world-readable file | **Unprivileged users *on* the host** |

That second row is the one it would be easy to miss. A host is not only its
administrator: service accounts, a shared login, a compromised unprivileged
process — any of them can open a loopback connection, because loopback is not a
privilege. What they cannot do is read a config file they have no permission to
read. So loopback narrows access to the host, and the secret narrows it to
privileged users on that host. The result is "the administrator", which is the
boundary the reasoning above assumed all along.

**This makes the file permissions load-bearing, so they must be specified rather
than left to the installer's judgement:**

- The file holding the `cli` secret is installed `0640 root:fileengine` (or
  `0600`), never world-readable, by the `.deb`, `.rpm` and `PKGBUILD` packaging.
- **The CLI refuses to use a world-readable secret file**, in the manner `ssh`
  refuses a world-readable private key. A permission mistake should stop the tool
  rather than silently widen the boundary — this is precisely the failure that is
  invisible until someone exploits it.

There is also a useful asymmetry: **the core stores only the hash** (§3.3), while
the plaintext lives solely in the CLI's own config. Reading the core's
configuration — which the `fileengine` service user necessarily can — does not
yield a usable `cli` token. The most privileged credential is not present in the
most widely-read file.

Requiring both conditions has one more consequence: **a leaked `cli` token is not
catastrophic.** Copied into a remote service's config, committed to a repository,
or pulled from a shared secret store, it still cannot be used from off the host —
the transport constraint holds independently of who knows the secret. The one
identity whose compromise would matter most is the one whose credential matters
least on its own.

> **Stronger variant worth considering: a Unix domain socket.** gRPC supports
> `unix:` addresses, and a UDS restricted to the `fileengine` user or group makes
> `cli` access local-only *by construction* and adds OS-level identity —
> filesystem permissions decide who may connect at all, before any token is
> examined. It removes the loopback check rather than implementing it, and cannot
> be widened by a misconfigured bind.
>
> Note it collapses both constraints above into one mechanism: the socket's
> permissions exclude off-host callers *and* unprivileged local ones, using the
> same filesystem permissions that would otherwise be protecting the secret file.
> The token becomes belt-and-braces rather than load-bearing. The cost is a second
> listener and a small amount of packaging work.

### 6.2 Deriving the assignments — measure, do not guess

The temptation is to write the service-to-capability matrix from intuition. That
produces a matrix that is subtly wrong in the permissive direction, because
guessing errs toward "it probably needs that".

Derive it instead: **start every service at deny-all, run its existing E2E
suite, and add only the capabilities the failures demand.** The suites already
exist and are the regression gate for each service, so the result is grounded in
observed behaviour and stays self-documenting — a later capability addition
shows up as a test that needed it.

A few assignments are clear enough to state now, and they illustrate the value:

| Service | Capability set | Why it matters |
|---|---|---|
| `audit_service` | `accountability` only | It is a reader. Today it could write files; nothing stops it |
| `mcp` | `read`, `write` — no `delete`, no `destroy` | See §6.3 |
| `cmis`, `webdav_bridge` | no `destroy` | §5.4.9 of the accountability proposal restricts erasure to the admin surface **by convention**; this makes it a property of the core |
| `csai`, `difference`, `discussion` | `read` + narrow `write` for their own derived output | None has business managing ACLs or roles |
| `cli` | **all nine capabilities**, reserved — the sole exception to §6.1, bounded by loopback-only transport instead | The one place irreversible operations belong. It gets `accountability` too: someone on the host can already read the audit tables directly, so withholding it would be theatre rather than a control |

### 6.3 The MCP case

`mcp` is documented as *"append-only, recoverable"* — an AI agent should be able
to add and revise, never to destroy. Today that property lives entirely in the
MCP service's own code: it holds full core authority and simply chooses not to
use the destructive parts.

Granting it `read` and `write` and withholding `delete` and `destroy` moves that
guarantee into the core. A prompt-injected agent, or a bug in tool dispatch, then
cannot delete a document however convincingly it is instructed to — the core
refuses the call before any handler sees it.

Of every caller on the platform, the AI door is the one whose behaviour is least
predictable from its code. It is the strongest case for enforcing intent rather
than trusting it.

### 6.4 Config may narrow, never widen

The **grouping** of RPCs into capabilities is a property of the API and belongs
in code. The **assignment** of capabilities to services is more tempting to put
in config — but a config-granted capability is one deployment-pressure change
away from a bridge holding `destroy`.

So: assignments live in code, and configuration may only **subtract**. An
operator can tighten a service below its compiled set for a hardened deployment;
nobody can widen one without a code change and a review. This supersedes the
looser "code for destructive operations, config for the rest" in §8.4.

---

## 6.5 The end user's IP — already built, one door short

Recording the *external* client's address, rather than the internal peer, is the
natural companion to `source_iface`. Together the three fields answer the whole
question:

| Field | Answers | State |
|---|---|---|
| `actor` | **who** | works |
| `source_iface` | **through which door** | exists but hardcoded `"grpc"` (§2) |
| `source_addr` | **from where** | **works** — see below |

**This is already implemented.** `AuthenticationContext.source_addr` (proto field
5) is documented as *"client IP forwarded by the bridge (audit)"*, and it flows
end to end: the caller sets it, the core lifts it into `t_audit_source_`, and the
audit entry carries it.

The doors populate it from a **trusted-proxy-aware** resolver rather than the raw
peer, which is the part that makes it trustworthy:

| Door | Populates `source_addr` |
|---|---|
| `http_bridge` | ✅ `http_server.cpp:568`, from `clientIp(req)` |
| `webdav_bridge` | ✅ `webdav_server.cpp:230`, `:368`, via `resolveRequestIp(request, trusted_proxies)` |
| `mcp`, `csai`, `discussion`, `share`, `folder_actions` | ✅ via the Python SDK's `source_addr` parameter |
| **`bcf_services`** | ❌ **the gap** |

`bcf_services` is a genuine external door (BCF-API, OAuth/Bearer) and **has** the
client address to hand — `auth.py:167` and `app.py:83` both read
`request.client.host` — but never forwards it, and sets no `X-Forwarded-For` on
its upstream calls either. So for BCF traffic the audit trail records an internal
address, or nothing.

That is the whole of the work: **fix one service, not build a mechanism.**

### 6.5.1 Chained doors lose it silently

The `bcf_services` case generalises. Whenever one service fronts another, the end
user's address survives only if it is deliberately propagated — either in
`source_addr` when calling the core directly, or as `X-Forwarded-For` when
calling through a bridge that resolves it. A hop that forgets does not error; it
simply substitutes its own container address, and the audit trail quietly becomes
less true.

Worth a note in `EVENT_CONTRACT.md` or the bridge developer docs, since it is the
kind of thing each new door has to be told once.

### 6.5.2 Empty is correct, and must stay correct

`source_addr` is **optional and legitimately empty**. Plenty of core traffic
originates from event-driven internal work — `folder_actions` reacting to a
`file.created`, csai indexing, `difference` generating a comparison — where there
is no external user and no address to record. Empty means "not user-initiated",
which is itself information.

So it must never be validated as required, and an empty value must not be treated
as a defect to be "fixed" by substituting the peer address. Doing so would
manufacture a plausible-looking external origin for work no external user asked
for — worse than the blank it replaced.

#### Worked example: rendition generation

A user uploads a document through `http_bridge`. That write records the user and
their address. csai then consumes the resulting event and writes a rendition back
into the core.

**The rendition write should carry no `source_addr`.** Copying the user's address
forward would be redundant — it is already recorded on the upload that caused it —
and worse, it would be *false in a specific way*: it asserts the user made a
request from that address at that moment, when what actually happened is a
service reacting to an event, possibly minutes later. One user action would
produce several records all claiming the same origin, and the log would overstate
how much the user did.

The three fields then tell the true story, which none of them could alone:

| Field | Rendition write | Meaning |
|---|---|---|
| `actor` | the user | csai acts **on the user's authority** — feature services re-check access as the end user |
| `source_iface` | `csai` | the work was done **by csai**, not by a door the user was holding |
| `source_addr` | *empty* | **no user connection originated it** |

Read together: *"csai did this on the user's authority, not from a user
connection."* Copying the IP would collapse that into *"the user did this from
their browser"*, which is the one reading that is untrue.

**The general rule: `source_addr` records where a request came *from*, never what
caused it.** Causation is a different relation. If the link between an upload and
its derived writes is ever wanted explicitly, the right mechanism is a causation
id — the triggering event already carries an `event_id` a derived write could
reference — not a copied address. Not proposed here; noted so that the need, if
it arises, is not met by overloading this field.

### 6.5.3 Why it stays in the message, not a metadata header

The service token goes in call metadata (§3.1); `source_addr` stays in
`AuthenticationContext`. That is a principled split rather than an
inconsistency:

- The **token** describes the *connection* — which service is speaking, stable
  across every call it makes.
- **`source_addr`** describes *this request's end user*, exactly like `user`,
  `roles` and `tenant`, and belongs with them.

Splitting one request's identity across two transports invites the state where a
call carries a user but not their origin, or vice versa. And it already works
where it is — moving it would be churn against a live audit path for no gain.

This touches every caller, so sequencing matters more than usual.

1. **Core accepts and records, does not require.** Add the interceptor with
   `FILEENGINE_SERVICE_AUTH_REQUIRED=false`, resolve identities when present, and
   populate `source_iface`. Nothing breaks; attribution starts working
   immediately for services that have been updated.
2. **Roll the callers.** Each service, the CLI, and the SDK examples gain a
   token. Progress is directly observable — audit records still saying `"grpc"`
   are callers not yet migrated, which makes the migration self-tracking.
3. **Flip to required.** Once no unattributed calls remain, default it on.
4. **Add the allowlist** (§6), starting with the destructive operations, since
   those carry the most value and the least legitimate cross-service traffic.
5. **Later, if the port must ever leave a private network: mTLS**, which replaces
   the token rather than supplementing it.

Step 1 is worth having even alone: it closes the `source_iface` gap and makes
every subsequent step measurable.

---

## 8. Open decisions

1. **`authorization: Bearer` or a custom header?** Reusing `authorization` is
   conventional and interceptor-friendly, but risks confusion with the end-user
   bearer tokens the bridges already handle. A distinct `x-fe-service-token`
   makes it unambiguous in logs and code. Recommend the distinct header.
2. **Does the SDK path get tokens?** `python_interface` and `javascript_interface`
   speak gRPC directly and are documented as safe only server-side. Issuing them
   tokens legitimises a path that is already the weakest link; refusing to
   means any server-side SDK user must obtain a service identity deliberately.
   Recommend the latter — it makes SDK usage a decision rather than a default.
3. **One token per service, or per instance?** Per-instance narrows revocation
   blast radius and identifies *which* replica acted, at the cost of managing
   many more secrets. Recommend per-service until there is an operational reason.
4. ~~**Should the allowlist live in config or in code?**~~ **Answered in §6.4:**
   capability *grouping* and service *assignments* both live in code, and config
   may only subtract. Widening requires a code change and a review.
5. **Rate limiting / lockout on repeated bad tokens?** Cheap to add at the
   interceptor and a useful signal, but on a private network it may be noise.

---

## 9. Acceptance

1. A call with **no token** is rejected with `UNAUTHENTICATED` and never reaches
   a handler, when auth is required.
2. A call with an **unknown or malformed token** is rejected identically —
   including one that is a valid token for a *different* deployment.
3. A call with a valid token succeeds, and its audit and accountability records
   carry the **resolved service name** in `source_iface`, not `"grpc"`.
4. **Every one of the 41 RPCs** is covered, verified by enumerating the service
   descriptor rather than by a hand-written list — the whole point of enforcing
   centrally is that coverage is not per-method.
5. Rotation works with overlap: with old and new secrets both configured, a
   service presenting either succeeds; after the old is removed, it fails.
6. Secrets never appear in logs, error messages, or audit records — including on
   the rejection path, which is where they are most likely to be echoed.
7. With capability gating active, a service invoking a method outside its set is
   refused with `PERMISSION_DENIED` **even when the end-user identity it presents
   would otherwise be authorized** — proving the two axes are independent.
   Specifically: the `mcp` identity cannot call `RemoveFile` while presenting a
   user who holds `DELETE` on that file (§6.3), and `audit_service` cannot call
   `PutFile` at all.
8. **Every RPC is classified.** A test enumerates the service descriptor and
   fails if any method belongs to no capability — so a newly added RPC cannot
   reach production unclassified, and is denied to everyone until it is.
9. **Config cannot widen.** A configuration attempting to grant a service a
   capability outside its compiled set is rejected at startup rather than
   honoured (§6.4).
10. **No blanket grant is representable**, with `cli` the sole reserved
    exception. No `*` form exists for any other identity, and:
    - An **unclassified RPC is refused to every identity including `cli`** —
      proving `cli` holds the full capability set rather than bypassing
      classification, which is the distinction that keeps §6.1 intact.
    - A call presenting `cli` **from a non-loopback peer is refused**, tested
      against a server bound to `0.0.0.0` so the check is proven independent of
      the bind address.
    - A local process presenting **no token cannot act as `cli`** — loopback and
      the token are both required, not either. Tested as an *unprivileged* local
      user, since that is the population the token exists to exclude.
    - The CLI **refuses to run against a world-readable secret file**, and the
      packaging installs it non-world-readable.
11. Comparison is constant-time, and the core stores no plaintext secret.
12. **The map resolves the source correctly:** each service's token yields that
    service's name and no other, and a token whose secret half is altered by one
    character resolves to nothing rather than to a neighbouring entry.
    Format-specific (§3.3):
    - A token for a service id containing an underscore (`http_bridge`) parses
      correctly — the separator is the first `.`, not the first `_`.
    - A malformed token (no prefix, no separator, empty secret half) is rejected
      rather than parsed into a partial match.
    - An **unknown service id costs the same time as a valid id with a wrong
      secret**, so timing does not enumerate service names.
13. **End-user origin survives every external door (§6.5).** A request through
    `bcf_services` records the caller's real address, not an internal one — the
    one door currently missing it. And an event-driven internal call records an
    **empty** `source_addr` rather than a substituted peer address, so
    "not user-initiated" stays distinguishable from "user at 10.0.0.4".
14. **Derived writes do not inherit the originator's address (§6.5.2).** After a
    user uploads a file and csai writes its rendition, exactly **one** record
    carries the user's address — the upload. The rendition record shows the user
    as `actor`, `csai` as `source_iface`, and an empty `source_addr`.
