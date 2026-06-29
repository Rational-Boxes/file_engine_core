# file_engine_core — Postgres read-only failover

Status: **Partial on `feature/replica-failover`** — activation + read-only-mode
lifecycle complete; read routing to the secondary is the remaining step (below).

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

## Remaining step — read routing to the secondary

The database issues queries via `connection_pool_->acquire()` at ~54 call sites
(primary pool only); there is no central read/write helper. To serve reads from the
standby while degraded, the planned change is:

- give `Database` a **secondary `ConnectionPool`** built from `secondary_conn_info_`
  (created in `configure_secondary_connection`);
- add `ConnectionPool& active_read_pool()` returning the secondary when
  `using_secondary_` is set, else the primary;
- route the read call sites through it (writes are already gated off at the gRPC layer
  while read-only, so only reads reach the DB during an outage).

This touches the hot query path and must be implemented and validated with the core's
C++ build + test suite (`tests/`), which is why it is staged separately from the
low-risk activation/lifecycle changes above.

## Testing

- Activation + lifecycle changes are config/flag/atomic level (no hot-path edits).
- Read-routing should land with unit/integration coverage under `tests/` (primary-up
  reads from primary; primary-down reads from the secondary; writes rejected while
  degraded; recovery), run via the core build.
