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