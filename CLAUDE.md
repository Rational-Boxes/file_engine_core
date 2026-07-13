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
| **DEPRICATED_file_engine_cpp** | C++ | **Deprecated** predecessor of the core — do not modify | — | — |

**Shared infra (dev):** PostgreSQL `:5434`, OpenLDAP `:1389`, MinIO `:9000`, Redis `:6379`.

### Security / trust model (read before touching auth)
- **Core trusts its input.** Every RPC carries `AuthenticationContext{user, roles, tenant, claims}`; the core does **not** authenticate — it authorizes. Whoever calls gRPC directly is fully trusted, so gRPC (`:50051`) must never be network-exposed. Authentication lives in the bridges/LDAP.
- **`system_admin` role bypasses all ACL checks** in the core. The SDKs pass roles verbatim, so an untrusted SDK caller can forge it — SDKs are safe only server-side.
- **Bridges are the security boundary:** LDAP bind → JWT (HS256, `FILEENGINE_JWT_SECRET` shared across services for local verification), tenant resolution, request-size caps, CORS scoping.
- **Feature services fail-closed:** permission cache (TTL ≤ 5 min, event-invalidated); core unreachable ⇒ deny.
- **Monitoring endpoints** (`/healthz` `/readyz` `/poolz`) are unauthenticated and **must bind loopback-only** (the core's REST monitor defaults to `0.0.0.0:8081` — that is a known exposure to verify per deployment).
- **Audit is tamper-evident** (SHA-256 hash chain) and, for auth events, **fail-closed** (login refused if the audit stream is unreachable).

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
| `AclManager` | acl_manager.h | POSIX ACL permissions with role-based support. Permissions: READ, WRITE, DELETE, LIST_DELETED, UNDELETE, VIEW_VERSIONS, RETRIEVE_BACK_VERSION, RESTORE_TO_VERSION, EXECUTE. |
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
| `ConnectionPoolManager` | connection_pool_manager.h | Singleton ensuring all DB instances share one pool. Supports read-only failover mode. |
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
- Tables per schema: `files`, `versions`, `metadata`, `acls`, `roles`, `user_roles`
- Global `tenants` registry table
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
```bash
cd build_test   # or build/tests/
cmake .. && make -j$(nproc)
./basic_tests   # or other test binaries
```

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
- S3 objects are immutable by design — deletion is not supported in the object store
- All file operations use UUIDs, not paths, for distributed handling
- ACL tables live in tenant-specific schemas (not PUBLIC) to prevent data leakage
- All tenants share one `ConnectionPool` — do not create per-tenant pools
- PUT operations return immediately after local storage; S3 backup is async via background worker thread

## Current Development State
- **Active branch:** `ACL-improvments` (4 commits ahead of main)
- **Recent work:** Role-based ACL permission implementation and comprehensive test coverage. A new `test_acl_rbac_comprehensive.cpp` suite plus updates to `test_comprehensive_acl_roles.cpp` and `tests/CMakeLists.txt` are currently uncommitted.
- **Main implementation** is in `/core/` with well-defined interfaces and dependency injection
- **backup_working_implementation/** referenced in older docs no longer exists in the tree — active development is in `/core/`
- **Other AI-context files** present at the root: `GEMINI.md`, `QWEN.md`, `README.md`, `DOCUMENTATION.md`, `SPECIFICATIONS.md`, `database_architecture.md`
