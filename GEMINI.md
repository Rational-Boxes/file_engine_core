# FileEngine Core - Project Context

## Overview
FileEngine Core is a simplified, focused implementation of a distributed virtual filesystem with horizontal scaling and hybrid cloud/on-premises deployment support. It refactors the original complex FileEngine project to be more maintainable and focused.

## Project Structure
```
/home/telendry/code/file_projects/file_engine_core/
├── .env.backup
├── .gitignore
├── CMakeLists.txt
├── database_architecture.md
├── DOCUMENTATION.md
├── README.md
├── SPECIFICATIONS.md
├── backup_working_implementation/     # Working backup implementation
├── build/
├── build_test/
├── cli/                              # Command-line interface
├── core/                             # Core implementation (C++ source)
├── proto/                            # Protocol Buffers definitions
└── tests/                            # Test suite
```

## Architecture Components

### Core Components
- **Database Layer**: PostgreSQL with connection pooling and multi-tenant schema support
- **Storage Layer**: Local filesystem with SHA256 desaturation and S3/MinIO integration
- **Access Control**: POSIX ACLs for granular permissions with RBAC/ABAC
- **Caching**: LRU-based file caching with automatic space management
- **Synchronization**: Automatic sync between local and remote (S3/MinIO) storage
- **Multitenancy**: Complete tenant isolation with separate schemas and storage

### Key Classes (in core/include/fileengine/)
- `FileSystem` - Main filesystem operations interface
- `TenantManager` - Manages tenant contexts with database, storage, and object store
- `AclManager` - POSIX ACL-based permission system
- `CacheManager` - LRU-based caching with configurable thresholds
- `ConnectionPoolManager` - Database connection pooling
- `S3Storage` - S3/MinIO object store integration
- `StorageTracker` - Per-host storage usage tracking

## Key Features
- **UUID-based file identification** for distributed handling
- **Automatic versioning** with microsecond precision timestamps
- **POSIX-compliant ACLs** for granular access control
- **Intelligent file culling** with configurable thresholds
- **Hybrid cloud/on-premises** deployment support
- **Protocol Buffers interface** for all operations
- **S3/MinIO synchronization** with automatic recovery
- **Detailed storage tracking** per host and tenant
- **Asynchronous object store backup** - Performance enhancement: Put operations return immediately after local storage completion while object store backup happens in background
- **Enhanced concurrency logging** - Detailed debugging information for troubleshooting potential race conditions and concurrent operations
- **Thread-safe operation queues** - Safe handling of concurrent file operations with mutex-protected shared resources

## Technical Requirements
- C++17 compatible compiler
- CMake 3.15+
- PostgreSQL 12+
- libpq development headers
- AWS SDK for C++ (for S3 support)

## Critical environment and configuration background

- All connections for database and object-store are in the .env file
- Never edit the .env file
- The .env needs to be symlinked into the build directory so the binaries can find it

## Current Implementation State
The project has a backup working implementation in the `backup_working_implementation/` directory with functional components including:
- Database connection pooling
- Tenant management
- S3 storage integration
- ACL management
- gRPC service interface
- File culling logic

The main implementation appears to be in the `/core/` directory with a well-defined architecture using interfaces and dependency injection.

## Build Process
```bash
mkdir build
cd build
cmake ..
make -j$(nproc)
```

## Configuration
The system is configured through environment variables or configuration files for:
- Database connection parameters
- S3/MinIO connection parameters
- Caching thresholds and policies
- Encryption and compression settings

## Security Features
- At-rest encryption using AES-256-GCM
- Secure connection handling
- Access control enforcement at all operation levels
- Audit trails for security monitoring
- Tenant isolation for multi-tenant deployments

## Performance Optimizations
- Directory desaturation to prevent filesystem bottlenecks
- Connection pooling for database operations
- In-memory caching for frequently accessed files
- Chunked streaming for large file operations
- Readers-writer locks for maximum read concurrency
- Asynchronous object store backup for improved PUT operation response times
- Thread-safe background worker for offloading compute-intensive operations
- Detailed performance logging for troubleshooting concurrency issues

## Architecture Additions and Improvements

### Asynchronous Object Store Backup
The system now implements a background worker thread for object store backups:

- PUT operations return immediately after local storage completion
- Object store backup happens asynchronously in the background
- Improved response times for file upload operations
- Thread-safe queue management with mutex protection
- Proper lifecycle management with startup and shutdown methods

### Enhanced Concurrency Handling
- Detailed logging for identifying potential race conditions
- Thread-safe operation queues with mutex protection
- Conditional variables for proper thread synchronization
- Comprehensive debug logging for troubleshooting concurrent operations
- Proper cleanup of worker threads during shutdown