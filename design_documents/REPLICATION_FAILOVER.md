# file_engine_core — Postgres read-only failover

Status: **Implemented on `feature/replica-failover`** — config activation, read-only-mode
lifecycle, and read/write connection routing are all in place.

Part of the workspace-wide replica-failover feature (see the matching branches in
`convert_search_ai`, `mcp`, `http_bridge`, `webdav_bridge`). The core owns the
canonical Postgres database.

## Topology

```
        reads + writes                         streaming replication
data ────────────────▶ MASTER (cloud) ──────────────────────────▶ REPLICA (on-prem, localhost)
                        Postgres primary                            read-only hot standby
```

Master (cloud) is the primary for reads + writes. An on-prem standby is kept current
by Postgres streaming replication. When the master is unreachable the server enters
**disconnected read-only mode**: writes are rejected; reads should serve from the
local standby; normal operation resumes when the master returns.

Unlike the LDAP services (lazy circuit-breaker), the core already uses a **background
connection monitor thread** (`Database::start_connection_monitoring`), so this work
completes that existing design rather than introducing a breaker.

## What already existed

- `ConnectionPoolManager` global `server_in_readonly_mode_` flag (+ get/setter).
- `grpc_service` rejects every mutating RPC while read-only ("Server is in read-only
  mode due to database disconnection").
- `Database` secondary members: `secondary_conn_info_`, `using_secondary_`,
  `primary_available_`, the monitor thread, `configure_secondary_connection()`.
- `server.cpp` wires the secondary + starts monitoring when one is configured.
- `rest_server` health exposes `readonly_mode`.

## What this branch adds

1. **Config activation (was dormant).** `config_loader` now parses the replica from
   env into `secondary_db_*`, so the feature can actually be enabled:

   | Env var | Default | Meaning |
   |---------|---------|---------|
   | `FILEENGINE_PG_REPLICA_HOST` | _(unset)_ | Replica host. **Setting it enables failover.** |
   | `FILEENGINE_PG_REPLICA_ENABLED` | `false` | When true and no host given, host defaults to `localhost`. |
   | `FILEENGINE_PG_REPLICA_PORT/DATABASE/USER/PASSWORD` | = primary | Replica params (a standby normally shares them). |

2. **Read-only-mode lifecycle (completes the TODO).** The monitor now, on primary
   loss, sets `primary_available_=false`, `using_secondary_=true`, and
   `ConnectionPoolManager::set_server_in_readonly_mode(true)`; while down it re-probes
   the primary and, on success, clears all three and resumes. Previously it only
   handled the recovery half, so read-only mode was never actually entered.

   Net effect today: with a replica configured, a primary outage cleanly rejects
   writes (clear error) and the health endpoint reports `readonly_mode`, and the
   server auto-recovers when the primary returns.

## Read/write connection routing

The database previously issued every query via `connection_pool_->acquire()` (primary
pool only) at 54 call sites. Reads and writes are now **cleanly separated by connection**:

- `connection_router.h` — `enum class DbOp { Read, Write }` and a pure
  `select_pool(op, primary, secondary, using_secondary)`:
  - **Write** → always the primary (never the read-only replica).
  - **Read** → the replica only while failed over *and* a replica exists, else the primary.
- `Database` owns a dedicated **secondary `ConnectionPool`** (built in
  `configure_secondary_connection`) and a single `acquire(DbOp)` helper that applies
  `select_pool`.
- All 54 sites were reclassified: **24 writes → `acquire(DbOp::Write)`**,
  **28 reads → `acquire(DbOp::Read)`**. The two primary health probes
  (`is_connected`, `check_connection`) intentionally stay on the primary pool so the
  monitor can detect the master's state.

So during an outage: writes are rejected at the gRPC layer (and never reach the
replica); reads transparently serve from the local standby; recovery restores both to
the primary.

## Testing

Both halves of the failover logic are extracted as **pure functions** so they are
exhaustively unit-testable without a database or a background thread — the database is
"mocked" by a reachability boolean / sentinel pool pointers. See
`tests/test_connection_router.cpp` (target `test_connection_router`):

- **Routing decision** (`select_pool`): writes always to the primary (incl. during
  failover and with no replica); reads to the primary in normal operation; reads to the
  replica only while failed over *and* a replica exists.
- **Failover state machine** (`next_failover_state`, driven by the connection monitor):
  healthy+reachable → unchanged; healthy+unreachable → read-only (using the replica if
  present, else read-only with no replica); degraded+still-unreachable → stays degraded
  (no flapping); degraded+reachable → full recovery. Plus the end-to-end
  healthy → down → down → recovered sequence checked together with `select_pool`.

The monitor thread is a thin driver around `next_failover_state`, and `Database::acquire`
a thin driver around `select_pool`, so the unit tests cover the decision logic directly.
Method-level wiring (each of the 54 sites passing the right `DbOp`) and real libpq
behavior against an actual standby are covered by the core integration suite via the
full build.
