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
| **Prefixed token** (recommended) | Token is `<service_id>.<secret>`; look up by `service_id`, then verify the secret half | O(1), and mirrors the platform's existing `fesk_`/`fesks_` shape where a public key id accompanies a secret |

The prefixed form is recommended: constant-time lookup, no per-call sweep, and it
matches a credential shape already in use in-house. The trade is that a stolen
token reveals which service it belongs to — which is not secret information, and
is knowable from the traffic anyway.

Either way the **verification** of the secret half stays constant-time; only the
lookup is indexed.

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
| CLI / admin surface | everything, including `destroy` | The one place irreversible operations belong |

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

## 7. Rollout

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
10. Comparison is constant-time, and the core stores no plaintext secret.
11. **The map resolves the source correctly:** each service's token yields that
    service's name and no other, and a token whose secret half is altered by one
    character resolves to nothing rather than to a neighbouring entry.
