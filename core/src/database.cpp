#include "fileengine/database.h"
#include "fileengine/utils.h"
#include "fileengine/server_logger.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <chrono>

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
            info.version = Utils::get_timestamp_string(); // Use current timestamp as version
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

    std::string query_sql = "SELECT uid, name, size, owner, permission_map, is_container "
                            "FROM \"" + schema_name + "\".files "
                            "WHERE parent_uid = $1 AND deleted = FALSE "
                            "ORDER BY name;";
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
            // Use current time for timestamps since we don't have these in the schema
            auto now = std::chrono::system_clock::now();
            info.created_at = now;
            info.modified_at = now;
            info.version = Utils::get_timestamp_string(); // Use current timestamp as version
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

    std::string query_sql = "SELECT uid, name, size, owner, permission_map, is_container, deleted "
                            "FROM \"" + schema_name + "\".files "
                            "WHERE parent_uid = $1 "
                            "ORDER BY name;";
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
            // Use current time for timestamps since we don't have these in the schema
            auto now = std::chrono::system_clock::now();
            info.created_at = now;
            info.modified_at = now;
            info.version = Utils::get_timestamp_string(); // Use current timestamp as version
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
            info.version = Utils::get_timestamp_string(); // Use current timestamp as version
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
            info.version = Utils::get_timestamp_string(); // Use current timestamp as version
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
            info.version = Utils::get_timestamp_string(); // Use current timestamp as version
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
            info.version = Utils::get_timestamp_string(); // Use current timestamp as version
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

    // In the current schema, we don't store separate version info
    // For this implementation, we'll just return the current version timestamp
    std::string query_sql = "SELECT uid FROM \"" + schema_name + "\".files WHERE uid = $1;";
    const char* param_values[1] = {file_uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql.c_str(), 1, nullptr, param_values, nullptr, nullptr, 0);

    std::vector<std::string> versions;
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int nrows = PQntuples(res);
        if (nrows > 0) {
            // Return a single current version timestamp
            versions.push_back(Utils::get_timestamp_string()); // Current timestamp as the "version"
        }
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<std::string>>::ok(versions);
    } else {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<std::string>>::err("Failed to check file existence for versions: " + error);
    }
}

Result<bool> Database::restore_to_version(const std::string& file_uid, const std::string& version_timestamp, const std::string& user, const std::string& tenant) {
    // Since we don't have a current_version column in the schema, this operation is not supported
    // The version functionality would need to be implemented differently in the current schema
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<bool>::err("Failed to acquire database connection for restore operation");
    }

    // Just return an error as versioning isn't supported with the current schema
    connection_pool_->release(conn);
    return Result<bool>::err("Restore to version not supported with current schema. Versioning needs to be implemented differently.");
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
        "storage_path TEXT NOT NULL "
        ");";

    std::string create_idx_versions = "CREATE INDEX IF NOT EXISTS idx_versions_file_uid_" + escaped_schema +
        " ON \"" + escaped_schema + "\".versions(file_uid);";

    std::string create_metadata_table = "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".metadata ("
        "id BIGSERIAL PRIMARY KEY, "
        "file_uid VARCHAR(64) NOT NULL, "
        "version_timestamp TEXT NOT NULL, "
        "key_name TEXT NOT NULL, "
        "value TEXT NOT NULL, "
        "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP "
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
    }

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
    // Always prefix with "tenant_" to avoid conflicts with reserved keywords like "default"
    if (tenant.empty()) {
        return "tenant_default";  // Use tenant_default instead of just "default"
    }
    return validate_schema_name("tenant_" + tenant);
}

// ACL operations implementations
Result<void> Database::add_acl(const std::string& resource_uid, const std::string& principal,
                               int type, int permissions, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Create ACL table if it doesn't exist
    const char* create_acl_table_sql = R"SQL(
        CREATE TABLE IF NOT EXISTS acls (
            id BIGSERIAL PRIMARY KEY,
            resource_uid VARCHAR(64) NOT NULL,
            principal VARCHAR(255) NOT NULL,
            principal_type INTEGER NOT NULL,
            permissions INTEGER NOT NULL,
            created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(resource_uid, principal, principal_type)
        );

        CREATE INDEX IF NOT EXISTS idx_acls_resource_uid ON acls(resource_uid);
        CREATE INDEX IF NOT EXISTS idx_acls_principal ON acls(principal);
    )SQL";

    PGresult* create_res = PQexec(pg_conn, create_acl_table_sql);
    if (PQresultStatus(create_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to create ACL table: " + std::string(PQerrorMessage(pg_conn));
        PQclear(create_res);
        connection_pool_->release(conn);
        return Result<void>::err(error);
    }
    PQclear(create_res);

    const char* insert_sql = R"SQL(
        INSERT INTO acls (resource_uid, principal, principal_type, permissions)
        VALUES ($1, $2, $3, $4)
        ON CONFLICT (resource_uid, principal, principal_type)
        DO UPDATE SET permissions = $4, updated_at = CURRENT_TIMESTAMP;
    )SQL";

    const char* param_values[4] = {
        resource_uid.c_str(),
        principal.c_str(),
        std::to_string(type).c_str(),
        std::to_string(permissions).c_str()
    };

    PGresult* res = PQexecParams(pg_conn, insert_sql, 4, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to add ACL: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err(error);
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<void>::ok();
}

Result<void> Database::remove_acl(const std::string& resource_uid, const std::string& principal,
                                  int type, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* delete_sql = "DELETE FROM acls WHERE resource_uid = $1 AND principal = $2 AND principal_type = $3;";
    const char* param_values[3] = {
        resource_uid.c_str(),
        principal.c_str(),
        std::to_string(type).c_str()
    };

    PGresult* res = PQexecParams(pg_conn, delete_sql, 3, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to remove ACL: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err(error);
    }

    PQclear(res);
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

    const char* query_sql = "SELECT resource_uid, principal, principal_type, permissions FROM acls WHERE resource_uid = $1;";
    const char* param_values[1] = {resource_uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

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

        acls.push_back(entry);
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::vector<IDatabase::AclEntry>>::ok(acls);
}

Result<std::vector<IDatabase::AclEntry>> Database::get_user_acls(const std::string& resource_uid,
                                                                 const std::string& principal,
                                                                 const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<IDatabase::AclEntry>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = "SELECT resource_uid, principal, principal_type, permissions FROM acls WHERE resource_uid = $1 AND principal = $2;";
    const char* param_values[2] = {resource_uid.c_str(), principal.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 2, nullptr, param_values, nullptr, nullptr, 0);

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

        acls.push_back(entry);
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::vector<IDatabase::AclEntry>>::ok(acls);
}

} // namespace fileengine