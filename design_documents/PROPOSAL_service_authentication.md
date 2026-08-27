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
for user service credentials, with constant-time comparison. The row is readable,
but the secret is not in it — a database dump yields no usable token.

#### Why a fast MAC is enough, and the condition it depends on

Password hashing normally demands a deliberately slow KDF (bcrypt, argon2)
because human-chosen passwords have little entropy and a fast hash can be
brute-forced. That reasoning does not apply here, and `ldap_manager` already
states why:

> *"The server pepper defends a bare DB read; a fast MAC is sufficient for a
> full-entropy secret."* — `service_cred.py`

The secret half is **256 bits of machine-generated randomness** (§3.3). There is
no dictionary to run and no candidate space to search, so slowing the hash buys
nothing. The pepper defends the narrower case of an attacker who has the dump
*and* candidate secrets to verify — belt-and-braces at this entropy, and the
reason it must live outside the database (§3.5).

> **The guardrail this rests on: service tokens are always *generated*, never
> chosen.** The moment any path lets an operator type a memorable token, the
> entropy assumption fails and HMAC stops being sufficient — quietly, with no
> visible change, because the stored value looks identical. If hand-chosen tokens
> are ever wanted, the storage must move to a slow KDF at the same time. Better
> to keep generation the only path.

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

### 3.5 Where the map lives

The map needs a home that is durable, operable, and does not become a liability
when something leaks. An earlier draft of this section specified an **encrypted
file on disk**, then spent most of its length listing that approach's drawbacks —
which is a fair sign it was the wrong answer. Reconsidered:

| Option | Assessment |
|---|---|
| **Encrypted file on disk** | ❌ **rejected** — the key must reach the process at boot, the file gets copied into backups, images and tickets, it is fixed for the process lifetime, a wrong key is easily mistaken for an empty map, and above all **it cannot record its own changes** |
| **Core PostgreSQL** ✅ **chosen, and the only supported store** | Adds **no new dependency** — the core already cannot start without it. Transactional, immediately consistent, multi-instance safe, already backed up, and map changes become recorded |
| **Asymmetric — core stores public keys** | Strictly best on leak-resistance: nothing at rest is a secret at all. More machinery; converges on mTLS (§4) |

#### Why PostgreSQL is not a new failure mode

The usual objection to putting credentials in a database is the added
dependency. **Here there isn't one.** `server.cpp:101` exits with *"Failed to
connect to database"* if PostgreSQL is unreachable — the core cannot serve at all
without it, so a map stored there is available in exactly the situations the core
is running.

That is precisely the argument that ruled out fetching the key from a vault at
boot (below): a vault would be a *new* thing that must be reachable before the
core can serve, and given a core outage takes the platform down, that is a new
platform-wide single point of failure. PostgreSQL is not new. It is the one
dependency the core already has absolutely.

The read-only failover path (`ConnectionPoolManager::server_in_readonly_mode_`)
also covers the degraded case: reading a map is a read, so authentication keeps
working when the primary is gone.

#### What moving it to the database fixes

- **No artefact to leak on its own.** The map stops being a separate file that
  gets copied into container images, backups and support tickets. It folds into
  the database — which is already the most sensitive artefact the platform has,
  holding every file's metadata and ACLs, and is already protected accordingly.
  The map is not the weak link there.
- **No reload signalling.** The boot-fixed problem disappears. Adding a service
  or finishing a rotation is a transactional update, picked up by a short-TTL
  cache. Routine credential administration stops costing a core restart — which,
  since a core restart is a platform outage, is what would otherwise guarantee it
  gets deferred.
- **Multi-instance consistency for free.** A file must be distributed to every
  core replica and can skew between them. A shared table cannot.
- **Map changes become accountability records.** Granting a service a capability,
  or rotating its secret, is exactly the security-relevant act
  `PROPOSAL_accountability_record.md` exists to record — and a DB write can be
  recorded in the same transaction (§4.2 there). A file edit cannot be recorded
  at all; it happens outside the system.

That last point is the strongest. With the map in a file, the most sensitive
administrative action on the platform leaves no trace in the platform.

#### The pepper is the one secret, and it stays out of the database

The stored values are `HMAC-SHA256(secret, pepper)` (§3.3). The **pepper must not
live in the database**, or a single dump yields both halves and the hashing stops
buying anything. It stays in the environment, injected the same way `AT_REST_KEY`
already is — `openssl rand -hex 32`, compose or Ansible Vault, never fetched at
runtime.

So secret zero shrinks rather than moves: one short value in the environment,
instead of a key that decrypts a whole file of identities. And unlike the file
key, losing the pepper does not lock the core out of its own map — it invalidates
the hashes, which is a rotation, not an outage.

#### There is no file fallback

An earlier draft kept the encrypted-file form as an option for deployments that
could not use the database. **Dropped — the database is the only supported
store.**

The decisive reason is not simplification, it is that **a file cannot record its
own changes.** §3.6 makes every credential change an accountability record
written in the same transaction as the change, so modifying the map without
leaving a trace is impossible. A file path reintroduces exactly that: edit the
file, restart, and a service's credentials have changed with nothing anywhere
recording it. Keeping the option would mean keeping a supported way to bypass the
guarantee the section above exists to provide.

The rest follows the usual fate of optional weaker paths — rarely exercised,
therefore under-tested, therefore where the bugs live — and it would oblige the
design to carry the file's drawbacks (key at boot, copies in images and backups,
no online rotation, wrong-key-versus-empty-map ambiguity) in perpetuity for a
deployment nobody has asked for. The platform is pre-production; adding the
option later if a real constraint appears costs far less than supporting it
speculatively now.

#### Rotation is online, across every node

This is where database storage pays off most, because rotation stops being a
deployment event:

**Rotating a service token.** Insert the new secret so the service has two valid
entries (§3.4 overlap), roll that service's instances onto it at leisure, then
delete the old row. Every core node observes both changes within the cache TTL —
**no restart, no coordination between nodes, no file to distribute.** With a
file, each of those two steps would mean copying to every core host and reloading
each one, in order.

**Rotating the pepper — corrected.** An earlier draft of this section said the
pepper is rotated by re-hashing the stored values under the new one. **That is
impossible:** the core holds `HMAC(secret, pepper)` and never the plaintext, so
it cannot recompute anything. Taken at face value it would have meant pepper
rotation requires re-issuing every service token — a genuine outage-shaped
operation.

It does not, because of *when* the plaintext is available: **at successful
authentication the caller has just presented it.** So the standard
upgrade-on-login pattern applies:

1. Store a `pepper_version` alongside each hash.
2. Accept the old and new pepper during the transition, trying the row's own
   version first.
3. On a successful authentication against the old pepper, recompute the hash
   under the new one and update that row.
4. Services authenticate constantly, so rows migrate on their own. Retire the old
   pepper once none remain — which is a query, not a guess.

Online, node-agnostic, and no token re-issue. Two implementation notes: the
rehash is a write on the authentication path, so it must be best-effort and
strictly once-per-row-per-rotation rather than per call; and two nodes rehashing
the same row concurrently is harmless, since both compute the same value.

The two rotations stay independent — either can be done without the other — and
neither requires a core restart, which matters because a core restart is a
platform outage (`PROPOSAL_accountability_record.md` §7.8). **A credential
lifecycle that costs an outage is one that gets skipped.**

#### The row carries the capabilities too

A service's granted capabilities live in the same row as its secret. Only the
capability **definitions** — which RPCs make up `destroy`, `acl`, `read` — stay
compiled, because those describe the API rather than a deployment (§6.4).

So a service row is `service_id`, hashed secret, pepper version, granted
capabilities. **Adding a service is one insert**, and it takes effect on every
node within the cache TTL with no release and no restart.

#### Not the CLI's own token

The `cli` secret comes from the secrets store like every other (§6.1), but is
*delivered* differently: injected as `FILEENGINE_CLI_TOKEN` for automated use,
and materialised as a `0640` root-owned file for interactive use, because the CLI
is the one credential consumer with no supervisor to inject for it. Encrypting
that file separately would need a key the operator must supply to decrypt their
own credential — recursion with no gain.

The asymmetry is deliberate: the core runs unattended and validates many
identities, so its map lives in the database; the CLI holds one secret and is
often driven by a human, so it gets filesystem permissions and a tool that
refuses to run when they are wrong.

---

### 3.6 Key management belongs to the CLI

All service-credential administration goes through `fileengine_cli`. Direct
manipulation of the map — `psql`, a migration, a hand-written `UPDATE` — is not a
supported path.

#### Not convenience. It is what enforces the invariants

The storage design rests on properties that live in the *writing* code, not in
the schema, and every one of them is silently lost if an operator edits the table
directly:

| Invariant | How a hand-written `UPDATE` breaks it |
|---|---|
| Tokens are **generated, never chosen** (§3.3) | An operator typing a memorable token voids the entropy assumption that makes a fast MAC sufficient — invisibly, since the stored value looks identical |
| Values are **peppered HMACs** | A plaintext or unpeppered value stores fine and authenticates fine, until a dump leaks it |
| Changes are **recorded** (§3.5) | A direct write happens outside the system, so the most sensitive administrative action on the platform leaves no trace |
| Rotation **overlaps** (§3.4) | Replacing a row in place instead of adding one strands every instance still holding the old secret |

So the CLI is the enforcement point, not a nicety. A schema cannot express "this
value must have come from a CSPRNG".

#### Commands

```
fileengine_cli service-token issue    <service_id>     # generate, store hash, print ONCE
fileengine_cli service-token rotate   <service_id>     # issue alongside the old (overlap)
fileengine_cli service-token prune    <service_id>     # drop the superseded entry post-rollout
fileengine_cli service-token revoke   <service_id>
fileengine_cli service-token list                      # ids, created, last-used, pepper version
fileengine_cli service add            <service_id> --capabilities read,write
fileengine_cli service capabilities   <service_id>     # show the granted set
fileengine_cli service grant          <service_id> <capability>
fileengine_cli service revoke-cap     <service_id> <capability>
fileengine_cli pepper rotate                           # begin; both accepted
fileengine_cli pepper status                           # rows per pepper version — when is it safe to retire
```

`list` never prints a secret; `issue` and `rotate` print it exactly once, since
there is nothing to recover it from afterwards. `grant` validates the capability
against the compiled definitions, so a typo is rejected rather than stored as a
capability nothing will ever match — and granting a **high-risk** capability
requires the explicit confirmation described in §6.4.

`pepper status` deserves its place: §3.5's online pepper rotation completes when
no rows remain on the old version, and that is a question an operator must be
able to *ask* rather than estimate.

#### Scriptable

- `--json` on every read command; human-formatted output is not an API.
- Meaningful exit codes; no interactive prompts in scripted use.
- `revoke` and `prune` are idempotent. `issue` is not, and must not be — it
  generates.
- **The secret goes to stdout and nowhere else.** Never to a log, never to the
  audit record, never echoed back by a later command. A caller piping it is fine;
  a caller running under `set -x` or a CI job archiving stdout is the realistic
  leak, so the docs must say so plainly.

#### Two consequences worth stating

**Automation runs on the core host.** The `cli` identity is loopback-only
(§6.1), so token administration cannot be driven from a central CI runner
reaching in over the network. Scripts run on the box, or via something that does
— Ansible with a task on the core host, rather than a pipeline calling an API.
That is a direct consequence of the loopback decision and worth knowing before
someone designs the pipeline the other way.

**Records name a person, on evidence.** The `actor` comes from the **credential
presented**, not from the invoking OS username — `cli:alice`'s secret records
`alice` (§6.1). Attributing every administrative action to a generic `cli` would
make the record technically complete and practically useless; attributing it to a
self-reported username would make it forgeable by anyone holding the same token,
which on the identity permitted every capability is the worst possible place for
that weakness.

There is no `--actor` override for a human credential. Automation identities
(`cli:ansible`) are separate credentials, so an unattended job is already named by
the secret it presents and needs no flag to say so.

#### Every credential change is a recorded event

Credential lifecycle is exactly the class of act
`PROPOSAL_accountability_record.md` exists to capture — it changes who can speak
to the core at all — so each operation writes an accountability record in the
**same transaction** as the map change (§4.2 there). If the record cannot be
written, the credential change does not happen.

| Event | Category | Written when |
|---|---|---|
| `service_token.issued` | `identity` | a service identity gains its first credential |
| `service_token.rotated` | `identity` | a second secret is added alongside the current one |
| `service_token.pruned` | `identity` | the superseded secret is removed after rollout |
| `service_token.revoked` | `identity` | a credential is invalidated |
| `pepper.rotation_started` / `…_completed` | `identity` | the map-wide re-key begins / finishes |
| `service_capability.narrowed` | `authorization` | configuration subtracts a capability (§6.4) |
| `service_capability.granted` / `.revoked` | `authorization` | a capability is added to or removed from a service (§6.4) |
| `service_capability.changed` | `authorization` | a startup diff finds a set the log cannot account for — **an out-of-band table edit**, not a routine change |

`identity` rather than `authorization` for the token events: what a service *may
do* lives in code and does not change here (§6.4). What changes is what it can
*prove it is*.

**Record the decision, not the mechanics.** Pepper rotation migrates rows
opportunistically as services authenticate (§3.5), and emitting an event per
row would produce one record per service per rotation — noise that buries the two
records anyone actually wants. Start and completion are the administrative acts;
the migration in between is bookkeeping.

> **The record must never contain the credential.** Not the secret, not its hash,
> not the pepper. This follows the rule in `PROPOSAL_accountability_record.md`
> §5.4.7 — the chain carries identifiers and structure, never payload — and the
> failure here would be the sharpest possible version of it: a tamper-evident,
> never-culled, permanently-retained log containing every service secret ever
> issued. Record the `service_id`, the action, the operator and the pepper
> version. Nothing else is needed to answer "who granted what, when".

**Record the capability set with every credential event.** Each record carries
the service's granted capabilities at that moment, so the log answers not just
*"`cmis` got a credential on the 14th"* but *"…and that credential could do these
nine things"*.

With assignments in the database (§6.4) this is partly redundant with the grant
records themselves — but only partly, and cheaply: it means a single credential
record is self-contained, and reconstructing a service's authority at a past
instant does not require replaying every grant and revoke that preceded it.

##### The startup diff is now tamper detection, not change detection

An earlier draft justified a startup diff on the grounds that capabilities were
compiled, so a release could change them with nothing recording it. **With
assignments in the database (§6.4) that argument is void** — every grant and
revoke is a CLI operation recorded at the moment it happens, so there is no
unrecorded change path to catch.

The diff is still worth keeping, for a different and better reason. On startup
the core compares each service's stored capability set against the last one the
accountability log recorded, and writes `service_capability.changed` only where
they differ. Since every legitimate change already produced a record, **a
difference the log cannot account for means the table was edited outside the
CLI.** That is precisely what §3.6 declares unsupported, and it should be visible
rather than assumed away.

- **Only on difference**, so ordinary restarts do not fill the log with identical
  snapshots.
- A record with no matching grant is an **integrity signal**, not routine
  bookkeeping — the same shape as the chain-break alarms in
  `PROPOSAL_accountability_record.md` §4.3.2.

Capabilities are structure, not payload, so recording them is consistent with
§5.4.7's rule.

One useful property falls out of the core writing these rather than the CLI:
**revoking a service's credential is recorded independently of that service.**
Cutting off `audit_service` itself still produces a durable record, because the
core writes it transactionally and the consumer pulls later (§4.3 there) — the
act does not depend on the party being cut off remaining functional.

**Cross-document addition.** `PROPOSAL_accountability_record.md` §4.1's scope
table should gain these events. They fit its stated rule cleanly — security
relevant, needing a guaranteed record, with no versioned home of their own.

#### Bootstrapping the first credential

The map starts empty, and the CLI needs a credential to talk to the core — so the
first one cannot be issued by the CLI. **Packaging generates a single bootstrap
identity at install time** — `cli:bootstrap` — writing it `0640` (§6.1) and
registering its hash. Every other credential, including each administrator's own
`cli:<name>` (§6.1), is then issued through the CLI.

The bootstrap identity should be revoked once real administrator credentials
exist. It is the one credential nobody is individually accountable for, so
leaving it in place permanently reintroduces the shared-token attribution problem
the per-administrator design removes — and `service-token list` showing it still
active is an easy thing to check for.

This keeps the awkward case where it belongs: one credential created by the
installer, under the same generation rules, rather than a bootstrap mode that
accepts unauthenticated calls while the map is empty — which would be a
permanently exploitable state that only *looks* transient.

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
| `FILEENGINE_SERVICE_TOKEN` | each calling service | the `fesvc_…` secret it presents (§3.3) |
| `FILEENGINE_CLI_TOKEN` | CLI, automated use | the `cli` secret when a supervisor injects it; takes precedence over the file (§6.1) |
| `FILEENGINE_SERVICE_TOKEN_PEPPER` | core | **the one secret** — HMAC pepper for the stored hashes. Injected, never in the database (§3.5) |
| `FILEENGINE_SERVICE_AUTH_REQUIRED` | core | default **`true`** |
| `FILEENGINE_SERVICE_MAP_CACHE_TTL` | core | how long the map is cached between reads (§3.5); short, so an update takes effect without a restart |

The map itself lives in the core's PostgreSQL (§3.5), so it is not configuration:
adding a service is a transactional update, not a file edit and a restart.

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

`cli:*` is a **reserved class of identities holding every capability** — one per
administrator and per automation account (§6.1 below), and the only identities
permitted the full set. The justification is not that the CLI is trusted, but
that its access is bounded by something stronger than a capability list: **it may
connect only over loopback.**

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

##### Where the `cli` secret comes from

**Treat it exactly as `FILEENGINE_PG_PASSWORD` is treated** — same sensitivity
class, same handling, same machinery. The platform already has a well-worn answer
for a credential of this weight: vaulted in Ansible Vault, injected from `.env`
in the compose stack, present in the root-owned config on a packaged install,
never committed, never logged. Nothing about this credential warrants inventing a
new class, and inventing one would mean a second set of habits for operators to
get right.

That parity also settles delivery, because the database password already faces
the same split: a supervised service receives it in its environment, while an
administrator running `psql` reads it from the root-owned config. **The CLI is
simply the interactive case of a pattern that already exists.** It needs both
routes for the same reason — services get an environment from systemd or compose;
a human at a shell prompt does not, so "inject it into the environment" means
somebody puts it there, which interactively degrades into a shell profile or a
command line.

So both, with different jobs:

| Invocation | Source |
|---|---|
| **Automated** — an Ansible task on the core host, a timer, a systemd unit | `FILEENGINE_CLI_TOKEN` in the environment, injected by the supervisor. Identical to every other service credential |
| **Interactive** — an administrator at a shell | A configuration file **owned by that administrator**, mode `0600` — `~/.config/fileengine/credentials` |

The env var takes precedence when both are present.

##### Per-administrator, not one shared token

The per-user file is the better location, and not only for permissions. It makes
the natural unit **one credential per administrator** rather than one shared
`cli` token — which fixes a real weakness in the design as written.

With a single shared secret, the accountability record's `actor` comes from the
invoking OS username, which the CLI *asserts* rather than proves — and §3.6 makes
it overridable with `--actor` for automation. So any administrator holding the
shared token can attribute an action to a colleague. That is a poor property
anywhere; on the identity permitted **every capability**, it is the worst place
on the platform to have it.

So `cli` becomes a small class of identities — `cli:alice`, `cli:bob`,
`cli:ansible` — each with its own secret and each carrying the reserved full
capability set (§6.1) and the loopback constraint. Nothing about the authority
changes; what changes is that **the secret proves who acted**, so the
accountability record for a credential grant or an erasure names a person on
evidence rather than on assertion.

It also makes revocation proportionate. An administrator leaves, and you revoke
their credential — one CLI command, recorded, affecting nobody else. Under a
shared token the same event means rotating for everyone and re-distributing, so
in practice it is deferred, and the credential of a departed administrator stays
valid.

This follows the convention administrators already expect from `ssh`, `aws` and
`kubectl`: a per-user credential file under their own home directory, at `0600`,
that identifies them. A system-wide `/etc/fileengine` token remains available for
the unattended case, but should be understood as a *service* credential —
`cli:ansible` — rather than a human one.

**File permissions are therefore load-bearing for the interactive path**, and
must be specified rather than left to the installer:

**The CLI enforces exactly `0600` on its own credential file** — it does not
merely object to a wrong mode, it makes the mode right. Owner read/write and
**nothing else**:

- **Group access is the point, not just world access.** `0640` is as
  unacceptable as `0644` here. On many systems a user's primary group is shared
  — `users`, `staff`, a team group — so group-readable can mean readable by
  every other administrator on the box, which is precisely the separation
  per-administrator credentials exist to create (§6.1). Any bit beyond owner
  `rw` is stripped.
- **Created correctly, not corrected afterwards.** The file is opened
  `O_CREAT|O_EXCL` with mode `0600` explicitly, so it is never briefly readable
  and then tightened. A create-then-`chmod` sequence leaves a window in which the
  secret is exposed, and that window is enough.
- **`umask`-proof.** An operator's `umask` must not be able to widen it — and
  `umask 002`, common in shared-development environments, yields group-writable
  files by default. So the mode is specified at creation and verified afterwards
  rather than inherited from the process default.
- **Containing directory `0700`.** Defence in depth — the file mode is the
  control, but a group-writable directory invites replacement and rename games
  even when the file itself is `0600`.
- **Tightened on read.** If the file is found with any bit beyond owner `rw`, the
  CLI narrows it to `0600` and continues.

> **Tightening is not remediation, and the tool must say so.** A file that sat at
> `0644` for a week may have been read; `chmod` afterwards does not un-expose the
> secret. So the CLI warns loudly and **recommends rotating that credential**,
> rather than reporting a quiet fix. A tool that silently repairs permissions
> teaches operators that the problem was cosmetic, which is the opposite of
> true — and rotation is one recorded command away (§3.6).

This is stronger than the `ssh` model of refusing to proceed. Refusing is right
for a key `ssh` did not create; here the CLI **owns** the file it writes, so
getting the mode right is its responsibility rather than the operator's homework.

The system-wide file for unattended use is a different artefact: `0640
root:fileengine`, installed by the `.deb`, `.rpm` and `PKGBUILD` packaging, and
deliberately group-readable so the service account can use it. The CLI does not
rewrite that one — it is not its file — but it applies the same read-time check
and warning.

> **The failure mode to warn about explicitly:
> `FILEENGINE_CLI_TOKEN=fesvc_… fileengine_cli …` on a command line.** That lands
> in shell history, and is visible in `ps` and `/proc/<pid>/environ` for the life
> of the process. The file path exists precisely so an administrator never has to
> do that; the documentation should say so, because the env var is otherwise the
> obvious thing to reach for.

**Why this is not the fallback pattern rejected in §3.5.** That one was dropped
because a file *cannot record its own changes*, so keeping it meant keeping a way
to bypass a guarantee. Here both paths deliver the same credential to the same
tool and neither weakens anything — the choice is about how a secret reaches a
process that has no supervisor, not about two different sources of truth.

##### A note on relative danger

Calibrating the `cli` token against the database password is right for
*handling*, but the two are not equally dangerous, and it is worth saying which
way round:

| | `FILEENGINE_PG_PASSWORD` | `cli` token |
|---|---|---|
| Reach | Every tenant's data, ACLs and audit tables | Every capability, but only through the core |
| Constrained by transport | No — any host that can reach PostgreSQL | **Yes** — loopback only (§6.1) |
| Actions recorded | **No** — direct SQL bypasses the accountability record entirely | **Yes** — every operation, attributed to the operator (§3.6) |

So the more dangerous credential of the two is the one that already exists. That
is not an argument for treating the `cli` token casually; it is a reminder that
the controls in this proposal — transport constraint, capability set, recorded
actions — apply to the CLI and do not apply to a `psql` session, which is why
§3.6 makes direct database manipulation unsupported rather than merely
discouraged.

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

### 6.4 Definitions in code, assignments in the database

Two different things were conflated in an earlier draft of this section, which
put both in code:

| | Where | Why |
|---|---|---|
| Capability **definitions** — which RPCs constitute `destroy`, `acl`, `read` | **Code** | A property of the API itself. Expressing it in data would mean inventing a config language for something that changes only when the API does |
| Capability **assignments** — which services hold which capabilities | **Database**, alongside the secret | An operational fact about a deployment, not a property of the API |

**So adding a service is a row.** Insert `service_id`, its hashed secret and its
granted capabilities via the CLI (§3.6); it works immediately, on every node,
within the cache TTL.

#### Zero-downtime onboarding is the feature, not a side effect

No build, no release, no restart — which matters disproportionately here, because
a core restart is a platform outage (`PROPOSAL_accountability_record.md` §7.8).
Compiled assignments would mean **taking the whole platform down to add a
microservice**, and a process that expensive does not get followed carefully; it
gets worked around, usually by granting the new service an existing service's
credential and moving on. The friction would actively produce the outcome the
capability system exists to prevent.

Deploying a new door becomes: issue its credential, grant its capabilities, start
it. Every step online, every step recorded, no window in which the platform is
unavailable. That is worth having on its own terms — it makes the platform
extensible without a maintenance window — and it is only possible because the
registry is data rather than a compiled table.

#### Correcting the earlier argument

The previous draft argued assignments must be compiled so that widening requires
"a code change and a review", on the grounds that a data-driven grant is one
pressured change away from a bridge holding `destroy`.

That reasoning does not survive contact with the rest of this design. Under
§3.6, a grant made through the CLI is **recorded as an accountability event
naming the operator**, in the same transaction, permanently. A code change is
reviewed at pull-request time and then deployed by someone, somewhere, with no
comparable record of the act. **An audit record with your name on it is a
stronger deterrent than a diff in a rushed review** — and it is certainly better
evidence afterwards.

The control was never really "it's in code". It is "the change is deliberate,
attributed and visible", and the database path delivers that better.

#### Preserving the actual intent

What the earlier rule was protecting — that nobody casually hands a bridge
`destroy` — is kept by making dangerous grants *deliberate* rather than
*impossible*:

- **High-risk capabilities are flagged** (`destroy`, `accountability`) and cannot
  be granted in the same operation that issues a credential. Onboarding a service
  and arming it are separate acts.
- Granting one requires an explicit confirmation flag, and writes its own
  accountability record distinct from the ordinary assignment event.
- The startup diff (§3.6) changes role usefully: with assignments in data, a
  `service_capability.changed` record that no CLI operation accounts for is
  **evidence of an out-of-band database edit** — it becomes tamper detection
  rather than deploy-change detection.

#### What remains compiled

Definitions, and one guardrail worth keeping: **the set of capabilities a service
*may ever* hold is not itself data.** `cli` is the only identity permitted the
full set (§6.1), and that stays in code — otherwise the reserved-identity rule is
one `UPDATE` from being untrue.

---

### 6.5 The end user's IP — already built, one door short

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
4. ~~**Should the allowlist live in config or in code?**~~ **Answered in §6.4,
   then revised:** capability *definitions* stay compiled, because they describe
   the API; *assignments* live in the database beside the secret, so onboarding a
   service needs no release and no core restart. The safety the original answer
   was reaching for comes from the grant being deliberate, attributed and
   recorded — not from being compiled.
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
9. **Onboarding is zero-downtime (§6.4).** A new service is added, granted
   capabilities and serving traffic **without restarting the core** — verified
   against a running instance, with an unrelated request in flight throughout.
   Additionally:
   - A grant naming a capability that does not exist is **rejected**, not stored.
   - Granting a **high-risk** capability (`destroy`, `accountability`) requires
     explicit confirmation and cannot ride along with credential issue.
   - `cli` remains the only identity able to hold the full set, and that cannot
     be conferred on another service by any database change.
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
    - The CLI **enforces `0600`** on its own credential file: created with that
      mode atomically, unaffected by the invoking `umask`, and narrowed on read
      if wider. Specifically **`0640` is rejected too** — group access is the
      case that matters, since a shared primary group would otherwise expose one
      administrator's credential to the others.
    - Narrowing a wide file emits a warning **recommending rotation**, not a
      silent fix.
    - `FILEENGINE_CLI_TOKEN` **takes precedence** over the file when both are
      present, so an automated invocation needs no file at all — and the secret
      appears in neither the process's own log output nor any error message.
    - **Per-administrator identity is proven, not asserted:** an action run with
      `cli:alice`'s credential records `alice` even when invoked with
      `--actor bob`, and revoking `cli:alice` leaves `cli:bob` working.
11. Comparison is constant-time, and the core stores no plaintext secret.
    Guardrail (§3.3): **no code path accepts an operator-supplied service
    token** — issuing one always generates 256 bits, so the entropy assumption
    that makes a fast MAC sufficient cannot be weakened by a convenience feature
    added later.
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
14. **The map at rest is unusable alone (§3.5).** A database dump yields no
    usable token, because the pepper is not in the database.
    - **Adding a service takes effect without a core restart** — a transactional
      update, visible within the cache TTL. This is the property that keeps
      credential rotation from costing a platform outage.
    - **Map changes are recorded**: granting a service a capability or rotating
      its secret writes an accountability record in the same transaction.
    - Rotating a **service token** and rotating the **pepper** are independent:
      either can be done without the other, and **neither requires a core
      restart** (§3.5).
    - **Multi-node rotation:** with two core instances running, a token added on
      one is accepted by both within the cache TTL, and a token removed is
      rejected by both — no per-node action.
    - **Pepper rotation is online:** with both peppers configured, rows migrate
      to the new one as services authenticate, no token is re-issued, and the
      old pepper is retired only once a query shows no rows remain on it.
15. **The CLI is the only supported management path (§3.6).**
    - `issue` prints the secret **once** and never again; `list` never prints one.
    - Every management command writes an accountability record naming the
      **invoking operator**, not `cli`, in the same transaction — a failed record
      write means the credential change does not happen.
    - **No record contains a secret, hash or pepper**, asserted by scanning the
      accountability table for the issued token after a full issue → rotate →
      prune → revoke cycle and finding nothing.
    - Pepper rotation emits **exactly two** records (start, completion) however
      many services migrate in between.
    - Revoking `audit_service`'s own credential still produces a durable record.
    - Every credential record carries the service's **effective capability set**
      at that moment, so the log shows what the credential could do without
      consulting the deployed binary.
    - **Out-of-band edits are caught:** a capability added by direct `UPDATE`
      produces a `service_capability.changed` record at next startup that no
      grant accounts for, while a restart after a normal CLI grant produces none
      — the diff signals tampering rather than routine change.
    - `--json` output and exit codes make the full issue → rotate → prune cycle
      scriptable without parsing human text.
    - `pepper status` reports rows per pepper version, so "is the rotation
      finished" is answerable rather than estimated.
    - The first credential (`cli:bootstrap`) is created by **packaging at
      install**, under the same generation rules — there is no unauthenticated
      bootstrap mode — and can be revoked once per-administrator credentials
      exist, without locking anyone out.
    - There is **no file-based map path** to configure, so no way to change
      service credentials without an accountability record (§3.5).
13. **End-user origin survives every external door (§6.5).** A request through
    `bcf_services` records the caller's real address, not an internal one — the
    one door currently missing it. And an event-driven internal call records an
    **empty** `source_addr` rather than a substituted peer address, so
    "not user-initiated" stays distinguishable from "user at 10.0.0.4".
14. **Derived writes do not inherit the originator's address (§6.5.2).** After a
    user uploads a file and csai writes its rendition, exactly **one** record
    carries the user's address — the upload. The rendition record shows the user
    as `actor`, `csai` as `source_iface`, and an empty `source_addr`.
