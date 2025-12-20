# FileEngine Core

A simplified, focused implementation of the FileEngine distributed virtual filesystem.

## Overview

FileEngine Core is a refactor of the original FileEngine project, focusing on core functionality while maintaining the advanced features needed for production use:

- Distributed virtual filesystem with horizontal scaling
- Multi-tenant architecture with complete isolation
- S3/MinIO integration for archival and cloud storage
- Advanced access control with POSIX ACLs
- Intelligent caching with configurable thresholds
- Protocol Buffers interface for all operations

## Architecture

The system consists of these main components:

- **Database Layer**: PostgreSQL with connection pooling and multi-tenant schema support
- **Storage Layer**: Local filesystem with SHA256 desaturation and object store integration
- **Access Control**: ACL-based permissions with role-based and attribute-based controls
- **Caching**: LRU-based file caching with automatic space management
- **Synchronization**: Automatic sync between local and remote (S3/MinIO) storage
- **Multitenancy**: Complete tenant isolation with separate schemas and storage

## Features

- **UUID-based file identification** for better distributed handling
- **Automatic versioning** with microsecond precision timestamps
- **POSIX-compliant ACLs** for granular access control
- **Intelligent file culling** with configurable thresholds
- **Hybrid cloud/on-premises** deployment support
- **Protocol Buffers interface** for all filesystem operations
- **S3/MinIO synchronization** with automatic recovery
- **Detailed storage tracking** per host and per tenant

## Installation

### Prerequisites

- C++17 compatible compiler (GCC 9+, Clang 10+)
- CMake 3.15+
- PostgreSQL 12+ (running instance required)
- libpq development headers
- For S3 support: AWS SDK for C++

### Build

```bash
mkdir build
cd build
cmake ..
make -j$(nproc)
```

### Configuration

See `DOCUMENTATION.md` for detailed configuration options.

## Usage

### Command Line Interface

The system includes a basic CLI for administrative functions:

```bash
# Create a directory
./fileengine_cli mkdir <parent_uid> <name>

# List directory contents
./fileengine_cli ls <dir_uid>

# Create a file
./fileengine_cli touch <parent_uid> <name>

# Get file information
./fileengine_cli stat <file_uid>

# Create a tenant
./fileengine_cli tenant-create <tenant_id>
```

### Library Integration

The core components can be integrated directly into other applications:

```cpp
#include "filesystem.h"
#include "tenant_manager.h"

// Initialize the system
fileengine::TenantConfig config;
// ... configure parameters
auto tenant_manager = std::make_shared<fileengine::TenantManager>(config);
auto filesystem = std::make_shared<fileengine::FileSystem>(tenant_manager);

// Use the filesystem
auto result = filesystem->mkdir("", "my_directory", "user1");
```

## Testing

Run the basic tests:
```bash
./tests/basic_tests
```

## Contributing

We welcome contributions! Please see the issue tracker for current tasks or to report issues.

## License

This project is licensed under the GPL-3.0 License - see the LICENSE file for details.

## Support

For support, please open an issue in the repository.