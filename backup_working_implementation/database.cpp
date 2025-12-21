#include "database.h"
#include <sstream>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <algorithm>
#include <unistd.h>

namespace fileengine {

Database::Database(const std::string& host, int port, const std::string& dbname,
                   const std::string& user, const std::string& password)
    : host_(host), port_(port), dbname_(dbname), user_(user), password_(password) {
    // Build connection string for creating connections on demand
    std::stringstream conninfo;
    conninfo << "host=" << host_
             << " port=" << port_
             << " dbname=" << dbname_
             << " user=" << user_
             << " password=" << password_;
    connection_info_ = conninfo.str();
}

Database::~Database() {
    // In per-thread connection model, no persistent connection to close
}

bool Database::connect() {
    // Test connection by creating and destroying a temporary connection
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    bool connected = (PQstatus(conn) == CONNECTION_OK);
    PQfinish(conn);
    return connected;
}

void Database::disconnect() {
    // No-op in per-thread connection model
}

bool Database::is_connected() const {
    // Test by attempting to create a connection
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    bool connected = (PQstatus(conn) == CONNECTION_OK);
    PQfinish(conn);
    return connected;
}

Result<void> Database::check_connection() const {
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<void>::err("Database connection failed: " + error);
    }
    
    // Simple test query
    PGresult* res = PQexec(conn, "SELECT 1;");
    ExecStatusType status = PQresultStatus(res);
    bool success = (status == PGRES_TUPLES_OK);
    std::string error;
    if (!success) {
        error = PQerrorMessage(conn);
    }
    
    PQclear(res);
    PQfinish(conn);
    
    if (success) {
        return Result<void>::ok();
    } else {
        return Result<void>::err("Database test query failed: " + error);
    }
}

std::string Database::escape_string(const std::string& str, PGconn* conn) const {
    if (!conn) return str;

    char* escaped = new char[str.length() * 2 + 1];
    PQescapeStringConn(conn, escaped, str.c_str(), str.length(), nullptr);
    std::string result(escaped);
    delete[] escaped;
    return result;
}

std::string Database::get_hostname() const {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        return std::string(hostname);
    } else {
        // Return a default hostname if we can't get the actual hostname
        return "default-host";
    }
}

Result<void> Database::create_schema() {
    // Create a connection for schema operations
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<void>::err("Failed to connect to database: " + error);
    }

    const char* schema_sql = R"SQL(
        CREATE TABLE IF NOT EXISTS files (
            id BIGSERIAL PRIMARY KEY,
            uid VARCHAR(64) UNIQUE NOT NULL,      -- UUID4 identifier for the file (increased to support special IDs like ROOT_DIR_*)
            path TEXT,                           -- Path for path-to-uid lookup (optional)
            name TEXT NOT NULL,
            parent_uid VARCHAR(64),              -- Parent UUID to support directory structure (increased to support special IDs like ROOT_DIR_*)
            type INTEGER NOT NULL,
            owner TEXT NOT NULL,
            permissions INTEGER NOT NULL DEFAULT 755,
            current_version TEXT,                -- Current version as timestamp string
            created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            modified_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            deleted BOOLEAN NOT NULL DEFAULT FALSE, -- Flag indicating if file is deleted (soft delete)
            deleted_at TIMESTAMP                -- Timestamp when file was deleted
        );

        CREATE INDEX IF NOT EXISTS idx_files_uid ON files(uid);
        CREATE INDEX IF NOT EXISTS idx_files_path ON files(path);
        CREATE INDEX IF NOT EXISTS idx_files_parent_uid ON files(parent_uid);

        CREATE TABLE IF NOT EXISTS versions (
            id BIGSERIAL PRIMARY KEY,
            file_uid VARCHAR(64) NOT NULL REFERENCES files(uid) ON DELETE CASCADE,  -- Changed from file_id to file_uid (increased to support special IDs like ROOT_DIR_*)
            version_timestamp TEXT NOT NULL,     -- UNIX timestamp as string instead of version_number
            size BIGINT NOT NULL,
            storage_path TEXT NOT NULL,
            created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(file_uid, version_timestamp)  -- Changed from file_id, version_number to file_uid, version_timestamp
        );

        CREATE INDEX IF NOT EXISTS idx_versions_file_uid ON versions(file_uid);

        CREATE TABLE IF NOT EXISTS metadata (
            id BIGSERIAL PRIMARY KEY,
            file_uid VARCHAR(64) NOT NULL REFERENCES files(uid) ON DELETE CASCADE,  -- Changed from file_id to file_uid (increased to support special IDs like ROOT_DIR_*)
            version_timestamp TEXT NOT NULL,     -- Changed from version_number to version_timestamp
            key TEXT NOT NULL,
            value TEXT NOT NULL,
            created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(file_uid, version_timestamp, key)  -- Changed to use file_uid and version_timestamp
        );

        CREATE INDEX IF NOT EXISTS idx_metadata_file_version ON metadata(file_uid, version_timestamp);  -- Changed to use file_uid and version_timestamp
        CREATE INDEX IF NOT EXISTS idx_metadata_key ON metadata(key);

        CREATE TABLE IF NOT EXISTS file_access_stats (
            id BIGSERIAL PRIMARY KEY,
            file_uid VARCHAR(64) NOT NULL REFERENCES files(uid) ON DELETE CASCADE,  -- Increased to support special IDs like ROOT_DIR_*
            hostname VARCHAR(255) NOT NULL,
            last_accessed TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            access_count INTEGER NOT NULL DEFAULT 0,
            created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(file_uid, hostname)
        );

        CREATE INDEX IF NOT EXISTS idx_file_access_stats_file_uid ON file_access_stats(file_uid);
        CREATE INDEX IF NOT EXISTS idx_file_access_stats_last_accessed ON file_access_stats(last_accessed);
        CREATE INDEX IF NOT EXISTS idx_file_access_stats_access_count ON file_access_stats(access_count);
        CREATE INDEX IF NOT EXISTS idx_file_access_stats_hostname ON file_access_stats(hostname);
        CREATE INDEX IF NOT EXISTS idx_file_access_stats_hostname_access_count ON file_access_stats(hostname, access_count);
        CREATE INDEX IF NOT EXISTS idx_file_access_stats_hostname_last_accessed ON file_access_stats(hostname, last_accessed);
    )SQL";

    PGresult* res = PQexec(conn, schema_sql);
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<void>::err("Failed to create schema: " + error);
    }

    PQclear(res);

    // Add the deleted columns to existing tables if they don't exist
    const char* alter_files_sql = R"SQL(
        DO $$
        BEGIN
            -- Add deleted column if it doesn't exist
            IF NOT EXISTS (SELECT 1 FROM information_schema.columns
                           WHERE table_name='files' AND column_name='deleted') THEN
                ALTER TABLE files ADD COLUMN deleted BOOLEAN NOT NULL DEFAULT FALSE;
            END IF;
            -- Add deleted_at column if it doesn't exist
            IF NOT EXISTS (SELECT 1 FROM information_schema.columns
                           WHERE table_name='files' AND column_name='deleted_at') THEN
                ALTER TABLE files ADD COLUMN deleted_at TIMESTAMP;
            END IF;
        END $$;
    )SQL";

    PGresult* alter_res = PQexec(conn, alter_files_sql);
    if (PQresultStatus(alter_res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(alter_res);
        PQfinish(conn);
        return Result<void>::err("Failed to alter files table: " + error);
    }
    PQclear(alter_res);
    PQfinish(conn);

    // Insert the root record to satisfy the self-referencing foreign key constraint
    // This allows root-level files and directories to be created properly
    PGconn* root_conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(root_conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(root_conn);
        PQfinish(root_conn);
        return Result<void>::err("Failed to connect to database for root record creation: " + error);
    }

    // Check if root record already exists
    const char* check_sql = "SELECT COUNT(*) FROM files WHERE uid = 'ROOT_DIR_00000000-0000-4000-8000-000000000000' AND parent_uid = 'ROOT_DIR_00000000-0000-4000-8000-000000000000';";
    PGresult* check_res = PQexec(root_conn, check_sql);

    if (PQresultStatus(check_res) != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(root_conn);
        PQclear(check_res);
        PQfinish(root_conn);
        return Result<void>::err("Failed to check for existing root record: " + error);
    }

    int count = std::stoi(PQgetvalue(check_res, 0, 0));
    PQclear(check_res);

    if (count == 0) {
        // Insert root record where parent_uid equals uid (self-referencing)
        const char* insert_sql = R"SQL(
            INSERT INTO files (uid, path, name, parent_uid, type, owner, permissions, created_at, modified_at)
            VALUES ('ROOT_DIR_00000000-0000-4000-8000-000000000000', '/', 'root', 'ROOT_DIR_00000000-0000-4000-8000-000000000000', 1, 'system', 755, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)
            ON CONFLICT (uid) DO NOTHING;
        )SQL";

        PGresult* insert_res = PQexec(root_conn, insert_sql);
        if (PQresultStatus(insert_res) != PGRES_COMMAND_OK && PQresultStatus(insert_res) != PGRES_TUPLES_OK) {
            std::string error = PQerrorMessage(root_conn);
            PQclear(insert_res);
            PQfinish(root_conn);
            return Result<void>::err("Failed to create root record: " + error);
        }
        PQclear(insert_res);
    }

    PQfinish(root_conn);
    return Result<void>::ok();
}

Result<void> Database::drop_schema() {
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<void>::err("Failed to connect to database: " + error);
    }

    const char* drop_sql = "DROP TABLE IF EXISTS metadata CASCADE; DROP TABLE IF EXISTS versions CASCADE; DROP TABLE IF EXISTS files CASCADE;";

    PGresult* res = PQexec(conn, drop_sql);
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<void>::err("Failed to drop schema: " + error);
    }

    PQclear(res);
    PQfinish(conn);
    return Result<void>::ok();
}

Result<std::string> Database::insert_file(const std::string& uid, const std::string& name,
                                           const std::string& path, const std::string& parent_uid,
                                           FileType type, const std::string& owner,
                                           int permissions, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<std::string>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "INSERT INTO files (uid, path, name, parent_uid, type, owner, permissions) VALUES ("
        << "'" << escape_string(uid, conn) << "', "
        << "'" << escape_string(path, conn) << "', "
        << "'" << escape_string(name, conn) << "', "
        << "'" << escape_string(parent_uid, conn) << "', "
        << static_cast<int>(type) << ", "
        << "'" << escape_string(owner, conn) << "', "
        << permissions << ") RETURNING uid";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<std::string>::err("Failed to insert file: " + error);
    }

    std::string inserted_uid = PQgetvalue(res, 0, 0);
    PQclear(res);
    PQfinish(conn);

    return Result<std::string>::ok(inserted_uid);
}

Result<void> Database::update_file_modified(const std::string& uid, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<void>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "UPDATE files SET modified_at = CURRENT_TIMESTAMP WHERE uid = '" 
        << escape_string(uid, conn) << "'";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<void>::err("Failed to update file: " + error);
    }

    PQclear(res);
    PQfinish(conn);
    return Result<void>::ok();
}

Result<void> Database::update_file_current_version(const std::string& uid, const std::string& version_timestamp, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<void>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "UPDATE files SET current_version = '" << escape_string(version_timestamp, conn)
        << "' WHERE uid = '" << escape_string(uid, conn) << "'";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<void>::err("Failed to update file current version: " + error);
    }

    PQclear(res);
    PQfinish(conn);
    return Result<void>::ok();
}

Result<void> Database::update_file_name(const std::string& uid, const std::string& new_name, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<void>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "UPDATE files SET name = '" << escape_string(new_name, conn)
        << "' WHERE uid = '" << escape_string(uid, conn) << "'";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<void>::err("Failed to update file name: " + error);
    }

    // Check if the update affected any rows
    if (PQcmdTuples(res) == 0) {
        PQclear(res);
        PQfinish(conn);
        return Result<void>::err("File not found: " + uid);
    }

    PQclear(res);
    PQfinish(conn);
    return Result<void>::ok();
}

Result<bool> Database::delete_file(const std::string& uid, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<bool>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "UPDATE files SET deleted = TRUE, deleted_at = CURRENT_TIMESTAMP WHERE uid = '" << escape_string(uid, conn) << "' AND deleted = FALSE AND uid != ''";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<bool>::err("Failed to delete file: " + error);
    }

    long rows_affected = strtol(PQcmdTuples(res), nullptr, 10);
    PQclear(res);
    PQfinish(conn);

    return Result<bool>::ok(rows_affected > 0);
}

Result<bool> Database::undelete_file(const std::string& uid, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<bool>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "UPDATE files SET deleted = FALSE, deleted_at = NULL WHERE uid = '" << escape_string(uid, conn) << "' AND deleted = TRUE AND uid != ''";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<bool>::err("Failed to undelete file: " + error);
    }

    long rows_affected = strtol(PQcmdTuples(res), nullptr, 10);
    PQclear(res);
    PQfinish(conn);

    return Result<bool>::ok(rows_affected > 0);
}

Result<int64_t> Database::get_file_size(const std::string& file_uid, const std::string& tenant) {
    (void)tenant;
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<int64_t>::err("Failed to connect to database: " + error);
    }

    // Get the latest version of the file
    std::stringstream version_sql;
    version_sql << "SELECT version_timestamp FROM versions WHERE file_uid = '"
                << escape_string(file_uid, conn)
                << "' ORDER BY version_timestamp DESC LIMIT 1";

    PGresult* version_res = PQexec(conn, version_sql.str().c_str());
    ExecStatusType version_status = PQresultStatus(version_res);

    if (version_status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(version_res);
        PQfinish(conn);
        return Result<int64_t>::err("Failed to query file versions: " + error);
    }

    if (PQntuples(version_res) == 0) {
        // File has no versions, size is 0
        PQclear(version_res);
        PQfinish(conn);
        return Result<int64_t>::ok(0);
    }

    std::string latest_version = PQgetvalue(version_res, 0, 0);
    PQclear(version_res);

    // Get size of the latest version
    std::stringstream size_sql;
    size_sql << "SELECT size FROM versions WHERE file_uid = '"
             << escape_string(file_uid, conn)
             << "' AND version_timestamp = '" << escape_string(latest_version, conn) << "'";

    PGresult* size_res = PQexec(conn, size_sql.str().c_str());
    ExecStatusType size_status = PQresultStatus(size_res);

    if (size_status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(size_res);
        PQfinish(conn);
        return Result<int64_t>::err("Failed to query file size: " + error);
    }

    if (PQntuples(size_res) == 0) {
        PQclear(size_res);
        PQfinish(conn);
        return Result<int64_t>::err("File version not found");
    }

    int64_t size = std::stoll(PQgetvalue(size_res, 0, 0));

    PQclear(size_res);
    PQfinish(conn);
    return Result<int64_t>::ok(size);
}

Result<int64_t> Database::get_directory_size(const std::string& dir_uid, const std::string& tenant) {
    (void)tenant;
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<int64_t>::err("Failed to connect to database: " + error);
    }

    // First get all files in this directory (not including subdirectories)
    std::stringstream sql;
    sql << "SELECT uid FROM files WHERE parent_uid = '" << escape_string(dir_uid, conn)
        << "' AND deleted = FALSE AND type = 0"; // 0 for REGULAR_FILE

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<int64_t>::err("Failed to query directory contents: " + error);
    }

    int64_t total_size = 0;
    int nrows = PQntuples(res);

    // Sum up sizes of all files in this directory
    for (int i = 0; i < nrows; i++) {
        std::string file_uid = PQgetvalue(res, i, 0);
        auto size_result = get_file_size(file_uid, tenant);
        if (size_result.success) {
            total_size += size_result.value;
        } else {
            // Log warning but continue processing other files
            std::cout << "Warning: Could not get size for file " << file_uid << std::endl;
        }
    }

    // Now get all subdirectories and add their sizes recursively
    std::stringstream subdir_sql;
    subdir_sql << "SELECT uid FROM files WHERE parent_uid = '" << escape_string(dir_uid, conn)
               << "' AND deleted = FALSE AND type = 1"; // 1 for DIRECTORY

    PGresult* subdir_res = PQexec(conn, subdir_sql.str().c_str());
    ExecStatusType subdir_status = PQresultStatus(subdir_res);

    if (subdir_status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(subdir_res);
        PQfinish(conn);
        // Continue with just the file sizes calculated so far
    } else {
        int subdir_rows = PQntuples(subdir_res);
        for (int i = 0; i < subdir_rows; i++) {
            std::string subdir_uid = PQgetvalue(subdir_res, i, 0);
            auto subdir_size_result = get_directory_size(subdir_uid, tenant);
            if (subdir_size_result.success) {
                total_size += subdir_size_result.value;
            } else {
                // Log warning but continue
                std::cout << "Warning: Could not get size for directory " << subdir_uid << std::endl;
            }
        }
        PQclear(subdir_res);
    }

    PQclear(res);
    PQfinish(conn);
    return Result<int64_t>::ok(total_size);
}

Result<std::vector<FileInfo>> Database::list_files_in_directory_with_deleted(const std::string& parent_uid, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<std::vector<FileInfo>>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "SELECT uid, path, name, parent_uid, type, owner, permissions, current_version "
        << "FROM files WHERE parent_uid = '" << escape_string(parent_uid, conn) << "'";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<std::vector<FileInfo>>::err("Failed to list files: " + error);
    }

    std::vector<FileInfo> files;
    int nrows = PQntuples(res);

    for (int i = 0; i < nrows; i++) {
        FileInfo info;
        info.uid = PQgetvalue(res, i, 0);
        info.path = PQgetvalue(res, i, 1);
        info.name = PQgetvalue(res, i, 2);
        info.parent_uid = PQgetvalue(res, i, 3);
        info.type = static_cast<FileType>(std::stoi(PQgetvalue(res, i, 4)));
        info.owner = PQgetvalue(res, i, 5);
        info.permissions = std::stoi(PQgetvalue(res, i, 6));
        info.version = std::string(PQgetvalue(res, i, 7)); // Now a string timestamp
        info.size = 0; // To be filled from versions table if needed

        files.push_back(info);
    }

    PQclear(res);
    PQfinish(conn);
    return Result<std::vector<FileInfo>>::ok(files);
}

Result<std::optional<FileInfo>> Database::get_file_by_uid_include_deleted(const std::string& uid, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<std::optional<FileInfo>>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "SELECT uid, path, name, parent_uid, type, owner, permissions, current_version, "
        << "created_at, modified_at FROM files WHERE uid = '" << escape_string(uid, conn) << "'";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<std::optional<FileInfo>>::err("Failed to get file: " + error);
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        PQfinish(conn);
        return Result<std::optional<FileInfo>>::ok(std::nullopt);
    }

    FileInfo info;
    info.uid = PQgetvalue(res, 0, 0);
    info.path = PQgetvalue(res, 0, 1);
    info.name = PQgetvalue(res, 0, 2);
    info.parent_uid = PQgetvalue(res, 0, 3);
    info.type = static_cast<FileType>(std::stoi(PQgetvalue(res, 0, 4)));
    info.owner = PQgetvalue(res, 0, 5);
    info.permissions = std::stoi(PQgetvalue(res, 0, 6));
    info.version = std::string(PQgetvalue(res, 0, 7)); // Now a string timestamp
    // NOTE: Size and timestamps would need more complex handling

    PQclear(res);
    PQfinish(conn);
    return Result<std::optional<FileInfo>>::ok(info);
}

Result<std::optional<FileInfo>> Database::get_file_by_uid(const std::string& uid, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<std::optional<FileInfo>>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "SELECT uid, path, name, parent_uid, type, owner, permissions, current_version, "
        << "created_at, modified_at FROM files WHERE uid = '" << escape_string(uid, conn) << "' AND deleted = FALSE";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<std::optional<FileInfo>>::err("Failed to get file: " + error);
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        PQfinish(conn);
        return Result<std::optional<FileInfo>>::ok(std::nullopt);
    }

    FileInfo info;
    info.uid = PQgetvalue(res, 0, 0);
    info.path = PQgetvalue(res, 0, 1);
    info.name = PQgetvalue(res, 0, 2);
    info.parent_uid = PQgetvalue(res, 0, 3);
    info.type = static_cast<FileType>(std::stoi(PQgetvalue(res, 0, 4)));
    info.owner = PQgetvalue(res, 0, 5);
    info.permissions = std::stoi(PQgetvalue(res, 0, 6));
    info.version = std::string(PQgetvalue(res, 0, 7)); // Now a string timestamp
    // NOTE: Size and timestamps would need more complex handling
    
    PQclear(res);
    PQfinish(conn);
    return Result<std::optional<FileInfo>>::ok(info);
}

Result<std::optional<FileInfo>> Database::get_file_by_path(const std::string& path, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<std::optional<FileInfo>>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "SELECT uid, path, name, parent_uid, type, owner, permissions, current_version, "
        << "created_at, modified_at FROM files WHERE path = '" << escape_string(path, conn) << "'";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<std::optional<FileInfo>>::err("Failed to get file: " + error);
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        PQfinish(conn);
        return Result<std::optional<FileInfo>>::ok(std::nullopt);
    }

    FileInfo info;
    info.uid = PQgetvalue(res, 0, 0);
    info.path = PQgetvalue(res, 0, 1);
    info.name = PQgetvalue(res, 0, 2);
    info.parent_uid = PQgetvalue(res, 0, 3);
    info.type = static_cast<FileType>(std::stoi(PQgetvalue(res, 0, 4)));
    info.owner = PQgetvalue(res, 0, 5);
    info.permissions = std::stoi(PQgetvalue(res, 0, 6));
    info.version = std::string(PQgetvalue(res, 0, 7)); // Now a string timestamp
    
    PQclear(res);
    PQfinish(conn);
    return Result<std::optional<FileInfo>>::ok(info);
}

Result<std::vector<FileInfo>> Database::list_files_in_directory(const std::string& parent_uid, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<std::vector<FileInfo>>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "SELECT uid, path, name, parent_uid, type, owner, permissions, current_version "
        << "FROM files WHERE parent_uid = '" << escape_string(parent_uid, conn) << "' AND deleted = FALSE";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<std::vector<FileInfo>>::err("Failed to list files: " + error);
    }

    std::vector<FileInfo> files;
    int nrows = PQntuples(res);

    for (int i = 0; i < nrows; i++) {
        FileInfo info;
        info.uid = PQgetvalue(res, i, 0);
        info.path = PQgetvalue(res, i, 1);
        info.name = PQgetvalue(res, i, 2);
        info.parent_uid = PQgetvalue(res, i, 3);
        info.type = static_cast<FileType>(std::stoi(PQgetvalue(res, i, 4)));
        info.owner = PQgetvalue(res, i, 5);
        info.permissions = std::stoi(PQgetvalue(res, i, 6));
        info.version = std::string(PQgetvalue(res, i, 7)); // Now a string timestamp
        info.size = 0; // To be filled from versions table if needed

        files.push_back(info);
    }

    PQclear(res);
    PQfinish(conn);
    return Result<std::vector<FileInfo>>::ok(files);
}

Result<std::optional<FileInfo>> Database::get_file_by_name_and_parent(const std::string& name, const std::string& parent_uid, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<std::optional<FileInfo>>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "SELECT uid, path, name, parent_uid, type, owner, permissions, current_version "
        << "FROM files WHERE name = '" << escape_string(name, conn)
        << "' AND parent_uid = '" << escape_string(parent_uid, conn)
        << "' AND deleted = FALSE";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<std::optional<FileInfo>>::err("Failed to query file: " + error);
    }

    std::optional<FileInfo> file_info;
    int nrows = PQntuples(res);

    if (nrows > 0) {
        // Only take the first result if multiple exist (shouldn't happen if we maintain uniqueness)
        FileInfo info;
        info.uid = PQgetvalue(res, 0, 0);
        info.path = PQgetvalue(res, 0, 1);
        info.name = PQgetvalue(res, 0, 2);
        info.parent_uid = PQgetvalue(res, 0, 3);
        info.type = static_cast<FileType>(std::stoi(PQgetvalue(res, 0, 4)));
        info.owner = PQgetvalue(res, 0, 5);
        info.permissions = std::stoi(PQgetvalue(res, 0, 6));
        info.version = std::string(PQgetvalue(res, 0, 7)); // Now a string timestamp
        info.size = 0; // To be filled from versions table if needed

        file_info = info;
    }

    PQclear(res);
    PQfinish(conn);
    return Result<std::optional<FileInfo>>::ok(file_info);
}

Result<std::optional<FileInfo>> Database::get_file_by_name_and_parent_include_deleted(const std::string& name, const std::string& parent_uid, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<std::optional<FileInfo>>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "SELECT uid, path, name, parent_uid, type, owner, permissions, current_version "
        << "FROM files WHERE name = '" << escape_string(name, conn)
        << "' AND parent_uid = '" << escape_string(parent_uid, conn)
        << "'";  // No AND deleted = FALSE, so it includes deleted files

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<std::optional<FileInfo>>::err("Failed to query file: " + error);
    }

    std::optional<FileInfo> file_info;
    int nrows = PQntuples(res);

    if (nrows > 0) {
        // Only take the first result if multiple exist (shouldn't happen if we maintain uniqueness)
        FileInfo info;
        info.uid = PQgetvalue(res, 0, 0);
        info.path = PQgetvalue(res, 0, 1);
        info.name = PQgetvalue(res, 0, 2);
        info.parent_uid = PQgetvalue(res, 0, 3);
        info.type = static_cast<FileType>(std::stoi(PQgetvalue(res, 0, 4)));
        info.owner = PQgetvalue(res, 0, 5);
        info.permissions = std::stoi(PQgetvalue(res, 0, 6));
        info.version = std::string(PQgetvalue(res, 0, 7)); // Now a string timestamp
        info.size = 0; // To be filled from versions table if needed

        file_info = info;
    }

    PQclear(res);
    PQfinish(conn);
    return Result<std::optional<FileInfo>>::ok(file_info);
}

Result<std::string> Database::path_to_uid(const std::string& path, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<std::string>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "SELECT uid FROM files WHERE path = '" << escape_string(path, conn) << "'";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<std::string>::err("Failed to look up UID for path: " + error);
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        PQfinish(conn);
        return Result<std::string>::err("Path not found: " + path);
    }

    std::string uid = PQgetvalue(res, 0, 0);
    PQclear(res);
    PQfinish(conn);

    return Result<std::string>::ok(uid);
}

Result<std::vector<std::string>> Database::uid_to_path(const std::string& uid, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<std::vector<std::string>>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "SELECT path FROM files WHERE uid = '" << escape_string(uid, conn) << "'";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<std::vector<std::string>>::err("Failed to look up path for UID: " + error);
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        PQfinish(conn);
        return Result<std::vector<std::string>>::err("UID not found: " + uid);
    }

    std::string path_str = PQgetvalue(res, 0, 0);
    PQclear(res);
    PQfinish(conn);

    // Split path into components
    std::vector<std::string> path_components;
    if (!path_str.empty()) {
        // Split by '/' character
        size_t start = 0;
        size_t end = path_str.find('/');
        while (end != std::string::npos) {
            if (end != start) { // skip empty segments
                path_components.push_back(path_str.substr(start, end - start));
            }
            start = end + 1;
            end = path_str.find('/', start);
        }
        if (start < path_str.length()) {
            path_components.push_back(path_str.substr(start));
        }
    }

    return Result<std::vector<std::string>>::ok(path_components);
}

Result<int64_t> Database::insert_version(const std::string& file_uid, const std::string& version_timestamp,
                                          int64_t size, const std::string& storage_path, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<int64_t>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "INSERT INTO versions (file_uid, version_timestamp, size, storage_path) VALUES ("
        << "'" << escape_string(file_uid, conn) << "', "
        << "'" << escape_string(version_timestamp, conn) << "', "
        << size << ", "
        << "'" << escape_string(storage_path, conn) << "') RETURNING id";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<int64_t>::err("Failed to insert version: " + error);
    }

    int64_t id = std::stoll(PQgetvalue(res, 0, 0));
    PQclear(res);
    PQfinish(conn);

    return Result<int64_t>::ok(id);
}

Result<std::optional<std::string>> Database::get_version_storage_path(const std::string& file_uid, const std::string& version_timestamp, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<std::optional<std::string>>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "SELECT storage_path FROM versions WHERE file_uid = '" << escape_string(file_uid, conn)
        << "' AND version_timestamp = '" << escape_string(version_timestamp, conn) << "'";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<std::optional<std::string>>::err("Failed to get version: " + error);
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        PQfinish(conn);
        return Result<std::optional<std::string>>::ok(std::nullopt);
    }

    std::string storage_path = PQgetvalue(res, 0, 0);
    PQclear(res);
    PQfinish(conn);

    return Result<std::optional<std::string>>::ok(storage_path);
}

Result<std::vector<std::string>> Database::list_versions(const std::string& file_uid, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<std::vector<std::string>>::err("Failed to connect to database: " + error);
    }

    // First, check if the file exists and is not deleted
    std::stringstream check_sql;
    check_sql << "SELECT COUNT(*) FROM files WHERE uid = '" << escape_string(file_uid, conn)
              << "' AND deleted = FALSE";

    PGresult* check_res = PQexec(conn, check_sql.str().c_str());
    ExecStatusType check_status = PQresultStatus(check_res);

    if (check_status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(check_res);
        PQfinish(conn);
        return Result<std::vector<std::string>>::err("Failed to check file existence: " + error);
    }

    if (PQntuples(check_res) == 0 || std::stoi(PQgetvalue(check_res, 0, 0)) == 0) {
        PQclear(check_res);
        PQfinish(conn);
        return Result<std::vector<std::string>>::err("File not found or is deleted: " + file_uid);
    }
    PQclear(check_res);

    std::stringstream sql;
    sql << "SELECT version_timestamp FROM versions WHERE file_uid = '"
        << escape_string(file_uid, conn) << "' ORDER BY version_timestamp DESC";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<std::vector<std::string>>::err("Failed to list versions: " + error);
    }

    std::vector<std::string> versions;
    int nrows = PQntuples(res);

    for (int i = 0; i < nrows; i++) {
        versions.push_back(PQgetvalue(res, i, 0));
    }

    PQclear(res);
    PQfinish(conn);
    return Result<std::vector<std::string>>::ok(versions);
}

Result<void> Database::set_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& value, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<void>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "INSERT INTO metadata (file_uid, version_timestamp, key, value) VALUES ("
        << "'" << escape_string(file_uid, conn) << "', "
        << "'" << escape_string(version_timestamp, conn) << "', "
        << "'" << escape_string(key, conn) << "', "
        << "'" << escape_string(value, conn) << "') "
        << "ON CONFLICT (file_uid, version_timestamp, key) DO UPDATE SET value = EXCLUDED.value";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<void>::err("Failed to set metadata: " + error);
    }

    PQclear(res);
    PQfinish(conn);
    return Result<void>::ok();
}

Result<std::optional<std::string>> Database::get_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<std::optional<std::string>>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "SELECT value FROM metadata WHERE file_uid = '" << escape_string(file_uid, conn)
        << "' AND version_timestamp = '" << escape_string(version_timestamp, conn)
        << "' AND key = '" << escape_string(key, conn) << "'";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<std::optional<std::string>>::err("Failed to get metadata: " + error);
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        PQfinish(conn);
        return Result<std::optional<std::string>>::ok(std::nullopt);
    }

    std::string value = PQgetvalue(res, 0, 0);
    PQclear(res);
    PQfinish(conn);

    return Result<std::optional<std::string>>::ok(value);
}

Result<std::map<std::string, std::string>> Database::get_all_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<std::map<std::string, std::string>>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "SELECT key, value FROM metadata WHERE file_uid = '" << escape_string(file_uid, conn)
        << "' AND version_timestamp = '" << escape_string(version_timestamp, conn) << "'";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<std::map<std::string, std::string>>::err("Failed to get all metadata: " + error);
    }

    std::map<std::string, std::string> metadata;
    int nrows = PQntuples(res);

    for (int i = 0; i < nrows; i++) {
        std::string key = PQgetvalue(res, i, 0);
        std::string value = PQgetvalue(res, i, 1);
        metadata[key] = value;
    }

    PQclear(res);
    PQfinish(conn);
    return Result<std::map<std::string, std::string>>::ok(metadata);
}

Result<void> Database::delete_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<void>::err("Failed to connect to database: " + error);
    }

    std::stringstream sql;
    sql << "DELETE FROM metadata WHERE file_uid = '" << escape_string(file_uid, conn)
        << "' AND version_timestamp = '" << escape_string(version_timestamp, conn)
        << "' AND key = '" << escape_string(key, conn) << "'";

    PGresult* res = PQexec(conn, sql.str().c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<void>::err("Failed to delete metadata: " + error);
    }

    // Check if anything was deleted
    char* affected = PQcmdTuples(res);
    int count = affected ? std::atoi(affected) : 0;
    PQclear(res);
    PQfinish(conn);

    if (count == 0) {
        return Result<void>::err("Metadata key not found");
    }

    return Result<void>::ok();
}

Result<void> Database::execute(const std::string& sql, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<void>::err("Failed to connect to database: " + error);
    }

    PGresult* res = PQexec(conn, sql.c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<void>::err("Execute failed: " + error);
    }

    PQclear(res);
    PQfinish(conn);
    return Result<void>::ok();
}

Result<std::vector<std::vector<std::string>>> Database::query(const std::string& sql, const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<std::vector<std::vector<std::string>>>::err("Failed to connect to database: " + error);
    }

    PGresult* res = PQexec(conn, sql.c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<std::vector<std::vector<std::string>>>::err("Query failed: " + error);
    }

    std::vector<std::vector<std::string>> results;
    int nrows = PQntuples(res);
    int ncols = PQnfields(res);

    for (int i = 0; i < nrows; i++) {
        std::vector<std::string> row;
        for (int j = 0; j < ncols; j++) {
            row.push_back(PQgetvalue(res, i, j));
        }
        results.push_back(row);
    }

    PQclear(res);
    PQfinish(conn);
    return Result<std::vector<std::vector<std::string>>>::ok(results);
}

Result<void> Database::update_file_access_stats(const std::string& uid, const std::string& user, const std::string& tenant) {
    // In a real implementation, we might track who accessed the file
    // For now, we just update the access time and increment the counter
    (void)user;      // Mark parameter as intentionally unused to avoid compiler warning
    (void)tenant;    // Mark parameter as intentionally unused to avoid compiler warning
    // According to the spec, file usage tracking should be stored in the primary database
    // rather than tenant-specific databases, as the usage metrics need to account for
    // all tenant files held in local storage to prevent running out of space
    // Also, the file usage/age tables need to separate tracking by the current host name

    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<void>::err("Failed to connect to database: " + error);
    }

    std::string hostname = get_hostname();

    // Check if record exists - tracking by file_uid and hostname
    const char* check_sql = "SELECT 1 FROM file_access_stats WHERE file_uid = $1 AND hostname = $2";
    const char* check_values[] = {uid.c_str(), hostname.c_str()};
    int check_lengths[] = {static_cast<int>(uid.length()), static_cast<int>(hostname.length())};
    int check_formats[] = {0, 0}; // text format

    PGresult* check_res = PQexecParams(conn, check_sql, 2, nullptr, check_values, check_lengths, check_formats, 0);
    ExecStatusType check_status = PQresultStatus(check_res);

    if (check_status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(check_res);
        PQfinish(conn);
        return Result<void>::err("Failed to check access stats record: " + error);
    }

    bool record_exists = (PQntuples(check_res) > 0);
    PQclear(check_res);

    Result<void> result;
    if (record_exists) {
        // Update existing record
        const char* update_sql = R"(
            UPDATE file_access_stats
            SET last_accessed = CURRENT_TIMESTAMP, access_count = access_count + 1, updated_at = CURRENT_TIMESTAMP
            WHERE file_uid = $1 AND hostname = $2
        )";
        const char* update_values[] = {uid.c_str(), hostname.c_str()};
        int update_lengths[] = {static_cast<int>(uid.length()), static_cast<int>(hostname.length())};
        int update_formats[] = {0, 0}; // text format

        PGresult* update_res = PQexecParams(conn, update_sql, 2, nullptr, update_values, update_lengths, update_formats, 0);
        ExecStatusType update_status = PQresultStatus(update_res);

        if (update_status != PGRES_COMMAND_OK) {
            std::string error = PQerrorMessage(conn);
            PQclear(update_res);
            PQfinish(conn);
            return Result<void>::err("Failed to update access stats: " + error);
        }

        PQclear(update_res);
        result = Result<void>::ok();
    } else {
        // Insert new record
        const char* insert_sql = R"(
            INSERT INTO file_access_stats (file_uid, hostname, last_accessed, access_count, created_at, updated_at)
            VALUES ($1, $2, CURRENT_TIMESTAMP, 1, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)
        )";
        const char* insert_values[] = {uid.c_str(), hostname.c_str()};
        int insert_lengths[] = {static_cast<int>(uid.length()), static_cast<int>(hostname.length())};
        int insert_formats[] = {0, 0}; // text format

        PGresult* insert_res = PQexecParams(conn, insert_sql, 2, nullptr, insert_values, insert_lengths, insert_formats, 0);
        ExecStatusType insert_status = PQresultStatus(insert_res);

        if (insert_status != PGRES_COMMAND_OK) {
            std::string error = PQerrorMessage(conn);
            PQclear(insert_res);
            PQfinish(conn);
            return Result<void>::err("Failed to insert access stats: " + error);
        }

        PQclear(insert_res);
        result = Result<void>::ok();
    }

    PQfinish(conn);
    return result;
}

Result<std::vector<std::string>> Database::get_least_accessed_files(int limit, const std::string& tenant) {
    // According to the spec, file usage tracking should be stored in the primary database
    // rather than tenant-specific databases, as the usage metrics need to account for
    // all tenant files held in local storage to prevent running out of space
    // Also, track by hostname as specified in the requirements
    (void)tenant;  // Mark parameter as intentionally unused to avoid compiler warning

    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<std::vector<std::string>>::err("Failed to connect to database: " + error);
    }

    std::string hostname = get_hostname();

    // Query for least accessed files for this specific host
    std::string query = "SELECT file_uid FROM file_access_stats WHERE hostname = '" + hostname + "' ORDER BY access_count ASC, last_accessed ASC LIMIT " + std::to_string(limit);

    PGresult* res = PQexec(conn, query.c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<std::vector<std::string>>::err("Failed to query least accessed files: " + error);
    }

    std::vector<std::string> result;
    int nrows = PQntuples(res);

    for (int i = 0; i < nrows; i++) {
        result.push_back(std::string(PQgetvalue(res, i, 0)));
    }

    PQclear(res);
    PQfinish(conn);
    return Result<std::vector<std::string>>::ok(result);
}

Result<std::vector<std::string>> Database::get_infrequently_accessed_files(int days_threshold, const std::string& tenant) {
    // According to the spec, file usage tracking should be stored in the primary database
    // rather than tenant-specific databases, as the usage metrics need to account for
    // all tenant files held in local storage to prevent running out of space
    // Also, track by hostname as specified in the requirements
    (void)tenant;  // Mark parameter as intentionally unused to avoid compiler warning

    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<std::vector<std::string>>::err("Failed to connect to database: " + error);
    }

    std::string hostname = get_hostname();

    std::string query = "SELECT file_uid FROM file_access_stats WHERE hostname = '" + hostname +
                        "' AND last_accessed < CURRENT_TIMESTAMP - INTERVAL '" +
                        std::to_string(days_threshold) + " days' ORDER BY last_accessed ASC";

    PGresult* res = PQexec(conn, query.c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<std::vector<std::string>>::err("Failed to query infrequently accessed files: " + error);
    }

    std::vector<std::string> result;
    int nrows = PQntuples(res);

    for (int i = 0; i < nrows; i++) {
        result.push_back(std::string(PQgetvalue(res, i, 0)));
    }

    PQclear(res);
    PQfinish(conn);
    return Result<std::vector<std::string>>::ok(result);
}

Result<int64_t> Database::get_storage_usage(const std::string& tenant) {
    // According to the spec, file usage tracking should be stored in the primary database
    // rather than tenant-specific databases, as the usage metrics need to account for
    // all tenant files held in local storage to prevent running out of space
    // However, for storage usage calculation, we need the actual file sizes from the versions table
    (void)tenant;  // Mark parameter as intentionally unused to avoid compiler warning

    // This function would need to be implemented based on the actual file sizes in the system
    // For now, we'll return a placeholder implementation
    // In a real implementation, this would query the sum of file sizes from the versions table

    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<int64_t>::err("Failed to connect to database: " + error);
    }

    // Query the total size of all versions across all tenants for local storage calculation
    // This represents the total file size that would be stored in local cache
    const char* query = "SELECT COALESCE(SUM(size), 0) FROM versions";

    PGresult* res = PQexec(conn, query);
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<int64_t>::err("Failed to query storage usage: " + error);
    }

    if (PQntuples(res) != 1) {
        PQclear(res);
        PQfinish(conn);
        return Result<int64_t>::err("Unexpected result from storage usage query");
    }

    int64_t result = std::stoll(std::string(PQgetvalue(res, 0, 0)));

    PQclear(res);
    PQfinish(conn);
    return Result<int64_t>::ok(result);
}

Result<int64_t> Database::get_storage_capacity(const std::string& tenant) {
    (void)tenant;  // For multitenancy, this would determine which schema to use
    // This function needs to be implemented based on the underlying file system
    // In a real system, this would call system-specific APIs to get disk capacity

    // For now, we'll return an invalid value to indicate this needs further implementation
    // This function should be implemented by the storage system, not the database
    return Result<int64_t>::err("get_storage_capacity should be implemented at the filesystem/storage layer, not database layer");
}

Result<void> Database::create_tenant_schema(const std::string& tenant) {
    if (tenant.empty()) {
        return Result<void>::err("Cannot create schema for empty tenant name");
    }

    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<void>::err("Failed to connect to database: " + error);
    }

    // Create tenant schema if it doesn't exist
    std::string schema_name = "tenant_" + tenant;

    // Escape the schema name to prevent SQL injection
    std::string escaped_schema = schema_name;
    // Replace any special characters that might be problematic
    std::replace(escaped_schema.begin(), escaped_schema.end(), '-', '_');
    std::replace(escaped_schema.begin(), escaped_schema.end(), '.', '_');
    std::replace(escaped_schema.begin(), escaped_schema.end(), ' ', '_');

    // Create schema if it doesn't exist
    std::string create_schema_sql = "CREATE SCHEMA IF NOT EXISTS \"" + escaped_schema + "\";";
    PGresult* schema_res = PQexec(conn, create_schema_sql.c_str());

    if (PQresultStatus(schema_res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(schema_res);
        PQfinish(conn);
        return Result<void>::err("Failed to create tenant schema: " + error);
    }

    PQclear(schema_res);

    // Create the same tables in the tenant schema as in the default schema
    std::string create_files_table = "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".files ("
        "id BIGSERIAL PRIMARY KEY, "
        "uid VARCHAR(64) UNIQUE NOT NULL, "
        "name TEXT NOT NULL, "
        "path TEXT, "
        "parent_uid VARCHAR(64), "
        "type INTEGER NOT NULL, "
        "owner TEXT NOT NULL, "
        "permissions INTEGER NOT NULL DEFAULT 755, "
        "current_version TEXT, "
        "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
        "modified_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
        "access_count INTEGER NOT NULL DEFAULT 0, "
        "last_access TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ");";

    std::string create_idx_uid = "CREATE INDEX IF NOT EXISTS idx_files_uid_" + escaped_schema +
        " ON \"" + escaped_schema + "\".files(uid);";
    std::string create_idx_path = "CREATE INDEX IF NOT EXISTS idx_files_path_" + escaped_schema +
        " ON \"" + escaped_schema + "\".files(path);";
    std::string create_idx_parent_uid = "CREATE INDEX IF NOT EXISTS idx_files_parent_uid_" + escaped_schema +
        " ON \"" + escaped_schema + "\".files(parent_uid);";

    std::string create_versions_table = "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".versions ("
        "id BIGSERIAL PRIMARY KEY, "
        "file_uid VARCHAR(64) NOT NULL, "
        "version_timestamp TEXT NOT NULL, "
        "size BIGINT NOT NULL, "
        "storage_path TEXT NOT NULL, "
        "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
        "FOREIGN KEY (file_uid) REFERENCES \"" + escaped_schema + "\".files(uid) ON DELETE CASCADE, "
        "UNIQUE(file_uid, version_timestamp)"
        ");";

    std::string create_idx_versions = "CREATE INDEX IF NOT EXISTS idx_versions_file_uid_" + escaped_schema +
        " ON \"" + escaped_schema + "\".versions(file_uid);";

    std::string create_metadata_table = "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".metadata ("
        "id BIGSERIAL PRIMARY KEY, "
        "file_uid VARCHAR(64) NOT NULL, "
        "version_timestamp TEXT NOT NULL, "
        "key TEXT NOT NULL, "
        "value TEXT NOT NULL, "
        "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
        "FOREIGN KEY (file_uid) REFERENCES \"" + escaped_schema + "\".files(uid) ON DELETE CASCADE, "
        "UNIQUE(file_uid, version_timestamp, key)"
        ");";

    std::string create_idx_metadata = "CREATE INDEX IF NOT EXISTS idx_metadata_file_version_" + escaped_schema +
        " ON \"" + escaped_schema + "\".metadata(file_uid, version_timestamp);";
    std::string create_idx_metadata_key = "CREATE INDEX IF NOT EXISTS idx_metadata_key_" + escaped_schema +
        " ON \"" + escaped_schema + "\".metadata(key);";

    std::string create_access_stats_table = "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".file_access_stats ("
        "id BIGSERIAL PRIMARY KEY, "
        "file_uid VARCHAR(64) NOT NULL, "
        "hostname VARCHAR(255) NOT NULL, "
        "last_accessed TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
        "access_count INTEGER NOT NULL DEFAULT 0, "
        "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
        "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
        "FOREIGN KEY (file_uid) REFERENCES \"" + escaped_schema + "\".files(uid) ON DELETE CASCADE, "
        "UNIQUE(file_uid, hostname)"
        ");";

    // Execute all the statements
    PGresult* res = PQexec(conn, create_files_table.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<void>::err("Failed to create tenant files table: " + error);
    }
    PQclear(res);

    res = PQexec(conn, create_idx_uid.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); } // Index creation failure is non-critical

    res = PQexec(conn, create_idx_path.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); } // Index creation failure is non-critical

    res = PQexec(conn, create_idx_parent_uid.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); } // Index creation failure is non-critical

    res = PQexec(conn, create_versions_table.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<void>::err("Failed to create tenant versions table: " + error);
    }
    PQclear(res);

    res = PQexec(conn, create_idx_versions.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); } // Index creation failure is non-critical

    res = PQexec(conn, create_metadata_table.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<void>::err("Failed to create tenant metadata table: " + error);
    }
    PQclear(res);

    res = PQexec(conn, create_idx_metadata.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); } // Index creation failure is non-critical

    res = PQexec(conn, create_idx_metadata_key.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); } // Index creation failure is non-critical

    res = PQexec(conn, create_access_stats_table.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<void>::err("Failed to create tenant access stats table: " + error);
    }
    PQclear(res);

    // Create indexes for tenant-specific access stats table
    std::string create_access_idx_hostname = "CREATE INDEX IF NOT EXISTS idx_file_access_stats_hostname_" + escaped_schema +
        " ON \"" + escaped_schema + "\".file_access_stats(hostname);";
    std::string create_access_idx_hostname_access_count = "CREATE INDEX IF NOT EXISTS idx_file_access_stats_hostname_access_count_" + escaped_schema +
        " ON \"" + escaped_schema + "\".file_access_stats(hostname, access_count);";
    std::string create_access_idx_hostname_last_accessed = "CREATE INDEX IF NOT EXISTS idx_file_access_stats_hostname_last_accessed_" + escaped_schema +
        " ON \"" + escaped_schema + "\".file_access_stats(hostname, last_accessed);";

    res = PQexec(conn, create_access_idx_hostname.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); } // Index creation failure is non-critical

    res = PQexec(conn, create_access_idx_hostname_access_count.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); } // Index creation failure is non-critical

    res = PQexec(conn, create_access_idx_hostname_last_accessed.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); } // Index creation failure is non-critical

    // After creating all the main tables and indexes, now create ACL-related tables in the tenant schema
    std::string create_permissions_table = "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".permissions ("
        "id SERIAL PRIMARY KEY, "
        "name VARCHAR(255) UNIQUE NOT NULL, "
        "description TEXT, "
        "category VARCHAR(100), "
        "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, "
        "updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ");";

    std::string create_roles_table = "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".roles ("
        "id SERIAL PRIMARY KEY, "
        "name VARCHAR(255) UNIQUE NOT NULL, "
        "description TEXT, "
        "parent_role_id INTEGER REFERENCES \"" + escaped_schema + "\".roles(id), "
        "is_system_role BOOLEAN DEFAULT FALSE, "
        "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, "
        "updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ");";

    std::string create_file_acls_table = "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".file_acls ("
        "id SERIAL PRIMARY KEY, "
        "file_uid VARCHAR(64) NOT NULL REFERENCES \"" + escaped_schema + "\".files(uid) ON DELETE CASCADE, "
        "principal_type VARCHAR(20) NOT NULL CHECK (principal_type IN ('user', 'role')), "
        "principal_id INTEGER NOT NULL, "
        "permission_id INTEGER NOT NULL REFERENCES \"" + escaped_schema + "\".permissions(id) ON DELETE CASCADE, "
        "access_type VARCHAR(10) NOT NULL CHECK (access_type IN ('allow', 'deny')), "
        "scope VARCHAR(20) DEFAULT 'exact' CHECK (scope IN ('exact', 'children', 'descendants')), "
        "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, "
        "created_by INTEGER, "  // Can be NULL - references user who created the ACL entry
        "expires_at TIMESTAMP, "
        "UNIQUE(file_uid, principal_type, principal_id, permission_id, scope)"
        ");";

    // Execute ACL table creation statements
    res = PQexec(conn, create_permissions_table.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<void>::err("Failed to create tenant permissions table: " + error);
    }
    PQclear(res);

    res = PQexec(conn, create_roles_table.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<void>::err("Failed to create tenant roles table: " + error);
    }
    PQclear(res);

    res = PQexec(conn, create_file_acls_table.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<void>::err("Failed to create tenant file_acls table: " + error);
    }
    PQclear(res);

    // Create indexes for ACL tables
    std::string create_acl_idx_file = "CREATE INDEX IF NOT EXISTS idx_file_acls_file_uid_" + escaped_schema +
        " ON \"" + escaped_schema + "\".file_acls(file_uid);";
    std::string create_acl_idx_principal = "CREATE INDEX IF NOT EXISTS idx_file_acls_principal_" + escaped_schema +
        " ON \"" + escaped_schema + "\".file_acls(principal_type, principal_id);";
    std::string create_acl_idx_permission = "CREATE INDEX IF NOT EXISTS idx_file_acls_permission_" + escaped_schema +
        " ON \"" + escaped_schema + "\".file_acls(permission_id);";

    res = PQexec(conn, create_acl_idx_file.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); } // Index creation failure is non-critical

    res = PQexec(conn, create_acl_idx_principal.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); } // Index creation failure is non-critical

    res = PQexec(conn, create_acl_idx_permission.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); } // Index creation failure is non-critical

    // Create the filesystem root directory record with default permissions
    // The root directory is identified by an empty string UID and has a NULL parent_uid
    std::string root_uid = "";  // Root directory UID is empty string
    std::string root_name = "ROOT";  // Name for the root directory
    int directory_type = static_cast<int>(fileengine::FileType::DIRECTORY);
    std::string root_owner = "system";  // Owned by system
    int root_permissions = 755;  // Default permissions

    // First, check if the root directory already exists
    std::string check_root_sql = "SELECT COUNT(*) FROM \"" + escaped_schema + "\".files WHERE uid = ''";
    res = PQexec(conn, check_root_sql.c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<void>::err("Failed to check for existing root directory: " + error);
    }

    int root_exists = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);

    if (root_exists == 0) {
        // Root doesn't exist, so create it
        std::string insert_root_sql = "INSERT INTO \"" + escaped_schema + "\".files "
            "(uid, path, name, parent_uid, type, owner, permissions) VALUES ("
            "'" + escape_string(root_uid, conn) + "', "
            "'" + escape_string("/", conn) + "', "
            "'" + escape_string(root_name, conn) + "', "
            "NULL, "  // parent_uid is NULL for the root directory
            + std::to_string(directory_type) + ", "
            "'" + escape_string(root_owner, conn) + "', "
            + std::to_string(root_permissions) + ")";

        res = PQexec(conn, insert_root_sql.c_str());
        if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK) {
            std::string error = PQerrorMessage(conn);
            PQclear(res);
            PQfinish(conn);
            return Result<void>::err("Failed to create root directory: " + error);
        }
        PQclear(res);
    }

    // Now add ACL entries to allow full access for administrator role to the root directory
    // First, check if the admin role exists, and if not, create it
    std::string check_admin_role_sql = "SELECT id FROM \"" + escaped_schema + "\".roles WHERE name = 'admin'";
    res = PQexec(conn, check_admin_role_sql.c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<void>::err("Failed to check for admin role: " + error);
    }

    int admin_role_id = 0;
    if (PQntuples(res) > 0) {
        admin_role_id = atoi(PQgetvalue(res, 0, 0));
    }
    PQclear(res);

    // If admin role doesn't exist, create it
    if (admin_role_id == 0) {
        std::string insert_admin_role_sql = "INSERT INTO \"" + escaped_schema + "\".roles "
            "(name, description, is_system_role) VALUES ("
            "'admin', 'Administrator with full access', true) "
            "RETURNING id";
        res = PQexec(conn, insert_admin_role_sql.c_str());
        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            std::string error = PQerrorMessage(conn);
            PQclear(res);
            PQfinish(conn);
            return Result<void>::err("Failed to create admin role: " + error);
        }
        admin_role_id = atoi(PQgetvalue(res, 0, 0));
        PQclear(res);
    }

    // Insert necessary permissions if they don't exist and get their IDs
    struct PermissionInfo {
        std::string name;
        std::string description;
        std::string category;
    };

    std::vector<PermissionInfo> permissions_to_create = {
        {"file.read", "Read file contents", "file"},
        {"file.write", "Write/modify file contents", "file"},
        {"file.delete", "Delete file", "file"},
        {"directory.create", "Create items in directory", "directory"},
        {"directory.delete", "Delete directory", "directory"},
        {"directory.list", "List directory contents", "directory"}
    };

    // Create permissions if they don't exist
    for (const auto& perm : permissions_to_create) {
        std::string insert_perm_sql = "INSERT INTO \"" + escaped_schema + "\".permissions "
            "(name, description, category) VALUES ("
            "'" + escape_string(perm.name, conn) + "', "
            "'" + escape_string(perm.description, conn) + "', "
            "'" + escape_string(perm.category, conn) + "') "
            "ON CONFLICT (name) DO NOTHING;";
        res = PQexec(conn, insert_perm_sql.c_str());
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            std::string error = PQerrorMessage(conn);
            PQclear(res);
            PQfinish(conn);
            return Result<void>::err("Failed to create permission: " + error);
        }
        PQclear(res);
    }

    // Now add ACL entries for the admin role with full access to the root directory
    for (const auto& perm : permissions_to_create) {
        // Get the permission ID
        std::string get_perm_id_sql = "SELECT id FROM \"" + escaped_schema + "\".permissions "
            "WHERE name = '" + escape_string(perm.name, conn) + "'";
        res = PQexec(conn, get_perm_id_sql.c_str());
        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            std::string error = PQerrorMessage(conn);
            PQclear(res);
            continue; // Try to continue with other permissions
        }

        if (PQntuples(res) > 0) {
            int perm_id = atoi(PQgetvalue(res, 0, 0));
            PQclear(res);

            // Insert ACL entry for admin role with this permission on root directory
            std::string insert_acl_sql = "INSERT INTO \"" + escaped_schema + "\".file_acls "
                "(file_uid, principal_type, principal_id, permission_id, access_type, scope) VALUES ("
                "'" + escape_string(root_uid, conn) + "', "  // file_uid (root directory)
                "'role', "  // principal_type
                + std::to_string(admin_role_id) + ", "  // principal_id (admin role id)
                + std::to_string(perm_id) + ", "  // permission_id
                "'allow', "  // access_type
                "'exact') "  // scope (apply only to exact resource, not descendants)
                "ON CONFLICT DO NOTHING;";

            res = PQexec(conn, insert_acl_sql.c_str());
            if (PQresultStatus(res) != PGRES_COMMAND_OK) {
                std::string error = PQerrorMessage(conn);
                PQclear(res);
                // Continue even if ACL insertion fails - this is not critical for basic functionality
                std::cout << "Warning: Failed to create ACL entry for permission " << perm.name
                         << ": " << error << std::endl;
            }
            PQclear(res);
        } else {
            PQclear(res);
        }
    }

    PQfinish(conn);

    return Result<void>::ok();
}

Result<bool> Database::tenant_schema_exists(const std::string& tenant) {
    if (tenant.empty()) {
        return Result<bool>::ok(false);
    }

    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<bool>::err("Failed to connect to database: " + error);
    }

    // Construct schema name and escape it
    std::string schema_name = "tenant_" + tenant;
    std::string escaped_schema = schema_name;
    std::replace(escaped_schema.begin(), escaped_schema.end(), '-', '_');
    std::replace(escaped_schema.begin(), escaped_schema.end(), '.', '_');
    std::replace(escaped_schema.begin(), escaped_schema.end(), ' ', '_');

    std::string check_sql = "SELECT 1 FROM information_schema.schemata WHERE schema_name = '" + escaped_schema + "';";

    PGresult* res = PQexec(conn, check_sql.c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<bool>::err("Failed to check schema existence: " + error);
    }

    bool exists = (PQntuples(res) > 0);

    PQclear(res);
    PQfinish(conn);

    return Result<bool>::ok(exists);
}

Result<void> Database::cleanup_tenant_data(const std::string& tenant) {
    if (tenant.empty()) {
        return Result<void>::err("Cannot cleanup data for empty tenant name");
    }

    PGconn* conn = PQconnectdb(connection_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        PQfinish(conn);
        return Result<void>::err("Failed to connect to database: " + error);
    }

    // Construct schema name and escape it
    std::string schema_name = "tenant_" + tenant;
    std::string escaped_schema = schema_name;
    std::replace(escaped_schema.begin(), escaped_schema.end(), '-', '_');
    std::replace(escaped_schema.begin(), escaped_schema.end(), '.', '_');
    std::replace(escaped_schema.begin(), escaped_schema.end(), ' ', '_');

    // Drop the tenant schema and all its contents
    std::string drop_sql = "DROP SCHEMA IF EXISTS \"" + escaped_schema + "\" CASCADE;";

    PGresult* res = PQexec(conn, drop_sql.c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(conn);
        PQclear(res);
        PQfinish(conn);
        return Result<void>::err("Failed to cleanup tenant data: " + error);
    }

    PQclear(res);
    PQfinish(conn);

    return Result<void>::ok();
}

} // namespace fileengine