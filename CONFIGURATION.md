# FileEngine Core — Configuration Guide

This is a stand-alone guide for configuring a FileEngine Core server
(`fileengine_server`). It explains where configuration comes from, how the
sources are layered, and documents every setting the server reads at startup.

It assumes the binaries are already built/installed (see `CLAUDE.md` /
`DOCUMENTATION.md` for build instructions). You do **not** need to read the
source to follow this guide.

---

## 1. How configuration is loaded

The server reads its settings from up to five sources. They are applied in the
order below, and **each later source overrides the earlier ones** for any key
it defines:

| # | Source | Typical use |
|---|--------|-------------|
| 1 | `/etc/fileengine/core.conf` | System-wide defaults (installed by the package) |
| 2 | `.env` in the **current working directory** | Per-deployment / developer overrides |
| 3 | File named with `--config <file>` (or `-c <file>`) | Alternate named config |
| 4 | Process environment variables | Container / orchestration overrides |
| 5 | Command-line arguments | One-off overrides (highest priority) |

So a value set on the command line always wins; a value set only in
`/etc/fileengine/core.conf` is the fallback.

> **Note:** Sources 1–3 are all parsed as `KEY=value` files using the same
> format. The environment-variable layer (4) and CLI layer (5) only override a
> setting when it differs from the built-in default, so leaving them unset is
> safe.

### File format

All config files use a simple `KEY=value` format:

```ini
# Lines beginning with # or ; are comments
FILEENGINE_PG_HOST=localhost
FILEENGINE_PG_PORT=5432
# Surrounding single or double quotes are stripped:
FILEENGINE_S3_SECRET_KEY="some secret"
```

- One setting per line.
- Whitespace around the key and value is trimmed.
- Boolean values are `true` / `false` (also accepts `1`, and `TRUE` for some
  keys — prefer `true`/`false`).

> **Important:** The `.env` file holds live database and S3 credentials. Never
> commit it and never edit it casually. When running from a build directory,
> the project convention is to **symlink** the deployment `.env` into the build
> directory so the binary finds it in its working directory.

---

## 2. Quick start

Minimum viable configuration for a single-node deployment:

1. **Provision PostgreSQL** (section 4) and note the host, port, database,
   user, and password.
2. **Provision an object store** (S3 or MinIO — section 5), or disable sync if
   you only need local storage.
3. Create `/etc/fileengine/core.conf` (the package installs a template) and set
   at least the database and storage values:

   ```ini
   FILEENGINE_PG_HOST=localhost
   FILEENGINE_PG_PORT=5432
   FILEENGINE_PG_DATABASE=fileengine
   FILEENGINE_PG_USER=fileengine_user
   FILEENGINE_PG_PASSWORD=change_me
   FILEENGINE_STORAGE_BASE=/var/lib/fileengine/storage
   FILEENGINE_GRPC_HOST=0.0.0.0
   FILEENGINE_GRPC_PORT=50051
   ```

4. Start the server: `fileengine_server` (or `systemctl start fileengine`).
   On first start it **auto-creates the database schema** — the PostgreSQL user
   must have permission to create schemas and tables.
5. Verify it is healthy: `curl http://localhost:8081/healthz` (section 7).

---

## 3. Configuration reference

All settings are listed below by category, with their config-file key, default,
and meaning.

### Database (PostgreSQL) — required

| Key | Default | Description |
|-----|---------|-------------|
| `FILEENGINE_PG_HOST` | `localhost` | PostgreSQL host |
| `FILEENGINE_PG_PORT` | `5432` | PostgreSQL port |
| `FILEENGINE_PG_DATABASE` | `fileengine` | Database name |
| `FILEENGINE_PG_USER` | `fileengine_user` | Database user |
| `FILEENGINE_PG_PASSWORD` | `fileengine_password` | Database password |

The server connects on startup, ensures the schema exists, and runs a
background connection monitor that reconnects automatically if the database
becomes unavailable.

### Storage (local filesystem) — required

| Key | Default | Description |
|-----|---------|-------------|
| `FILEENGINE_STORAGE_BASE` | `/tmp/fileengine_storage` | Root directory for local file storage. Set this to a durable path (e.g. `/var/lib/fileengine/storage`). |

The directory must exist and be writable by the service user. PUT operations
write here first and return immediately; the object store is updated
asynchronously.

### Encryption & compression at rest

| Key | Default | Description |
|-----|---------|-------------|
| `FILEENGINE_ENCRYPT_DATA` | `false` | Encrypt stored file content with AES-256-GCM |
| `FILEENGINE_COMPRESS_DATA` | `false` | zlib-compress stored file content |
| `AT_REST_KEY` | *(empty)* | Encryption key — **required when `FILEENGINE_ENCRYPT_DATA=true`** |

The encryption key must be a **32-byte AES-256 key**, supplied as **64 hex
characters** (recommended) or a 32-byte raw string. Generate one with:

```bash
openssl rand -hex 32
```

> Keep the key safe and stable — data encrypted with one key cannot be read
> after the key changes.

### Object store (S3 / MinIO)

| Key | Default | Description |
|-----|---------|-------------|
| `FILEENGINE_S3_ENDPOINT` | `http://localhost:9000` | S3/MinIO endpoint URL |
| `FILEENGINE_S3_REGION` | `us-east-1` | Region |
| `FILEENGINE_S3_BUCKET` | `fileengine` | Base bucket name (per-tenant buckets derive from this) |
| `FILEENGINE_S3_ACCESS_KEY` | `minioadmin` | Access key |
| `FILEENGINE_S3_SECRET_KEY` | `minioadmin` | Secret key |
| `FILEENGINE_S3_PATH_STYLE` | `true` | Use path-style addressing (`true` for MinIO; `false` for AWS S3 virtual-hosted style) |

If the object store is unreachable at startup the server logs a warning and
**continues** — local storage works without it. S3 objects are immutable by
design; deletes are not propagated to the object store.

### Object store synchronization

| Key | Default | Description |
|-----|---------|-------------|
| `FILEENGINE_S3_SYNC_SUPPORT` | `true` | Enable background sync to the object store. Accepts `true`, `minio`, or `s3`. |
| `FILEENGINE_S3_RETRY_SECONDS` | `60` | Retry interval (seconds) after a sync failure |
| `FILEENGINE_S3_SYNC_ON_STARTUP` | `true` | Run a sync pass when the server starts |
| `FILEENGINE_S3_SYNC_ON_DEMAND` | `true` | Allow operator-triggered sync via the admin API |
| `FILEENGINE_S3_SYNC_PATTERN` | `all` | Which objects to sync |
| `FILEENGINE_S3_SYNC_BIDIRECTIONAL` | `true` | Sync in both directions (object store ↔ local) |

### Cache

| Key | Default | Description |
|-----|---------|-------------|
| `FILEENGINE_CACHE_THRESHOLD` | `0.8` | LRU eviction threshold as a fraction of max cache size (0.0–1.0) |
| `FILEENGINE_MAX_CACHE_SIZE_MB` | `1024` | Maximum in-memory file cache size in MB |

### gRPC server

| Key | Default | Description |
|-----|---------|-------------|
| `FILEENGINE_GRPC_HOST` | `0.0.0.0` | Address the gRPC server binds to |
| `FILEENGINE_GRPC_PORT` | `50051` | gRPC port |
| `FILEENGINE_HTTP_THREAD_POOL` | `10` | Server worker thread-pool size (also the DB connection-pool size) |

### Monitoring REST listener

A lightweight HTTP listener for health checks and status. The trust boundary is
the network perimeter — there is **no in-process auth or TLS on this port**, so
restrict access at the firewall/network layer.

| Key | Default | Description |
|-----|---------|-------------|
| `FILEENGINE_HTTP_METRICS_ENABLED` | `true` | Enable the monitoring REST listener |
| `FILEENGINE_HTTP_METRICS_ADDR` | `0.0.0.0` | Address the listener binds to |
| `FILEENGINE_HTTP_METRICS_PORT` | `8081` | Listener port |
| `FILEENGINE_METRICS_TENANT_LABEL` | `true` | Emit a tenant label on metrics |

Endpoints (see section 7): `/healthz`, `/readyz`, `/v1/version`, `/v1/status`.

### Security

| Key | Default | Description |
|-----|---------|-------------|
| `FILEENGINE_ROOT_USER` | `false` | Enable a privileged root user that bypasses ACL checks |
| `FILEENGINE_DEFAULT_WORLD_READABLE` | `false` | When `true`, new files/directories get `OTHER→READ` (world-readable) by default. When `false`, new resources are private to the creator. |

> The default config template ships `FILEENGINE_ROOT_USER=true`. For
> production, review whether you want the root bypass enabled.

### Multi-tenancy

| Key | Default | Description |
|-----|---------|-------------|
| `FILEENGINE_MULTI_TENANT_ENABLED` | `true` | Enable multi-tenant isolation (per-tenant DB schema, storage dir, and bucket) |

### Logging

| Key | Default | Description |
|-----|---------|-------------|
| `FILEENGINE_LOG_LEVEL` | `INFO` | One of `DEBUG`, `INFO`, `WARN`, `ERROR`, `FATAL` |
| `FILEENGINE_LOG_FILE_PATH` | `/tmp/fileengine.log` | Log file path (used when file logging is on). For production set e.g. `/var/log/fileengine/fileengine.log`. |
| `FILEENGINE_LOG_TO_CONSOLE` | `true` | Write logs to stdout/console |
| `FILEENGINE_LOG_TO_FILE` | `false` | Write logs to the file path above |
| `FILEENGINE_LOG_ROTATION_SIZE_MB` | `10` | Rotate the log file at this size (MB) |
| `FILEENGINE_LOG_RETENTION_DAYS` | `7` | Days of rotated logs to keep |

Under systemd, console output is captured by the journal
(`journalctl -u fileengine`).

---

## 4. PostgreSQL setup

1. Create a database and a dedicated user:

   ```sql
   CREATE USER fileengine_user WITH PASSWORD 'change_me';
   CREATE DATABASE fileengine OWNER fileengine_user;
   ```

2. Ensure the user can create schemas and tables in that database — the server
   creates per-tenant schemas (`tenant_default`, `tenant_<name>`, …) and the
   global `tenants` registry on first start. Granting ownership of the database
   (as above) is the simplest way.

3. Set the matching `FILEENGINE_PG_*` values in your config.

No manual schema/migration step is required — the server runs
`create_schema()` automatically at startup.

---

## 5. S3 / MinIO setup

### MinIO (default template values)

```ini
FILEENGINE_S3_ENDPOINT=http://localhost:9000
FILEENGINE_S3_REGION=us-east-1
FILEENGINE_S3_BUCKET=fileengine
FILEENGINE_S3_ACCESS_KEY=minioadmin
FILEENGINE_S3_SECRET_KEY=minioadmin
FILEENGINE_S3_PATH_STYLE=true
```

### AWS S3

```ini
FILEENGINE_S3_ENDPOINT=https://s3.us-east-1.amazonaws.com
FILEENGINE_S3_REGION=us-east-1
FILEENGINE_S3_BUCKET=my-fileengine-bucket
FILEENGINE_S3_ACCESS_KEY=<aws-access-key-id>
FILEENGINE_S3_SECRET_KEY=<aws-secret-access-key>
FILEENGINE_S3_PATH_STYLE=false
```

### Local-only (no object store)

To run without an object store, disable sync:

```ini
FILEENGINE_S3_SYNC_SUPPORT=false
FILEENGINE_S3_SYNC_ON_STARTUP=false
```

Files are then served entirely from `FILEENGINE_STORAGE_BASE`.

---

## 6. Command-line argument overrides

A subset of settings can be overridden on the command line (highest priority).
Useful for testing without editing config files:

| Argument | Overrides |
|----------|-----------|
| `--config <file>` / `-c <file>` | Load this file as source #3 |
| `--db-host <host>` | `FILEENGINE_PG_HOST` |
| `--db-port <port>` | `FILEENGINE_PG_PORT` |
| `--db-name <name>` | `FILEENGINE_PG_DATABASE` |
| `--db-user <user>` | `FILEENGINE_PG_USER` |
| `--db-password <pw>` | `FILEENGINE_PG_PASSWORD` |
| `--storage-path <dir>` | `FILEENGINE_STORAGE_BASE` |
| `--s3-endpoint <url>` | `FILEENGINE_S3_ENDPOINT` |
| `--s3-region <region>` | `FILEENGINE_S3_REGION` |
| `--s3-bucket <bucket>` | `FILEENGINE_S3_BUCKET` |
| `--s3-access-key <key>` | `FILEENGINE_S3_ACCESS_KEY` |
| `--s3-secret-key <key>` | `FILEENGINE_S3_SECRET_KEY` |
| `--listen-addr <addr>` | `FILEENGINE_GRPC_HOST` |
| `--listen-port <port>` | `FILEENGINE_GRPC_PORT` |
| `--thread-pool-size <n>` | `FILEENGINE_HTTP_THREAD_POOL` |

Example:

```bash
fileengine_server --config /etc/fileengine/core.conf \
  --db-host db.internal --listen-port 50052
```

---

## 7. Verifying the configuration

After starting the server, confirm it loaded the expected values and is
healthy:

- **Startup output** — the server prints the resolved DB host/port/name,
  storage path, S3 endpoint, and log settings on stdout (or in the journal).
- **Liveness:** `curl http://<host>:8081/healthz` → `200` when the process is up.
- **Readiness:** `curl http://<host>:8081/readyz` → `200` when dependencies
  (DB, listeners) are ready.
- **Version:** `curl http://<host>:8081/v1/version`
- **Status:** `curl http://<host>:8081/v1/status`

(Use the port set in `FILEENGINE_HTTP_METRICS_PORT`.)

---

## 8. Running under systemd

The package installs `fileengine.service` (runs as the `fileengine` user with
`Type=notify`). It reads `/etc/fileengine/core.conf` as the base configuration.

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now fileengine
systemctl status fileengine
journalctl -u fileengine -f
```

The unit declares `ReadWritePaths=/var/lib/fileengine /var/log/fileengine
/tmp/fileengine` and binds with `CAP_NET_BIND_SERVICE`. If you change
`FILEENGINE_STORAGE_BASE` or `FILEENGINE_LOG_FILE_PATH` to a path outside those
directories, add the new path to `ReadWritePaths` (via a drop-in override) or
the service will be denied write access. The service waits for PostgreSQL
(`After=postgresql.service`).

Reload after editing config:

```bash
sudo systemctl restart fileengine
```

---

## 9. Notes & gotchas

- **Boolean parsing:** use `true`/`false`. Several keys also accept `1`; some
  accept `TRUE`. Anything else is treated as `false`.
- **`FILEENGINE_S3_SYNC_SUPPORT`** is the on/off switch for sync and also
  accepts `minio` or `s3` as truthy aliases.
- **Encryption requires a valid 32-byte key.** Enabling
  `FILEENGINE_ENCRYPT_DATA` without a valid `AT_REST_KEY` (64 hex chars or 32
  raw bytes) will fail at encrypt/decrypt time.
- **Path-style addressing:** keep `FILEENGINE_S3_PATH_STYLE=true` for MinIO and
  most self-hosted gateways; set `false` for AWS S3.
- **Never store the only copy of `AT_REST_KEY` or DB/S3 credentials in a file
  under version control.** Keep `.env` out of git.

### Reserved / unused settings

The following keys may appear in the shipped `.env` or older config samples but
are **not read by the current `fileengine_server`** — setting them has no
effect. They are reserved for planned features. Do not rely on them:

| Key | Status |
|-----|--------|
| `FILEENGINE_HTTP_LISTEN_ADDR` | Not parsed. The monitoring listener uses `FILEENGINE_HTTP_METRICS_ADDR`. |
| `FILEENGINE_HTTP_LISTEN_PORT` | Not parsed. The monitoring listener uses `FILEENGINE_HTTP_METRICS_PORT`. |
| `FILEENGINE_S3_SUPPORT` | Not parsed. Use `FILEENGINE_S3_SYNC_SUPPORT` to toggle sync. |
| `FILEENGINE_SWAGGER_ENABLED` | Not parsed. No Swagger/OpenAPI UI is served in this version. |
</content>
</invoke>
