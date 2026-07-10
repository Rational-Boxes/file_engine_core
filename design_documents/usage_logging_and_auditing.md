# Full Usage Logging & Auditing — Design Plan

**Status:** Design draft (no code yet) — key decisions resolved (§13); ready for review
**Branch:** `design/audit-logging`
**Audience:** people deciding what to build before any code lands

## 0. Goal

A complete, immutable, **per-tenant** audit trail of *who did what to what,
when, from where, and whether it was allowed*. Every **file access** (reads
included), every **change to a file**, and every **change to permissions or
user privileges** must produce a durable audit record that a tenant
administrator (and only they) can query and export for compliance — through a
dedicated **Audit & Events console in the frontend** (§10). Denied attempts are
recorded too — a rejected read of a sensitive file is exactly the signal a
security team wants. And the trail isn't only for hindsight: the same aggregated
stream drives a **security rules engine** (§11) that detects suspicious activity
in real time and can **flag, alert, or auto-disable**.

This is distinct from the existing telemetry (aggregate ops metrics — see
[`monitoring_and_telemetry.md`](monitoring_and_telemetry.md)) and from the event
stream (best-effort notifications). Audit is *evidence*: complete, ordered,
tamper-evident, and retained.

---

## 1. What already exists (and why it isn't enough)

| Facility | Covers | Gap for audit |
|---|---|---|
| **`acl_audit`** table, per tenant (`database.cpp`) — `resource_uid, principal, principal_type, action('grant'|'revoke'), permissions_before/after, performed_by, performed_at` | ACL grant/revoke | No role/privilege changes, no request context (source/session), no tamper-evidence, mutable |
| **Event system** — `FileEventType` (`File/Dir Created/Updated/Deleted/Moved/Renamed/Restored`, `AclChanged`), `emit_fs_event`/`emit_acl_event` → Redis (`redis_event_queueing_plan.md`) | Mutations, for notifications/search/digests | **Fail-open by design** ("event emission must never disturb the filesystem operation") → not a complete record. **Reads are not emitted at all.** |
| **`StorageTracker`** | per-file *aggregate* access counts | No per-access actor/time/result — can't answer "who read file X on the 3rd" |
| **Core `RoleManager`** (`create_role`, `assign_user_to_role`, …) | the RBAC roles the core *consumes* on file ACLs | **Nothing is logged** |
| **`ldap_manager` service** — the tenant admin console: user/group/role lifecycle, email invitations, self-service password reset/recovery, profile/password changes | where **user management and authentication self-service actually happen** | **Nothing is audited** — and password failures, lockouts, and recovery attempts have no record anywhere |
| **Authentication at each door** (http-bridge, webdav, MCP, discussion bind to LDAP) | resolves identity per request | **Login failures / successes are not recorded** — no brute-force or takeover signal |

So: permission changes are *partially* audited; mutations are logged only as
fail-open events; **file reads, user/privilege management, and every
authentication event (incl. password failures and recovery) are not audited at
all**; and nothing is immutable or tamper-evident. Critically, the trail is
**distributed** — file activity lives in the core, but user and auth activity
lives in `ldap_manager` and the authenticating doors (§5).

---

## 2. Audit ≠ Events (two pipelines, on purpose)

| | **Audit log** (new) | **Event stream** (exists) |
|---|---|---|
| Purpose | Compliance evidence | Notifications, search index, digests |
| Delivery | **Complete + durable** (buffered, spooled, never silently dropped) | Best-effort, **fail-open** |
| Reads | **Yes** (access is the point) | No |
| Mutability | Append-only, tamper-evident | Ephemeral queue |
| Consumer | Tenant admin / auditor, gated | Discussion service, CSAI indexer, dashboard |

They share emit points but are independent sinks. A failure to enqueue an event
must never block an op; a failure to *durably* record an audit entry is a
policy decision (§6), not a silent drop.

---

## 3. What gets audited (the taxonomy)

Grouped by `category`, each an `action`. The **emitter** matters — audit is
written by more than one service (§5), not the core alone:

- **access** *(Core gRPC)* — `read`/`get`, `stat`, `list`, `list_deleted`,
  `read_version`, `download_stream`, `exists`, and **denied** access attempts.
  (`check_permission` itself is not audited to avoid recursion/noise; the
  *guarded operation* is.)
- **mutate** *(Core gRPC)* — `create_file`, `create_dir`, `write`/`put`,
  `rename`, `move`, `copy`, `soft_delete`, `undelete`, `restore_version`,
  `cull_versions`, `set_metadata`, `delete_metadata`.
- **permission** *(Core gRPC)* — `acl_grant`, `acl_revoke`, `acl_inherit`,
  `acl_default_apply` (supersedes/absorbs today's `acl_audit`). These act on file
  resources, so they stay in the core.
- **user** *(ldap_manager)* — the tenant admin console owns the directory, so
  **user and privilege lifecycle emits from `ldap_manager`, not the core**:
  `user_create`, `user_delete`, `user_disable`, `role_create`, `role_delete`,
  `role_assign_user`, `role_remove_user`, `group_add_member`,
  `group_remove_member`, `profile_update`, `invite_send`. (The RBAC roles the core
  *consumes* on file ACLs are managed here.)
- **auth** *(every authenticating door + ldap_manager)* — authentication and
  account recovery, including events that occur **before a session exists**:
  `login_success`, `login_failure` (bad password / failed LDAP bind),
  `account_lockout`, `password_reset_request`, `password_reset_complete`,
  `password_change`. Actor is the *attempted* identity; these are the
  brute-force / account-takeover signals a security team lives on.
- **admin** *(Core + ldap_manager)* — `purge_versions`, `trigger_sync`, tenant
  lifecycle (global, §8), and **`audit_read`/`audit_export`** (audit the
  auditors).

Every entry also records the **outcome**: `ok` | `denied` | `error`.

---

## 4. Record schema (per-tenant `audit_log`)

Provisioned into every tenant schema alongside `acl_audit` (same code path).
Append-only.

```sql
CREATE TABLE "<schema>".audit_log (
    seq            BIGSERIAL PRIMARY KEY,      -- per-tenant monotonic order
    ts             TIMESTAMPTZ NOT NULL DEFAULT now(),
    category       SMALLINT     NOT NULL,      -- access|mutate|permission|user|auth|admin
    action         VARCHAR(32)  NOT NULL,
    outcome        SMALLINT     NOT NULL,      -- ok|denied|error
    actor          VARCHAR(255) NOT NULL,      -- resolved end-user identity
    actor_roles    TEXT,                       -- effective roles at decision time
    target_uid     VARCHAR(64),                -- file/dir/role/principal
    target_name    VARCHAR(1024),              -- name/path snapshot (best-effort)
    target_type    SMALLINT,                   -- file|dir|role|acl|version
    detail         JSONB,                      -- action-specific: {before,after} perms,
                                               --   dest_uid for move, version ts, byte range…
    source_iface   VARCHAR(16),                -- grpc|rest|webdav|mcp
    source_addr    VARCHAR(64),                -- client IP (from the bridge)
    request_id     VARCHAR(64),                -- correlates multi-hop (bridge→core)
    prev_hash      BYTEA,                       -- tamper-evidence chain (§7)
    row_hash       BYTEA NOT NULL
);
CREATE INDEX idx_audit_actor  ON "<schema>".audit_log(actor, ts);
CREATE INDEX idx_audit_target ON "<schema>".audit_log(target_uid, ts);
CREATE INDEX idx_audit_action ON "<schema>".audit_log(category, action, ts);
-- Monthly range partitioning on ts for retention/archival (§7).
```

`detail` as JSONB keeps the table narrow while preserving action-specific context
(the ACL `permissions_before/after`, a move's destination, a version timestamp,
the read byte range, the reviewer set, etc.).

---

## 5. Where it's emitted (distributed emitters, one per-tenant sink)

There is **no single choke point**. Audit originates in three places, all writing
the *same* per-tenant `audit_log`:

1. **Core gRPC service handlers** — `access`, `mutate`, `permission`. Every RPC
   already holds the `AuthenticationContext` (user, roles, tenant, claims) and a
   `Result`, so one `audit(...)` per handler captures actor + outcome. Emitting
   here — rather than in the inner `FileSystem`/`AclManager` — uniquely sees the
   *source* metadata the bridges forward (interface, client IP, request-id). The
   existing FileSystem event emit points stay as-is; they feed the separate
   *event* pipeline. A thin `AuditSink` is threaded like today's `event_sink_`.
2. **`ldap_manager` service** — the `user` category (user/role/group/profile/
   invite lifecycle) and the self-service half of `auth` (`password_reset_request`
   / `_complete`, `password_change`). It owns the directory and its Postgres
   directly, so it writes its own audit entries. **User management does not pass
   through the core**, so the core can't and shouldn't try to log it.
3. **Every authenticating door** (http-bridge, webdav-bridge, MCP, discussion,
   and ldap_manager's own login) — the `auth` outcomes. A failed LDAP bind happens
   *before* a session exists, at whichever door the request hit, so each door
   emits `login_failure` (actor = attempted username, outcome = denied, source =
   that interface + IP) and `login_success`. This is what makes password-failure
   and lockout auditing possible — the signal lives at the authentication layer,
   not the core.

**The shared write contract (decided): publish to a queue, one consumer writes
the tables.** Because several services append to one tenant's `audit_log`, there
is exactly one sanctioned way to write it — and it is **asynchronous through the
existing Redis event queue** ([`redis_event_queueing_plan.md`](redis_event_queueing_plan.md)),
not a synchronous DB insert from each emitter:

- **Emitters publish security/audit events to a durable queue.** Every emitter —
  core handlers, `ldap_manager`, each authenticating door — serializes an audit
  entry (schema of §4, tagged with `tenant`) and enqueues it. Emitters never
  touch `audit_log` directly and hold no chain state.
- **A dedicated audit-consumer process drains the queue and writes the tables.**
  One consumer owns ordering, the append-only guarantee, and the per-tenant hash
  chain (§7) — the single-writer invariant that makes the chain safe without any
  cross-service advisory lock. It batches multi-row INSERTs per tenant on the
  append-only DB role, and is the same process that hosts the security rules
  engine (§11), which already needs to see every event.

This supersedes the earlier A/B framing. **Option A** (direct per-emitter DB
append) is rejected: concurrent writers across the core, `ldap_manager`, and the
doors make the hash chain fragile. **Option B** (a synchronous `AppendAudit` RPC)
is rejected for the auth-failure hot path: a queue publish is cheaper and does
not couple a login attempt's latency to the audit writer's availability. The
queue gives us B's single-writer tamper-evidence with A's decoupling.

**Durability, not best-effort.** This queue is *not* the fail-open event stream
of §2. The publish must be durable: an emitter that cannot enqueue treats it per
the §6 failure policy (fail-closed categories reject the op; `mutate`/`access`
spool to the local WAL and retry). The consumer acks only after the row is
committed, so a consumer crash re-delivers rather than drops. Either way the
schema, categories, and access controls are identical across emitters, and each
entry carries `source_iface` so a reader can tell a core file-read from an
ldap_manager role change from a bridge login failure.

---

## 6. Delivery guarantees & performance

Audit must be **complete**, but reads are high-volume. Design:

- **Publish off the hot path:** handlers serialize an entry and publish it to the
  shared queue (§5) via a bounded in-process buffer; the request path is a single
  enqueue (~microseconds), never a DB round-trip. The out-of-process
  audit-consumer flushes to `audit_log` in batches (multi-row INSERT) on the
  per-tenant connection.
- **Spool-ahead durability:** the in-process buffer is backed by an append-only
  local WAL file (fsync-batched) so a queue outage or crash doesn't lose buffered
  records; the emitter drains the WAL to the queue and truncates on success — the
  consumer then owns the DB write. (Mirrors the core's existing "write local, sync
  async" philosophy.) "Durably spooled" for the §6 failure policy means the entry
  is committed to the WAL or accepted by the queue, not that the row has reached
  `audit_log`.
- **Failure policy is per-category, configurable:**
  - `permission`, `user`, `auth`, `admin` → **fail-closed** by default: if the
    entry can't be durably spooled, the *operation* is rejected. These are rare
    and security-critical (a permission change, a role grant, or a password reset
    must never happen silently).
  - `mutate` → spooled durably; op proceeds (WAL guarantees eventual capture).
  - `access` → spooled; on sustained backpressure, degrade per a knob
    (`AUDIT_ACCESS_MODE = full | sample:N | count`) — never silently to nothing;
    dropping to `count` still records aggregate access, and the mode change is
    itself audited.
- **Read-path budget:** a single enqueue + name snapshot already resolved by the
  op. No extra DB round-trip on the hot path.

---

## 7. Immutability, tamper-evidence, retention

- **Append-only at the DB role level:** the app's runtime DB role gets
  `INSERT, SELECT` on `audit_log` but **not** `UPDATE`/`DELETE`; schema/partition
  management runs as a separate migration role. (Enforced in provisioning.)
- **Hash chain per tenant:** `row_hash = H(prev_hash ‖ canonical(row))`. Any
  edit/removal breaks the chain; a `VerifyAuditChain` op (and a periodic job)
  walks it. Optionally anchor the latest hash to the object store / an external
  notary on a schedule for external verifiability.
- **Retention & archival:** monthly range partitions; a retention job seals old
  partitions and archives them (encrypted) to the tenant's S3 bucket, then drops
  them per a per-tenant `AUDIT_RETENTION_DAYS`. Archived segments keep their hash
  linkage so the chain remains verifiable end-to-end.

---

## 8. Access control on the audit itself + tenancy

- Reading/exporting the audit log is **privileged**: a new `Permission::AUDIT_READ`
  (next free bit, e.g. `0x4000`), granted to tenant admins; `system_admin`
  bypasses as everywhere. It is **never** read-by-default.
- **Per-tenant isolation is structural:** `audit_log` lives in the tenant schema,
  so a query can only ever see one tenant's rows — consistent with the ACL model
  and the existing `acl_audit`.
- **Audit the auditors:** every `audit_read`/`audit_export` writes its own
  `admin` entry.
- **Global audit** (tenant create/drop, cross-tenant admin) goes to a small
  `public.audit_log_global` with the same shape, readable only by `system_admin`.

---

## 9. Query & export API (new RPCs → REST/MCP)

- `QueryAuditLog(tenant, {actor?, target_uid?, category?, action?, outcome?,
  from?, to?}, page)` → filtered, ordered page. Requires `AUDIT_READ`.
- `ExportAuditLog(...)` → **streaming** RPC for compliance dumps (NDJSON/CSV),
  including archived partitions.
- `VerifyAuditChain(tenant, range)` → integrity result.

Surfaced through the http-bridge (REST) and MCP (so an agent can answer "who
changed the Q3 deck's permissions?" — under the caller's `AUDIT_READ`, and that
query is itself audited). Each is ACL-gated identically to every other door.

---

## 10. Frontend surface — tenant-admin Audit & Events console

Tenant administrators get a dedicated **Audit & Events** view **folded into the
existing `ldap_manager` tenant-admin console** ("LDAP-admin+"), co-located with
the user/role/group management the admin already uses — not a separate route in
the main SPA. It is gated by `AUDIT_READ` (§8) and hidden for everyone else,
consumes the §9 REST endpoints (the audit *data* still comes from the core via
the REST bridge; `ldap_manager` only hosts the surface and the `user`/`auth`
audit it emits itself), and is scoped to the caller's tenant by construction
(subdomain / `X-Tenant`). This keeps every tenant-administration action — user
lifecycle, role grants, and now audit review + security rules — behind one
console and one auth surface. Three tabs:

- **Audit** — the immutable log. A filterable, paginated table over
  `QueryAuditLog`: filter by **actor**, **target** (file / user / role),
  **category** (access · mutate · permission · user · auth · admin), **action**,
  **outcome** (ok · denied · error), and **time range**. A one-click
  *denied/error* filter for security review. Each row expands to its `detail`
  (before/after permissions, move destination, version, byte range, and the
  `source_iface`/`source_addr` — "from where"). An **Export** button streams
  `ExportAuditLog` (NDJSON/CSV) for compliance dumps, and a **chain-integrity
  badge** reflects `VerifyAuditChain` (green = verified, red = tampered).
- **Events** — the live operational feed. Reuses the existing bounded real-time
  layer (the discussion service's WS activity/presence, §10h of the discussion
  plan) but in an **admin, tenant-wide** scope rather than the personal dashboard
  feed — recent file/permission activity across the tenant as it happens, still
  ACL-filtered to what the admin may see.
- **Security** — the rules-engine surface (§11): open **incidents** (flagged
  suspicious activity, each linking to its evidence rows in the Audit tab), the
  **rule builder** (guided form *or* raw-DSL, with enable/disable, mode = flag ·
  alert · auto-disable, severity, thresholds, dry-run, and validate-against-history),
  a **disabled-accounts** review with one-click re-enable, and the **admin
  notification settings** (which admins receive the mandatory serious-alert emails,
  digest cadence for non-serious alerts).

Design notes:

- **Read-only + gated.** The console never mutates; it's purely a window. It is
  hidden unless the identity holds `AUDIT_READ`, and every query it issues is
  itself audited (`audit_read`) — the admin's own inspection is on the record.
- **Per-tenant by construction.** Every call carries the tenant, so an admin only
  ever sees their own tenant's trail — matching the schema isolation.
- **Pivotable.** Audit rows deep-link to their target — `/preview/:uid` for a
  file, the user/role admin page for a principal — so an admin moves from
  "who changed this and from where" straight to the resource.
- **Reuse.** Built from the app's existing table / filter / detail-drawer
  components and the dashboard's live-event plumbing; no new UI framework.

---

## 11. Security rules engine (detect → flag / alert / auto-disable)

The trail isn't only for hindsight. A **rules engine** rides the aggregated
activity stream to catch suspicious behaviour as it happens and respond. It is a
**consumer** of the same per-tenant feed the audit writer produces (§5) and the
dashboard activity aggregator (discussion service) already builds — not a new
emit point, and not in any request's hot path.

**Where it runs.** A dedicated consumer alongside the existing event/activity
aggregator: it subscribes to the audit/event stream (Redis), keeps small
per-tenant sliding-window counters, evaluates rules, and acts. Per-tenant
isolation is inherited from the stream; rules + responses are configured per
tenant (with conservative system defaults).

**What it detects.**
- **Rate / threshold** — N `login_failure` for one actor or source IP in M minutes
  (brute force); X `access` reads or `download_stream` bytes in Y minutes (bulk
  exfiltration); a burst of `soft_delete` (mass deletion).
- **Sequence / pattern** — `login_failure`×k then `login_success` (a successful
  guess); a `permission`/`user` change immediately followed by broad access;
  first-ever access from a new source IP/interface; access to a sensitive-tagged
  subtree.
- **Baseline / anomaly** (later phase) — deviation from an actor's rolling norm
  (volume, hours, breadth of tree touched).

**Graduated response, per rule.**
- **flag** — record an incident, surface it in the console's Security tab (§10)
  and the admin digest. No user impact.
- **alert** — notify tenant admins out-of-band via the existing digest channel
  (email/push), rate-limited so it never becomes a notification treadmill (the
  Phase-3 anti-goal). **Serious alerts email every Administrator-level user
  (mandatory).** Each rule carries a `severity`; at `severity = serious` (or
  higher) the response is a **guaranteed, immediate email to all admin-role users
  of the tenant** — resolved from `ldap_manager` (the admin role membership), sent
  the moment the incident is raised. This path is *not* subject to the digest
  rate-limit or the every-N-minutes rollup: a brute-force lockout, a detected
  successful password guess, or a bulk-exfiltration incident reaches a human
  inbox immediately. Rate-limiting still applies *within* a single serious
  incident (dedupe repeats of the same incident) so one attack is one email, not a
  storm — but serious severity can never be silenced down to nothing, and the
  email send is itself an `admin`-category audit entry. Non-serious alerts stay in
  the rate-limited digest.
- **auto-disable** — the strong response: disable the actor via `ldap_manager`
  (`user_disable` / `account_lockout`). Live sessions expire on their own because
  the bridges issue **short-TTL tokens** (decided, §13): revocation is *implicit*
  — once the directory account is disabled, the next token refresh fails, so no
  shared deny-list or token-store purge is needed. This bounds the exposure window
  to the token TTL and keeps the bridges' stateless-JWT model intact; the priority
  case it hardens is a **brute-force login attack**, where the account is locked
  before a guessed password yields a durable session. It orchestrates *existing*
  enforcement — it invents no new kill path.

**Auto-disable safeguards (this is the dangerous lever).**
- **Off by default.** Every rule ships in `flag` or `alert` mode; auto-disable is
  an explicit per-rule opt-in, and starts in a **dry-run** ("would have disabled")
  window so an admin can confirm the rule is sane before it bites.
- **Never lock everyone out.** Refuse to auto-disable the last enabled tenant
  admin; service accounts are exemptible via an allowlist; a cooldown prevents
  flapping.
- **Reversible, human-in-the-loop.** A disabled account appears in the Security
  tab with one-click re-enable; auto-disable can require admin confirmation rather
  than acting unattended (a per-rule choice).
- **Fully audited.** Every detect / flag / alert / disable is itself an
  `admin`-category audit entry (which rule, why, the evidence rows), so the
  engine's own actions are on the same immutable, tamper-evident record; an
  auto-disable also emits the corresponding `user`/`auth` event.

**Reuse, not reinvention.** The engine composes what already exists — the event
aggregator, the Phase-3 digest delivery, `ldap_manager`'s disable/lockout, and the
bridges' token store — behind a per-tenant rule set. It runs in the same
audit-consumer process (§5), so it sees every event as it is written.

**Rules are a Security DSL (decided), seeded from defaults, edited in-console.**
Not a hardcoded catalog: rules are expressed in a small, declarative
**security DSL** — a rule is a `when` (event category/action + a sliding-window
predicate: count/rate/sequence over an actor, source IP, or target subtree), a
`threshold`, a `severity` (`info` · `warn` · `serious` · `critical` — drives the
mandatory-admin-email path, above), and a graduated `response` (`flag` · `alert` ·
`auto-disable`, with `dry-run`). The DSL is **deterministic** by construction
(§13): fixed operators over the windowed counters, no learned baselines yet.

- **System-default rule set to import from.** Every tenant is seeded with a
  conservative, versioned **default rule pack** the tenant administrator imports
  and then tunes — brute-force lockout, exfiltration flag, mass-delete flag — so
  protection exists on day one without authoring anything. Defaults ship in
  `flag`/`alert` (auto-disable stays opt-in, §11 safeguards).
- **Rule editor in the console — build rules two ways.** The Security tab (§10)
  gets a **rule builder** with a choice of entry:
  - a **guided builder** (the default option) — a form/wizard that assembles a
    valid rule from dropdowns (event → window → threshold → severity → response)
    without the admin writing any DSL, then shows the DSL it produced;
  - a **raw-DSL editor** for power users, with syntax validation.

  Both paths let the admin add/clone/disable rules, adjust thresholds and windows,
  set severity and the response mode, toggle dry-run, and **validate against
  recent history** ("this rule would have fired N times last week") before
  enabling. Rule edits are themselves `admin`-category audit entries.

A richer expression language (nested boolean logic, cross-rule correlation) can
grow from this grammar later; behavioural/anomaly baselines remain deferred
(§13).

---

## 12. Rollout phases

1. **Schema + write contract** — provision `audit_log` (+ global) in tenant
   setup; stand up the queue publish path (§5) with the emitter-side WAL and the
   dedicated audit-consumer that writes the tables, so *every* emitter uses one
   sanctioned, async append.
2. **Permission (core)** — fold `acl_audit` into `audit_log` (keep `acl_audit` as
   a compatibility view during transition); fail-closed policy.
3. **Auth (doors + ldap_manager)** — `login_failure`/`_success`, lockouts, and the
   self-service `password_reset_*`/`password_change` from `ldap_manager`. Highest
   security value, low volume — do it early.
4. **User management (ldap_manager)** — user/role/group/profile/invite lifecycle
   emitted from the admin console; fail-closed policy.
5. **Mutations (core)** — audit all write/delete/restore/metadata ops with `detail`.
6. **Access logging (core)** — audit reads/list/stat/version-reads incl. denials,
   with the `AUDIT_ACCESS_MODE` throughput controls.
7. **Tamper-evidence** — hash chain + `VerifyAuditChain` + append-only DB grants.
8. **Query/export + retention** — the RPCs, REST/MCP exposure, partitioning,
   archival, per-tenant retention.
9. **Frontend Audit & Events console** (§10) — folded into the `ldap_manager`
   tenant-admin console, over the query/export/verify APIs plus the admin-scoped
   live events feed, and the security rule editor (§11).

Forward-only: no backfill (there's no prior read history to reconstruct).

---

## 13. Decisions

### Resolved (calls made — folded into the sections above)

- **Shared write path (§5): queue + consumer.** Emitters publish security/audit
  events to the existing Redis queue; a dedicated audit-consumer process drains it
  and writes `audit_log`, owning ordering, the append-only guarantee, and the hash
  chain. Rejects both direct per-emitter DB append (fragile chain under concurrent
  writers) and a synchronous `AppendAudit` RPC (couples the auth-failure path to
  the writer's availability).
- **Where auth failures are emitted (§5):** each authenticating door publishes its
  *own* `login_failure`/`login_success` to the queue — no common auth choke point
  to build. The queue is the shared write helper, so a door needs only a serialize
  + publish, not a DB connection or chain state.
- **Source metadata (§4/§5):** **yes — collect it.** Bridges forward client IP,
  interface, and request-id into the `AuthenticationContext` (small proto/auth
  change) so `source_addr`/`source_iface`/`request_id` are populated. "From where"
  is a first-class field, not best-effort.
- **Admin console location (§10): fold into `ldap_manager`.** The Audit & Events +
  Security surface lives in the existing `ldap_manager` tenant-admin console
  ("LDAP-admin+"), co-located with user/role management, *not* a separate SPA
  route. The audit data still comes from the core via the REST bridge; the console
  only hosts the view.
- **Rules engine — DSL, not a fixed catalog (§11):** a small deterministic
  **security DSL** with a **system-default rule pack** the tenant admin imports and
  tunes, plus an in-console **rule builder** offering both a guided form and a
  raw-DSL editor. Supersedes the earlier "catalog first, DSL later" lean.
- **Serious alerts email admins — mandatory (§11):** every rule carries a
  `severity`; a `serious`/`critical` incident sends a guaranteed, immediate email
  to *all* Administrator-level users of the tenant (resolved via `ldap_manager`),
  outside the digest rate-limit and never silenceable to nothing. Non-serious
  alerts stay in the rate-limited digest.
- **Auto-disable token revocation (§11): short token TTLs.** The bridges keep
  stateless JWTs with short TTLs; disabling the directory account makes the next
  refresh fail, so revocation is implicit — no shared deny-list. The priority case
  is brute-force login lockout (lock the account before a guess yields a durable
  session).
- **Anomaly/baseline detection (§11): deterministic rules initially.** Ship
  rate/sequence/threshold rules over sliding-window counters; defer per-actor
  behavioural baselines to a later phase.

### Still open (need a call before the relevant phase)

- **Access-log volume default (§6):** full-fidelity reads vs sampled vs aggregate
  `count` as the `AUDIT_ACCESS_MODE` *default*. Compliance usually wants full; cost
  may push to sample-with-force-full-on-sensitive-subtrees. (Phase 6.)
- **Fail-closed scope (§6):** confirm `permission`/`user`/`auth`/`admin` block the
  op when the entry can't be durably enqueued/spooled (recommended) vs degrade to
  WAL-only. (Phases 2–4.)
- **Retention defaults & archival target** per tenant; external hash-anchoring
  cadence, if any. (Phase 8.)
- **Consolidation with the MCP tool-audit:** the MCP already writes a local
  tool-call audit; the core `audit_log` becomes the source of truth and the MCP's
  is either dropped or kept as a thin app-layer trace.

---

## 14. Non-goals

- Replacing telemetry/metrics (that's `monitoring_and_telemetry.md`).
- Replacing the event stream (notifications/search keep using it).
- Cross-tenant analytics (each tenant's audit is isolated by schema).
- Integrating with an external SIEM / alerting platform. Real-time alerting
  *within* the product — the rules engine's incidents and the mandatory
  serious-alert admin emails (§11) — is in scope; shipping the trail to a
  third-party SIEM is a downstream consumer of the export API (§9), not core scope.
