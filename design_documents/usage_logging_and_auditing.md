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
| **RoleManager** (`create_role`, `delete_role`, `assign_user_to_role`, `remove_user_from_role`) | privilege changes happen here | **Nothing is logged** |

So: permission changes are *partially* audited, mutations are logged only as
fail-open events, **file reads and privilege changes are not audited at all**,
and nothing is immutable or tamper-evident.

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

Grouped by `category`, each an `action`:

- **access** — `read`/`get`, `stat`, `list`, `list_deleted`, `read_version`,
  `download_stream`, `exists`, and **denied** access attempts. (`check_permission`
  itself is not audited to avoid recursion/noise; the *guarded operation* is.)
- **mutate** — `create_file`, `create_dir`, `write`/`put`, `rename`, `move`,
  `copy`, `soft_delete`, `undelete`, `restore_version`, `cull_versions`,
  `set_metadata`, `delete_metadata`.
- **permission** — `acl_grant`, `acl_revoke`, `acl_inherit`, `acl_default_apply`
  (supersedes/absorbs today's `acl_audit`).
- **privilege** — `role_create`, `role_delete`, `role_assign_user`,
  `role_remove_user`. (Tenant lifecycle — `tenant_create`/`drop` — is a *global*
  audit, §8.)
- **admin** — `purge_versions`, `trigger_sync`, and **`audit_read`/`audit_export`**
  (audit the auditors).

Every entry also records the **outcome**: `ok` | `denied` | `error`.

---

## 4. Record schema (per-tenant `audit_log`)

Provisioned into every tenant schema alongside `acl_audit` (same code path).
Append-only.

```sql
CREATE TABLE "<schema>".audit_log (
    seq            BIGSERIAL PRIMARY KEY,      -- per-tenant monotonic order
    ts             TIMESTAMPTZ NOT NULL DEFAULT now(),
    category       SMALLINT     NOT NULL,      -- access|mutate|permission|privilege|admin
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

## 5. Where it's emitted (the single choke point)

Three candidate layers:

1. **gRPC service handlers** (`grpc_service`) — every RPC already has the
   `AuthenticationContext` (user, roles, tenant, claims) and returns a
   `Result`. This is the **natural cross-cutting seam**: one `audit(...)` helper
   invoked once per handler with the resolved outcome. It also uniquely sees the
   *source* metadata forwarded by the bridges (interface, client IP, request-id
   — added to `AuthenticationContext`/metadata).
2. **`FileSystem`/`AclManager`/`RoleManager`** — where the existing *events* are
   emitted. Good for deep detail but blind to the request source and duplicated
   across entry points.
3. **A gRPC interceptor** — uniform, but too coarse to know target UIDs/outcomes
   per method.

**Recommendation: emit at the service-handler layer** (1), reusing the identity
and outcome already in hand, and carry `source_iface`/`source_addr`/`request_id`
by extending the auth/metadata the bridges pass. The FileSystem event emit points
stay as-is (they feed the *event* pipeline). A thin `AuditSink` is threaded like
the existing `event_sink_`. This keeps reads audited (they flow through the read
RPCs) without touching hot inner loops.

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
  - `permission`, `privilege`, `admin` → **fail-closed** by default: if the entry
    can't be durably spooled, the *operation* is rejected. These are rare and
    security-critical.
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
  **category** (access · mutate · permission · privilege · admin), **action**,
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

1. **Schema + writer** — provision `audit_log` (+ global) in tenant setup;
   durable async writer with WAL; `AuditSink` wired into the service layer.
2. **Privilege + permission** — audit RoleManager ops and fold `acl_audit` into
   `audit_log` (keep `acl_audit` as a compatibility view during transition).
   Fail-closed policy for these categories.
3. **Mutations** — audit all write/delete/restore/metadata ops with `detail`.
4. **Access logging** — audit reads/list/stat/version-reads incl. denials, with
   the `AUDIT_ACCESS_MODE` throughput controls.
5. **Tamper-evidence** — hash chain + `VerifyAuditChain` + append-only DB grants.
6. **Query/export + retention** — the RPCs, REST/MCP exposure, partitioning,
   archival, per-tenant retention.
7. **Frontend Audit & Events console** (§10) — the tenant-admin SPA view over the
   query/export/verify APIs plus the admin-scoped live events feed.

Forward-only: no backfill (there's no prior read history to reconstruct).

---

## 12. Open decisions (need a call before coding)

- **Access-log volume:** full-fidelity reads vs sampled vs aggregate `count` as
  the *default* (`AUDIT_ACCESS_MODE`). Compliance usually wants full; cost may
  push to sample-with-force-full-on-sensitive-subtrees.
- **Fail-closed scope:** confirm `permission`/`privilege`/`admin` should block the
  op on audit-write failure (recommended) vs degrade to WAL-only.
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
