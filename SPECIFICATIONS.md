# File Engine core and Commandline interface

This project is a simplification and refactor of the project in ../file_engine_cpp

The functionality is the core distributed virtual-filesystem supporting
horizontal scaling and hybrid cloud and on-premises deployment options.

## Core

Provides interface to the virtual-file functionality via Protocol Buffers.

Incoming requests require the user identifier, and lists of roles and claims for the user.
The system connecting to the Protocol Buffers interface is trusted to authenticate users
and retrieve roles and claims. The username, roles, and claims are processed against
POSIX ACLs to control filesystem entity visibility and permissions.

### Requirements

- Postgres connection pooling
- Multitenancy using database schemas
- Query builder utility to sanitize all queries
- Preserver S3 synchronization support
- Accommodate configuration requirements for MinIO as well as S3
- If S3 enables, sync files on startup
- If S3 connection fails on normal file system operations, use sentinel thread to attempt reconnection and resync
- Detailed per-host tracking of local filesystem usage and intelligent culling of files
- If a file does not exist in local storage, fetch the payload from the S3 service
- All filesystem operations and ACL permission management behind Protocol Buffers interface
- Prevent duplicate file object names, except deleted items

### Default permissions for ACL system

- read
- write
- delete
- list deleted
- undelete
- view versions
- retrieve back version
- restore to version

## Command-line interface

Commandline tool to connect to the Protocol Buffers interface locally for filesystem
and administrative functions. Implement in C++.

- All filesystem operations
- ACL and permission control operations
- Diagnostic checks for filesystem usage, S3 synchronization status, logged errors

### Filesystem operations

- list, option to show deleted
- mkdir
- touch
- put
- upload, combines touch and put
- download, optional select version
- view versions
- restore to version
- delete
- undelete

## Database structure

The Postgres database must implement a management
module that manages a global connection pool manager.

The default schema is used for tables that are not unique
to a tenant, specifically the tables needed for tracking
file usage per host for local space management.

Multi-tenancy is implemented in schemas, when no schema
specified the fallback is `default`.

When a tenant is accessed for the first time the required
are created and in the `files` table the root record is
created, the root node is identified by a blank UUID string.

The `files` table needs to track:

- UUID
- Name
- parent UUID
- size in bytes, for files
- owner user
- permission bit map
- is a container, folder flag
- deleted flag

`versions` table:

- file_uuid
- timestamp
- size
- user who saved this version

`metadata`

- file_uuid
- timestamp
- key name
- value
- metadata creation date
- user identity

### Usage tracking

Global table to track file access and usage. Used to identify
files to cull from the local storage when free space is running
out. Uses last access and access frequency to determine what
files to delete to recover space in the local cache. Only
can be active when data is backed up to an object store.

## Object-store integration

The set of files needs to be stored in an S3 compatible service.
The full history of all files is stored in object store for fully
a immutable file system. This also allows for managing the local
cache of file payloads to cull less used files to below a space
usage threshold.

The object-store integration needs to support both MinIO and native
AWS S3 using the official AWS-SDK library.

### On-demand upload to object store

After writing  file payload to local storage, a background thread
uploads it to the S3/object-store service. When a file that is
defined in the database is accessed but the local store does not
contain the payload, it is fetched from the object store.

Implement functions for uploading and downloading a single file
from S3/object-store as this will also be used in synchronization.

### Storage synchronization

This operation scans all local files and if not in the S3/object-store
uploads them. This needs to run as a background thread that is started
when the service loads. It runs once at start-up to make sure any local
files are saved. Then it is idle unless the connection to the object-store
fails. Then it wakes every FILEENGINE_S3_RETRY_SECONDS and attempts to
conduct a synchronization. There needs to be a flag for a loss of connectivity
that causes the sync thread to become active until connection restored
and any new files synced up.