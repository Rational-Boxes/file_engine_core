# FileEngine Core - Project Context

## Overview
FileEngine Core is a C++17 distributed virtual filesystem with horizontal scaling and hybrid cloud/on-premises deployment. It provides multi-tenant file management with POSIX ACLs, S3/MinIO object store integration, and a gRPC API. Version 1.0.0.

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

Key test suites:
- `test_comprehensive_acl_roles.cpp` — Full ACL + role coverage (~26K lines)
- `test_role_based_access_scenarios.cpp` — RBAC scenarios (~25K lines)
- `test_acl_group_role_permissions.cpp` — Group/role permissions (~19K lines)
- `filesystem_tests.cpp`, `database_tests.cpp`, `storage_tests.cpp`, `cache_tests.cpp`, etc.
- Shell scripts: `test_acl_roles.sh`, `test_permissions.sh`

Ad-hoc test files in project root (test_*.cpp) are standalone integration tests compiled separately.

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
- **Active branch:** `ACL-improvments` (3 commits ahead of main)
- **Recent work:** Role-based ACL permission implementation and comprehensive test coverage
- **Main implementation** is in `/core/` with well-defined interfaces and dependency injection
- **backup_working_implementation/** is a legacy reference — active development is in `/core/`
