#include "fileengine/database.h"
#include "fileengine/utils.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <memory>

namespace fileengine {

// Include the Database class implementation methods in full
Database::Database(const std::string& host, int port, const std::string& dbname,
                   const std::string& user, const std::string& password, int pool_size)
    : connection_pool_(std::make_shared<ConnectionPool>(host, port, dbname, user, password, pool_size)),
      hostname_(host),
      retry_interval_seconds_(30) {  // Set default retry interval
}

Database::~Database() {
    stop_connection_monitoring();
    disconnect();
}

bool Database::connect() {
    return connection_pool_ && connection_pool_->initialize();
}

void Database::disconnect() {
    if (connection_pool_) {
        connection_pool_->shutdown();
    }
}

bool Database::is_connected() const {
    // Check if connection pool is initialized and a connection can be acquired
    if (!connection_pool_) return false;
    
    auto conn = connection_pool_->acquire();
    if (conn && conn->is_valid()) {
        connection_pool_->release(conn);
        return true;
    }
    return false;
}

Result<void> Database::create_schema() {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection for schema creation");
    }

    PGconn* pg_conn = conn->get_connection();

    // SQL for creating the files table
    const char* files_sql = R"SQL(
        CREATE TABLE IF NOT EXISTS files (
            uid VARCHAR(64) PRIMARY KEY,
            name TEXT NOT NULL,
            path TEXT,                           -- Path for backward compatibility or path-based operations
            parent_uid VARCHAR(64),              -- Parent UUID to support directory structure
            type INTEGER NOT NULL,
            size BIGINT NOT NULL DEFAULT 0,
            owner TEXT NOT NULL,
            permissions INTEGER NOT NULL DEFAULT 755,
            current_version TEXT,                -- Current version as timestamp string
            created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            modified_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            is_deleted BOOLEAN NOT NULL DEFAULT FALSE  -- Flag indicating if file is deleted (soft delete)
        );

        CREATE INDEX IF NOT EXISTS idx_files_parent_uid ON files(parent_uid);
        CREATE INDEX IF NOT EXISTS idx_files_uid ON files(uid);
        CREATE INDEX IF NOT EXISTS idx_files_is_deleted ON files(is_deleted);
    )SQL";

    // SQL for creating the versions table
    const char* versions_sql = R"SQL(
        CREATE TABLE IF NOT EXISTS versions (
            file_uid VARCHAR(64) NOT NULL,
            version_timestamp VARCHAR(30) NOT NULL,
            size BIGINT NOT NULL,
            storage_path TEXT NOT NULL,
            created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            PRIMARY KEY (file_uid, version_timestamp)
        );

        CREATE INDEX IF NOT EXISTS idx_versions_file_uid ON versions(file_uid);
        CREATE INDEX IF NOT EXISTS idx_versions_timestamp ON versions(version_timestamp);
    )SQL";

    // SQL for creating the ACL table
    const char* acl_sql = R"SQL(
        CREATE TABLE IF NOT EXISTS acl (
            resource_uid VARCHAR(64) NOT NULL,
            principal TEXT NOT NULL,
            principal_type INTEGER NOT NULL,  -- 0=user, 1=group, 2=other
            permissions INTEGER NOT NULL,   -- bit mask of permissions
            tenant TEXT DEFAULT 'default',
            created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            PRIMARY KEY (resource_uid, principal, principal_type, tenant)
        );

        CREATE INDEX IF NOT EXISTS idx_acl_resource_uid ON acl(resource_uid);
        CREATE INDEX IF NOT EXISTS idx_acl_tenant ON acl(tenant);
    )SQL";

    // SQL for creating the metadata table
    const char* metadata_sql = R"SQL(
        CREATE TABLE IF NOT EXISTS metadata (
            file_uid VARCHAR(64) NOT NULL,
            version_timestamp VARCHAR(30) NOT NULL,
            key_name TEXT NOT NULL,
            value TEXT,
            created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            PRIMARY KEY (file_uid, version_timestamp, key_name)
        );

        CREATE INDEX IF NOT EXISTS idx_metadata_file_uid ON metadata(file_uid);
    )SQL";

    // Execute all SQL statements
    Result<void> result = Result<void>::ok();

    PGresult* res1 = PQexec(pg_conn, files_sql);
    if (PQresultStatus(res1) != PGRES_COMMAND_OK) {
        result = Result<void>::err("Failed to create files table: " + std::string(PQerrorMessage(pg_conn)));
    }
    PQclear(res1);

    if (result.success) {
        PGresult* res2 = PQexec(pg_conn, versions_sql);
        if (PQresultStatus(res2) != PGRES_COMMAND_OK) {
            result = Result<void>::err("Failed to create versions table: " + std::string(PQerrorMessage(pg_conn)));
        }
        PQclear(res2);
    }

    if (result.success) {
        PGresult* res3 = PQexec(pg_conn, acl_sql);
        if (PQresultStatus(res3) != PGRES_COMMAND_OK) {
            result = Result<void>::err("Failed to create ACL table: " + std::string(PQerrorMessage(pg_conn)));
        }
        PQclear(res3);
    }

    if (result.success) {
        PGresult* res4 = PQexec(pg_conn, metadata_sql);
        if (PQresultStatus(res4) != PGRES_COMMAND_OK) {
            result = Result<void>::err("Failed to create metadata table: " + std::string(PQerrorMessage(pg_conn)));
        }
        PQclear(res4);
    }

    // Release the connection back to the pool
    connection_pool_->release(conn);

    return result;
}

Result<void> Database::drop_schema() {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Drop tables in reverse dependency order
    const char* drop_acl_sql = "DROP TABLE IF EXISTS acl;";
    const char* drop_metadata_sql = "DROP TABLE IF EXISTS metadata;";
    const char* drop_versions_sql = "DROP TABLE IF EXISTS versions;";
    const char* drop_files_sql = "DROP TABLE IF EXISTS files;";

    Result<void> result = Result<void>::ok();

    // Execute drops in order
    PGresult* res1 = PQexec(pg_conn, drop_acl_sql);
    if (PQresultStatus(res1) != PGRES_COMMAND_OK && PQresultStatus(res1) != PGRES_BAD_RESPONSE) {
        result = Result<void>::err("Error dropping ACL table: " + std::string(PQerrorMessage(pg_conn)));
    }
    PQclear(res1);

    if (result.success) {
        PGresult* res2 = PQexec(pg_conn, drop_metadata_sql);
        if (PQresultStatus(res2) != PGRES_COMMAND_OK && PQresultStatus(res2) != PGRES_BAD_RESPONSE) {
            result = Result<void>::err("Error dropping metadata table: " + std::string(PQerrorMessage(pg_conn)));
        }
        PQclear(res2);
    }

    if (result.success) {
        PGresult* res3 = PQexec(pg_conn, drop_versions_sql);
        if (PQresultStatus(res3) != PGRES_COMMAND_OK && PQresultStatus(res3) != PGRES_BAD_RESPONSE) {
            result = Result<void>::err("Error dropping versions table: " + std::string(PQerrorMessage(pg_conn)));
        }
        PQclear(res3);
    }

    if (result.success) {
        PGresult* res4 = PQexec(pg_conn, drop_files_sql);
        if (PQresultStatus(res4) != PGRES_COMMAND_OK && PQresultStatus(res4) != PGRES_BAD_RESPONSE) {
            result = Result<void>::err("Error dropping files table: " + std::string(PQerrorMessage(pg_conn)));
        }
        PQclear(res4);
    }

    connection_pool_->release(conn);
    return result;
}

Result<std::string> Database::insert_file(const std::string& uid, const std::string& name,
                                          const std::string& path, const std::string& parent_uid,
                                          FileType type, const std::string& owner,
                                          int permissions, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::string>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Prepare SQL with INSERT/ON CONFLICT handling to avoid duplicates
    const char* insert_sql = R"SQL(
        INSERT INTO files (uid, name, path, parent_uid, type, size, owner, permissions, current_version, created_at, modified_at, is_deleted)
        VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, FALSE)
        ON CONFLICT (uid) DO NOTHING
        RETURNING uid;
    )SQL";

    // Convert file type to integer
    int type_int = static_cast<int>(type);
    int64_t size = 0; // New files start with 0 size
    std::string version_timestamp = Utils::get_timestamp_string(); // Current timestamp as version

    const char* param_values[9] = {
        uid.c_str(),          // $1
        name.c_str(),         // $2
        path.c_str(),         // $3
        parent_uid.c_str(),   // $4
        std::to_string(type_int).c_str(),  // $5
        std::to_string(size).c_str(),      // $6
        owner.c_str(),        // $7
        std::to_string(permissions).c_str(), // $8
        version_timestamp.c_str()            // $9
    };

    PGresult* res = PQexecParams(pg_conn, insert_sql, 9, nullptr, param_values, nullptr, nullptr, 0);

    std::string result_uid;
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        if (PQntuples(res) > 0) {
            result_uid = PQgetvalue(res, 0, 0);
        }
        if (result_uid.empty()) {
            // The insert was ignored due to conflict - return error for duplicate prevention
            PQclear(res);
            connection_pool_->release(conn);
            return Result<std::string>::err("File/directory with this UID already exists");
        }
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::string>::ok(result_uid);
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::string>::err("Failed to insert file: " + error);
    }
}

Result<void> Database::update_file_modified(const std::string& uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* update_sql = "UPDATE files SET modified_at = CURRENT_TIMESTAMP WHERE uid = $1;";
    const char* param_values[1] = {uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, update_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::ok();
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to update file modified time: " + error);
    }
}

Result<void> Database::update_file_current_version(const std::string& uid, const std::string& version_timestamp, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* update_sql = "UPDATE files SET current_version = $2, modified_at = CURRENT_TIMESTAMP WHERE uid = $1;";
    const char* param_values[2] = {uid.c_str(), version_timestamp.c_str()};

    PGresult* res = PQexecParams(pg_conn, update_sql, 2, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::ok();
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to update file current version: " + error);
    }
}

Result<bool> Database::delete_file(const std::string& uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<bool>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Soft delete - update the is_deleted flag
    const char* delete_sql = "UPDATE files SET is_deleted = TRUE, modified_at = CURRENT_TIMESTAMP WHERE uid = $1;";
    const char* param_values[1] = {uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, delete_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
        int rows_affected = std::stoi(PQcmdTuples(res));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<bool>::ok(rows_affected > 0);
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<bool>::err("Failed to delete file: " + error);
    }
}

Result<bool> Database::undelete_file(const std::string& uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<bool>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* undelete_sql = "UPDATE files SET is_deleted = FALSE, modified_at = CURRENT_TIMESTAMP WHERE uid = $1;";
    const char* param_values[1] = {uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, undelete_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
        int rows_affected = std::stoi(PQcmdTuples(res));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<bool>::ok(rows_affected > 0);
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<bool>::err("Failed to undelete file: " + error);
    }
}

Result<std::optional<FileInfo>> Database::get_file_by_uid(const std::string& uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::optional<FileInfo>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = R"SQL(
        SELECT name, path, parent_uid, type, size, owner, permissions, EXTRACT(EPOCH FROM created_at)::BIGINT, EXTRACT(EPOCH FROM modified_at)::BIGINT, current_version
        FROM files
        WHERE uid = $1 AND is_deleted = FALSE
        LIMIT 1;
    )SQL";
    const char* param_values[1] = {uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        if (PQntuples(res) > 0) {
            // Get the file info from the database row
            std::string name = PQgetvalue(res, 0, 0);
            std::string path = PQgetvalue(res, 0, 1);
            std::string parent_uid = PQgetvalue(res, 0, 2);
            FileType type = static_cast<FileType>(std::stoi(PQgetvalue(res, 0, 3)));
            int64_t size = std::stoll(PQgetvalue(res, 0, 4));
            std::string owner = PQgetvalue(res, 0, 5);
            int permissions = std::stoi(PQgetvalue(res, 0, 6));
            int64_t created_at = std::stoll(PQgetvalue(res, 0, 7));
            int64_t modified_at = std::stoll(PQgetvalue(res, 0, 8));
            std::string version = PQgetvalue(res, 0, 9);

            FileInfo info;
            info.uid = uid;
            info.name = name;
            info.path = path;
            info.parent_uid = parent_uid;
            info.type = type;
            info.size = size;
            info.owner = owner;
            info.permissions = permissions;
            info.created_at = std::chrono::system_clock::time_point(std::chrono::seconds(created_at));
            info.modified_at = std::chrono::system_clock::time_point(std::chrono::seconds(modified_at));
            info.version = version;
            info.version_count = 1; // For this implementation, use 1

            PQclear(res);
            connection_pool_->release(conn);
            return Result<std::optional<FileInfo>>::ok(info);
        } else {
            // File not found
            PQclear(res);
            connection_pool_->release(conn);
            return Result<std::optional<FileInfo>>::ok(std::nullopt);
        }
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::err("Failed to get file by UID: " + error);
    }
}

Result<std::optional<FileInfo>> Database::get_file_by_path(const std::string& path, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::optional<FileInfo>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = R"SQL(
        SELECT uid, name, parent_uid, type, size, owner, permissions, EXTRACT(EPOCH FROM created_at)::BIGINT, EXTRACT(EPOCH FROM modified_at)::BIGINT, current_version
        FROM files
        WHERE path = $1 AND is_deleted = FALSE
        LIMIT 1;
    )SQL";
    const char* param_values[1] = {path.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        if (PQntuples(res) > 0) {
            std::string uid = PQgetvalue(res, 0, 0);
            std::string name = PQgetvalue(res, 0, 1);
            std::string parent_uid = PQgetvalue(res, 0, 2);
            FileType type = static_cast<FileType>(std::stoi(PQgetvalue(res, 0, 3)));
            int64_t size = std::stoll(PQgetvalue(res, 0, 4));
            std::string owner = PQgetvalue(res, 0, 5);
            int permissions = std::stoi(PQgetvalue(res, 0, 6));
            int64_t created_at = std::stoll(PQgetvalue(res, 0, 7));
            int64_t modified_at = std::stoll(PQgetvalue(res, 0, 8));
            std::string version = PQgetvalue(res, 0, 9);

            FileInfo info;
            info.uid = uid;
            info.name = name;
            info.path = path;
            info.parent_uid = parent_uid;
            info.type = type;
            info.size = size;
            info.owner = owner;
            info.permissions = permissions;
            info.created_at = std::chrono::system_clock::time_point(std::chrono::seconds(created_at));
            info.modified_at = std::chrono::system_clock::time_point(std::chrono::seconds(modified_at));
            info.version = version;
            info.version_count = 1; // For this implementation, use 1

            PQclear(res);
            connection_pool_->release(conn);
            return Result<std::optional<FileInfo>>::ok(info);
        } else {
            // File not found
            PQclear(res);
            connection_pool_->release(conn);
            return Result<std::optional<FileInfo>>::ok(std::nullopt);
        }
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::err("Failed to get file by path: " + error);
    }
}

Result<void> Database::update_file_name(const std::string& uid, const std::string& new_name, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* update_sql = "UPDATE files SET name = $2, modified_at = CURRENT_TIMESTAMP WHERE uid = $1;";
    const char* param_values[2] = {uid.c_str(), new_name.c_str()};

    PGresult* res = PQexecParams(pg_conn, update_sql, 2, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::ok();
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to update file name: " + error);
    }
}

Result<std::vector<FileInfo>> Database::list_files_in_directory(const std::string& parent_uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<FileInfo>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = R"SQL(
        SELECT uid, name, path, type, size, owner, permissions, EXTRACT(EPOCH FROM created_at)::BIGINT, EXTRACT(EPOCH FROM modified_at)::BIGINT, current_version
        FROM files
        WHERE parent_uid = $1 AND is_deleted = FALSE
        ORDER BY name;
    )SQL";
    const char* param_values[1] = {parent_uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    std::vector<FileInfo> result_files;
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int nrows = PQntuples(res);
        for (int i = 0; i < nrows; ++i) {
            FileInfo info;
            info.uid = PQgetvalue(res, i, 0);
            info.name = PQgetvalue(res, i, 1);
            info.path = PQgetvalue(res, i, 2);
            info.parent_uid = parent_uid;
            info.type = static_cast<FileType>(std::stoi(PQgetvalue(res, i, 3)));
            info.size = std::stoll(PQgetvalue(res, i, 4));
            info.owner = PQgetvalue(res, i, 5);
            info.permissions = std::stoi(PQgetvalue(res, i, 6));
            int64_t created_at = std::stoll(PQgetvalue(res, i, 7));
            int64_t modified_at = std::stoll(PQgetvalue(res, i, 8));
            info.created_at = std::chrono::system_clock::time_point(std::chrono::seconds(created_at));
            info.modified_at = std::chrono::system_clock::time_point(std::chrono::seconds(modified_at));
            info.version = PQgetvalue(res, i, 9);
            info.version_count = 1; // For this implementation, use 1

            result_files.push_back(info);
        }
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<FileInfo>>::ok(result_files);
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<FileInfo>>::err("Failed to list files in directory: " + error);
    }
}

Result<std::vector<FileInfo>> Database::list_files_in_directory_with_deleted(const std::string& parent_uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<FileInfo>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = R"SQL(
        SELECT uid, name, path, type, size, owner, permissions, EXTRACT(EPOCH FROM created_at)::BIGINT, EXTRACT(EPOCH FROM modified_at)::BIGINT, current_version
        FROM files
        WHERE parent_uid = $1
        ORDER BY name;
    )SQL";
    const char* param_values[1] = {parent_uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    std::vector<FileInfo> result_files;
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int nrows = PQntuples(res);
        for (int i = 0; i < nrows; ++i) {
            FileInfo info;
            info.uid = PQgetvalue(res, i, 0);
            info.name = PQgetvalue(res, i, 1);
            info.path = PQgetvalue(res, i, 2);
            info.parent_uid = parent_uid;
            info.type = static_cast<FileType>(std::stoi(PQgetvalue(res, i, 3)));
            info.size = std::stoll(PQgetvalue(res, i, 4));
            info.owner = PQgetvalue(res, i, 5);
            info.permissions = std::stoi(PQgetvalue(res, i, 6));
            int64_t created_at = std::stoll(PQgetvalue(res, i, 7));
            int64_t modified_at = std::stoll(PQgetvalue(res, i, 8));
            info.created_at = std::chrono::system_clock::time_point(std::chrono::seconds(created_at));
            info.modified_at = std::chrono::system_clock::time_point(std::chrono::seconds(modified_at));
            info.version = PQgetvalue(res, i, 9);
            info.version_count = 1; // For this implementation, use 1

            result_files.push_back(info);
        }
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<FileInfo>>::ok(result_files);
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<FileInfo>>::err("Failed to list files in directory (with deleted): " + error);
    }
}

Result<std::optional<FileInfo>> Database::get_file_by_name_and_parent(const std::string& name, const std::string& parent_uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::optional<FileInfo>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = R"SQL(
        SELECT uid, path, type, size, owner, permissions, EXTRACT(EPOCH FROM created_at)::BIGINT, EXTRACT(EPOCH FROM modified_at)::BIGINT, current_version
        FROM files
        WHERE name = $1 AND parent_uid = $2 AND is_deleted = FALSE
        LIMIT 1;
    )SQL";
    const char* param_values[2] = {name.c_str(), parent_uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 2, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        if (PQntuples(res) > 0) {
            std::string uid = PQgetvalue(res, 0, 0);
            std::string path = PQgetvalue(res, 0, 1);
            FileType type = static_cast<FileType>(std::stoi(PQgetvalue(res, 0, 2)));
            int64_t size = std::stoll(PQgetvalue(res, 0, 3));
            std::string owner = PQgetvalue(res, 0, 4);
            int permissions = std::stoi(PQgetvalue(res, 0, 5));
            int64_t created_at = std::stoll(PQgetvalue(res, 0, 6));
            int64_t modified_at = std::stoll(PQgetvalue(res, 0, 7));
            std::string version = PQgetvalue(res, 0, 8);

            FileInfo info;
            info.uid = uid;
            info.name = name;
            info.path = path;
            info.parent_uid = parent_uid;
            info.type = type;
            info.size = size;
            info.owner = owner;
            info.permissions = permissions;
            info.created_at = std::chrono::system_clock::time_point(std::chrono::seconds(created_at));
            info.modified_at = std::chrono::system_clock::time_point(std::chrono::seconds(modified_at));
            info.version = version;
            info.version_count = 1; // For this implementation, use 1

            PQclear(res);
            connection_pool_->release(conn);
            return Result<std::optional<FileInfo>>::ok(info);
        } else {
            // File not found
            PQclear(res);
            connection_pool_->release(conn);
            return Result<std::optional<FileInfo>>::ok(std::nullopt);
        }
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::err("Failed to get file by name and parent: " + error);
    }
}

Result<std::optional<FileInfo>> Database::get_file_by_name_and_parent_include_deleted(const std::string& name, const std::string& parent_uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::optional<FileInfo>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = R"SQL(
        SELECT uid, path, type, size, owner, permissions, EXTRACT(EPOCH FROM created_at)::BIGINT, EXTRACT(EPOCH FROM modified_at)::BIGINT, current_version
        FROM files
        WHERE name = $1 AND parent_uid = $2
        LIMIT 1;
    )SQL";
    const char* param_values[2] = {name.c_str(), parent_uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 2, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        if (PQntuples(res) > 0) {
            std::string uid = PQgetvalue(res, 0, 0);
            std::string path = PQgetvalue(res, 0, 1);
            FileType type = static_cast<FileType>(std::stoi(PQgetvalue(res, 0, 2)));
            int64_t size = std::stoll(PQgetvalue(res, 0, 3));
            std::string owner = PQgetvalue(res, 0, 4);
            int permissions = std::stoi(PQgetvalue(res, 0, 5));
            int64_t created_at = std::stoll(PQgetvalue(res, 0, 6));
            int64_t modified_at = std::stoll(PQgetvalue(res, 0, 7));
            std::string version = PQgetvalue(res, 0, 8);

            FileInfo info;
            info.uid = uid;
            info.name = name;
            info.path = path;
            info.parent_uid = parent_uid;
            info.type = type;
            info.size = size;
            info.owner = owner;
            info.permissions = permissions;
            info.created_at = std::chrono::system_clock::time_point(std::chrono::seconds(created_at));
            info.modified_at = std::chrono::system_clock::time_point(std::chrono::seconds(modified_at));
            info.version = version;
            info.version_count = 1; // For this implementation, use 1

            PQclear(res);
            connection_pool_->release(conn);
            return Result<std::optional<FileInfo>>::ok(info);
        } else {
            // File not found
            PQclear(res);
            connection_pool_->release(conn);
            return Result<std::optional<FileInfo>>::ok(std::nullopt);
        }
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::err("Failed to get file by name and parent (include deleted): " + error);
    }
}

Result<int64_t> Database::get_file_size(const std::string& file_uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<int64_t>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = "SELECT size FROM files WHERE uid = $1 AND is_deleted = FALSE LIMIT 1;";
    const char* param_values[1] = {file_uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        if (PQntuples(res) > 0) {
            int64_t size = std::stoll(PQgetvalue(res, 0, 0));
            PQclear(res);
            connection_pool_->release(conn);
            return Result<int64_t>::ok(size);
        } else {
            // File not found
            PQclear(res);
            connection_pool_->release(conn);
            return Result<int64_t>::err("File not found");
        }
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<int64_t>::err("Failed to get file size: " + error);
    }
}

Result<int64_t> Database::get_directory_size(const std::string& dir_uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<int64_t>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = "SELECT COALESCE(SUM(size), 0) FROM files WHERE parent_uid = $1 AND is_deleted = FALSE;";
    const char* param_values[1] = {dir_uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        if (PQntuples(res) > 0) {
            int64_t total_size = std::stoll(PQgetvalue(res, 0, 0));
            PQclear(res);
            connection_pool_->release(conn);
            return Result<int64_t>::ok(total_size);
        } else {
            // Directory not found or no files in it
            PQclear(res);
            connection_pool_->release(conn);
            return Result<int64_t>::ok(0);
        }
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<int64_t>::err("Failed to get directory size: " + error);
    }
}

Result<std::optional<FileInfo>> Database::get_file_by_uid_include_deleted(const std::string& uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::optional<FileInfo>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = R"SQL(
        SELECT name, path, parent_uid, type, size, owner, permissions, EXTRACT(EPOCH FROM created_at)::BIGINT, EXTRACT(EPOCH FROM modified_at)::BIGINT, current_version
        FROM files
        WHERE uid = $1
        LIMIT 1;
    )SQL";
    const char* param_values[1] = {uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        if (PQntuples(res) > 0) {
            std::string name = PQgetvalue(res, 0, 0);
            std::string path = PQgetvalue(res, 0, 1);
            std::string parent_uid = PQgetvalue(res, 0, 2);
            FileType type = static_cast<FileType>(std::stoi(PQgetvalue(res, 0, 3)));
            int64_t size = std::stoll(PQgetvalue(res, 0, 4));
            std::string owner = PQgetvalue(res, 0, 5);
            int permissions = std::stoi(PQgetvalue(res, 0, 6));
            int64_t created_at = std::stoll(PQgetvalue(res, 0, 7));
            int64_t modified_at = std::stoll(PQgetvalue(res, 0, 8));
            std::string version = PQgetvalue(res, 0, 9);

            FileInfo info;
            info.uid = uid;
            info.name = name;
            info.path = path;
            info.parent_uid = parent_uid;
            info.type = type;
            info.size = size;
            info.owner = owner;
            info.permissions = permissions;
            info.created_at = std::chrono::system_clock::time_point(std::chrono::seconds(created_at));
            info.modified_at = std::chrono::system_clock::time_point(std::chrono::seconds(modified_at));
            info.version = version;
            info.version_count = 1; // For this implementation, use 1

            PQclear(res);
            connection_pool_->release(conn);
            return Result<std::optional<FileInfo>>::ok(info);
        } else {
            // File not found
            PQclear(res);
            connection_pool_->release(conn);
            return Result<std::optional<FileInfo>>::ok(std::nullopt);
        }
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::err("Failed to get file by UID (with deleted): " + error);
    }
}

Result<std::string> Database::path_to_uid(const std::string& path, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::string>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = "SELECT uid FROM files WHERE path = $1 AND is_deleted = FALSE LIMIT 1;";
    const char* param_values[1] = {path.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        if (PQntuples(res) > 0) {
            std::string uid = PQgetvalue(res, 0, 0);
            PQclear(res);
            connection_pool_->release(conn);
            return Result<std::string>::ok(uid);
        } else {
            // Path not found
            PQclear(res);
            connection_pool_->release(conn);
            return Result<std::string>::err("Path not found");
        }
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::string>::err("Failed to convert path to UID: " + error);
    }
}

Result<std::vector<std::string>> Database::uid_to_path(const std::string& uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<std::string>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = "SELECT path FROM files WHERE uid = $1 AND is_deleted = FALSE;";
    const char* param_values[1] = {uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    std::vector<std::string> paths;
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int nrows = PQntuples(res);
        for (int i = 0; i < nrows; ++i) {
            if (PQgetvalue(res, i, 0) != nullptr) {
                paths.push_back(PQgetvalue(res, i, 0));
            }
        }
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<std::string>>::ok(paths);
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<std::string>>::err("Failed to convert UID to paths: " + error);
    }
}

Result<int64_t> Database::insert_version(const std::string& file_uid, const std::string& version_timestamp,
                                          int64_t size, const std::string& storage_path, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<int64_t>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* insert_sql = R"SQL(
        INSERT INTO versions (file_uid, version_timestamp, size, storage_path)
        VALUES ($1, $2, $3, $4)
        RETURNING file_uid;
    )SQL";

    const char* param_values[4] = {
        file_uid.c_str(),
        version_timestamp.c_str(),
        std::to_string(size).c_str(),
        storage_path.c_str()
    };

    PGresult* res = PQexecParams(pg_conn, insert_sql, 4, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        if (PQntuples(res) > 0) {
            PQclear(res);
            connection_pool_->release(conn);
            return Result<int64_t>::ok(size);
        } else {
            PQclear(res);
            connection_pool_->release(conn);
            return Result<int64_t>::err("Version was not inserted");
        }
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<int64_t>::err("Failed to insert version: " + error);
    }
}

Result<std::optional<std::string>> Database::get_version_storage_path(const std::string& file_uid, const std::string& version_timestamp, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::optional<std::string>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = "SELECT storage_path FROM versions WHERE file_uid = $1 AND version_timestamp = $2 LIMIT 1;";
    const char* param_values[2] = {file_uid.c_str(), version_timestamp.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 2, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        if (PQntuples(res) > 0) {
            std::string path = PQgetvalue(res, 0, 0);
            PQclear(res);
            connection_pool_->release(conn);
            return Result<std::optional<std::string>>::ok(path);
        } else {
            // Version not found
            PQclear(res);
            connection_pool_->release(conn);
            return Result<std::optional<std::string>>::ok(std::nullopt);
        }
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<std::string>>::err("Failed to get version storage path: " + error);
    }
}

Result<std::vector<std::string>> Database::list_versions(const std::string& file_uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<std::string>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = "SELECT version_timestamp FROM versions WHERE file_uid = $1 ORDER BY created_at DESC;";
    const char* param_values[1] = {file_uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    std::vector<std::string> versions;
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int nrows = PQntuples(res);
        for (int i = 0; i < nrows; ++i) {
            versions.push_back(PQgetvalue(res, i, 0));
        }
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<std::string>>::ok(versions);
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<std::string>>::err("Failed to list versions: " + error);
    }
}

Result<bool> Database::restore_to_version(const std::string& file_uid, const std::string& version_timestamp, const std::string& user, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<bool>::err("Failed to acquire database connection for restore operation");
    }

    PGconn* pg_conn = conn->get_connection();

    // Begin transaction
    PGresult* begin_res = PQexec(pg_conn, "BEGIN;");
    if (PQresultStatus(begin_res) != PGRES_COMMAND_OK) {
        PQclear(begin_res);
        connection_pool_->release(conn);
        return Result<bool>::err("Failed to begin transaction for restore: " + std::string(PQerrorMessage(pg_conn)));
    }
    PQclear(begin_res);

    // Update the current version of the file to the specified version
    const char* update_sql = "UPDATE files SET current_version = $2, modified_at = CURRENT_TIMESTAMP WHERE uid = $1;";
    const char* param_values[2] = {file_uid.c_str(), version_timestamp.c_str()};

    PGresult* res = PQexecParams(pg_conn, update_sql, 2, nullptr, param_values, nullptr, nullptr, 0);

    bool success = false;
    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
        int rows_affected = std::stoi(PQcmdTuples(res));
        if (rows_affected > 0) {
            // Transaction successful, commit
            PGresult* commit_res = PQexec(pg_conn, "COMMIT;");
            if (PQresultStatus(commit_res) == PGRES_COMMAND_OK) {
                PQclear(commit_res);
                success = true;
            } else {
                // If commit fails, rollback
                PGresult* rollback_res = PQexec(pg_conn, "ROLLBACK;");
                PQclear(rollback_res);
                PQclear(commit_res);
                PQclear(res);
                connection_pool_->release(conn);
                return Result<bool>::err("Failed to commit restore transaction: " + std::string(PQerrorMessage(pg_conn)));
            }
        } else {
            // Rollback transaction if no rows were affected (file not found)
            PGresult* rollback_res = PQexec(pg_conn, "ROLLBACK;");
            PQclear(rollback_res);
            PQclear(res);
            connection_pool_->release(conn);
            return Result<bool>::err("File not found for restore operation");
        }
    } else {
        // Rollback transaction on error
        PGresult* rollback_res = PQexec(pg_conn, "ROLLBACK;");
        PQclear(rollback_res);
        std::string error = "Failed to restore to version: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<bool>::err(error);
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<bool>::ok(success);
}

// Add all missing methods here
Result<void> Database::set_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& value, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* sql = R"(
        INSERT INTO metadata (file_uid, version_timestamp, key_name, value)
        VALUES ($1, $2, $3, $4)
        ON CONFLICT (file_uid, version_timestamp, key_name)
        DO UPDATE SET value = $4, created_at = CURRENT_TIMESTAMP;
    )";

    const char* params[4] = {file_uid.c_str(), version_timestamp.c_str(), key.c_str(), value.c_str()};

    PGresult* res = PQexecParams(pg_conn, sql, 4, nullptr, params, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to set metadata: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err(error);
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<void>::ok();
}

Result<std::optional<std::string>> Database::get_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::optional<std::string>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* sql = "SELECT value FROM metadata WHERE file_uid = $1 AND version_timestamp = $2 AND key_name = $3;";
    const char* params[3] = {file_uid.c_str(), version_timestamp.c_str(), key.c_str()};

    PGresult* res = PQexecParams(pg_conn, sql, 3, nullptr, params, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = "Failed to get metadata: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<std::string>>::err(error);
    }

    if (PQntuples(res) == 0) {
        // Key not found
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<std::string>>::ok(std::nullopt);
    }

    std::string value = PQgetvalue(res, 0, 0);
    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::optional<std::string>>::ok(value);
}

Result<std::map<std::string, std::string>> Database::get_all_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::map<std::string, std::string>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* sql = "SELECT key_name, value FROM metadata WHERE file_uid = $1 AND version_timestamp = $2;";
    const char* params[2] = {file_uid.c_str(), version_timestamp.c_str()};

    PGresult* res = PQexecParams(pg_conn, sql, 2, nullptr, params, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = "Failed to get all metadata: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::map<std::string, std::string>>::err(error);
    }

    std::map<std::string, std::string> metadata_map;
    int nrows = PQntuples(res);
    for (int i = 0; i < nrows; ++i) {
        std::string key = PQgetvalue(res, i, 0);
        std::string value = PQgetvalue(res, i, 1);
        metadata_map[key] = value;
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::map<std::string, std::string>>::ok(metadata_map);
}

Result<void> Database::delete_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* sql = "DELETE FROM metadata WHERE file_uid = $1 AND version_timestamp = $2 AND key_name = $3;";
    const char* params[3] = {file_uid.c_str(), version_timestamp.c_str(), key.c_str()};

    PGresult* res = PQexecParams(pg_conn, sql, 3, nullptr, params, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to delete metadata: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err(error);
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<void>::ok();
}

Result<void> Database::execute(const std::string& sql, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    PGresult* res = PQexec(pg_conn, sql.c_str());

    if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = "Failed to execute SQL: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err(error);
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<void>::ok();
}

Result<std::vector<std::vector<std::string>>> Database::query(const std::string& sql, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<std::vector<std::string>>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    PGresult* res = PQexec(pg_conn, sql.c_str());

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = "Failed to execute query: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<std::vector<std::string>>>::err(error);
    }

    std::vector<std::vector<std::string>> result_set;
    int nrows = PQntuples(res);
    int ncols = PQnfields(res);

    for (int row = 0; row < nrows; ++row) {
        std::vector<std::string> row_data;
        for (int col = 0; col < ncols; ++col) {
            char* value = PQgetvalue(res, row, col);
            row_data.push_back(value ? std::string(value) : "");
        }
        result_set.push_back(row_data);
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::vector<std::vector<std::string>>>::ok(result_set);
}

Result<void> Database::update_file_access_stats(const std::string& uid, const std::string& user, const std::string& tenant) {
    // This would update the access statistics for a file
    // In a real implementation, this might update an access log or increment counters
    // For now, just return success
    return Result<void>::ok();
}

Result<std::vector<std::string>> Database::get_least_accessed_files(int limit, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<std::string>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    std::string sql = "SELECT uid FROM files WHERE is_deleted = FALSE ORDER BY modified_at ASC LIMIT " + std::to_string(limit) + ";";

    PGresult* res = PQexec(pg_conn, sql.c_str());

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = "Failed to get least accessed files: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<std::string>>::err(error);
    }

    std::vector<std::string> files;
    int nrows = PQntuples(res);
    for (int i = 0; i < nrows; ++i) {
        files.push_back(PQgetvalue(res, i, 0));
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::vector<std::string>>::ok(files);
}

Result<std::vector<std::string>> Database::get_infrequently_accessed_files(int days_threshold, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<std::string>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    std::string sql = "SELECT uid FROM files WHERE is_deleted = FALSE AND modified_at < (CURRENT_TIMESTAMP - INTERVAL '" + std::to_string(days_threshold) + " days') ORDER BY modified_at ASC;";

    PGresult* res = PQexec(pg_conn, sql.c_str());

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = "Failed to get infrequently accessed files: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<std::string>>::err(error);
    }

    std::vector<std::string> files;
    int nrows = PQntuples(res);
    for (int i = 0; i < nrows; ++i) {
        files.push_back(PQgetvalue(res, i, 0));
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::vector<std::string>>::ok(files);
}

Result<int64_t> Database::get_storage_usage(const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<int64_t>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* sql = "SELECT COALESCE(SUM(size), 0) FROM files WHERE is_deleted = FALSE;";

    PGresult* res = PQexec(pg_conn, sql);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = "Failed to get storage usage: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<int64_t>::err(error);
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<int64_t>::ok(0);
    }

    int64_t usage = std::stoll(PQgetvalue(res, 0, 0));
    PQclear(res);
    connection_pool_->release(conn);
    return Result<int64_t>::ok(usage);
}

Result<int64_t> Database::get_storage_capacity(const std::string& tenant) {
    // In a real system, this would retrieve the actual storage capacity
    // For now, return a placeholder value of 1TB
    return Result<int64_t>::ok(1024LL * 1024 * 1024 * 1024); // 1 TB
}

Result<void> Database::create_tenant_schema(const std::string& tenant) {
    // In a real implementation, this would create tenant-specific schemas
    // For now, just return success
    return Result<void>::ok();
}

Result<bool> Database::tenant_schema_exists(const std::string& tenant) {
    // In a real implementation, this would check if the tenant-specific schema exists
    // For now, return true to indicate that it exists
    return Result<bool>::ok(true);
}

Result<void> Database::cleanup_tenant_data(const std::string& tenant) {
    // In a real implementation, this would clean up tenant-specific data
    // For now, just return success
    return Result<void>::ok();
}

// Add the database connection monitoring methods
void Database::configure_secondary_connection(const std::string& host, int port, const std::string& database_name,
                                             const std::string& user, const std::string& password) {
    std::ostringstream conn_stream;
    conn_stream << "host=" << host << " port=" << port 
                << " dbname=" << database_name << " user=" << user
                << " password=" << password;
    secondary_conn_info_ = conn_stream.str();
}

void Database::start_connection_monitoring() {
    if (monitoring_active_.load()) {
        return; // Already running
    }

    monitoring_active_.store(true);
    
    connection_monitor_thread_ = std::thread([this]() {
        while (monitoring_active_.load()) {
            // Check if primary is down but was previously available
            if (!is_connected() && primary_available_.load()) {
                // Try to reconnect to primary
                if (connect()) {
                    // Primary connection restored
                    primary_available_.store(true);
                    using_secondary_.store(false);
                    std::cout << "Database connection to primary restored." << std::endl;
                }
            }
            
            // Sleep for a while before checking again
            std::this_thread::sleep_for(std::chrono::seconds(retry_interval_seconds_));
        }
    });
}

void Database::stop_connection_monitoring() {
    if (!monitoring_active_.load()) {
        return;
    }

    monitoring_active_.store(false);

    if (connection_monitor_thread_.joinable()) {
        connection_monitor_thread_.join();
    }
}

std::string Database::get_connection_info() const {
    if (connection_pool_) {
        return connection_pool_->get_connection_info();
    }
    return "No connection pool";
}

// Helper methods implementation
Result<void> Database::check_connection() const {
    if (!connection_pool_) {
        return Result<void>::err("Connection pool not initialized");
    }

    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire connection");
    }

    // Test a simple query
    PGresult* res = PQexec(conn->get_connection(), "SELECT 1;");
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        return Result<void>::err("Basic query failed: " + std::string(PQerrorMessage(conn->get_connection())));
    }

    PQclear(res);
    return Result<void>::ok();
}

std::string Database::escape_string(const std::string& str, PGconn* conn) const {
    int error;
    char* escaped = PQescapeLiteral(conn, str.c_str(), str.length());
    std::string result = escaped ? escaped : str;
    PQfreemem(escaped);
    return result;
}

std::string Database::validate_schema_name(const std::string& schema_name) const {
    std::string validated = schema_name;

    // Remove any potentially dangerous characters
    validated.erase(std::remove_if(validated.begin(), validated.end(),
        [](char c) { return !std::isalnum(c) && c != '_'; }), validated.end());

    // Ensure it starts with an alphanumeric character or underscore
    if (!validated.empty() && !std::isalnum(validated[0]) && validated[0] != '_') {
        validated = "_" + validated;
    }

    // Limit length to prevent SQL injection
    if (validated.length() > 63) {
        validated = validated.substr(0, 63);
    }

    return validated;
}

std::string Database::get_schema_prefix(const std::string& tenant) const {
    if (tenant.empty()) {
        return "public";
    }
    return validate_schema_name("tenant_" + tenant);
}

} // namespace fileengine