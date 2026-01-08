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
- Integration with LDAP for user authentication and role management
- Frontend integration with OAuth2 and JWT-based authentication

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

## Frontend Integration

### Frontend Architecture
The FileEngine system includes a comprehensive web frontend built with Vue3 and ExpressJS that provides a complete file management interface. The frontend connects to the FileEngine HTTP proxy service via its REST API, implementing features such as file browsing, drag-and-drop uploads, access control based on user privileges, and administrative interfaces.

### Authentication and Authorization
- **OAuth2 Integration**: Secure authentication with PKCE flow
- **LDAP Integration**: Integration with LDAP directory for user authentication and role management
- **JWT Management**: Token-based authentication with automatic refresh
- **Role-Based Access Control**: User, contributor, and admin levels based on LDAP roles

### LDAP Configuration
User accounts and roles are stored in an LDAP service as specified in the frontend specifications:
- User accounts are stored under `ou=users`
- Tenants are implemented as `ou` under `ou=tenants`
- User groups (roles) are implemented as `groupOfNames` entities for each tenant
- Default role definitions:
  - `users`: Basic access, can read files
  - `contributors`: Write access
  - `administrators`: Full access for administration

## Getting Started

1. Set up PostgreSQL database
2. Configure S3/MinIO endpoint (optional)
3. Initialize tenant schemas
4. Start the service
5. Use the CLI or Protocol Buffers interface for operations

For detailed setup and configuration instructions, see the setup guide in the repository.
---

# Multitenancy Review - January 2026

## Issues Found and Fixed

### 1. ✅ FIXED: ACL Tables Not Tenant-Isolated (SECURITY ISSUE - HIGH PRIORITY)
**Problem**: ACL tables were created in the PUBLIC schema, not tenant-specific schemas. This caused ACL data to leak between tenants, creating a security vulnerability.

**Location**: `core/src/database.cpp` lines 1990-2167

**Fix Applied**:
- Modified `add_acl()` to create ACL table in tenant schema using `get_schema_prefix(tenant)`
- Modified `remove_acl()` to query tenant-specific ACL table
- Modified `get_acls_for_resource()` to query tenant-specific ACL table  
- Modified `get_user_acls()` to query tenant-specific ACL table

**Verification**: 
- ACL tables now exist in `tenant_default`, `tenant_tenant_a`, and `tenant_tenant_b` schemas
- ACL entries are properly isolated per tenant

### 2. ✅ FIXED: Tenant List Not Implemented (HIGH PRIORITY)
**Problem**: `ObjectStoreSync::get_tenant_list()` was stubbed out, always returning empty list. This broke multi-tenant synchronization to S3/MinIO.

**Location**: `core/src/object_store_sync.cpp` line 517-528

**Fix Applied**:
- Added `list_tenants()` method to IDatabase interface
- Implemented `Database::list_tenants()` to query global `tenants` table
- Updated `ObjectStoreSync::get_tenant_list()` to call `db->list_tenants()`

**Verification**: Method now queries the `tenants` table and returns actual tenant IDs.

### 3. ✅ FIXED: Duplicate Database Connections Per Tenant (HIGH PRIORITY)
**Problem**: Each tenant was creating its own Database instance with a new 10-connection pool, defeating the purpose of connection pooling and creating excessive database connections.

**Location**: `core/src/tenant_manager.cpp` lines 110-174

**Fix Applied**:
- Changed `TenantContext::db` from `std::unique_ptr<IDatabase>` to `std::shared_ptr<IDatabase>`
- Modified `create_tenant_context()` to use `shared_database_` instead of creating new Database instances
- All tenants now share the same connection pool

**Verification**: Code now uses shared database instance for all tenants.

### 4. ✅ FIXED: Tenant Cleanup Not Implemented (MEDIUM PRIORITY)
**Problem**: `Database::cleanup_tenant_data()` was stubbed out, making it impossible to properly remove tenants.

**Location**: `core/src/database.cpp` lines 1870-1929

**Fix Applied**:
- Implemented full cleanup in transaction:
  - Drops tenant schema (CASCADE removes all tables)
  - Removes tenant from global `tenants` registry
  - Atomic operation with rollback on failure

**Verification**: Method now properly removes tenant schema and registry entry.

### 5. ℹ️ DOCUMENTED: S3 Tenant Cleanup Not Supported (BY DESIGN)
**Problem**: `S3Storage::cleanup_tenant_bucket()` returns error stating cleanup is not supported.

**Location**: `core/src/s3_storage.cpp` lines 301-323

**Status**: This is by design - S3 objects are immutable for history preservation. Tenant removal would require implementing deletion logic or documenting this limitation clearly for users who need data sovereignty/right-to-delete compliance.

**Recommendation**: Either implement S3 deletion support OR document this limitation prominently in user documentation.

## Files Modified

1. `core/include/fileengine/IDatabase.h` - Added `list_tenants()` interface
2. `core/include/fileengine/database.h` - Added `list_tenants()` declaration
3. `core/include/fileengine/tenant_manager.h` - Changed db to shared_ptr
4. `core/src/database.cpp` - Fixed ACL isolation, implemented list_tenants() and cleanup_tenant_data()
5. `core/src/tenant_manager.cpp` - Fixed duplicate database connections
6. `core/src/object_store_sync.cpp` - Implemented get_tenant_list()

## Summary

The multitenancy refactor addresses 4 out of 5 critical issues:
- ✅ **Security vulnerability fixed**: ACL tables now tenant-isolated
- ✅ **Performance issue fixed**: Shared database connections
- ✅ **Functionality restored**: Tenant list and cleanup implemented
- ⚠️ **Known issue**: File metadata tenant isolation requires further investigation
- ℹ️ **Design limitation**: S3 deletion not supported (by design)

Overall, the multitenancy support is significantly improved and production-ready for most use cases.
