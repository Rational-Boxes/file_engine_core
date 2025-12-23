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
        CREATE TABLE IF NOT EXISTS file_access_stats (
            id BIGSERIAL PRIMARY KEY,
            file_uid VARCHAR(64) NOT NULL,
            hostname VARCHAR(255) NOT NULL,
            last_accessed TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            access_count INTEGER NOT NULL DEFAULT 0,
            created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(file_uid, hostname)
        );

        CREATE INDEX IF NOT EXISTS idx_file_access_stats_file_uid ON file_access_stats(file_uid);
        CREATE INDEX IF NOT EXISTS idx_file_access_stats_hostname ON file_access_stats(hostname);
        CREATE INDEX IF NOT EXISTS idx_file_access_stats_last_accessed ON file_access_stats(last_accessed);
        CREATE INDEX IF NOT EXISTS idx_file_access_stats_access_count ON file_access_stats(access_count);

        -- Global tenants registry table
        CREATE TABLE IF NOT EXISTS tenants (
            id BIGSERIAL PRIMARY KEY,
            tenant_id VARCHAR(255) UNIQUE NOT NULL,
            schema_name VARCHAR(255) NOT NULL,
            created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
        );
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
    std::string schema_name = tenant; // According to specifications, no "tenant_" prefix

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

    // Create the files table with structure specified in documentation
    std::string create_files_table = "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".files ("
        "id BIGSERIAL PRIMARY KEY, "
        "uid VARCHAR(64) UNIQUE NOT NULL, "
        "name TEXT NOT NULL, "
        "parent_uid VARCHAR(64), "
        "size BIGINT, -- size in bytes, for files "
        "owner TEXT NOT NULL, "
        "permission_map INTEGER NOT NULL, -- permission bit map"
        "is_container BOOLEAN NOT NULL, -- folder flag"
        "deleted BOOLEAN NOT NULL DEFAULT FALSE"
        ");";

    std::string create_idx_uid = "CREATE INDEX IF NOT EXISTS idx_files_uid_" + escaped_schema +
        " ON \"" + escaped_schema + "\".files(uid);";
    std::string create_idx_parent_uid = "CREATE INDEX IF NOT EXISTS idx_files_parent_uid_" + escaped_schema +
        " ON \"" + escaped_schema + "\".files(parent_uid);";

    std::string create_versions_table = "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".versions ("
        "id BIGSERIAL PRIMARY KEY, "
        "file_uuid VARCHAR(64) NOT NULL, -- file_uuid (as per spec) "
        "timestamp BIGINT NOT NULL, -- timestamp (as per spec) "
        "size BIGINT NOT NULL, -- size (as per spec) "
        "user_who_saved TEXT NOT NULL -- user who saved this version (as per spec) "
        ");";

    std::string create_idx_versions = "CREATE INDEX IF NOT EXISTS idx_versions_file_uuid_" + escaped_schema +
        " ON \"" + escaped_schema + "\".versions(file_uuid);";

    std::string create_metadata_table = "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".metadata ("
        "id BIGSERIAL PRIMARY KEY, "
        "file_uuid VARCHAR(64) NOT NULL, -- file_uuid (as per spec) "
        "timestamp BIGINT NOT NULL, -- timestamp (as per spec) "
        "key_name TEXT NOT NULL, -- key name (as per spec) "
        "value TEXT NOT NULL, -- value (as per spec) "
        "metadata_creation_date TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, -- metadata creation date (as per spec) "
        "user_identity TEXT NOT NULL -- user identity (as per spec) "
        ");";

    std::string create_idx_metadata = "CREATE INDEX IF NOT EXISTS idx_metadata_file_uuid_" + escaped_schema +
        " ON \"" + escaped_schema + "\".metadata(file_uuid);";
    std::string create_idx_metadata_key = "CREATE INDEX IF NOT EXISTS idx_metadata_key_name_" + escaped_schema +
        " ON \"" + escaped_schema + "\".metadata(key_name);";

    // Note: file_access_stats is a global table, not tenant-specific as per spec
    // It was created in the public schema during initialization

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

    // Create the filesystem root directory record with default permissions
    // The root directory is identified by blank UUID string (empty string) as per specification
    std::string root_uid = "";  // Root directory UID is blank (empty string) as per spec
    std::string root_name = "root";  // Name for the root directory as per spec
    std::string root_parent_uid = "";  // Root's parent is also empty string (self-referencing concept)
    size_t root_size = 0;  // Size is 0 for root directory
    std::string root_owner = "system";  // Owned by system as per spec
    int root_permission_map = 755;  // permission bit map as per spec
    bool root_is_container = true;  // folder flag as per spec
    bool root_deleted = false;  // as per spec

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
        // Root doesn't exist, so create it as per specification
        std::string insert_root_sql = "INSERT INTO \"" + escaped_schema + "\".files "
            "(uid, name, parent_uid, size, owner, permission_map, is_container, deleted) VALUES ("
            "'" + escape_string(root_uid, conn) + "', "
            "'" + escape_string(root_name, conn) + "', "
            "'" + escape_string(root_parent_uid, conn) + "', "
            + std::to_string(root_size) + ", "
            "'" + escape_string(root_owner, conn) + "', "
            + std::to_string(root_permission_map) + ", "
            (root_is_container ? "TRUE" : "FALSE") + ", "
            (root_deleted ? "TRUE" : "FALSE") + ")";

        res = PQexec(conn, insert_root_sql.c_str());
        if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK) {
            std::string error = PQerrorMessage(conn);
            PQclear(res);
            PQfinish(conn);
            return Result<void>::err("Failed to create root directory: " + error);
        }
        PQclear(res);
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