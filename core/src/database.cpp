#include "fileengine/database.h"
#include "fileengine/utils.h"
#include "fileengine/server_logger.h"
#include "fileengine/connection_pool_manager.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <chrono>
#include <cstdio>
#include <ctime>

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
    SERVER_LOG_DEBUG("Database", "Attempting to connect to database using connection pool.");
    if (!connection_pool_) {
        SERVER_LOG_ERROR("Database", "Connection pool not initialized during connect attempt.");
        return false;
    }
    bool connected = connection_pool_->initialize();
    if (connected) {
        SERVER_LOG_INFO("Database", "Successfully initialized database connection pool.");
    } else {
        SERVER_LOG_ERROR("Database", "Failed to initialize database connection pool.");
    }
    return connected;
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
    SERVER_LOG_DEBUG("Database", "Attempting to create global schema.");
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        SERVER_LOG_ERROR("Database", "Failed to acquire database connection for schema creation.");
        return Result<void>::err("Failed to acquire database connection for schema creation");
    }

    PGconn* pg_conn = conn->get_connection();

    // Create global tables for file access stats and tenant registry as per specification
    const char* global_tables_sql = R"SQL(
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

    // Execute global schema SQL statements
    SERVER_LOG_DEBUG("Database", "Executing SQL to create global tables.");
    PGresult* res1 = PQexec(pg_conn, global_tables_sql);
    if (PQresultStatus(res1) != PGRES_COMMAND_OK) {
        std::string error_msg = "Failed to create global tables: " + std::string(PQerrorMessage(pg_conn));
        SERVER_LOG_ERROR("Database", error_msg);
        Result<void> result_err = Result<void>::err(error_msg);
        PQclear(res1);
        connection_pool_->release(conn);
        return result_err;
    }
    SERVER_LOG_INFO("Database", "Successfully created or verified global tables.");
    PQclear(res1);

    // Release the connection back to the pool
    connection_pool_->release(conn);

    return Result<void>::ok();
}

Result<void> Database::drop_schema() {
    // This method is no longer used as data storage is immutable
    // Resets are performed manually by administrators as needed
    return Result<void>::err("drop_schema not supported - data storage is immutable");
}

Result<std::string> Database::insert_file(const std::string& uid, const std::string& name,
                                          const std::string& path, const std::string& parent_uid,
                                          FileType type, const std::string& owner,
                                          int permissions, const std::string& tenant) {
    SERVER_LOG_DEBUG("Database::insert_file", ServerLogger::getInstance().detailed_log_prefix() +
              "Entering insert_file operation - uid: " + uid +
              ", name: " + name + ", path: " + path +
              ", parent_uid: " + parent_uid + ", type: " + std::to_string(static_cast<int>(type)) +
              ", owner: " + owner + ", permissions: " + std::to_string(permissions) +
              ", tenant: " + tenant);

    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        SERVER_LOG_ERROR("Database::insert_file", ServerLogger::getInstance().detailed_log_prefix() +
                  "Failed to acquire database connection for UID: " + uid);
        return Result<std::string>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();
    SERVER_LOG_DEBUG("Database::insert_file", ServerLogger::getInstance().detailed_log_prefix() +
              "Acquired database connection for UID: " + uid);

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    // Prepare SQL with INSERT/ON CONFLICT handling to avoid duplicates
    std::string insert_sql = "INSERT INTO \"" + schema_name + "\".files (uid, name, parent_uid, size, owner, permission_map, is_container, deleted) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8) "
        "ON CONFLICT (uid) DO UPDATE SET "
            "name = EXCLUDED.name, "
            "parent_uid = EXCLUDED.parent_uid, "
            "size = EXCLUDED.size, "
            "owner = EXCLUDED.owner, "
            "permission_map = EXCLUDED.permission_map, "
            "is_container = EXCLUDED.is_container "
        "RETURNING uid;";

    // Convert file type to integer
    int type_int = static_cast<int>(type);
    int64_t size = 0; // New files start with 0 size
    bool is_container = (type == FileType::DIRECTORY); // Check if it's a directory

    // Prepare parameter values - ensure they are properly converted to strings
    std::string size_str = std::to_string(size);
    std::string perms_str = std::to_string(permissions);
    std::string container_str = is_container ? "TRUE" : "FALSE";
    std::string deleted_str = "FALSE"; // Files are not deleted by default

    if (name.empty()) {
        SERVER_LOG_ERROR("Database::insert_file", ServerLogger::getInstance().detailed_log_prefix() +
                  "Invalid parameter: name is empty for uid: " + uid);
        connection_pool_->release(conn);
        return Result<std::string>::err("Invalid parameter: name is empty");
    }

    if (schema_name.empty()) {
        SERVER_LOG_ERROR("Database::insert_file", ServerLogger::getInstance().detailed_log_prefix() +
                  "Invalid parameter: schema_name is empty for tenant: " + tenant);
        connection_pool_->release(conn);
        return Result<std::string>::err("Invalid parameter: schema_name is empty");
    }

    const char* param_values[8] = {
        uid.c_str(),              // $1
        name.c_str(),             // $2
        parent_uid.c_str(),       // $3
        size_str.c_str(),         // $4
        owner.c_str(),            // $5
        perms_str.c_str(),        // $6
        container_str.c_str(),    // $7
        deleted_str.c_str()       // $8
    };

    // Validate that none of the parameter values are null
    for (int i = 0; i < 8; ++i) {
        if (param_values[i] == nullptr) {
            SERVER_LOG_ERROR("Database::insert_file", ServerLogger::getInstance().detailed_log_prefix() +
                      "Invalid parameter: param_values[" + std::to_string(i) + "] is null for uid: " + uid);
            connection_pool_->release(conn);
            return Result<std::string>::err("Invalid parameter: param_values[" + std::to_string(i) + "] is null");
        }
    }

    SERVER_LOG_DEBUG("Database::insert_file", ServerLogger::getInstance().detailed_log_prefix() +
              "Executing SQL INSERT with parameters for UID: " + uid +
              ", name: " + name + ", parent_uid: " + parent_uid +
              ", owner: " + owner + ", permissions: " + perms_str);

    PGresult* res = PQexecParams(pg_conn, insert_sql.c_str(), 8, nullptr, param_values, nullptr, nullptr, 0);

    std::string result_uid;
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        if (PQntuples(res) > 0) {
            result_uid = PQgetvalue(res, 0, 0);
        }
        if (result_uid.empty()) {
            SERVER_LOG_WARN("Database::insert_file", ServerLogger::getInstance().detailed_log_prefix() +
                     "File/directory with UID already exists: " + uid);
            // The insert was ignored due to conflict - return error for duplicate prevention
            PQclear(res);
            connection_pool_->release(conn);
            return Result<std::string>::err("File/directory with this UID already exists");
        }
        SERVER_LOG_DEBUG("Database::insert_file", ServerLogger::getInstance().detailed_log_prefix() +
                  "Successfully inserted file with UID: " + result_uid);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::string>::ok(result_uid);
    } else {
        std::string error = PQerrorMessage(pg_conn);
        SERVER_LOG_ERROR("Database::insert_file", ServerLogger::getInstance().detailed_log_prefix() +
                  "Failed to insert file with UID: " + uid + ", error: " + error);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::string>::err("Failed to insert file: " + error);
    }
}

Result<void> Database::update_file_modified(const std::string& uid, const std::string& tenant) {
// This is redundant, all versions are stored and tracked by time stamp. The first and last vrtdion time dytsmps are ctime and mtime
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // In the current schema, we don't have a modified_at column, so this operation is redundant
    // Just return success since the modification time is tracked elsewhere (e.g., in versions)
    connection_pool_->release(conn);
    return Result<void>::ok();
}

Result<void> Database::update_file_current_version(const std::string& uid, const std::string& version_timestamp, const std::string& tenant) {
// This is redundant, all versions are stored and tracked by time stamp. The first and last vrtdion time dytsmps are ctime and mtime
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // In the current schema, we don't have a current_version or modified_at column, so this operation is redundant
    // Just return success since versioning info is tracked in the versions table
    connection_pool_->release(conn);
    return Result<void>::ok();
}

Result<bool> Database::delete_file(const std::string& uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<bool>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    // Soft delete - update the deleted flag
    std::string delete_sql = "UPDATE \"" + schema_name + "\".files SET deleted = TRUE WHERE uid = $1;";
    const char* param_values[1] = {uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, delete_sql.c_str(), 1, nullptr, param_values, nullptr, nullptr, 0);

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

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    std::string undelete_sql = "UPDATE \"" + schema_name + "\".files SET deleted = FALSE WHERE uid = $1;";
    const char* param_values[1] = {uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, undelete_sql.c_str(), 1, nullptr, param_values, nullptr, nullptr, 0);

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

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    // Note: Empty UID is valid for root directory which has an empty UID by design
    // No need to validate that uid is not empty

    if (schema_name.empty()) {
        SERVER_LOG_ERROR("Database::get_file_by_uid", ServerLogger::getInstance().detailed_log_prefix() +
                  "Invalid parameter: schema_name is empty for tenant: " + tenant);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::err("Invalid parameter: schema_name is empty");
    }

    std::string query_sql = "SELECT name, parent_uid, size, owner, permission_map, is_container, deleted "
                            "FROM \"" + schema_name + "\".files "
                            "WHERE uid = $1 AND deleted = FALSE "
                            "LIMIT 1;";
    const char* param_values[1] = {uid.c_str()};

    // Log the query and parameters for debugging
    SERVER_LOG_DEBUG("Database::get_file_by_uid", ServerLogger::getInstance().detailed_log_prefix() +
              "Executing query: " + query_sql + " with param[0]: '" + uid + "'");

    PGresult* res = PQexecParams(pg_conn, query_sql.c_str(), 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        if (PQntuples(res) > 0) {
            // Get the file info from the database row
            std::string name = PQgetvalue(res, 0, 0);
            std::string parent_uid = PQgetvalue(res, 0, 1);
            int64_t size = std::stoll(PQgetvalue(res, 0, 2));
            std::string owner = PQgetvalue(res, 0, 3);
            int permissions = std::stoi(PQgetvalue(res, 0, 4));
            bool is_container = (strcmp(PQgetvalue(res, 0, 5), "t") == 0 || strcmp(PQgetvalue(res, 0, 5), "1") == 0);
            bool is_deleted = (strcmp(PQgetvalue(res, 0, 6), "t") == 0 || strcmp(PQgetvalue(res, 0, 6), "1") == 0);

            FileInfo info;
            info.uid = uid;
            info.name = name;
            info.path = "/" + name;  // Simple path calculation - in a real system this would be more complex
            info.parent_uid = parent_uid;
            info.type = is_container ? FileType::DIRECTORY : FileType::REGULAR_FILE;
            info.size = size;
            info.owner = owner;
            info.permissions = permissions;
            // Use current time for timestamps since we don't have these in the schema
            auto now = std::chrono::system_clock::now();
            info.created_at = now;
            info.modified_at = now;
            // Get the latest version from the versions table
            std::string version_query = "SELECT version_timestamp FROM \"" + schema_name + "\".versions WHERE file_uid = $1 ORDER BY version_timestamp DESC LIMIT 1;";
            const char* version_param_values[1] = {info.uid.c_str()};

            PGresult* version_res = PQexecParams(pg_conn, version_query.c_str(), 1, nullptr, version_param_values, nullptr, nullptr, 0);
            if (PQresultStatus(version_res) == PGRES_TUPLES_OK && PQntuples(version_res) > 0) {
                info.version = PQgetvalue(version_res, 0, 0);
            } else {
                // If no version is found in the versions table, use a default
                info.version = "";
            }
            PQclear(version_res);
            info.version_count = 1; // For this implementation, use 1

            // Hidden child renditions (files only; a directory's children are
            // not renditions, so leave 0).
            info.rendition_count = 0;
            if (!is_container) {
                std::string rc_query = "SELECT COUNT(*) FROM \"" + schema_name +
                                       "\".files WHERE parent_uid = $1 AND deleted = FALSE;";
                const char* rc_params[1] = {info.uid.c_str()};
                PGresult* rc_res = PQexecParams(pg_conn, rc_query.c_str(), 1, nullptr, rc_params, nullptr, nullptr, 0);
                if (PQresultStatus(rc_res) == PGRES_TUPLES_OK && PQntuples(rc_res) > 0) {
                    info.rendition_count = std::stoi(PQgetvalue(rc_res, 0, 0));
                }
                PQclear(rc_res);
            }

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

    // Since we don't have a 'path' column in the schema, we need to find files differently
    // For now, we'll return an error since path-based lookup isn't supported with this schema
    // In a real implementation, you'd either need path storage or implement traversal
    PQclear(PQexec(pg_conn, "SELECT 1")); // Just to use the conn before releasing
    connection_pool_->release(conn);
    return Result<std::optional<FileInfo>>::err("Path-based lookup not supported with current schema. Use UID-based lookup instead.");
}

Result<void> Database::update_file_name(const std::string& uid, const std::string& new_name, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    std::string update_sql = "UPDATE \"" + schema_name + "\".files SET name = $2 WHERE uid = $1;";
    const char* param_values[2] = {uid.c_str(), new_name.c_str()};

    PGresult* res = PQexecParams(pg_conn, update_sql.c_str(), 2, nullptr, param_values, nullptr, nullptr, 0);

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

Result<void> Database::update_file_size(const std::string& uid, int64_t size, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    std::string update_sql = "UPDATE \"" + schema_name + "\".files SET size = $2 WHERE uid = $1;";
    std::string size_str = std::to_string(size);
    const char* param_values[2] = {uid.c_str(), size_str.c_str()};

    PGresult* res = PQexecParams(pg_conn, update_sql.c_str(), 2, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::ok();
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to update file size: " + error);
    }
}

Result<std::vector<FileInfo>> Database::list_files_in_directory(const std::string& parent_uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<FileInfo>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    // Validate parameters before executing query
    if (schema_name.empty()) {
        SERVER_LOG_ERROR("Database::list_files_in_directory", ServerLogger::getInstance().detailed_log_prefix() +
                  "Invalid parameter: schema_name is empty for tenant: " + tenant);
        connection_pool_->release(conn);
        return Result<std::vector<FileInfo>>::err("Invalid parameter: schema_name is empty");
    }

    // uid <> $1 excludes the directory's own record from its child listing. The
    // root record is self-referential (uid='' and parent_uid=''); without this
    // it would appear as a phantom "root" child of itself.
    // rendition_count is the number of non-deleted hidden children, but only for
    // file entities (a directory's children are not hidden renditions, so 0).
    std::string query_sql = "SELECT f.uid, f.name, f.size, f.owner, f.permission_map, f.is_container, "
                            "CASE WHEN f.is_container THEN 0 ELSE "
                            "(SELECT COUNT(*) FROM \"" + schema_name + "\".files c "
                            "WHERE c.parent_uid = f.uid AND c.deleted = FALSE) END AS rendition_count "
                            "FROM \"" + schema_name + "\".files f "
                            "WHERE f.parent_uid = $1 AND f.uid <> $1 AND f.deleted = FALSE "
                            "ORDER BY f.name;";
    const char* param_values[1] = {parent_uid.c_str()};

    // Validate that the parameter value is not null
    if (param_values[0] == nullptr) {
        SERVER_LOG_ERROR("Database::list_files_in_directory", ServerLogger::getInstance().detailed_log_prefix() +
                  "Invalid parameter: parent_uid is null for tenant: " + tenant);
        connection_pool_->release(conn);
        return Result<std::vector<FileInfo>>::err("Invalid parameter: parent_uid is null");
    }

    SERVER_LOG_DEBUG("Database::list_files_in_directory", ServerLogger::getInstance().detailed_log_prefix() +
              "Executing SQL query to list files in directory with parent_uid: " + parent_uid +
              ", tenant: " + tenant + ", schema: " + schema_name);

    PGresult* res = PQexecParams(pg_conn, query_sql.c_str(), 1, nullptr, param_values, nullptr, nullptr, 0);

    std::vector<FileInfo> result_files;
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int nrows = PQntuples(res);
        for (int i = 0; i < nrows; ++i) {
            FileInfo info;
            info.uid = PQgetvalue(res, i, 0);
            info.name = PQgetvalue(res, i, 1);
            info.path = "/" + info.name;  // Simple path calculation - in a real system this would be more complex
            info.parent_uid = parent_uid;
            int64_t size = std::stoll(PQgetvalue(res, i, 2));
            info.size = size;
            info.owner = PQgetvalue(res, i, 3);
            info.permissions = std::stoi(PQgetvalue(res, i, 4));
            bool is_container = (strcmp(PQgetvalue(res, i, 5), "t") == 0 || strcmp(PQgetvalue(res, i, 5), "1") == 0);
            info.type = is_container ? FileType::DIRECTORY : FileType::REGULAR_FILE;
            info.rendition_count = std::stoi(PQgetvalue(res, i, 6));  // hidden children (files only)
            // Use current time for timestamps since we don't have these in the schema
            auto now = std::chrono::system_clock::now();
            info.created_at = now;
            info.modified_at = now;
            // Get the latest version from the versions table
            std::string version_query = "SELECT version_timestamp FROM \"" + schema_name + "\".versions WHERE file_uid = $1 ORDER BY version_timestamp DESC LIMIT 1;";
            const char* version_param_values[1] = {info.uid.c_str()};

            PGresult* version_res = PQexecParams(pg_conn, version_query.c_str(), 1, nullptr, version_param_values, nullptr, nullptr, 0);
            if (PQresultStatus(version_res) == PGRES_TUPLES_OK && PQntuples(version_res) > 0) {
                info.version = PQgetvalue(version_res, 0, 0);
            } else {
                // If no version is found in the versions table, use a default
                info.version = "";
            }
            PQclear(version_res);
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

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    // Validate parameters before executing query
    if (schema_name.empty()) {
        SERVER_LOG_ERROR("Database::list_files_in_directory_with_deleted", ServerLogger::getInstance().detailed_log_prefix() +
                  "Invalid parameter: schema_name is empty for tenant: " + tenant);
        connection_pool_->release(conn);
        return Result<std::vector<FileInfo>>::err("Invalid parameter: schema_name is empty");
    }

    // uid <> $1 excludes the directory's own self-referential record (the root
    // record has uid='' and parent_uid='') from its child listing.
    std::string query_sql = "SELECT f.uid, f.name, f.size, f.owner, f.permission_map, f.is_container, f.deleted, "
                            "CASE WHEN f.is_container THEN 0 ELSE "
                            "(SELECT COUNT(*) FROM \"" + schema_name + "\".files c "
                            "WHERE c.parent_uid = f.uid AND c.deleted = FALSE) END AS rendition_count "
                            "FROM \"" + schema_name + "\".files f "
                            "WHERE f.parent_uid = $1 AND f.uid <> $1 "
                            "ORDER BY f.name;";
    const char* param_values[1] = {parent_uid.c_str()};


    // Validate that the parameter value is not null
    if (param_values[0] == nullptr) {
        SERVER_LOG_ERROR("Database::list_files_in_directory_with_deleted", ServerLogger::getInstance().detailed_log_prefix() +
                  "Invalid parameter: parent_uid is null for tenant: " + tenant);
        connection_pool_->release(conn);
        return Result<std::vector<FileInfo>>::err("Invalid parameter: parent_uid is null");
    }

    SERVER_LOG_DEBUG("Database::list_files_in_directory_with_deleted", ServerLogger::getInstance().detailed_log_prefix() +
              "Executing SQL query to list files in directory (with deleted) with parent_uid: " + parent_uid +
              ", tenant: " + tenant + ", schema: " + schema_name);

    PGresult* res = PQexecParams(pg_conn, query_sql.c_str(), 1, nullptr, param_values, nullptr, nullptr, 0);

    std::vector<FileInfo> result_files;
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int nrows = PQntuples(res);
        for (int i = 0; i < nrows; ++i) {
            // Skip deleted files unless specifically requested - this function includes deleted files
            FileInfo info;
            info.uid = PQgetvalue(res, i, 0);
            info.name = PQgetvalue(res, i, 1);
            info.path = "/" + info.name;  // Simple path calculation - in a real system this would be more complex
            info.parent_uid = parent_uid;
            int64_t size = std::stoll(PQgetvalue(res, i, 2));
            info.size = size;
            info.owner = PQgetvalue(res, i, 3);
            info.permissions = std::stoi(PQgetvalue(res, i, 4));
            bool is_container = (strcmp(PQgetvalue(res, i, 5), "t") == 0 || strcmp(PQgetvalue(res, i, 5), "1") == 0);
            info.type = is_container ? FileType::DIRECTORY : FileType::REGULAR_FILE;
            info.rendition_count = std::stoi(PQgetvalue(res, i, 7));  // index 7: after deleted (6)
            // Use current time for timestamps since we don't have these in the schema
            auto now = std::chrono::system_clock::now();
            info.created_at = now;
            info.modified_at = now;
            // Get the latest version from the versions table
            std::string version_query = "SELECT version_timestamp FROM \"" + schema_name + "\".versions WHERE file_uid = $1 ORDER BY version_timestamp DESC LIMIT 1;";
            const char* version_param_values[1] = {info.uid.c_str()};

            PGresult* version_res = PQexecParams(pg_conn, version_query.c_str(), 1, nullptr, version_param_values, nullptr, nullptr, 0);
            if (PQresultStatus(version_res) == PGRES_TUPLES_OK && PQntuples(version_res) > 0) {
                info.version = PQgetvalue(version_res, 0, 0);
            } else {
                // If no version is found in the versions table, use a default
                info.version = "";
            }
            PQclear(version_res);
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

Result<std::vector<FileInfo>> Database::list_all_files(const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<FileInfo>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    // Validate parameters before executing query
    if (schema_name.empty()) {
        SERVER_LOG_ERROR("Database::list_all_files", ServerLogger::getInstance().detailed_log_prefix() +
                  "Invalid parameter: schema_name is empty for tenant: " + tenant);
        connection_pool_->release(conn);
        return Result<std::vector<FileInfo>>::err("Invalid parameter: schema_name is empty");
    }

    std::string query_sql = "SELECT uid, name, size, owner, permission_map, is_container "
                            "FROM \"" + schema_name + "\".files "
                            "ORDER BY uid;";
    const char* param_values[0] = {}; // No parameters for this query

    SERVER_LOG_DEBUG("Database::list_all_files", ServerLogger::getInstance().detailed_log_prefix() +
              "Executing SQL query to list all files for tenant: " + tenant + ", schema: " + schema_name);

    PGresult* res = PQexecParams(pg_conn, query_sql.c_str(), 0, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        std::vector<FileInfo> files;
        int num_tuples = PQntuples(res);

        for (int i = 0; i < num_tuples; ++i) {
            FileInfo info;
            info.uid = PQgetvalue(res, i, 0);
            info.name = PQgetvalue(res, i, 1);
            info.size = std::stoll(PQgetvalue(res, i, 2));
            info.owner = PQgetvalue(res, i, 3);
            info.permissions = std::stoi(PQgetvalue(res, i, 4));
            bool is_container = (strcmp(PQgetvalue(res, i, 5), "t") == 0 || strcmp(PQgetvalue(res, i, 5), "1") == 0);
            info.type = is_container ? FileType::DIRECTORY : FileType::REGULAR_FILE;

            // Set default values for other fields
            info.path = "/" + info.name;  // Simple path calculation
            info.parent_uid = "";  // For top-level files
            auto now = std::chrono::system_clock::now();
            info.created_at = now;
            info.modified_at = now;
            // Get the latest version from the versions table
            std::string version_query = "SELECT version_timestamp FROM \"" + schema_name + "\".versions WHERE file_uid = $1 ORDER BY version_timestamp DESC LIMIT 1;";
            const char* version_param_values[1] = {info.uid.c_str()};

            PGresult* version_res = PQexecParams(pg_conn, version_query.c_str(), 1, nullptr, version_param_values, nullptr, nullptr, 0);
            if (PQresultStatus(version_res) == PGRES_TUPLES_OK && PQntuples(version_res) > 0) {
                info.version = PQgetvalue(version_res, 0, 0);
            } else {
                // If no version is found in the versions table, use a default
                info.version = "";
            }
            PQclear(version_res);
            info.version_count = 1; // For this implementation, use 1

            files.push_back(info);
        }

        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<FileInfo>>::ok(files);
    } else {
        std::string error = PQerrorMessage(pg_conn);
        SERVER_LOG_ERROR("Database::list_all_files", ServerLogger::getInstance().detailed_log_prefix() +
                  "Query failed: " + error);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<FileInfo>>::err("Query failed: " + error);
    }
}

Result<std::optional<FileInfo>> Database::get_file_by_name_and_parent(const std::string& name, const std::string& parent_uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::optional<FileInfo>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    // Validate parameters before executing query
    if (name.empty()) {
        SERVER_LOG_ERROR("Database::get_file_by_name_and_parent", ServerLogger::getInstance().detailed_log_prefix() +
                  "Invalid parameter: name is empty for parent_uid: " + parent_uid);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::err("Invalid parameter: name is empty");
    }

    if (schema_name.empty()) {
        SERVER_LOG_ERROR("Database::get_file_by_name_and_parent", ServerLogger::getInstance().detailed_log_prefix() +
                  "Invalid parameter: schema_name is empty for tenant: " + tenant);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::err("Invalid parameter: schema_name is empty");
    }

    std::string query_sql = "SELECT uid, size, owner, permission_map, is_container "
                            "FROM \"" + schema_name + "\".files "
                            "WHERE name = $1 AND parent_uid = $2 AND deleted = FALSE "
                            "LIMIT 1;";
    const char* param_values[2] = {name.c_str(), parent_uid.c_str()};

    // Validate that none of the parameter values are null
    for (int i = 0; i < 2; ++i) {
        if (param_values[i] == nullptr) {
            SERVER_LOG_ERROR("Database::get_file_by_name_and_parent", ServerLogger::getInstance().detailed_log_prefix() +
                      "Invalid parameter: param_values[" + std::to_string(i) + "] is null for name: " + name +
                      ", parent_uid: " + parent_uid);
            connection_pool_->release(conn);
            return Result<std::optional<FileInfo>>::err("Invalid parameter: param_values[" + std::to_string(i) + "] is null");
        }
    }

    SERVER_LOG_DEBUG("Database::get_file_by_name_and_parent", ServerLogger::getInstance().detailed_log_prefix() +
              "Executing SQL query to get file by name: " + name + " and parent_uid: " + parent_uid +
              ", tenant: " + tenant + ", schema: " + schema_name);

    PGresult* res = PQexecParams(pg_conn, query_sql.c_str(), 2, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        if (PQntuples(res) > 0) {
            std::string uid = PQgetvalue(res, 0, 0);
            int64_t size = std::stoll(PQgetvalue(res, 0, 1));
            std::string owner = PQgetvalue(res, 0, 2);
            int permissions = std::stoi(PQgetvalue(res, 0, 3));
            bool is_container = (strcmp(PQgetvalue(res, 0, 4), "t") == 0 || strcmp(PQgetvalue(res, 0, 4), "1") == 0);

            FileInfo info;
            info.uid = uid;
            info.name = name;
            info.path = "/" + name;  // Simple path calculation - in a real system this would be more complex
            info.parent_uid = parent_uid;
            info.type = is_container ? FileType::DIRECTORY : FileType::REGULAR_FILE;
            info.size = size;
            info.owner = owner;
            info.permissions = permissions;
            // Use current time for timestamps since we don't have these in the schema
            auto now = std::chrono::system_clock::now();
            info.created_at = now;
            info.modified_at = now;
            // Get the latest version from the versions table
            std::string version_query = "SELECT version_timestamp FROM \"" + schema_name + "\".versions WHERE file_uid = $1 ORDER BY version_timestamp DESC LIMIT 1;";
            const char* version_param_values[1] = {info.uid.c_str()};

            PGresult* version_res = PQexecParams(pg_conn, version_query.c_str(), 1, nullptr, version_param_values, nullptr, nullptr, 0);
            if (PQresultStatus(version_res) == PGRES_TUPLES_OK && PQntuples(version_res) > 0) {
                info.version = PQgetvalue(version_res, 0, 0);
            } else {
                // If no version is found in the versions table, use a default
                info.version = "";
            }
            PQclear(version_res);
            info.version_count = 1; // For this implementation, use 1

            // Hidden child renditions (files only; a directory's children are
            // not renditions, so leave 0).
            info.rendition_count = 0;
            if (!is_container) {
                std::string rc_query = "SELECT COUNT(*) FROM \"" + schema_name +
                                       "\".files WHERE parent_uid = $1 AND deleted = FALSE;";
                const char* rc_params[1] = {info.uid.c_str()};
                PGresult* rc_res = PQexecParams(pg_conn, rc_query.c_str(), 1, nullptr, rc_params, nullptr, nullptr, 0);
                if (PQresultStatus(rc_res) == PGRES_TUPLES_OK && PQntuples(rc_res) > 0) {
                    info.rendition_count = std::stoi(PQgetvalue(rc_res, 0, 0));
                }
                PQclear(rc_res);
            }

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

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    // Validate parameters before executing query
    if (name.empty()) {
        SERVER_LOG_ERROR("Database::get_file_by_name_and_parent_include_deleted", ServerLogger::getInstance().detailed_log_prefix() +
                  "Invalid parameter: name is empty for parent_uid: " + parent_uid);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::err("Invalid parameter: name is empty");
    }

    if (schema_name.empty()) {
        SERVER_LOG_ERROR("Database::get_file_by_name_and_parent_include_deleted", ServerLogger::getInstance().detailed_log_prefix() +
                  "Invalid parameter: schema_name is empty for tenant: " + tenant);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::err("Invalid parameter: schema_name is empty");
    }

    std::string query_sql = "SELECT uid, size, owner, permission_map, is_container "
                            "FROM \"" + schema_name + "\".files "
                            "WHERE name = $1 AND parent_uid = $2 "
                            "LIMIT 1;";
    const char* param_values[2] = {name.c_str(), parent_uid.c_str()};

    // Validate that none of the parameter values are null
    for (int i = 0; i < 2; ++i) {
        if (param_values[i] == nullptr) {
            SERVER_LOG_ERROR("Database::get_file_by_name_and_parent_include_deleted", ServerLogger::getInstance().detailed_log_prefix() +
                      "Invalid parameter: param_values[" + std::to_string(i) + "] is null for name: " + name +
                      ", parent_uid: " + parent_uid);
            connection_pool_->release(conn);
            return Result<std::optional<FileInfo>>::err("Invalid parameter: param_values[" + std::to_string(i) + "] is null");
        }
    }

    SERVER_LOG_DEBUG("Database::get_file_by_name_and_parent_include_deleted", ServerLogger::getInstance().detailed_log_prefix() +
              "Executing SQL query to get file by name: " + name + " and parent_uid: " + parent_uid +
              ", tenant: " + tenant + ", schema: " + schema_name);

    PGresult* res = PQexecParams(pg_conn, query_sql.c_str(), 2, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        if (PQntuples(res) > 0) {
            std::string uid = PQgetvalue(res, 0, 0);
            int64_t size = std::stoll(PQgetvalue(res, 0, 1));
            std::string owner = PQgetvalue(res, 0, 2);
            int permissions = std::stoi(PQgetvalue(res, 0, 3));
            bool is_container = (strcmp(PQgetvalue(res, 0, 4), "t") == 0 || strcmp(PQgetvalue(res, 0, 4), "1") == 0);

            FileInfo info;
            info.uid = uid;
            info.name = name;
            info.path = "/" + name;  // Simple path calculation - in a real system this would be more complex
            info.parent_uid = parent_uid;
            info.type = is_container ? FileType::DIRECTORY : FileType::REGULAR_FILE;
            info.size = size;
            info.owner = owner;
            info.permissions = permissions;
            // Use current time for timestamps since we don't have these in the schema
            auto now = std::chrono::system_clock::now();
            info.created_at = now;
            info.modified_at = now;
            // Get the latest version from the versions table
            std::string version_query = "SELECT version_timestamp FROM \"" + schema_name + "\".versions WHERE file_uid = $1 ORDER BY version_timestamp DESC LIMIT 1;";
            const char* version_param_values[1] = {info.uid.c_str()};

            PGresult* version_res = PQexecParams(pg_conn, version_query.c_str(), 1, nullptr, version_param_values, nullptr, nullptr, 0);
            if (PQresultStatus(version_res) == PGRES_TUPLES_OK && PQntuples(version_res) > 0) {
                info.version = PQgetvalue(version_res, 0, 0);
            } else {
                // If no version is found in the versions table, use a default
                info.version = "";
            }
            PQclear(version_res);
            info.version_count = 1; // For this implementation, use 1

            // Hidden child renditions (files only; a directory's children are
            // not renditions, so leave 0).
            info.rendition_count = 0;
            if (!is_container) {
                std::string rc_query = "SELECT COUNT(*) FROM \"" + schema_name +
                                       "\".files WHERE parent_uid = $1 AND deleted = FALSE;";
                const char* rc_params[1] = {info.uid.c_str()};
                PGresult* rc_res = PQexecParams(pg_conn, rc_query.c_str(), 1, nullptr, rc_params, nullptr, nullptr, 0);
                if (PQresultStatus(rc_res) == PGRES_TUPLES_OK && PQntuples(rc_res) > 0) {
                    info.rendition_count = std::stoi(PQgetvalue(rc_res, 0, 0));
                }
                PQclear(rc_res);
            }

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

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    std::string query_sql = "SELECT size FROM \"" + schema_name + "\".files WHERE uid = $1 AND deleted = FALSE LIMIT 1;";
    const char* param_values[1] = {file_uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql.c_str(), 1, nullptr, param_values, nullptr, nullptr, 0);

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

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    std::string query_sql = "SELECT COALESCE(SUM(size), 0) FROM \"" + schema_name + "\".files WHERE parent_uid = $1 AND deleted = FALSE;";
    const char* param_values[1] = {dir_uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql.c_str(), 1, nullptr, param_values, nullptr, nullptr, 0);

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

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    std::string query_sql = "SELECT name, parent_uid, size, owner, permission_map, is_container, deleted "
                            "FROM \"" + schema_name + "\".files "
                            "WHERE uid = $1 "
                            "LIMIT 1;";
    const char* param_values[1] = {uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql.c_str(), 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        if (PQntuples(res) > 0) {
            std::string name = PQgetvalue(res, 0, 0);
            std::string parent_uid = PQgetvalue(res, 0, 1);
            int64_t size = std::stoll(PQgetvalue(res, 0, 2));
            std::string owner = PQgetvalue(res, 0, 3);
            int permissions = std::stoi(PQgetvalue(res, 0, 4));
            bool is_container = (strcmp(PQgetvalue(res, 0, 5), "t") == 0 || strcmp(PQgetvalue(res, 0, 5), "1") == 0);
            bool is_deleted = (strcmp(PQgetvalue(res, 0, 6), "t") == 0 || strcmp(PQgetvalue(res, 0, 6), "1") == 0);

            if (is_deleted) {
                // Even if we're including deleted files, return nullopt to indicate the file is not accessible
                PQclear(res);
                connection_pool_->release(conn);
                return Result<std::optional<FileInfo>>::ok(std::nullopt);
            }

            FileInfo info;
            info.uid = uid;
            info.name = name;
            info.path = "/" + name;  // Simple path calculation - in a real system this would be more complex
            info.parent_uid = parent_uid;
            info.type = is_container ? FileType::DIRECTORY : FileType::REGULAR_FILE;
            info.size = size;
            info.owner = owner;
            info.permissions = permissions;
            // Use current time for timestamps since we don't have these in the schema
            auto now = std::chrono::system_clock::now();
            info.created_at = now;
            info.modified_at = now;
            // Get the latest version from the versions table
            std::string version_query = "SELECT version_timestamp FROM \"" + schema_name + "\".versions WHERE file_uid = $1 ORDER BY version_timestamp DESC LIMIT 1;";
            const char* version_param_values[1] = {info.uid.c_str()};

            PGresult* version_res = PQexecParams(pg_conn, version_query.c_str(), 1, nullptr, version_param_values, nullptr, nullptr, 0);
            if (PQresultStatus(version_res) == PGRES_TUPLES_OK && PQntuples(version_res) > 0) {
                info.version = PQgetvalue(version_res, 0, 0);
            } else {
                // If no version is found in the versions table, use a default
                info.version = "";
            }
            PQclear(version_res);
            info.version_count = 1; // For this implementation, use 1

            // Hidden child renditions (files only; a directory's children are
            // not renditions, so leave 0).
            info.rendition_count = 0;
            if (!is_container) {
                std::string rc_query = "SELECT COUNT(*) FROM \"" + schema_name +
                                       "\".files WHERE parent_uid = $1 AND deleted = FALSE;";
                const char* rc_params[1] = {info.uid.c_str()};
                PGresult* rc_res = PQexecParams(pg_conn, rc_query.c_str(), 1, nullptr, rc_params, nullptr, nullptr, 0);
                if (PQresultStatus(rc_res) == PGRES_TUPLES_OK && PQntuples(rc_res) > 0) {
                    info.rendition_count = std::stoi(PQgetvalue(rc_res, 0, 0));
                }
                PQclear(rc_res);
            }

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

Result<void> Database::update_file_parent(const std::string& uid, const std::string& new_parent_uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    std::string update_sql = "UPDATE \"" + schema_name + "\".files SET parent_uid = $2 WHERE uid = $1;";
    const char* param_values[2] = {uid.c_str(), new_parent_uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, update_sql.c_str(), 2, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
        int rows_affected = std::stoi(PQcmdTuples(res));
        if (rows_affected == 0) {
            PQclear(res);
            connection_pool_->release(conn);
            return Result<void>::err("File with UID not found: " + uid);
        }
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::ok();
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to update file parent: " + error);
    }
}

Result<std::string> Database::path_to_uid(const std::string& path, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::string>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Since we don't have a 'path' column in the schema, this operation isn't supported
    // In a real system, you'd either store paths or implement a traversal mechanism
    PQclear(PQexec(pg_conn, "SELECT 1")); // Just to use the conn before releasing
    connection_pool_->release(conn);
    return Result<std::string>::err("Path-to-UID mapping not supported with current schema. Use UID-based operations instead.");
}

Result<std::vector<std::string>> Database::uid_to_path(const std::string& uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<std::string>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    // Since we don't have a 'path' column in the schema, we need to return a constructed path
    // For now, just return a simple path based on the file name
    std::string query_sql = "SELECT name FROM \"" + schema_name + "\".files WHERE uid = $1 AND deleted = FALSE;";
    const char* param_values[1] = {uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql.c_str(), 1, nullptr, param_values, nullptr, nullptr, 0);

    std::vector<std::string> paths;
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int nrows = PQntuples(res);
        for (int i = 0; i < nrows; ++i) {
            if (PQgetvalue(res, i, 0) != nullptr) {
                std::string name = PQgetvalue(res, i, 0);
                paths.push_back("/" + name);  // Construct a simple path
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

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    // Insert the version into the versions table with its storage path
    // Keep the version_timestamp as a string to avoid conversion issues
    std::string insert_sql = "INSERT INTO \"" + schema_name + "\".versions (file_uid, version_timestamp, size, storage_path) "
                             "VALUES ($1, $2, $3, $4) "
                             "ON CONFLICT (file_uid, version_timestamp) DO UPDATE SET "
                             "size = EXCLUDED.size, storage_path = EXCLUDED.storage_path "
                             "RETURNING id;";
    const char* param_values[4] = {
        file_uid.c_str(),
        version_timestamp.c_str(),  // Keep as string
        std::to_string(size).c_str(),
        storage_path.c_str()
    };

    PGresult* res = PQexecParams(pg_conn, insert_sql.c_str(), 4, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        if (PQntuples(res) > 0) {
            int64_t id = std::stoll(PQgetvalue(res, 0, 0));
            PQclear(res);
            connection_pool_->release(conn);
            return Result<int64_t>::ok(id);
        } else {
            PQclear(res);
            connection_pool_->release(conn);
            return Result<int64_t>::err("Failed to insert version record");
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

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    // Query the versions table to get the storage path for this specific version
    std::string query_sql = "SELECT storage_path FROM \"" + schema_name + "\".versions WHERE file_uid = $1 AND version_timestamp = $2 LIMIT 1;";
    const char* param_values[2] = {file_uid.c_str(), version_timestamp.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql.c_str(), 2, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        if (PQntuples(res) > 0) {
            std::string storage_path = PQgetvalue(res, 0, 0);
            PQclear(res);
            connection_pool_->release(conn);
            return Result<std::optional<std::string>>::ok(storage_path);
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

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    // Query the versions table to get all versions for this file
    std::string query_sql = "SELECT version_timestamp FROM \"" + schema_name + "\".versions WHERE file_uid = $1 ORDER BY version_timestamp DESC;";
    const char* param_values[1] = {file_uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql.c_str(), 1, nullptr, param_values, nullptr, nullptr, 0);

    std::vector<std::string> versions;
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int nrows = PQntuples(res);
        for (int i = 0; i < nrows; ++i) {
            std::string version_timestamp = PQgetvalue(res, i, 0);
            versions.push_back(version_timestamp);
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

Result<bool> Database::delete_version(const std::string& file_uid, const std::string& version_timestamp, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<bool>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();
    std::string schema_name = get_schema_prefix(tenant);

    std::string sql = "DELETE FROM \"" + schema_name + "\".versions WHERE file_uid = $1 AND version_timestamp = $2;";
    const char* param_values[2] = {file_uid.c_str(), version_timestamp.c_str()};

    PGresult* res = PQexecParams(pg_conn, sql.c_str(), 2, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
        int rows_affected = std::stoi(PQcmdTuples(res));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<bool>::ok(rows_affected > 0);
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<bool>::err("Failed to delete version: " + error);
    }
}

Result<bool> Database::restore_to_version(const std::string& file_uid, const std::string& version_timestamp, const std::string& user, const std::string& tenant) {
    // "Current version" is whichever versions row has the max version_timestamp
    // for this file_uid. To restore, insert a new versions row with a fresh
    // timestamp pointing at the requested version's storage_path. The
    // existing payload is reused (no re-encrypt / re-compress) and the new
    // row's timestamp wins on read.
    (void)user; // accepted for API parity; the FS layer already enforced WRITE permission
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<bool>::err("Failed to acquire database connection for restore operation");
    }
    PGconn* pg_conn = conn->get_connection();
    std::string schema = get_schema_prefix(tenant);

    // 1. Look up the source version's storage_path + size.
    std::string select_sql = "SELECT storage_path, size FROM \"" + schema + "\".versions "
                             "WHERE file_uid = $1 AND version_timestamp = $2 LIMIT 1;";
    const char* select_params[2] = { file_uid.c_str(), version_timestamp.c_str() };
    PGresult* sel = PQexecParams(pg_conn, select_sql.c_str(), 2, nullptr, select_params, nullptr, nullptr, 0);
    if (PQresultStatus(sel) != PGRES_TUPLES_OK) {
        std::string error = "Failed to look up version: " + std::string(PQerrorMessage(pg_conn));
        PQclear(sel);
        connection_pool_->release(conn);
        return Result<bool>::err(error);
    }
    if (PQntuples(sel) == 0) {
        PQclear(sel);
        connection_pool_->release(conn);
        return Result<bool>::err("Version " + version_timestamp + " not found for file " + file_uid);
    }
    std::string source_path = PQgetvalue(sel, 0, 0);
    std::string size_str = PQgetvalue(sel, 0, 1);
    PQclear(sel);

    // 2. Generate a new version_timestamp and insert a new row pointing at
    //    the same storage_path. The Utils helper format is used elsewhere;
    //    inline a compatible one here to avoid a new dependency.
    // UTC, matching Utils::get_timestamp_string used by the rest of the codebase.
    // Mixing local-time and UTC would break the "current = max(timestamp)" ordering.
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    std::tm tm_buf{};
    gmtime_r(&t, &tm_buf);
    char ts_buf[40];
    std::snprintf(ts_buf, sizeof(ts_buf), "%04d%02d%02d_%02d%02d%02d.%03lld",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                  static_cast<long long>(ms.count()));
    std::string new_ts = ts_buf;

    std::string insert_sql = "INSERT INTO \"" + schema + "\".versions "
                             "(file_uid, version_timestamp, size, storage_path) "
                             "VALUES ($1, $2, $3, $4);";
    const char* insert_params[4] = {
        file_uid.c_str(), new_ts.c_str(), size_str.c_str(), source_path.c_str()
    };
    PGresult* ins = PQexecParams(pg_conn, insert_sql.c_str(), 4, nullptr, insert_params, nullptr, nullptr, 0);
    if (PQresultStatus(ins) != PGRES_COMMAND_OK) {
        std::string error = "Failed to insert restored version: " + std::string(PQerrorMessage(pg_conn));
        PQclear(ins);
        connection_pool_->release(conn);
        return Result<bool>::err(error);
    }
    PQclear(ins);

    // 3. Bump files.modified-at-equivalent so observers see a "change".
    std::string update_sql = "UPDATE \"" + schema + "\".files SET size = $2 WHERE uid = $1;";
    const char* update_params[2] = { file_uid.c_str(), size_str.c_str() };
    PGresult* upd = PQexecParams(pg_conn, update_sql.c_str(), 2, nullptr, update_params, nullptr, nullptr, 0);
    if (upd) PQclear(upd);  // non-fatal

    connection_pool_->release(conn);
    return Result<bool>::ok(true);
}

// Add all missing methods here
Result<void> Database::set_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& value, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Get the schema prefix for the tenant
    std::string schema_prefix = get_schema_prefix(tenant);

    // Escape the schema name to prevent SQL injection
    std::string escaped_schema = schema_prefix;
    std::replace(escaped_schema.begin(), escaped_schema.end(), '-', '_');
    std::replace(escaped_schema.begin(), escaped_schema.end(), '.', '_');
    std::replace(escaped_schema.begin(), escaped_schema.end(), ' ', '_');

    std::string sql = "INSERT INTO \"" + escaped_schema + "\".metadata (file_uid, version_timestamp, key_name, value) "
                      "VALUES ($1, $2, $3, $4) "
                      "ON CONFLICT (file_uid, version_timestamp, key_name) "
                      "DO UPDATE SET value = $4, created_at = CURRENT_TIMESTAMP;";

    const char* params[4] = {file_uid.c_str(), version_timestamp.c_str(), key.c_str(), value.c_str()};

    PGresult* res = PQexecParams(pg_conn, sql.c_str(), 4, nullptr, params, nullptr, nullptr, 0);

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

    // Get the schema prefix for the tenant
    std::string schema_prefix = get_schema_prefix(tenant);

    // Escape the schema name to prevent SQL injection
    std::string escaped_schema = schema_prefix;
    std::replace(escaped_schema.begin(), escaped_schema.end(), '-', '_');
    std::replace(escaped_schema.begin(), escaped_schema.end(), '.', '_');
    std::replace(escaped_schema.begin(), escaped_schema.end(), ' ', '_');

    std::string sql = "SELECT value FROM \"" + escaped_schema + "\".metadata WHERE file_uid = $1 AND version_timestamp = $2 AND key_name = $3;";
    const char* params[3] = {file_uid.c_str(), version_timestamp.c_str(), key.c_str()};

    PGresult* res = PQexecParams(pg_conn, sql.c_str(), 3, nullptr, params, nullptr, nullptr, 0);

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

    // Get the schema prefix for the tenant
    std::string schema_prefix = get_schema_prefix(tenant);

    // Escape the schema name to prevent SQL injection
    std::string escaped_schema = schema_prefix;
    std::replace(escaped_schema.begin(), escaped_schema.end(), '-', '_');
    std::replace(escaped_schema.begin(), escaped_schema.end(), '.', '_');
    std::replace(escaped_schema.begin(), escaped_schema.end(), ' ', '_');

    std::string sql = "SELECT key_name, value FROM \"" + escaped_schema + "\".metadata WHERE file_uid = $1 AND version_timestamp = $2;";
    const char* params[2] = {file_uid.c_str(), version_timestamp.c_str()};

    PGresult* res = PQexecParams(pg_conn, sql.c_str(), 2, nullptr, params, nullptr, nullptr, 0);

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

    // Get the schema prefix for the tenant
    std::string schema_prefix = get_schema_prefix(tenant);

    // Escape the schema name to prevent SQL injection
    std::string escaped_schema = schema_prefix;
    std::replace(escaped_schema.begin(), escaped_schema.end(), '-', '_');
    std::replace(escaped_schema.begin(), escaped_schema.end(), '.', '_');
    std::replace(escaped_schema.begin(), escaped_schema.end(), ' ', '_');

    std::string sql = "DELETE FROM \"" + escaped_schema + "\".metadata WHERE file_uid = $1 AND version_timestamp = $2 AND key_name = $3;";
    const char* params[3] = {file_uid.c_str(), version_timestamp.c_str(), key.c_str()};

    PGresult* res = PQexecParams(pg_conn, sql.c_str(), 3, nullptr, params, nullptr, nullptr, 0);

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

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);
    std::string sql = "SELECT uid FROM \"" + schema_name + "\".files WHERE is_deleted = FALSE ORDER BY modified_at ASC LIMIT " + std::to_string(limit) + ";";

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

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);
    std::string sql = "SELECT uid FROM \"" + schema_name + "\".files WHERE is_deleted = FALSE AND modified_at < (CURRENT_TIMESTAMP - INTERVAL '" + std::to_string(days_threshold) + " days') ORDER BY modified_at ASC;";

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

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);
    std::string sql = "SELECT COALESCE(SUM(size), 0) FROM \"" + schema_name + "\".files WHERE is_deleted = FALSE;";

    PGresult* res = PQexec(pg_conn, sql.c_str());

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
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection for tenant schema creation");
    }

    PGconn* pg_conn = conn->get_connection();

    // Create tenant schema if it doesn't exist - according to specifications
    std::string schema_name = get_schema_prefix(tenant); // Always use prefix to avoid conflicts with reserved keywords like "default"

    // Escape the schema name to prevent SQL injection
    std::string escaped_schema = schema_name;
    // Replace any special characters that might be problematic
    std::replace(escaped_schema.begin(), escaped_schema.end(), '-', '_');
    std::replace(escaped_schema.begin(), escaped_schema.end(), '.', '_');
    std::replace(escaped_schema.begin(), escaped_schema.end(), ' ', '_');

    // Create schema if it doesn't exist
    std::string create_schema_sql = "CREATE SCHEMA IF NOT EXISTS \"" + escaped_schema + "\";";
    PGresult* schema_res = PQexec(pg_conn, create_schema_sql.c_str());

    if (PQresultStatus(schema_res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(schema_res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to create tenant schema: " + error);
    }

    PQclear(schema_res);

    // Create the files table with structure specified in documentation
    std::string create_files_table = "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".files ("
        "id BIGSERIAL PRIMARY KEY, "
        "uid VARCHAR(64) UNIQUE NOT NULL, "
        "name TEXT NOT NULL, "
        "parent_uid VARCHAR(64), "
        "size BIGINT, "
        "owner TEXT NOT NULL, "
        "permission_map INTEGER NOT NULL, "
        "is_container BOOLEAN NOT NULL, "
        "deleted BOOLEAN NOT NULL DEFAULT FALSE"
        ");";

    std::string create_idx_uid = "CREATE INDEX IF NOT EXISTS idx_files_uid_" + escaped_schema +
        " ON \"" + escaped_schema + "\".files(uid);";
    std::string create_idx_parent_uid = "CREATE INDEX IF NOT EXISTS idx_files_parent_uid_" + escaped_schema +
        " ON \"" + escaped_schema + "\".files(parent_uid);";

    std::string create_versions_table = "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".versions ("
        "id BIGSERIAL PRIMARY KEY, "
        "file_uid VARCHAR(64) NOT NULL, "
        "version_timestamp TEXT NOT NULL, "
        "size BIGINT NOT NULL, "
        "storage_path TEXT NOT NULL, "
        "UNIQUE (file_uid, version_timestamp) "
        ");";

    std::string create_idx_versions = "CREATE INDEX IF NOT EXISTS idx_versions_file_uid_" + escaped_schema +
        " ON \"" + escaped_schema + "\".versions(file_uid);";

    std::string create_metadata_table = "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".metadata ("
        "id BIGSERIAL PRIMARY KEY, "
        "file_uid VARCHAR(64) NOT NULL, "
        "version_timestamp TEXT NOT NULL, "
        "key_name TEXT NOT NULL, "
        "value TEXT NOT NULL, "
        "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
        "UNIQUE (file_uid, version_timestamp, key_name) "
        ");";

    std::string create_idx_metadata = "CREATE INDEX IF NOT EXISTS idx_metadata_file_uid_" + escaped_schema +
        " ON \"" + escaped_schema + "\".metadata(file_uid);";
    std::string create_idx_metadata_key = "CREATE INDEX IF NOT EXISTS idx_metadata_key_name_" + escaped_schema +
        " ON \"" + escaped_schema + "\".metadata(key_name);";

    // Execute all the statements
    PGresult* res = PQexec(pg_conn, create_files_table.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to create tenant files table: " + error);
    }
    PQclear(res);

    res = PQexec(pg_conn, create_idx_uid.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); } // Index creation failure is non-critical

    res = PQexec(pg_conn, create_idx_parent_uid.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); } // Index creation failure is non-critical

    res = PQexec(pg_conn, create_versions_table.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to create tenant versions table: " + error);
    }
    PQclear(res);

    res = PQexec(pg_conn, create_idx_versions.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); } // Index creation failure is non-critical

    res = PQexec(pg_conn, create_metadata_table.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to create tenant metadata table: " + error);
    }
    PQclear(res);

    res = PQexec(pg_conn, create_idx_metadata.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); } // Index creation failure is non-critical

    res = PQexec(pg_conn, create_idx_metadata_key.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); } // Index creation failure is non-critical

    // ACL + RBAC tables. Created here (not lazily in add_acl) so a freshly
    // initialized tenant can be queried for ACLs / roles before any write.
    //
    // The UNIQUE constraint is NAMED so we can later migrate it idempotently
    // (Postgres has no IF NOT EXISTS for constraints).
    std::string create_acls_table =
        "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".acls ("
        "    id BIGSERIAL PRIMARY KEY,"
        "    resource_uid VARCHAR(64) NOT NULL,"
        "    principal VARCHAR(255) NOT NULL,"
        "    principal_type INTEGER NOT NULL,"
        "    permissions INTEGER NOT NULL,"
        "    granted_by VARCHAR(255),"
        "    effect INTEGER NOT NULL DEFAULT 0,"
        "    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "    CONSTRAINT acls_principal_effect UNIQUE(resource_uid, principal, principal_type, effect)"
        ");";
    res = PQexec(pg_conn, create_acls_table.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to create tenant acls table: " + error);
    }
    PQclear(res);

    // Migration: legacy tenants created before Phase 5/6 won't have granted_by
    // or effect. ADD COLUMN IF NOT EXISTS is idempotent and cheap.
    std::string add_granted_by =
        "ALTER TABLE \"" + escaped_schema + "\".acls "
        "ADD COLUMN IF NOT EXISTS granted_by VARCHAR(255);";
    res = PQexec(pg_conn, add_granted_by.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); }
    else { PQclear(res); }

    std::string add_effect =
        "ALTER TABLE \"" + escaped_schema + "\".acls "
        "ADD COLUMN IF NOT EXISTS effect INTEGER NOT NULL DEFAULT 0;";
    res = PQexec(pg_conn, add_effect.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); }
    else { PQclear(res); }

    // Constraint migration: legacy tenants have an anonymous UNIQUE on
    // (resource_uid, principal, principal_type) that prevents adding a DENY
    // row for the same principal. Drop any pre-existing unique that isn't
    // our named one, then ensure the named (..., effect) version exists.
    std::string drop_legacy_unique =
        "DO $$ DECLARE c text; BEGIN "
        "  SELECT conname INTO c FROM pg_constraint "
        "   WHERE conrelid = ('\"" + escaped_schema + "\".acls')::regclass "
        "     AND contype = 'u' "
        "     AND conname <> 'acls_principal_effect' "
        "   LIMIT 1; "
        "  IF c IS NOT NULL THEN "
        "    EXECUTE format('ALTER TABLE \"" + escaped_schema + "\".acls DROP CONSTRAINT %I', c); "
        "  END IF; "
        "END $$;";
    res = PQexec(pg_conn, drop_legacy_unique.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); }
    else { PQclear(res); }

    std::string add_named_unique =
        "DO $$ BEGIN "
        "  ALTER TABLE \"" + escaped_schema + "\".acls "
        "  ADD CONSTRAINT acls_principal_effect UNIQUE(resource_uid, principal, principal_type, effect); "
        "EXCEPTION WHEN duplicate_object THEN NULL; "
        "END $$;";
    res = PQexec(pg_conn, add_named_unique.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); }
    else { PQclear(res); }

    // Audit table records every grant and revoke. permissions_before and
    // permissions_after are the masks on the acls row immediately before and
    // after the operation (after = 0 means the row was deleted).
    std::string create_acl_audit_table =
        "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".acl_audit ("
        "    id BIGSERIAL PRIMARY KEY,"
        "    resource_uid VARCHAR(64) NOT NULL,"
        "    principal VARCHAR(255) NOT NULL,"
        "    principal_type INTEGER NOT NULL,"
        "    action VARCHAR(16) NOT NULL,"   // 'grant' | 'revoke'
        "    permissions_before INTEGER,"
        "    permissions_after INTEGER,"
        "    performed_by VARCHAR(255),"
        "    performed_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ");";
    res = PQexec(pg_conn, create_acl_audit_table.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to create tenant acl_audit table: " + error);
    }
    PQclear(res);

    std::string create_idx_acl_audit_resource =
        "CREATE INDEX IF NOT EXISTS idx_acl_audit_resource_" + escaped_schema +
        " ON \"" + escaped_schema + "\".acl_audit(resource_uid, performed_at);";
    res = PQexec(pg_conn, create_idx_acl_audit_resource.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); }

    // The UNIQUE constraint covers (resource_uid, principal, principal_type) for
    // per-resource lookups. Add a (principal, principal_type) index so
    // "all ACLs for principal X" queries don't scan the whole table.
    std::string create_idx_acls_principal =
        "CREATE INDEX IF NOT EXISTS idx_acls_principal_type_" + escaped_schema +
        " ON \"" + escaped_schema + "\".acls(principal, principal_type);";
    res = PQexec(pg_conn, create_idx_acls_principal.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); }

    // Drop the legacy single-column principal index (created by the old lazy
    // path in add_acl) if it exists — the new composite index supersedes it.
    std::string drop_legacy_idx =
        "DROP INDEX IF EXISTS \"" + escaped_schema + "\".idx_acls_principal;";
    res = PQexec(pg_conn, drop_legacy_idx.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); }
    // The legacy idx_acls_resource_uid (also created by old lazy path) is
    // redundant with the UNIQUE constraint's index; drop it too.
    std::string drop_legacy_idx_resource =
        "DROP INDEX IF EXISTS \"" + escaped_schema + "\".idx_acls_resource_uid;";
    res = PQexec(pg_conn, drop_legacy_idx_resource.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); }

    std::string create_roles_table =
        "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".roles ("
        "    id BIGSERIAL PRIMARY KEY,"
        "    role_name VARCHAR(255) UNIQUE NOT NULL,"
        "    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ");";
    res = PQexec(pg_conn, create_roles_table.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to create tenant roles table: " + error);
    }
    PQclear(res);

    std::string create_user_roles_table =
        "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".user_roles ("
        "    id BIGSERIAL PRIMARY KEY,"
        "    user_name VARCHAR(255) NOT NULL,"
        "    role_name VARCHAR(255) NOT NULL,"
        "    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "    UNIQUE(user_name, role_name)"
        ");";
    res = PQexec(pg_conn, create_user_roles_table.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to create tenant user_roles table: " + error);
    }
    PQclear(res);

    std::string create_idx_user_roles_user =
        "CREATE INDEX IF NOT EXISTS idx_user_roles_user_" + escaped_schema +
        " ON \"" + escaped_schema + "\".user_roles(user_name);";
    res = PQexec(pg_conn, create_idx_user_roles_user.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); }

    std::string create_idx_user_roles_role =
        "CREATE INDEX IF NOT EXISTS idx_user_roles_role_" + escaped_schema +
        " ON \"" + escaped_schema + "\".user_roles(role_name);";
    res = PQexec(pg_conn, create_idx_user_roles_role.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); }

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
    res = PQexec(pg_conn, check_root_sql.c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to check for existing root directory: " + error);
    }

    int root_exists = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);

    if (root_exists == 0) {
        // Root doesn't exist, so create it as per specification
        std::string insert_root_sql = "INSERT INTO \"" + escaped_schema + "\".files "
            "(uid, name, parent_uid, size, owner, permission_map, is_container, deleted) VALUES ("
            + escape_string(root_uid, pg_conn) + ", "
            + escape_string(root_name, pg_conn) + ", "
            + escape_string(root_parent_uid, pg_conn) + ", "
            + std::to_string(root_size) + ", "
            + escape_string(root_owner, pg_conn) + ", "
            + std::to_string(root_permission_map) + ", "
            + (root_is_container ? "TRUE" : "FALSE") + ", "
            + (root_deleted ? "TRUE" : "FALSE") + ")";

        res = PQexec(pg_conn, insert_root_sql.c_str());
        if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK) {
            std::string error = PQerrorMessage(pg_conn);
            PQclear(res);
            connection_pool_->release(conn);
            return Result<void>::err("Failed to create root directory: " + error);
        }
        PQclear(res);

        // No bootstrap ACL rows for the empty-uid root resource. The
        // filesystem-root auto-read special case in grpc_service.h grants
        // READ access there regardless of ACLs, and mkdir at root is gated
        // by the system_admin role check in FileSystem::mkdir. The legacy
        // 'system'/'root'/'other' inserts that used to live here used a
        // 3-column ON CONFLICT that became invalid in Phase 6 (the named
        // constraint is on 4 columns including effect), and they aborted
        // create_tenant_schema before the public.tenants registration ran.
    }

    // Register the tenant in the global tenants table for multi-tenant sync
    std::string tenant_id_to_register = tenant.empty() ? "default" : tenant;
    std::string register_tenant_sql = "INSERT INTO tenants (tenant_id, schema_name, created_at, updated_at) "
        "VALUES (" + escape_string(tenant_id_to_register, pg_conn) + ", " +
        escape_string(escaped_schema, pg_conn) + ", CURRENT_TIMESTAMP, CURRENT_TIMESTAMP) "
        "ON CONFLICT (tenant_id) DO UPDATE SET updated_at = CURRENT_TIMESTAMP;";

    res = PQexec(pg_conn, register_tenant_sql.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        // Non-fatal: the tenant's own schema works whether or not it lands
        // in the global registry. But log loudly so this doesn't hide again.
        SERVER_LOG_WARN("Database::create_tenant_schema",
                        "Failed to register tenant '" + tenant_id_to_register +
                        "' in public.tenants: " + std::string(PQerrorMessage(pg_conn)));
    }
    PQclear(res);

    connection_pool_->release(conn);

    return Result<void>::ok();
}

Result<bool> Database::tenant_schema_exists(const std::string& tenant) {
    if (tenant.empty()) {
        return Result<bool>::err("Cannot check existence for empty tenant name");
    }

    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<bool>::err("Failed to acquire database connection for tenant schema check");
    }

    PGconn* pg_conn = conn->get_connection();

    // Use the schema prefix for consistency
    std::string schema_name = get_schema_prefix(tenant);

    // Escape the schema name to prevent SQL injection
    std::string escaped_schema = schema_name;
    std::replace(escaped_schema.begin(), escaped_schema.end(), '-', '_');
    std::replace(escaped_schema.begin(), escaped_schema.end(), '.', '_');
    std::replace(escaped_schema.begin(), escaped_schema.end(), ' ', '_');

    // Check if the schema exists
    std::string schema_check_sql = "SELECT EXISTS(SELECT 1 FROM information_schema.schemata WHERE schema_name = '" + escaped_schema + "');";
    PGresult* check_res = PQexec(pg_conn, schema_check_sql.c_str());

    if (PQresultStatus(check_res) != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(check_res);
        connection_pool_->release(conn);
        return Result<bool>::err("Failed to check tenant schema existence: " + error);
    }

    bool schema_exists = false;
    if (PQntuples(check_res) > 0 && PQnfields(check_res) > 0) {
        const char* result = PQgetvalue(check_res, 0, 0);
        std::string result_str(result ? result : "");
        schema_exists = (result_str == "t" || result_str == "1" || result_str == "true");
    }

    PQclear(check_res);
    connection_pool_->release(conn);

    return Result<bool>::ok(schema_exists);
}

Result<void> Database::cleanup_tenant_data(const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();
    std::string schema = get_schema_prefix(tenant);

    // Begin transaction for atomic cleanup
    PGresult* begin_res = PQexec(pg_conn, "BEGIN;");
    if (PQresultStatus(begin_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to begin transaction: " + std::string(PQerrorMessage(pg_conn));
        PQclear(begin_res);
        connection_pool_->release(conn);
        return Result<void>::err(error);
    }
    PQclear(begin_res);

    // Drop the tenant schema (CASCADE will remove all tables in it)
    std::string drop_schema_sql = "DROP SCHEMA IF EXISTS " + schema + " CASCADE;";
    PGresult* drop_res = PQexec(pg_conn, drop_schema_sql.c_str());
    if (PQresultStatus(drop_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to drop tenant schema: " + std::string(PQerrorMessage(pg_conn));
        PQclear(drop_res);
        PQexec(pg_conn, "ROLLBACK;");
        connection_pool_->release(conn);
        return Result<void>::err(error);
    }
    PQclear(drop_res);

    // Remove tenant from global tenants registry
    std::string tenant_id_to_remove = tenant.empty() ? "default" : tenant;
    std::string delete_tenant_sql = "DELETE FROM tenants WHERE tenant_id = $1;";
    const char* param_values[1] = {tenant_id_to_remove.c_str()};

    PGresult* delete_res = PQexecParams(pg_conn, delete_tenant_sql.c_str(), 1, nullptr, param_values, nullptr, nullptr, 0);
    if (PQresultStatus(delete_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to remove tenant from registry: " + std::string(PQerrorMessage(pg_conn));
        PQclear(delete_res);
        PQexec(pg_conn, "ROLLBACK;");
        connection_pool_->release(conn);
        return Result<void>::err(error);
    }
    PQclear(delete_res);

    // Commit transaction
    PGresult* commit_res = PQexec(pg_conn, "COMMIT;");
    if (PQresultStatus(commit_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to commit transaction: " + std::string(PQerrorMessage(pg_conn));
        PQclear(commit_res);
        PQexec(pg_conn, "ROLLBACK;");
        connection_pool_->release(conn);
        return Result<void>::err(error);
    }
    PQclear(commit_res);

    connection_pool_->release(conn);
    return Result<void>::ok();
}

Result<std::vector<std::string>> Database::list_tenants() {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<std::string>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = "SELECT tenant_id FROM tenants ORDER BY tenant_id;";
    PGresult* res = PQexec(pg_conn, query_sql);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = "Failed to list tenants: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<std::string>>::err(error);
    }

    std::vector<std::string> tenant_ids;
    int nrows = PQntuples(res);
    for (int i = 0; i < nrows; ++i) {
        tenant_ids.push_back(PQgetvalue(res, i, 0));
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::vector<std::string>>::ok(tenant_ids);
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
            if (!is_connected() && primary_available_.load()) {
                // Primary just went down -> enter disconnected read-only fallback
                // mode. Writes are rejected at the gRPC layer (is_server_in_readonly_mode);
                // reads serve from the configured secondary (REPLICATION_FAILOVER.md).
                primary_available_.store(false);
                if (!secondary_conn_info_.empty()) {
                    using_secondary_.store(true);
                }
                ConnectionPoolManager::get_instance().set_server_in_readonly_mode(true);
                std::cerr << "Database primary unavailable; entering read-only fallback mode."
                          << std::endl;
            } else if (!primary_available_.load()) {
                // Primary is down -> keep probing; resume normal operation on success.
                if (connect()) {
                    primary_available_.store(true);
                    using_secondary_.store(false);
                    ConnectionPoolManager::get_instance().set_server_in_readonly_mode(false);
                    std::cout << "Database connection to primary restored; resuming normal operation."
                              << std::endl;
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
    // Always prefix with "tenant_" to avoid conflicts with reserved keywords like "default"
    if (tenant.empty()) {
        return "tenant_default";  // Use tenant_default instead of just "default"
    }
    return validate_schema_name("tenant_" + tenant);
}

Result<std::string> Database::create_file_with_acls(const std::string& uid,
                                                     const std::string& name,
                                                     const std::string& path,
                                                     const std::string& parent_uid,
                                                     FileType type,
                                                     const std::string& owner,
                                                     int permissions,
                                                     const std::vector<AclGrant>& acl_grants,
                                                     const std::string& tenant) {
    (void)path; // path is implicit in (parent_uid, name); kept for API parity with insert_file
    if (name.empty()) {
        return Result<std::string>::err("Invalid parameter: name is empty");
    }

    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::string>::err("Failed to acquire database connection");
    }
    PGconn* pg_conn = conn->get_connection();
    std::string schema = get_schema_prefix(tenant);

    auto rollback_and_fail = [&](const std::string& msg) -> Result<std::string> {
        PGresult* rb = PQexec(pg_conn, "ROLLBACK;");
        if (rb) PQclear(rb);
        connection_pool_->release(conn);
        return Result<std::string>::err(msg);
    };

    // BEGIN — wraps the file insert and all ACL writes so a crash mid-way
    // can't leave a file without its ACLs (plan §6.2).
    PGresult* begin_res = PQexec(pg_conn, "BEGIN;");
    if (PQresultStatus(begin_res) != PGRES_COMMAND_OK) {
        std::string err = "Failed to BEGIN transaction: " + std::string(PQerrorMessage(pg_conn));
        PQclear(begin_res);
        connection_pool_->release(conn);
        return Result<std::string>::err(err);
    }
    PQclear(begin_res);

    // 1. INSERT file row (same SQL shape as insert_file).
    std::string insert_file_sql =
        "INSERT INTO \"" + schema + "\".files (uid, name, parent_uid, size, owner, permission_map, is_container, deleted) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8) "
        "ON CONFLICT (uid) DO UPDATE SET "
            "name = EXCLUDED.name, "
            "parent_uid = EXCLUDED.parent_uid, "
            "size = EXCLUDED.size, "
            "owner = EXCLUDED.owner, "
            "permission_map = EXCLUDED.permission_map, "
            "is_container = EXCLUDED.is_container "
        "RETURNING uid;";

    bool is_container = (type == FileType::DIRECTORY);
    std::string size_str = "0";
    std::string perms_str = std::to_string(permissions);
    std::string container_str = is_container ? "TRUE" : "FALSE";
    std::string deleted_str = "FALSE";

    const char* file_params[8] = {
        uid.c_str(), name.c_str(), parent_uid.c_str(),
        size_str.c_str(), owner.c_str(), perms_str.c_str(),
        container_str.c_str(), deleted_str.c_str()
    };
    PGresult* file_res = PQexecParams(pg_conn, insert_file_sql.c_str(), 8, nullptr,
                                      file_params, nullptr, nullptr, 0);
    if (PQresultStatus(file_res) != PGRES_TUPLES_OK && PQresultStatus(file_res) != PGRES_COMMAND_OK) {
        std::string err = "Failed to insert file: " + std::string(PQerrorMessage(pg_conn));
        PQclear(file_res);
        return rollback_and_fail(err);
    }
    PQclear(file_res);

    // 2. Apply every ACL grant. Each one upserts the acls row and writes an
    //    audit row, all inside the same transaction.
    std::string acl_upsert_sql =
        "INSERT INTO " + schema + ".acls (resource_uid, principal, principal_type, permissions, granted_by, effect) "
        "VALUES ($1, $2, $3, $4, NULLIF($5, ''), $6) "
        "ON CONFLICT ON CONSTRAINT acls_principal_effect "
        "DO UPDATE SET permissions = " + schema + ".acls.permissions | ($4::int), "
        "              granted_by = NULLIF($5, ''), updated_at = CURRENT_TIMESTAMP;";

    std::string audit_sql =
        "INSERT INTO " + schema + ".acl_audit "
        "(resource_uid, principal, principal_type, action, permissions_before, permissions_after, performed_by) "
        "VALUES ($1, $2, $3, $4, 0, $5, NULLIF($6, ''));";

    for (const auto& g : acl_grants) {
        std::string g_type = std::to_string(g.type);
        std::string g_perms = std::to_string(g.permissions);
        std::string g_effect = std::to_string(g.effect);

        const char* acl_params[6] = {
            uid.c_str(), g.principal.c_str(), g_type.c_str(),
            g_perms.c_str(), g.performed_by.c_str(), g_effect.c_str()
        };
        PGresult* acl_res = PQexecParams(pg_conn, acl_upsert_sql.c_str(), 6, nullptr,
                                         acl_params, nullptr, nullptr, 0);
        if (PQresultStatus(acl_res) != PGRES_COMMAND_OK) {
            std::string err = "Failed to insert ACL during create_file_with_acls: "
                              + std::string(PQerrorMessage(pg_conn));
            PQclear(acl_res);
            return rollback_and_fail(err);
        }
        PQclear(acl_res);

        std::string action = (g.effect == 1) ? "grant_deny" : "grant";
        const char* audit_params[6] = {
            uid.c_str(), g.principal.c_str(), g_type.c_str(),
            action.c_str(), g_perms.c_str(), g.performed_by.c_str()
        };
        PGresult* audit_res = PQexecParams(pg_conn, audit_sql.c_str(), 6, nullptr,
                                           audit_params, nullptr, nullptr, 0);
        if (PQresultStatus(audit_res) != PGRES_COMMAND_OK) {
            // Audit failure is non-fatal per plan §5 (audit trail), but we
            // already committed nothing — keep policy consistent: log and
            // continue. We're still inside the transaction; commit the rest.
            PQclear(audit_res);
        } else {
            PQclear(audit_res);
        }
    }

    // 3. COMMIT.
    PGresult* commit_res = PQexec(pg_conn, "COMMIT;");
    if (PQresultStatus(commit_res) != PGRES_COMMAND_OK) {
        std::string err = "Failed to COMMIT: " + std::string(PQerrorMessage(pg_conn));
        PQclear(commit_res);
        return rollback_and_fail(err);
    }
    PQclear(commit_res);

    connection_pool_->release(conn);
    return Result<std::string>::ok(uid);
}

// ACL operations implementations
Result<void> Database::add_acl(const std::string& resource_uid, const std::string& principal,
                               int type, int permissions,
                               const std::string& tenant,
                               const std::string& performed_by,
                               int effect) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();
    // The acls table is created at tenant init time (see create_tenant_schema).
    std::string schema = get_schema_prefix(tenant);

    std::string type_str = std::to_string(type);
    std::string perms_str = std::to_string(permissions);
    std::string effect_str = std::to_string(effect);

    // Capture the prior permissions bitmask for the (principal, type, effect)
    // row so the audit log can record before/after deltas.
    std::string select_before_sql =
        "SELECT permissions FROM " + schema + ".acls "
        "WHERE resource_uid = $1 AND principal = $2 AND principal_type = ($3::int) AND effect = ($4::int);";
    const char* before_params[4] = {
        resource_uid.c_str(), principal.c_str(), type_str.c_str(), effect_str.c_str() };
    int permissions_before = 0;
    bool had_row = false;
    PGresult* before_res = PQexecParams(pg_conn, select_before_sql.c_str(), 4,
                                        nullptr, before_params, nullptr, nullptr, 0);
    if (PQresultStatus(before_res) == PGRES_TUPLES_OK && PQntuples(before_res) > 0) {
        permissions_before = atoi(PQgetvalue(before_res, 0, 0));
        had_row = true;
    }
    PQclear(before_res);

    std::string insert_sql =
        "INSERT INTO " + schema + ".acls (resource_uid, principal, principal_type, permissions, granted_by, effect) "
        "VALUES ($1, $2, $3, $4, NULLIF($5, ''), $6) "
        "ON CONFLICT ON CONSTRAINT acls_principal_effect "
        "DO UPDATE SET permissions = " + schema + ".acls.permissions | ($4::int), "
        "              granted_by = NULLIF($5, ''), updated_at = CURRENT_TIMESTAMP;";

    const char* param_values[6] = {
        resource_uid.c_str(),
        principal.c_str(),
        type_str.c_str(),
        perms_str.c_str(),
        performed_by.c_str(),
        effect_str.c_str()
    };

    PGresult* res = PQexecParams(pg_conn, insert_sql.c_str(), 6, nullptr, param_values, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to add ACL: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err(error);
    }
    PQclear(res);

    // Audit row. Failures here are non-fatal — never fail an ACL change just
    // because the audit log couldn't be written; the caller has bigger problems.
    // The action label includes the effect so allow/deny grants are distinct.
    std::string action_label = (effect == 1) ? "grant_deny" : "grant";
    std::string audit_sql =
        "INSERT INTO " + schema + ".acl_audit "
        "(resource_uid, principal, principal_type, action, permissions_before, permissions_after, performed_by) "
        "VALUES ($1, $2, $3, $4, $5, $6, NULLIF($7, ''));";
    int before_value = had_row ? permissions_before : 0;
    int after_value = before_value | permissions;  // grants OR the bits in
    std::string before_str = std::to_string(before_value);
    std::string after_str = std::to_string(after_value);
    const char* audit_params[7] = {
        resource_uid.c_str(),
        principal.c_str(),
        type_str.c_str(),
        action_label.c_str(),
        before_str.c_str(),
        after_str.c_str(),
        performed_by.c_str()
    };
    PGresult* audit_res = PQexecParams(pg_conn, audit_sql.c_str(), 7, nullptr, audit_params, nullptr, nullptr, 0);
    if (PQresultStatus(audit_res) != PGRES_COMMAND_OK) { PQclear(audit_res); }
    else { PQclear(audit_res); }

    connection_pool_->release(conn);
    return Result<void>::ok();
}

Result<void> Database::remove_acl(const std::string& resource_uid, const std::string& principal,
                                  int type, int permissions,
                                  const std::string& tenant,
                                  const std::string& performed_by,
                                  int effect) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();
    std::string schema = get_schema_prefix(tenant);

    std::string type_str = std::to_string(type);
    std::string perms_str = std::to_string(permissions);
    std::string effect_str = std::to_string(effect);

    // Capture the prior permissions bitmask for the audit row.
    std::string select_before_sql =
        "SELECT permissions FROM " + schema + ".acls "
        "WHERE resource_uid = $1 AND principal = $2 AND principal_type = ($3::int) AND effect = ($4::int);";
    const char* before_params[4] = {
        resource_uid.c_str(), principal.c_str(), type_str.c_str(), effect_str.c_str() };
    int permissions_before = 0;
    bool had_row = false;
    PGresult* before_res = PQexecParams(pg_conn, select_before_sql.c_str(), 4,
                                        nullptr, before_params, nullptr, nullptr, 0);
    if (PQresultStatus(before_res) == PGRES_TUPLES_OK && PQntuples(before_res) > 0) {
        permissions_before = atoi(PQgetvalue(before_res, 0, 0));
        had_row = true;
    }
    PQclear(before_res);

    // Clear the bits in `permissions` from the row's existing bitmask, then
    // delete the row if it would be left with no bits. Single CTE = single
    // transaction.
    // Two-statement transaction: UPDATE (clear bits) then DELETE the row if
    // it has no bits left. CTE-based variants of this had subtle issues with
    // RETURNING visibility, so do it explicitly. Explicit ::int casts because
    // libpq sends bind params as text and `~` can't pick an overload otherwise.
    std::string update_sql =
        "UPDATE " + schema + ".acls "
        "SET permissions = permissions & ~($4::int), updated_at = CURRENT_TIMESTAMP "
        "WHERE resource_uid = $1 AND principal = $2 AND principal_type = ($3::int) AND effect = ($5::int);";
    // DELETE uses 4 params (no perms mask) — referencing an unused $4 makes
    // Postgres fail with "could not determine data type of parameter $4".
    std::string delete_sql =
        "DELETE FROM " + schema + ".acls "
        "WHERE resource_uid = $1 AND principal = $2 AND principal_type = ($3::int) AND effect = ($4::int) "
        "  AND permissions = 0;";

    const char* update_params[5] = {
        resource_uid.c_str(),
        principal.c_str(),
        type_str.c_str(),
        perms_str.c_str(),
        effect_str.c_str()
    };
    const char* delete_params[4] = {
        resource_uid.c_str(),
        principal.c_str(),
        type_str.c_str(),
        effect_str.c_str()
    };

    // Wrap UPDATE + conditional DELETE in a transaction so an observer never
    // sees the intermediate "permissions = 0" state.
    PGresult* begin_res = PQexec(pg_conn, "BEGIN;");
    if (PQresultStatus(begin_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to BEGIN remove_acl transaction: " + std::string(PQerrorMessage(pg_conn));
        PQclear(begin_res);
        connection_pool_->release(conn);
        return Result<void>::err(error);
    }
    PQclear(begin_res);

    auto rollback_and_fail = [&](const std::string& msg) {
        PGresult* rb = PQexec(pg_conn, "ROLLBACK;");
        if (rb) PQclear(rb);
        connection_pool_->release(conn);
        return Result<void>::err(msg);
    };

    PGresult* res = PQexecParams(pg_conn, update_sql.c_str(), 5, nullptr, update_params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to update ACL bits in remove_acl: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        return rollback_and_fail(error);
    }
    PQclear(res);

    res = PQexecParams(pg_conn, delete_sql.c_str(), 4, nullptr, delete_params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to delete zeroed ACL row in remove_acl: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        return rollback_and_fail(error);
    }
    PQclear(res);

    PGresult* commit_res = PQexec(pg_conn, "COMMIT;");
    if (PQresultStatus(commit_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to COMMIT remove_acl: " + std::string(PQerrorMessage(pg_conn));
        PQclear(commit_res);
        return rollback_and_fail(error);
    }
    PQclear(commit_res);

    // Only audit if there was a row to act on.
    if (had_row) {
        int permissions_after = permissions_before & ~permissions;
        std::string after_str = std::to_string(permissions_after);
        std::string before_str = std::to_string(permissions_before);
        std::string action_label = (effect == 1) ? "revoke_deny" : "revoke";
        std::string audit_sql =
            "INSERT INTO " + schema + ".acl_audit "
            "(resource_uid, principal, principal_type, action, permissions_before, permissions_after, performed_by) "
            "VALUES ($1, $2, $3, $4, $5, $6, NULLIF($7, ''));";
        const char* audit_params[7] = {
            resource_uid.c_str(),
            principal.c_str(),
            type_str.c_str(),
            action_label.c_str(),
            before_str.c_str(),
            after_str.c_str(),
            performed_by.c_str()
        };
        PGresult* audit_res = PQexecParams(pg_conn, audit_sql.c_str(), 7, nullptr, audit_params, nullptr, nullptr, 0);
        if (PQresultStatus(audit_res) != PGRES_COMMAND_OK) { PQclear(audit_res); }
        else { PQclear(audit_res); }
    }

    connection_pool_->release(conn);
    return Result<void>::ok();
}

Result<std::vector<IDatabase::AclEntry>> Database::get_acls_for_resource(const std::string& resource_uid,
                                                                         const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<IDatabase::AclEntry>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Get tenant-specific schema prefix
    std::string schema = get_schema_prefix(tenant);

    std::string query_sql =
        "SELECT resource_uid, principal, principal_type, permissions, effect "
        "FROM " + schema + ".acls WHERE resource_uid = $1;";
    const char* param_values[1] = {resource_uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql.c_str(), 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = "Failed to get ACLs for resource: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<IDatabase::AclEntry>>::err(error);
    }

    std::vector<IDatabase::AclEntry> acls;
    int nrows = PQntuples(res);
    for (int i = 0; i < nrows; ++i) {
        IDatabase::AclEntry entry;
        entry.resource_uid = PQgetvalue(res, i, 0);
        entry.principal = PQgetvalue(res, i, 1);
        entry.type = std::stoi(PQgetvalue(res, i, 2));
        entry.permissions = std::stoi(PQgetvalue(res, i, 3));
        entry.effect = std::stoi(PQgetvalue(res, i, 4));

        acls.push_back(entry);
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::vector<IDatabase::AclEntry>>::ok(acls);
}

Result<std::vector<IDatabase::AclEntry>> Database::get_user_acls(const std::string& resource_uid,
                                                                 const std::string& principal,
                                                                 int type,
                                                                 const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<IDatabase::AclEntry>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Get tenant-specific schema prefix
    std::string schema = get_schema_prefix(tenant);

    // Filter by principal_type so user/role/group names in the same namespace
    // are not conflated (e.g. a user "alice" and a role "alice").
    std::string query_sql =
        "SELECT resource_uid, principal, principal_type, permissions, effect "
        "FROM " + schema + ".acls "
        "WHERE resource_uid = $1 AND principal = $2 AND principal_type = $3;";
    std::string type_str = std::to_string(type);
    const char* param_values[3] = {resource_uid.c_str(), principal.c_str(), type_str.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql.c_str(), 3, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = "Failed to get user ACLs: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<IDatabase::AclEntry>>::err(error);
    }

    std::vector<IDatabase::AclEntry> acls;
    int nrows = PQntuples(res);
    for (int i = 0; i < nrows; ++i) {
        IDatabase::AclEntry entry;
        entry.resource_uid = PQgetvalue(res, i, 0);
        entry.principal = PQgetvalue(res, i, 1);
        entry.type = std::stoi(PQgetvalue(res, i, 2));
        entry.permissions = std::stoi(PQgetvalue(res, i, 3));
        entry.effect = std::stoi(PQgetvalue(res, i, 4));

        acls.push_back(entry);
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::vector<IDatabase::AclEntry>>::ok(acls);
}

Result<std::vector<std::string>> Database::list_claims(const std::string& prefix,
                                                       int limit,
                                                       const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<std::string>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();
    std::string schema = get_schema_prefix(tenant);

    // principal_type 4 == PrincipalType::CLAIM (ABAC "key=value" principals).
    std::string sql = "SELECT DISTINCT principal FROM " + schema + ".acls WHERE principal_type = 4";

    std::string pattern;
    if (!prefix.empty()) {
        // Escape LIKE metacharacters so the prefix matches literally.
        for (char c : prefix) {
            if (c == '\\' || c == '%' || c == '_') pattern += '\\';
            pattern += c;
        }
        pattern += '%';
        sql += " AND principal ILIKE $1";
    }
    sql += " ORDER BY principal";
    if (limit > 0) sql += " LIMIT " + std::to_string(limit);

    PGresult* res;
    if (!pattern.empty()) {
        const char* pv[1] = {pattern.c_str()};
        res = PQexecParams(pg_conn, sql.c_str(), 1, nullptr, pv, nullptr, nullptr, 0);
    } else {
        res = PQexecParams(pg_conn, sql.c_str(), 0, nullptr, nullptr, nullptr, nullptr, 0);
    }

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = "Failed to list claims: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<std::string>>::err(error);
    }

    std::vector<std::string> claims;
    int nrows = PQntuples(res);
    for (int i = 0; i < nrows; ++i) claims.push_back(PQgetvalue(res, i, 0));

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::vector<std::string>>::ok(claims);
}

// Role management implementations
// roles and user_roles tables are created at tenant init time by
// create_tenant_schema. Local role definitions persisted here are UNIONed
// with request-supplied roles from AuthenticationContext at permission-check
// time (see AclManager::check_permission).
Result<void> Database::create_role(const std::string& role, const std::string& tenant) {
    if (role.empty()) {
        return Result<void>::err("Role name cannot be empty");
    }
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }
    PGconn* pg_conn = conn->get_connection();
    std::string schema = get_schema_prefix(tenant);

    std::string sql = "INSERT INTO " + schema + ".roles (role_name) VALUES ($1) "
                      "ON CONFLICT (role_name) DO NOTHING;";
    const char* params[1] = { role.c_str() };
    PGresult* res = PQexecParams(pg_conn, sql.c_str(), 1, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to create role: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err(error);
    }
    PQclear(res);
    connection_pool_->release(conn);
    return Result<void>::ok();
}

Result<void> Database::delete_role(const std::string& role, const std::string& tenant) {
    if (role.empty()) {
        return Result<void>::err("Role name cannot be empty");
    }
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }
    PGconn* pg_conn = conn->get_connection();
    std::string schema = get_schema_prefix(tenant);

    // Deleting a role removes its user_roles mappings; ACL grants that named
    // this role become orphaned (they remain in the acls table but match no
    // assigned user). The caller can clean those up via revoke_permission.
    std::string sql_user_roles = "DELETE FROM " + schema + ".user_roles WHERE role_name = $1;";
    std::string sql_roles = "DELETE FROM " + schema + ".roles WHERE role_name = $1;";
    const char* params[1] = { role.c_str() };

    PGresult* res = PQexecParams(pg_conn, sql_user_roles.c_str(), 1, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to clear role assignments: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err(error);
    }
    PQclear(res);

    res = PQexecParams(pg_conn, sql_roles.c_str(), 1, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to delete role: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err(error);
    }
    PQclear(res);
    connection_pool_->release(conn);
    return Result<void>::ok();
}

Result<void> Database::assign_user_to_role(const std::string& user, const std::string& role, const std::string& tenant) {
    if (user.empty() || role.empty()) {
        return Result<void>::err("User and role names cannot be empty");
    }
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }
    PGconn* pg_conn = conn->get_connection();
    std::string schema = get_schema_prefix(tenant);

    std::string sql = "INSERT INTO " + schema + ".user_roles (user_name, role_name) VALUES ($1, $2) "
                      "ON CONFLICT (user_name, role_name) DO NOTHING;";
    const char* params[2] = { user.c_str(), role.c_str() };
    PGresult* res = PQexecParams(pg_conn, sql.c_str(), 2, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to assign user to role: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err(error);
    }
    PQclear(res);
    connection_pool_->release(conn);
    return Result<void>::ok();
}

Result<void> Database::remove_user_from_role(const std::string& user, const std::string& role, const std::string& tenant) {
    if (user.empty() || role.empty()) {
        return Result<void>::err("User and role names cannot be empty");
    }
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }
    PGconn* pg_conn = conn->get_connection();
    std::string schema = get_schema_prefix(tenant);

    std::string sql = "DELETE FROM " + schema + ".user_roles WHERE user_name = $1 AND role_name = $2;";
    const char* params[2] = { user.c_str(), role.c_str() };
    PGresult* res = PQexecParams(pg_conn, sql.c_str(), 2, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to remove user from role: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err(error);
    }
    PQclear(res);
    connection_pool_->release(conn);
    return Result<void>::ok();
}

Result<std::vector<std::string>> Database::get_roles_for_user(const std::string& user, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<std::string>>::err("Failed to acquire database connection");
    }
    PGconn* pg_conn = conn->get_connection();
    std::string schema = get_schema_prefix(tenant);

    std::string sql = "SELECT role_name FROM " + schema + ".user_roles WHERE user_name = $1;";
    const char* params[1] = { user.c_str() };
    PGresult* res = PQexecParams(pg_conn, sql.c_str(), 1, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = "Failed to get roles for user: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<std::string>>::err(error);
    }
    std::vector<std::string> roles;
    int rows = PQntuples(res);
    roles.reserve(rows);
    for (int i = 0; i < rows; ++i) {
        roles.emplace_back(PQgetvalue(res, i, 0));
    }
    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::vector<std::string>>::ok(roles);
}

Result<std::vector<std::string>> Database::get_users_for_role(const std::string& role, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<std::string>>::err("Failed to acquire database connection");
    }
    PGconn* pg_conn = conn->get_connection();
    std::string schema = get_schema_prefix(tenant);

    std::string sql = "SELECT user_name FROM " + schema + ".user_roles WHERE role_name = $1;";
    const char* params[1] = { role.c_str() };
    PGresult* res = PQexecParams(pg_conn, sql.c_str(), 1, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = "Failed to get users for role: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<std::string>>::err(error);
    }
    std::vector<std::string> users;
    int rows = PQntuples(res);
    users.reserve(rows);
    for (int i = 0; i < rows; ++i) {
        users.emplace_back(PQgetvalue(res, i, 0));
    }
    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::vector<std::string>>::ok(users);
}

Result<std::vector<std::string>> Database::get_all_roles(const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<std::string>>::err("Failed to acquire database connection");
    }
    PGconn* pg_conn = conn->get_connection();
    std::string schema = get_schema_prefix(tenant);

    std::string sql = "SELECT role_name FROM " + schema + ".roles ORDER BY role_name;";
    PGresult* res = PQexec(pg_conn, sql.c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = "Failed to get all roles: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<std::string>>::err(error);
    }
    std::vector<std::string> roles;
    int rows = PQntuples(res);
    roles.reserve(rows);
    for (int i = 0; i < rows; ++i) {
        roles.emplace_back(PQgetvalue(res, i, 0));
    }
    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::vector<std::string>>::ok(roles);
}

} // namespace fileengine