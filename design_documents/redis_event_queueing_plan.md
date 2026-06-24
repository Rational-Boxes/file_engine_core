# Optional Redis-based event queueing — implementation plan

Status: **Design / plan** (branch `feature/redis-event-queue`). Implements the
**publisher** side of the file-activity event contract; the consumer side and the
full envelope/semantics are specified in `convert_search_ai/design_documents/EVENT_CONTRACT.md`
(see especially §6 "Publisher requirements"). This document is the core-side
implementation plan only.

## 1. Goal & scope

Add **optional, opt-in** emission of generic file-activity events from
FileEngine core so that any number of independent downstream services
(`convert_search_ai` first; search, AI extraction, cache invalidation, audit
later) can react to filesystem changes. Redis (Streams) is the first transport.

Hard requirements:

- **Optional at build *and* runtime.** No Redis dependency unless explicitly
  enabled; **disabled by default** so existing deployments are byte-for-byte
  unaffected until they opt in.
- **Fail-open.** Event publication must never fail, block, slow, or roll back a
  user's filesystem operation. A dead broker degrades to dropped-events + a
  metric, never an error to the caller.
- **Broker-agnostic** at the call site: `FileSystem` emits to an `IEventSink`
  abstraction; Redis is one implementation.
- **Generic & reusable:** core publishes once; consumers subscribe independently.

Out of scope: the consumers themselves; metadata/ACL-change events (a possible
later addition — see §12).

## 2. Configuration

**Decision (resolved):** the connection uses the `FILEENGINE_REDIS_*` convention
(matching every other key). The original `REDDIS_*` keys are still honored as a
**legacy alias** (read first, so `FILEENGINE_REDIS_*` overrides) so an existing
`.env` keeps working. `*_HOST` accepts `host` or `host:port`; an explicit
`*_PORT` always wins.

`Config` fields (in `config_loader.h` `struct Config`), applied across all four
precedence layers in `config_loader.cpp` (system file → `.env` → `--config`
file → process env):

| Config field | Env var (canonical) | Default | Purpose |
|--------------|---------------------|---------|---------|
| `events_enabled` | `FILEENGINE_EVENTS_ENABLED` | `false` | Master runtime switch |
| `events_redis_host` | `FILEENGINE_REDIS_HOST` (alias `REDDIS_HOST`) | `localhost` | Broker host (`host[:port]`) |
| `events_redis_port` | `FILEENGINE_REDIS_PORT` (alias `REDDIS_PORT`) | `6379` | Used if host has no port |
| `events_redis_password` | `FILEENGINE_REDIS_PASSWORD` (alias `REDDIS_PASSWORD`) | _(empty)_ | AUTH; never logged |
| `events_redis_db` | `FILEENGINE_REDIS_DB` (alias `REDDIS_DB`) | `0` | Redis logical DB |
| `events_stream` | `FILEENGINE_EVENTS_STREAM` | `fileengine:events` | **Single** stream key (tenant is an event field) |
| `events_stream_maxlen` | `FILEENGINE_EVENTS_STREAM_MAXLEN` | `100000` | `XADD MAXLEN ~` cap (durability, not archival) |
| `events_outbox_capacity` | `FILEENGINE_EVENTS_OUTBOX_CAPACITY` | `10000` | Bounded in-memory outbox; overflow = drop-oldest+metric |

When `events_enabled=false` (default) the factory returns no sink (`nullptr`) and
nothing connects to Redis. (TLS via `hiredis_ssl` is a follow-up; not yet wired.)

## 3. Components (new)

All under `core/include/fileengine/` + `core/src/`:

1. **`FileEvent`** (`event.h`) — POD describing one event, matching the contract
   envelope: `event_id` (UUID via existing `Utils`), `type`, `tenant`,
   `file_uid`, `parent_uid`, `name`, `path` (best-effort), `is_folder`,
   `is_rendition`, `version`, `size`, `mime`, `actor`, `ts` (RFC3339),
   `schema=1`. Plus `to_json()` (reuse the project's JSON approach used by the
   REST monitoring listener / metrics).

2. **`IEventSink`** (`event_sink.h`) — `virtual void publish(const FileEvent&) noexcept = 0;`
   plus `start()`/`stop()`. `noexcept` enforces fail-open at the type level.

3. **`NullEventSink`** — no-op; the default and the compiled-out fallback.

4. **`EventOutbox`** — bounded queue + single worker thread, **mirroring the
   existing async object-store backup worker** in `filesystem.h`/`.cpp`
   (`backup_queue_`, `queue_mutex_`, `queue_cv_`, `backup_worker_thread_`,
   `backup_worker_running_`, `start/stop_async_backup_worker`,
   `backup_worker_loop`). `publish()` enqueues under the mutex (drop + increment
   `events_dropped_total` if at capacity) and notifies; the worker drains and
   hands batches to the transport. Drains on `stop()` with a bounded timeout.

5. **`RedisEventSink`** — `EventOutbox` + a hiredis connection. Worker does
   `XADD <prefix>:<tenant> MAXLEN ~ <maxlen> * field value …` (or a single JSON
   `payload` field). Lazy connect with capped exponential backoff; while
   disconnected the outbox keeps absorbing up to capacity then drops. SSL via
   `hiredis_ssl` when `events_tls`.

Factory: `make_event_sink(const Config&)` returns `RedisEventSink` when
`events_enabled && FILEENGINE_HAS_EVENTS`, else `NullEventSink`.

## 4. Emit points

Inject the sink into `FileSystem` (ctor param, default `NullEventSink`). Emit
**after** the authoritative DB commit succeeds, from these methods — each already
receives `user` (→ `actor`) and `tenant`:

| `FileSystem` method | Event `type` | Notes |
|---------------------|--------------|-------|
| `mkdir` | `dir.created` | `is_folder=true` |
| `rmdir` | `dir.deleted` | |
| `touch` | `file.created` | new empty file |
| `put` | `file.updated` | new content version; carries `version`, `size` |
| `remove` | `file.deleted` | soft/hard per existing semantics |
| `undelete` | `file.restored` | |
| `move` | `file.moved` | new `parent_uid`; no content change |
| `rename` | `file.renamed` | new `name` |
| `copy` | `dir.created` / `file.created` | one per new uid (recursive deep-copy) |
| `restore_to_version` | `file.updated` | only when the restore actually succeeds |
| `grant_permission` | `acl.changed` | **permission change** — carries `principal` + `permissions` bits |
| `revoke_permission` | `acl.changed` | as above; emitted on successful revoke |
| `AssignUserToRole` (gRPC) | `role.assigned` | carries `role` + `member` |
| `RemoveUserFromRole` (gRPC) | `role.member_removed` | carries `role` + `member` |
| `DeleteRole` (gRPC) | `role.deleted` | carries `role` (member empty — affects all members) |

ACL and role events are **first-class** for data governance (resolved decision):
consumers rely on them to invalidate cached permission decisions. `acl.changed`
targets a resource (`file_uid` + `principal` + `permissions`); the `role.*` events
have no resource — a role change alters *effective* access, so consumers
invalidate for the `member` (or all members of the `role` on `role.deleted`).
The gRPC ACL and role RPCs call `AclManager`/`RoleManager` directly, so they emit
via `FileSystem::publish_acl_change` / `publish_role_change`.

**Rendition feedback-loop handling (critical):** `convert_search_ai` writes
renditions as hidden children (`parent_uid` = a *file* uid). The emit helper
detects this — a best-effort lookup sets `is_rendition=true` when the parent
entity is a file (not a folder) — and **emits the event flagged** so consumers
ignore the conversion service's own output (`EVENT_CONTRACT.md §3`).

Two private `noexcept` helpers centralize envelope construction so the per-method
changes are one line each: `emit_fs_event(tenant, type, uid, user)` for file/dir
events and `emit_acl_event(tenant, resource_uid, principal, permissions, user)`
for ACL events. Both early-return when no sink is set (zero cost when disabled)
and best-effort enrich the envelope (name/parent/size/version/is_folder/
is_rendition) via a DB read. The public `publish_acl_change(...)` wrapper lets the
gRPC ACL RPCs emit (they bypass `FileSystem::grant_permission`).

## 5. Build system (optional dependency)

Mirror the existing optional-feature idiom in `core/CMakeLists.txt`
(`option(FILEENGINE_ENABLE_REST …)`, `find_package(AWSSDK QUIET)` +
`if(AWSSDK_FOUND)` + `target_compile_definitions`):

```cmake
option(FILEENGINE_ENABLE_EVENTS "Build with Redis event emission" OFF)
if(FILEENGINE_ENABLE_EVENTS)
    find_library(HIREDIS_LIB hiredis)            # libhiredis.so.1 present on dev host
    find_path(HIREDIS_INC hiredis/hiredis.h)     # needs hiredis-devel headers
    if(HIREDIS_LIB AND HIREDIS_INC)
        target_compile_definitions(fileengine_core PRIVATE FILEENGINE_HAS_EVENTS=1)
        target_link_libraries(fileengine_core ${HIREDIS_LIB})   # + hiredis_ssl when TLS
        target_include_directories(fileengine_core PRIVATE ${HIREDIS_INC})
    else()
        message(WARNING "FILEENGINE_ENABLE_EVENTS set but hiredis not found; events disabled")
    endif()
endif()
```

Source compiles under `#ifdef FILEENGINE_HAS_EVENTS` for the Redis code; the
`IEventSink`/`NullEventSink`/`FileEvent`/`EventOutbox` types always compile so
call sites need no `#ifdef`. **Build prereq:** `hiredis-devel` (only the `.so.1`
runtime is currently installed). Use C hiredis directly — no new C++ wrapper
dependency (`redis-plus-plus` not installed).

## 6. Lifecycle & threading

- Sink built in `server.cpp` from `Config`, passed into `FileSystem`; `start()`
  spins the outbox worker, `stop()` drains+joins on shutdown (hook into the
  existing graceful-shutdown / `sd_notify("STOPPING=1")` path).
- One worker thread, same shape as `backup_worker_loop`. No locks held during
  network I/O. `publish()` is wait-free beyond a short enqueue critical section.

## 7. Failure semantics (fail-open, restated)

- Emit strictly **after** commit; a publish failure cannot affect the committed op.
- Broker down / slow → events accumulate in the bounded outbox, then
  **drop-oldest** (resolved decision; freshest activity wins) with a dropped
  counter; consumers recover via their reconcile sweep (`EVENT_CONTRACT.md §7`).
- `IEventSink::publish` is `noexcept`; any internal error is swallowed + logged at
  WARN (rate-limited), never propagated.

## 8. Observability

Extend the existing Prometheus REST listener (`:8081`, `http_metrics_*`):

- `fileengine_events_emitted_total{tenant,type}`
- `fileengine_events_dropped_total{tenant,reason}`
- `fileengine_events_outbox_depth` (gauge)
- `fileengine_events_redis_up` (gauge 0/1)

Aligns with `design_documents/monitoring_and_telemetry.md`, which already
anticipated pushing "file events" to a stream as a future phase.

## 9. Tenancy, security, transport

- **Single stream** `events_stream` (default `fileengine:events`) shared by all
  tenants; the tenant is an event field and consumers are multi-tenant aware
  (resolved decision; `EVENT_CONTRACT.md §5`). `XADD … MAXLEN ~ <maxlen>`.
- **Auth** via `FILEENGINE_REDIS_PASSWORD` (`AUTH`); TLS via `hiredis_ssl`
  (follow-up). Password and payloads never logged; events carry uids/paths/actor,
  **not file content**.

## 10. Testing

Add to `tests/` + `tests/CMakeLists.txt` (the suite is `add_executable` per file):

- **Unit (no broker):** `FileEvent::to_json` shape vs contract; `EventOutbox`
  overflow→drop + metric; rendition suppression; `NullEventSink` path; factory
  selection by config. Build with a fake sink capturing events.
- **Integration (`@live`, dev Redis :6379, only when `FILEENGINE_ENABLE_EVENTS`):**
  drive `FileSystem` ops, `XREAD` the stream, assert one correctly-shaped event
  per op, correct `tenant`/`actor`/`type`/`version`, and **no** event for
  rendition-child writes. Fail-open test: stop Redis mid-run, assert fs ops still
  succeed and `events_dropped_total` rises.
- A consumer smoke test (`redis-cli XADD`-free) proving round-trip against the
  envelope `convert_search_ai` expects.

## 11. Phased delivery

1. **P1 — Skeleton, no Redis dep.** `FileEvent`+JSON, `IEventSink`,
   `NullEventSink`, `EventOutbox`, config fields + env mapping, factory. Default
   off; fully unit-tested. No build/runtime change to existing deployments.
2. **P2 — Redis transport.** `RedisEventSink` (hiredis), CMake option + guard,
   lifecycle wiring in `server.cpp`.
3. **P3 — Emit points + suppression.** Wire the 10 `FileSystem` methods via
   `emit_()`; rendition flagging/suppression.
4. **P4 — Metrics, integration tests, docs.** Metrics endpoints; `@live` tests;
   update `CONFIGURATION.md`, `core.conf`, `.env` example, and
   `EVENT_CONTRACT.md` cross-link.

Each phase is independently mergeable; after P1–P2 the feature exists but emits
nothing until P3; after P4 it's complete and documented. All gated behind
`FILEENGINE_ENABLE_EVENTS` (build) + `FILEENGINE_EVENTS_ENABLED` (runtime).

**Status:** P1–P3 implemented (all 13 emit points: mkdir/rmdir/touch/put/remove/
undelete/move/copy/rename/restore + grant/revoke). Verified end-to-end against
the dev Redis: the JS gRPC CRUD suite (73/73, unaffected) produced 49 events on a
single stream — every file/dir type plus 14 `acl.changed` carrying actor/
principal/permissions; fail-open confirmed (no-auth → all dropped, ops still
succeed). P4 remaining: Prometheus metrics (§8), `@live` integration tests in
`tests/CMakeLists.txt` (a manual `tests/event_sink_smoke.cpp` exists), TLS, and
`CONFIGURATION.md`/`core.conf` examples.

## 12. Decisions (resolved) & remaining open items

Resolved:
- **Env naming:** `FILEENGINE_REDIS_*` canonical; `REDDIS_*` legacy alias. (§2)
- **Stream topology:** single shared stream, tenant carried as a field. (§9)
- **Overflow policy:** drop-oldest. (§7)
- **`acl.changed`:** first-class and critical for data-governance / cache
  invalidation — emitted on grant + revoke (§4). The gRPC ACL RPCs call
  `AclManager` directly, so they emit via `FileSystem::publish_acl_change`.
- **Payload encoding:** single JSON `payload` field.

- **Role-membership events (resolved — implemented).** `role.assigned`,
  `role.member_removed`, `role.deleted` emitted from the gRPC role RPCs, carrying
  `role` + `member`. Granting a permission *to a role* already surfaces as
  `acl.changed` with the role as `principal`. Consumers invalidate cached
  decisions for the `member` (or all members on `role.deleted`).

Still open:
- **`metadata.changed`** — emit on set/delete metadata? Deferred unless needed.
- **TLS** (`hiredis_ssl`) and the **Prometheus metrics** in §8 are not yet wired.
