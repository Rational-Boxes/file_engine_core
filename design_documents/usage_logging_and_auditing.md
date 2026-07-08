# Full Usage Logging & Auditing — Design Plan

**Status:** Design draft (no code yet)
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
security team wants.

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

**The shared write contract.** Because several services append to one tenant's
`audit_log`, there must be exactly one sanctioned way to write it:

- *Option A — direct DB append.* Each service inserts into
  `"<tenant_schema>".audit_log` with the append-only DB role (§7). Simplest —
  every service already has Postgres — but the hash chain (§7) must be safe under
  concurrent writers (a per-tenant advisory lock, or serial `seq` with deferred
  chaining).
- *Option B — a thin audit-ingest RPC/endpoint* (`AppendAudit(entry)`), one
  writer owning ordering + the append-only guarantee + the hash chain; costs a
  network hop on the (hopefully rare) auth-failure path.

Leaning **B for the chain-critical categories** (one component owns tamper-
evidence) with A acceptable where a service already holds the tenant connection —
an open decision (§12). Either way the schema, categories, and access controls
are identical across emitters, and each entry carries `source_iface` so a reader
can tell a core file-read from an ldap_manager role change from a bridge login
failure.

---

## 6. Delivery guarantees & performance

Audit must be **complete**, but reads are high-volume. Design:

- **In-process durable writer:** handlers append to a bounded lock-free queue; a
  writer thread flushes to `audit_log` in batches (multi-row INSERT) on the
  per-tenant connection. This keeps the request path ~microseconds.
- **Spool-ahead durability:** the queue is backed by an append-only local WAL
  file (fsync-batched) so a DB blip or crash doesn't lose buffered records; the
  writer drains the WAL into Postgres and truncates on success. (Mirrors the
  core's existing "write local, sync async" philosophy.)
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

Tenant administrators get a dedicated **Audit & Events** view in the SPA, gated by
`AUDIT_READ` (§8) and hidden for everyone else. It lives in the admin area
alongside the existing tenant user/role console (served by `ldap_manager`),
consumes the §9 REST endpoints, and is scoped to the caller's tenant by
construction (subdomain / `X-Tenant`). Two tabs:

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

## 11. Rollout phases

1. **Schema + write contract** — provision `audit_log` (+ global) in tenant
   setup; the durable async writer with WAL; settle the shared write path (§5
   Option A/B) so *every* emitter uses one sanctioned append.
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
9. **Frontend Audit & Events console** (§10) — the tenant-admin SPA view over the
   query/export/verify APIs plus the admin-scoped live events feed.

Forward-only: no backfill (there's no prior read history to reconstruct).

---

## 12. Open decisions (need a call before coding)

- **Shared write path (§5 A vs B):** direct per-tenant DB append from each emitter
  vs a single `AppendAudit` ingest owned by one writer. Drives how the hash chain
  and append-only guarantee are enforced across the core, `ldap_manager`, and the
  doors.
- **Where auth failures are emitted:** each door on its own bind failure (needs a
  shared audit-write helper in the bridges) vs. routing all authentication through
  a common path that emits once. Doors are independent today, so the former is
  likely — confirm.
- **Access-log volume:** full-fidelity reads vs sampled vs aggregate `count` as
  the *default* (`AUDIT_ACCESS_MODE`). Compliance usually wants full; cost may
  push to sample-with-force-full-on-sensitive-subtrees.
- **Fail-closed scope:** confirm `permission`/`user`/`auth`/`admin` should block
  the op on audit-write failure (recommended) vs degrade to WAL-only.
- **Source metadata plumbing:** the bridges must forward client IP / interface /
  request-id into the `AuthenticationContext` (small proto/auth change) so
  `source_addr`/`source_iface` are populated — otherwise audit shows "who" but
  not "from where."
- **Retention defaults & archival target** per tenant; external hash anchoring
  cadence (if any).
- **Consolidation with the MCP tool-audit:** the MCP already writes a local
  tool-call audit; the core `audit_log` becomes the source of truth and the MCP's
  is either dropped or kept as a thin app-layer trace.
- **Where the admin console lives (§10):** a route in the main SPA admin area
  (reuses the app's auth/components, one place for admins) vs. folding it into the
  `ldap_manager`-served tenant-admin console (co-located with user/role
  management). Leaning SPA — the audit data comes from the core via the REST
  bridge, not from `ldap_manager`.

---

## 13. Non-goals

- Replacing telemetry/metrics (that's `monitoring_and_telemetry.md`).
- Replacing the event stream (notifications/search keep using it).
- Cross-tenant analytics (each tenant's audit is isolated by schema).
- Real-time alerting (a downstream consumer of the export API, not core scope).
