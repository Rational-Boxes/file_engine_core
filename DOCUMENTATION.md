# FileEngine Core Documentation

## Overview

FileEngine Core is a simplified, focused version of the original FileEngine project that provides a distributed virtual filesystem with horizontal scaling and hybrid cloud/on-premises deployment support. The system is designed to be more maintainable and focused than the original complex implementation.

## Architecture

The system is composed of several key components that work together:

### 1. Database Layer
- PostgreSQL-based metadata storage using UUIDs instead of paths for file identification
- Connection pooling for efficient database usage
- Multi-tenant support using separate database schemas per tenant
- Version tracking with UNIX timestamps for precise version control

### 2. Storage Layer
- Local file storage with SHA256-based directory desaturation to prevent performance bottlenecks
- Automatic compression and encryption of stored files
- Integration with object stores (S3/MinIO) for archival and scaling

### 3. Object Store Integration
- S3/MinIO support for archival and distributed storage
- Automatic synchronization between local and remote storage
- Recovery mechanisms for connection failures

### 4. Multitenancy
- Complete tenant isolation using separate database schemas and storage directories
- Tenant-specific configuration and resource management
- Global user and role management across tenants

### 5. Access Control
- ACL-based permissions system supporting POSIX-style permissions
- Role-based access control (RBAC) with hierarchical roles
- Attribute-based access control (ABAC) through user claims

### 6. Caching
- LRU-based caching with configurable thresholds
- Automatic space management when local storage limits are reached
- Intelligent culling of least-used files while ensuring they exist in object store

## Key Features

### Distributed Virtual Filesystem
- Files and directories identified by UUIDs instead of paths
- Full CRUD operations on files and directories
- Hierarchical directory structure with parent-child relationships
- Metadata storage and retrieval (versioned by timestamp)

### Horizontal Scaling
- Object store integration allows horizontal scaling across storage nodes
- Multi-tenant architecture with isolated resources per tenant
- Connection pooling for efficient resource utilization

### Hybrid Cloud Deployment
- Local storage for frequently accessed files
- Object store (S3/MinIO) for archival and backup
- Automatic synchronization between local and remote storage
- Offline access to locally cached files

### Advanced Access Control
- Unix-style permissions (owner/group/other with rwx bits)
- Role-based access control with configurable roles
- File-level ACLs for granular permission control
- Permission inheritance from parent to child resources

### Version Control
- Automatic versioning using UNIX timestamps with microsecond precision
- Version listing and retrieval operations
- Preservation of file history for recovery purposes

### Intelligent Caching
- LRU-based file caching with configurable thresholds (default 80%)
- Automatic cleanup of least-used files when storage limits reached
- Verification that files exist in object store before cache eviction
- Detailed per-host storage usage tracking

## API Overview

The system provides a Protocol Buffers interface for all filesystem operations, including:

- Directory operations (mkdir, rmdir, listdir)
- File operations (touch, remove, put, get)
- Metadata operations (stat, exists, set/get metadata)
- Version operations (list_versions, get_version)
- Access control operations (ACL management)
- Administrative functions

## Configuration

The system is configured through environment variables or configuration files:

- Database connection parameters
- Storage base path
- S3/MinIO connection parameters
- Tenant-specific configurations
- Caching thresholds and policies
- Encryption and compression settings

## Performance Optimizations

- Directory desaturation to prevent filesystem bottlenecks in high-file-count scenarios
- Connection pooling for database operations
- In-memory caching for frequently accessed files
- Chunked streaming for large file operations
- Readers-writer locks for maximum read concurrency with exclusive writes

## Security Features

- At-rest encryption using AES-256-GCM
- Secure connection handling
- Access control enforcement at all operation levels
- Audit trails for security monitoring
- Tenant isolation for multi-tenant deployments

## Getting Started

1. Set up PostgreSQL database
2. Configure S3/MinIO endpoint (optional)
3. Initialize tenant schemas
4. Start the service
5. Use the CLI or Protocol Buffers interface for operations

For detailed setup and configuration instructions, see the setup guide in the repository.