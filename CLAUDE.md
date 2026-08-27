# FileEngine Core - Project Context

> **This file leads with a workspace-wide map** (the section below) so that a
> cold start — no prior context — has the whole ecosystem in view. Everything
> after "## Overview" is specific to the `file_engine_core` service itself.

## Workspace Quick-Start (multi-project map)

The parent directory `/home/telendry/code/file_projects/` is a **meta-project**: a
platform of cooperating services around the canonical **FileEngine gRPC core**.
Sibling projects resolve each other by relative path (e.g. Python services import
`../python_interface`). There is no single git root — each project is versioned
independently.

### The one-paragraph architecture
A browser (**frontend**, Vue 3 SPA) talks to protocol **bridges** over
REST/WebDAV. The bridges authenticate the caller against **LDAP**, mint an
opaque **HS256 bearer token**, and forward the resolved identity to the
**core** as a trusted `AuthenticationContext`. The **core** (this project) is the
sole enforcer of ACLs, versioning, tenancy, and storage (PostgreSQL metadata +
local FS + S3/MinIO objects). Feature services (search/AI, discussion) sit
beside the bridges, re-checking every access *as the end-user* via the core's
`CheckPermission`. All services publish security events to a Redis stream that
the **audit_service** drains into a tamper-evident, hash-chained log.

### Service map (dev topology per `scripts/start_backend_services.sh`)

| Project | Lang | Role | Port(s) | Auth |
|---|---|---|---|---|
| **file_engine_core** | C++17 | Canonical gRPC filesystem + ACL/RBAC engine | gRPC **50051**, REST monitor **8081** | Trusts `AuthenticationContext` (upstream-authenticated) |
| **http_bridge** | C++17 | REST/JSON gateway → core; LDAP+JWT+OAuth | main **8090**, monitor **8091** (loopback) | Basic/Bearer/OAuth → LDAP → HS256 JWT |
| **webdav_bridge** | C++17 | WebDAV gateway → core; path→UUID resolver (Postgres) | main **8088**, monitor **8089** (loopback) | Basic/Digest → LDAP |
| **ldap_manager** | Python/FastAPI | Tenant user/role admin, invites, password reset | api **8093**, monitor **8094** (loopback) | Bearer (bridge JWT) |
| **convert_search_ai** (csai) | Python/FastAPI | Doc conversion, FTS/vector search, RAG chat | app **8092**, worker (portless) | Bearer/LDAP; per-user `CheckPermission` |
| **discussion_threaded_communication** | Python/FastAPI | Doc-anchored threads, reviews, live sync | api **8094**, mcp **8095**, consumer/digest (portless) | Bearer/LDAP; per-user `CheckPermission` |
| **mcp** | Python | MCP server exposing core to LLM agents (append-only, recoverable) | http **8096** | LDAP (stdio) / Basic+Bearer (http) |
| **audit_service** | Python | Drains Redis audit stream → hash-chained Postgres log; query API; rules engine | api **8097** (loopback), consumers (portless) | Bearer (JWT), AUDIT_READ scope |
| **frontend** | Vue 3 / TS | Web UI orchestrating the four services above | dev **3000** (Vite) | Opaque bearer token in localStorage |
| **python_interface** / **javascript_interface** | Python / TS | Client SDKs speaking gRPC to the core | — | Trusted upstream (identity passed verbatim) |
| **scripts** | Bash/Ansible | Dev launcher + centralized deploy (Ansible → Podman/Quadlet) | — | SSH / Ansible Vault |
| **docker_unified** | Compose | Single-host container stack (nginx TLS edge, per-tenant subdomains) | nginx **80/443** | — |

**Shared infra (dev):** PostgreSQL `:5434`, OpenLDAP `:1389`, MinIO `:9000`, Redis `:6379`.

### Security / trust model (read before touching auth)
- **Core trusts its input.** Every RPC carries `AuthenticationContext{user, roles, tenant, claims}`; the core does **not** authenticate — it authorizes. Whoever calls gRPC directly is fully trusted, so gRPC (`:50051`) must never be network-exposed. Authentication lives in the bridges/LDAP.
- **`system_admin` role bypasses all ACL checks** in the core. The SDKs pass roles verbatim, so an untrusted SDK caller can forge it — SDKs are safe only server-side.
- **Bridges are the security boundary:** LDAP bind → JWT (HS256, `FILEENGINE_JWT_SECRET` shared across services for local verification), tenant resolution, request-size caps, CORS scoping.
- **Feature services fail-closed:** permission cache (TTL ≤ 5 min, event-invalidated); core unreachable ⇒ deny.
- **Monitoring endpoints** (`/healthz` `/readyz` `/poolz` `/metrics` `/v1/status` `/v1/version`) are unauthenticated and **must bind loopback-only**. The core's REST monitor already defaults to `127.0.0.1:8081` — an earlier version of this note claimed `0.0.0.0`, which was stale and misleading in the dangerous direction, since it invited someone to widen a correct bind to match the documentation.
- **gRPC still defaults to `0.0.0.0:50051` on this branch**, in `config_loader.h` *and* in the shipped `core.conf` — so on the bare-metal/systemd paths the "never network-exposed" invariant is enforced by the host firewall, not by the application. Containers hold it by topology (the `core` service publishes no ports). The loopback default that inverts this failure mode lives on `security/grpc-loopback-default`; every authorization decision in the core, including the `ERASE` gate, assumes the invariant, so verify it per deployment until that lands.
- **Audit is tamper-evident** (SHA-256 hash chain) and, for auth events, **fail-closed** (login refused if the audit stream is unreachable).
- **Security-relevant core operations carry a *guaranteed* record**, separate from the audit stream. ACL grants/revokes, role changes, version culls and tenant lifecycle each write a hash-chained row to `accountability_record` **in the same transaction as the operation**: no configuration disables it, and if the record cannot be written the operation is refused. `audit_service` reads those forward by cursor over `ListAccountabilityRecords` (gated on the dedicated `accountability_reader` role) rather than off Redis, so no broker outage or lost node can lose one. See `design_documents/PROPOSAL_accountability_record.md`.
- **The audit log records identifiers and structure, never payload.** No filenames, no metadata values: emit the uid and let a viewer resolve the name at read time, so an erasure automatically stops the log disclosing it.

### Where to look first
- **Proto contract (source of truth):** `file_engine_core/proto/fileservice.proto` — copied into each bridge/SDK; keep in sync.
- **Run the whole stack (dev):** `scripts/start_backend_services.sh` (needs the 4 infra services up).
- **Run the whole stack (containers):** `docker_unified/` (`docker compose -f docker-compose.yml -f docker-compose.test.yml up -d`).
- **Deploy (prod):** `scripts/Ansible/` (roles per service; secrets in Ansible Vault).
- **End-user docs** live in **frontend**, not here (per project convention).

---

## Overview
FileEngine Core is a C++17 distributed virtual filesystem with horizontal scaling and hybrid cloud/on-premises deployment. It provides multi-tenant file management with POSIX ACLs, S3/MinIO object store integration, and a gRPC API. Version 2.1.0 (CMakeLists.txt).

## Project Structure
```
file_engine_core/
├── CMakeLists.txt                     # Root build config (CMake 3.15+)
├── Makefile                           # Package distribution (dist, arch/deb/rpm-package)
├── core/                              # Core C++ library
│   ├── include/fileengine/            # Public headers (~25 files)
│   ├── src/                           # Implementation (~23 files)
│   └── CMakeLists.txt                 # Core library + server build
├── cli/                               # Command-line client
│   ├── src/fileengine_cli.cpp         # CLI implementation
│   └── CMakeLists.txt
├── proto/
│   └── fileservice.proto              # gRPC service definition (40+ RPCs)
├── tests/                             # Test suite (~17 test files)
│   └── CMakeLists.txt
├── debian/                            # Debian packaging (control, rules, postinst, etc.)
├── PKGBUILD                           # Arch Linux packaging
├── fileengine-core.spec               # RPM packaging
├── fileengine.service                 # systemd unit file
├── fileengine.logrotate               # logrotate config
├── core.conf                          # Default server configuration
├── .env                               # Database and S3 connection settings
├── database_architecture.md           # Database schema documentation
├── DOCUMENTATION.md                   # Full project documentation
├── SPECIFICATIONS.md                  # System specifications
└── backup_working_implementation/     # Legacy reference implementation
```

## Architecture

### Interface-Based Design
The codebase uses abstract interfaces for dependency injection and testability:
- `IDatabase` → `Database` (PostgreSQL)
- `IStorage` → `Storage` (local filesystem)
- `IObjectStore` → `S3Storage` (S3/MinIO)

### Key Classes (core/include/fileengine/)

| Class | Header | Purpose |
|-------|--------|---------|
| `FileSystem` | filesystem.h | Main API: mkdir, rmdir, put, get, stat, move, copy, versions, ACLs. Manages async object store backup worker thread. |
| `Database` | database.h | PostgreSQL layer: schema management, file/version/ACL/role/metadata CRUD. Uses `ConnectionPool`. |
| `TenantManager` | tenant_manager.h | Multi-tenant context management. Each tenant gets isolated DB schema, storage dir, and S3 bucket. |
| `AclManager` | acl_manager.h | POSIX ACL permissions with role-based support. Permissions: READ, WRITE, DELETE, LIST_DELETED, UNDELETE, VIEW_VERSIONS, RETRIEVE_BACK_VERSION, RESTORE_TO_VERSION, EXECUTE, MANAGE_ACL, ACL_INHERIT, CULL_VERSIONS, ERASE. **The admin bypass is split:** `system_admin` passes every check; `tenant_admin` passes everything *except* the destroy-data bits (`CULL_VERSIONS`, `ERASE`), which need an explicit grant. |
| `accountability.h` | accountability.h | The guaranteed accountability record: the closed per-action `detail` schema, the canonical byte form and the chain hash. Mirrored byte-for-byte in `audit_service/src/audit_service/accountability.py` — keep the two in lockstep or every chain verification fails. |
| `RoleManager` | role_manager.h | RBAC: create/delete roles, assign/remove users, query role memberships. |
| `Storage` | storage.h | Local filesystem storage with SHA256-based directory desaturation, AES-256-GCM encryption, zlib compression. |
| `S3Storage` | s3_storage.h | AWS SDK-based S3/MinIO object store. Per-tenant buckets. |
| `CacheManager` | cache_manager.h | LRU in-memory file cache with configurable threshold. Fetches from object store if missing locally. |
| `StorageTracker` | storage_tracker.h | Per-host and per-tenant storage usage and file access pattern tracking. |
| `FileCuller` | file_culler.h | Automatic file cleanup using LRU or LFU strategy with configurable thresholds. |
| `ObjectStoreSync` | object_store_sync.h | Bidirectional S3/MinIO sync with health monitoring and automatic recovery. |
| `GRPCFileService` | grpc_service.h | gRPC server implementing all FileService RPCs including streaming upload/download. |
| `ConfigLoader` | config_loader.h | Loads config from environment variables, config files, and CLI arguments. |
| `ConnectionPool` | connection_pool.h | Reusable PostgreSQL connection pool. |
| `ConnectionPoolManager` | connection_pool_manager.h | **Holds the read-only failover flag.** Despite the name, it does NOT share a pool: `initialize_pool()` is never called anywhere, and every `Database` constructs its own `ConnectionPool`. Read pool telemetry via `Database::pool_stats()`. |
| `QueryBuilder` | query_builder.h | Fluent SQL query builder (SELECT, INSERT, UPDATE, DELETE with typed conditions). |
| `Logger` | logger.h | Singleton logger with rotation. Levels: DEBUG, INFO, WARN, ERROR, FATAL. |
| `ServerLogger` | server_logger.h | Dedicated gRPC server logger with thread ID tracking. |
| `CryptoUtils` | crypto_utils.h | AES-256-GCM encryption, zlib compression, hex/base64 encoding. |
| `Utils` | utils.h | UUID generation, timestamps, SHA256 hashing. |

### Core Data Types (types.h)
- `FileInfo` — UUID, name, parent_uid, size, owner, permissions, timestamps, folder flag, deleted flag
- `DirectoryEntry` — UUID, name, type, size, timestamps, version_count
- `Result<T>` — Success/error wrapper with message
- `FileType` enum — REGULAR_FILE, DIRECTORY, SYMLINK

### Database Schema
- Per-tenant schemas (e.g., `tenant_default`, `tenant_tenant_a`)
- Tables per schema: `files`, `versions`, `metadata`, `acls`, `roles`, `user_roles`,
  `acl_audit`, `audit_log`, plus `accountability_record` + `accountability_chain_head`
- Global `tenants` registry, `audit_log_global`, and
  `accountability_record_global` + `accountability_chain_head_global`
- UUID-based file identification with path-to-UUID mapping

### gRPC API (proto/fileservice.proto)
40+ RPCs covering: directory ops, file ops, stat/exists, move/copy/rename, versioning, metadata (including versioned variants), ACL management, role management, streaming upload/download, admin operations (storage usage, purge versions, trigger sync).

Authentication via `AuthenticationContext` message (user, roles, tenant, claims) — trusted upstream authentication model.

## Build Process
```bash
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/opt/file_engine_core
make -j$(nproc)
```

**Binaries produced:**
- `fileengine_server` — gRPC server (core/src/server.cpp)
- `fileengine_cli` — Command-line client
- `libfileengine_core.so` — Shared library

**Dependencies:** PostgreSQL (libpq), OpenSSL, zlib, gRPC++, Protobuf, libcurl, libuuid, AWS SDK for C++ (optional). Managed via pkg-config and CMake find_package.

## Testing

### The validation suite (ctest)
```bash
cmake -S . -B build && cmake --build build -j$(nproc)
cd build
ctest -L unit               # 10 tests, no infrastructure at all. Safe anywhere.
ctest -L lifecycle          # needs a database (skips cleanly without one)
ctest -L stress             # production-shaped load (needs the whole stack up)
ctest --output-on-failure   # everything registered
```

Labels select by what a test NEEDS, so a developer with no infrastructure can
still run a meaningful suite.

| Test | Label | What it proves |
|---|---|---|
| `test_acl_group_role_permissions`, `test_role_based_access_scenarios`, `test_comprehensive_acl_roles`, `test_acl_rbac_comprehensive`, `test_security_acl`, `test_deleted_reachability` | unit | ACL/RBAC behaviour against mocks |
| `test_connection_router` | unit | failover state transitions |
| `test_version_selection` | unit | newest-version selection ordering |
| `file_culler_tests`, `crypto_stream_tests` | unit | culling strategy; streaming crypto |
| `pool_lifecycle` | lifecycle | 8 callers over a pool of 2, so the queueing path is forced. A returned connection wakes a waiting caller, releases balance acquires, nobody is stranded. |
| `thread_lifecycle_stress` | lifecycle, stress | Real uploads through the HTTP bridge while sampling `/poolz`: every request completes, every connection comes back, workers return to waiting, the watchdog keeps probing under load. |
| `test_s3_integration` | integration | **DISABLED.** Its assertions pass, then the process segfaults on exit — see the note in `tests/CMakeLists.txt`. |

Registering these turned up two things that hand-running had hidden: a mock that
no longer compiled against `IDatabase`, and a test that segfaults on exit.

`enable_testing()` in the root `CMakeLists.txt` is what makes these visible —
without it CMake accepts `add_test()` and registers nothing, and `ctest` reports
zero tests while appearing to work.

**Forcing real contention.** A default core finishes operations faster than
clients can stack up, so the pool never drains and the queueing path goes
untested. Shrink the core rather than pushing harder — `FILEENGINE_HTTP_THREAD_POOL`
sets BOTH the gRPC worker count and the database pool size:
```bash
FILEENGINE_HTTP_THREAD_POOL=2 ./build/core/fileengine_server &
CORE_THREAD_POOL=2 python3 tests/thread_lifecycle_probe.py --files 300 --concurrency 32
```

### The older standalone binaries
```bash
cd build_test   # or build/tests/
cmake .. && make -j$(nproc)
./basic_tests   # or other test binaries
```
Most `tests/*.cpp` suites are still run this way and are NOT registered with
ctest; folding them in is outstanding work.

Key test suites (in `tests/`):
- `test_acl_rbac_comprehensive.cpp` — Newest comprehensive ACL+RBAC suite (~1255 lines, currently untracked)
- `test_comprehensive_acl_roles.cpp` — Full ACL + role coverage (~534 lines)
- `test_role_based_access_scenarios.cpp` — RBAC scenarios (~469 lines)
- `test_acl_group_role_permissions.cpp` — Group/role permissions (~375 lines)
- Core unit suites: `filesystem_tests.cpp`, `database_tests.cpp`, `storage_tests.cpp`, `cache_tests.cpp`, `acl_tests.cpp`, `tenant_tests.cpp`, `query_builder_tests.cpp`, `file_culler_tests.cpp`, `storage_tracker_tests.cpp`, `object_store_sync_tests.cpp`, `s3_tests.cpp`, `test_s3_integration.cpp`, `basic_tests.cpp`, `unit_tests.cpp`
- Shell scripts: `test_acl_roles.sh`, `test_permissions.sh`

Numerous ad-hoc `test_*.cpp` files at the project root are standalone integration tests (e.g., `test_grpc.cpp`, `test_s3_sync.cpp`, `test_tenant_manager.cpp`, `test_direct_filesystem*.cpp`) compiled separately from the main test suite.

## Deployment

**systemd service:** Runs as `fileengine` user with security hardening (NoNewPrivileges, PrivateTmp, ProtectSystem=strict, restricted capabilities).

**Paths:**
- Storage: `/var/lib/fileengine/storage/`
- Logs: `/var/log/fileengine/`
- Config: `/etc/fileengine/core.conf`

**Packaging:** Debian (.deb), Arch Linux (PKGBUILD), RPM (.spec). All handle user/group creation, directory setup, and systemd enablement.

## Critical Rules

- **Never edit the .env file** — it contains database and S3 connection credentials
- The `.env` must be symlinked into the build directory so binaries can find it
- S3 objects are treated as immutable by convention, **not by capability**: `S3Storage::delete_file` exists and works. That distinction matters now that erasure is on the roadmap — a compliance feature cannot be designed against a constraint that is not real, nor shipped against one that is. What genuinely cannot be rewritten is object-lock/WORM media, where only per-file keys and crypto-shredding would help, and those do not exist yet (`Storage` takes a deployment-wide `encrypt_data` flag, not a per-object key).
- All file operations use UUIDs, not paths, for distributed handling
- ACL tables live in tenant-specific schemas (not PUBLIC) to prevent data leakage
- Do not create per-tenant connection pools. (Note the intended "all tenants share one pool" invariant is not actually implemented — see `ConnectionPoolManager` above.)
- PUT operations return immediately after local storage; S3 backup is async via background worker thread

## Current Development State
- **Active branch:** `ACL-improvments` (4 commits ahead of main)
- **Recent work:** Role-based ACL permission implementation and comprehensive test coverage. A new `test_acl_rbac_comprehensive.cpp` suite plus updates to `test_comprehensive_acl_roles.cpp` and `tests/CMakeLists.txt` are currently uncommitted.
- **Main implementation** is in `/core/` with well-defined interfaces and dependency injection
- **backup_working_implementation/** referenced in older docs no longer exists in the tree — active development is in `/core/`
- **Other AI-context files** present at the root: `GEMINI.md`, `QWEN.md`, `README.md`, `DOCUMENTATION.md`, `SPECIFICATIONS.md`, `database_architecture.md`
