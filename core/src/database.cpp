#include "fileengine/database.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>

namespace fileengine {

Database::Database(const std::string& host, int port, const std::string& dbname,
                   const std::string& user, const std::string& password, int pool_size)
    : connection_pool_(std::make_shared<ConnectionPool>(host, port, dbname, user, password, pool_size)) {
    hostname_ = host;
}

Database::~Database() {
    disconnect();
}

bool Database::connect() {
    return connection_pool_->initialize();
}

void Database::disconnect() {
    connection_pool_->shutdown();
}

bool Database::is_connected() const {
    auto conn = connection_pool_->acquire();
    if (!conn) return false;
    
    bool connected = conn->is_valid();
    connection_pool_->release(conn);
    return connected;
}

Result<void> Database::create_schema() {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }
    
    PGconn* pg_conn = conn->get_connection();
    
    // Create tables for files, versions, and metadata
    const char* create_files_sql = R"(
        CREATE TABLE IF NOT EXISTS files (
            uid VARCHAR(36) PRIMARY KEY,
            name VARCHAR(255) NOT NULL,
            path TEXT,
            parent_uid VARCHAR(36),
            type INTEGER NOT NULL,
            owner VARCHAR(255) NOT NULL,
            permissions INTEGER NOT NULL,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            modified_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            current_version VARCHAR(30),
            is_deleted BOOLEAN DEFAULT FALSE
        );
        
        CREATE INDEX IF NOT EXISTS idx_files_parent ON files(parent_uid);
        CREATE INDEX IF NOT EXISTS idx_files_path ON files(path);
        CREATE INDEX IF NOT EXISTS idx_files_owner ON files(owner);
        CREATE INDEX IF NOT EXISTS idx_files_deleted ON files(is_deleted);
    )";
    
    const char* create_versions_sql = R"(
        CREATE TABLE IF NOT EXISTS versions (
            file_uid VARCHAR(36) NOT NULL,
            version_timestamp VARCHAR(30) NOT NULL,
            size BIGINT NOT NULL,
            storage_path TEXT NOT NULL,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            PRIMARY KEY (file_uid, version_timestamp)
        );
        
        CREATE INDEX IF NOT EXISTS idx_versions_file ON versions(file_uid);
    )";
    
    const char* create_metadata_sql = R"(
        CREATE TABLE IF NOT EXISTS metadata (
            file_uid VARCHAR(36) NOT NULL,
            version_timestamp VARCHAR(30) NOT NULL,
            key_name VARCHAR(255) NOT NULL,
            value TEXT,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            PRIMARY KEY (file_uid, version_timestamp, key_name)
        );
        
        CREATE INDEX IF NOT EXISTS idx_metadata_file_version ON metadata(file_uid, version_timestamp);
    )";
    
    PGresult* res = PQexec(pg_conn, create_files_sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to create files table: " + std::string(PQerrorMessage(pg_conn)));
    }
    PQclear(res);
    
    res = PQexec(pg_conn, create_versions_sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to create versions table: " + std::string(PQerrorMessage(pg_conn)));
    }
    PQclear(res);
    
    res = PQexec(pg_conn, create_metadata_sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to create metadata table: " + std::string(PQerrorMessage(pg_conn)));
    }
    PQclear(res);
    
    connection_pool_->release(conn);
    return Result<void>::ok();
}

Result<void> Database::drop_schema() {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }
    
    PGconn* pg_conn = conn->get_connection();
    
    // Drop tables in reverse dependency order
    const char* drop_metadata_sql = "DROP TABLE IF EXISTS metadata;";
    const char* drop_versions_sql = "DROP TABLE IF EXISTS versions;";
    const char* drop_files_sql = "DROP TABLE IF EXISTS files;";
    
    PGresult* res = PQexec(pg_conn, drop_metadata_sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to drop metadata table: " + std::string(PQerrorMessage(pg_conn)));
    }
    PQclear(res);
    
    res = PQexec(pg_conn, drop_versions_sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to drop versions table: " + std::string(PQerrorMessage(pg_conn)));
    }
    PQclear(res);
    
    res = PQexec(pg_conn, drop_files_sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to drop files table: " + std::string(PQerrorMessage(pg_conn)));
    }
    PQclear(res);
    
    connection_pool_->release(conn);
    return Result<void>::ok();
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
    
    // Prepare the SQL with parameters
    const char* insert_sql = "INSERT INTO files (uid, name, path, parent_uid, type, owner, permissions) VALUES ($1, $2, $3, $4, $5, $6, $7);";
    
    const char* param_values[7];
    param_values[0] = uid.c_str();
    param_values[1] = name.c_str();
    param_values[2] = path.c_str();
    param_values[3] = parent_uid.empty() ? nullptr : parent_uid.c_str();
    param_values[4] = std::to_string(static_cast<int>(type)).c_str();
    param_values[5] = owner.c_str();
    param_values[6] = std::to_string(permissions).c_str();
    
    PGresult* res = PQexecParams(pg_conn, insert_sql, 7, nullptr, param_values, nullptr, nullptr, 0);
    
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::string>::err("Failed to insert file: " + std::string(PQerrorMessage(pg_conn)));
    }
    
    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::string>::ok(uid);
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
    
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to update file modification time: " + std::string(PQerrorMessage(pg_conn)));
    }
    
    PQclear(res);
    connection_pool_->release(conn);
    return Result<void>::ok();
}

Result<std::optional<FileInfo>> Database::get_file_by_uid(const std::string& uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::optional<FileInfo>>::err("Failed to acquire database connection");
    }
    
    PGconn* pg_conn = conn->get_connection();
    
    const char* query_sql = R"(
        SELECT uid, name, path, parent_uid, type, owner, permissions, created_at, modified_at, current_version
        FROM files WHERE uid = $1 AND is_deleted = FALSE;
    )";
    
    const char* param_values[1] = {uid.c_str()};
    
    PGresult* res = PQexecParams(pg_conn, query_sql, 1, nullptr, param_values, nullptr, nullptr, 0);
    
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::err("Failed to query file: " + std::string(PQerrorMessage(pg_conn)));
    }
    
    if (PQntuples(res) == 0) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::ok(std::nullopt);
    }
    
    // Parse result
    FileInfo fileInfo;
    fileInfo.uid = PQgetvalue(res, 0, 0);
    fileInfo.name = PQgetvalue(res, 0, 1);
    fileInfo.path = PQgetvalue(res, 0, 2);
    fileInfo.parent_uid = PQgetvalue(res, 0, 3) ? PQgetvalue(res, 0, 3) : "";
    fileInfo.type = static_cast<FileType>(std::stoi(PQgetvalue(res, 0, 4)));
    fileInfo.owner = PQgetvalue(res, 0, 5);
    fileInfo.permissions = std::stoi(PQgetvalue(res, 0, 6));
    
    // For simplicity in this example, we'll set some default values for times
    fileInfo.size = 0; // This would normally come from version info or storage
    fileInfo.version = PQgetvalue(res, 0, 9) ? PQgetvalue(res, 0, 9) : "";
    
    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::optional<FileInfo>>::ok(fileInfo);
}

Result<bool> Database::delete_file(const std::string& uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<bool>::err("Failed to acquire database connection");
    }
    
    PGconn* pg_conn = conn->get_connection();
    
    const char* delete_sql = "UPDATE files SET is_deleted = TRUE WHERE uid = $1;";
    const char* param_values[1] = {uid.c_str()};
    
    PGresult* res = PQexecParams(pg_conn, delete_sql, 1, nullptr, param_values, nullptr, nullptr, 0);
    
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<bool>::err("Failed to delete file: " + std::string(PQerrorMessage(pg_conn)));
    }
    
    bool success = atoi(PQcmdTuples(res)) > 0;
    PQclear(res);
    connection_pool_->release(conn);
    return Result<bool>::ok(success);
}

Result<std::vector<FileInfo>> Database::list_files_in_directory(const std::string& parent_uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<FileInfo>>::err("Failed to acquire database connection");
    }
    
    PGconn* pg_conn = conn->get_connection();
    
    const char* query_sql;
    const char* param_values[1];
    int param_count = 1;
    
    if (parent_uid.empty()) {
        // List root directory (files with no parent or empty parent)
        query_sql = R"(
            SELECT uid, name, path, parent_uid, type, owner, permissions, created_at, modified_at, current_version
            FROM files WHERE (parent_uid IS NULL OR parent_uid = '') AND is_deleted = FALSE;
        )";
        param_values[0] = nullptr;
        param_count = 0;
    } else {
        query_sql = R"(
            SELECT uid, name, path, parent_uid, type, owner, permissions, created_at, modified_at, current_version
            FROM files WHERE parent_uid = $1 AND is_deleted = FALSE;
        )";
        param_values[0] = parent_uid.c_str();
    }
    
    PGresult* res = PQexecParams(pg_conn, query_sql, param_count, nullptr, param_values, nullptr, nullptr, 0);
    
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<FileInfo>>::err("Failed to list directory: " + std::string(PQerrorMessage(pg_conn)));
    }
    
    std::vector<FileInfo> files;
    int nrows = PQntuples(res);
    
    for (int i = 0; i < nrows; ++i) {
        FileInfo fileInfo;
        fileInfo.uid = PQgetvalue(res, i, 0);
        fileInfo.name = PQgetvalue(res, i, 1);
        fileInfo.path = PQgetvalue(res, i, 2);
        fileInfo.parent_uid = PQgetvalue(res, i, 3) ? PQgetvalue(res, i, 3) : "";
        fileInfo.type = static_cast<FileType>(std::stoi(PQgetvalue(res, i, 4)));
        fileInfo.owner = PQgetvalue(res, i, 5);
        fileInfo.permissions = std::stoi(PQgetvalue(res, i, 6));
        fileInfo.size = 0; // This would normally come from version info or storage
        fileInfo.version = PQgetvalue(res, i, 9) ? PQgetvalue(res, i, 9) : "";
        
        files.push_back(fileInfo);
    }
    
    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::vector<FileInfo>>::ok(files);
}

Result<void> Database::update_file_current_version(const std::string& uid, const std::string& version_timestamp, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* update_sql = "UPDATE files SET current_version = $2 WHERE uid = $1;";
    const char* param_values[2] = {uid.c_str(), version_timestamp.c_str()};

    PGresult* res = PQexecParams(pg_conn, update_sql, 2, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to update file current version: " + std::string(PQerrorMessage(pg_conn)));
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<void>::ok();
}

Result<bool> Database::undelete_file(const std::string& uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<bool>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* update_sql = "UPDATE files SET is_deleted = FALSE WHERE uid = $1;";
    const char* param_values[1] = {uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, update_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<bool>::err("Failed to undelete file: " + std::string(PQerrorMessage(pg_conn)));
    }

    bool success = atoi(PQcmdTuples(res)) > 0;
    PQclear(res);
    connection_pool_->release(conn);
    return Result<bool>::ok(success);
}

Result<std::optional<FileInfo>> Database::get_file_by_path(const std::string& path, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::optional<FileInfo>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = R"(
        SELECT uid, name, path, parent_uid, type, owner, permissions, created_at, modified_at, current_version
        FROM files WHERE path = $1 AND is_deleted = FALSE;
    )";

    const char* param_values[1] = {path.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::err("Failed to query file by path: " + std::string(PQerrorMessage(pg_conn)));
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::ok(std::nullopt);
    }

    // Parse result
    FileInfo fileInfo;
    fileInfo.uid = PQgetvalue(res, 0, 0);
    fileInfo.name = PQgetvalue(res, 0, 1);
    fileInfo.path = PQgetvalue(res, 0, 2);
    fileInfo.parent_uid = PQgetvalue(res, 0, 3) ? PQgetvalue(res, 0, 3) : "";
    fileInfo.type = static_cast<FileType>(std::stoi(PQgetvalue(res, 0, 4)));
    fileInfo.owner = PQgetvalue(res, 0, 5);
    fileInfo.permissions = std::stoi(PQgetvalue(res, 0, 6));
    fileInfo.size = 0; // This would normally come from version info or storage
    fileInfo.version = PQgetvalue(res, 0, 9) ? PQgetvalue(res, 0, 9) : "";

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::optional<FileInfo>>::ok(fileInfo);
}

Result<void> Database::update_file_name(const std::string& uid, const std::string& new_name, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* update_sql = "UPDATE files SET name = $2 WHERE uid = $1;";
    const char* param_values[2] = {uid.c_str(), new_name.c_str()};

    PGresult* res = PQexecParams(pg_conn, update_sql, 2, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to update file name: " + std::string(PQerrorMessage(pg_conn)));
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<void>::ok();
}

Result<std::vector<FileInfo>> Database::list_files_in_directory_with_deleted(const std::string& parent_uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<FileInfo>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql;
    const char* param_values[1];
    int param_count = 1;

    if (parent_uid.empty()) {
        // List root directory (files with no parent or empty parent)
        query_sql = R"(
            SELECT uid, name, path, parent_uid, type, owner, permissions, created_at, modified_at, current_version
            FROM files WHERE (parent_uid IS NULL OR parent_uid = '');
        )";
        param_values[0] = nullptr;
        param_count = 0;
    } else {
        query_sql = R"(
            SELECT uid, name, path, parent_uid, type, owner, permissions, created_at, modified_at, current_version
            FROM files WHERE parent_uid = $1;
        )";
        param_values[0] = parent_uid.c_str();
    }

    PGresult* res = PQexecParams(pg_conn, query_sql, param_count, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<FileInfo>>::err("Failed to list directory with deleted: " + std::string(PQerrorMessage(pg_conn)));
    }

    std::vector<FileInfo> files;
    int nrows = PQntuples(res);

    for (int i = 0; i < nrows; ++i) {
        FileInfo fileInfo;
        fileInfo.uid = PQgetvalue(res, i, 0);
        fileInfo.name = PQgetvalue(res, i, 1);
        fileInfo.path = PQgetvalue(res, i, 2);
        fileInfo.parent_uid = PQgetvalue(res, i, 3) ? PQgetvalue(res, i, 3) : "";
        fileInfo.type = static_cast<FileType>(std::stoi(PQgetvalue(res, i, 4)));
        fileInfo.owner = PQgetvalue(res, i, 5);
        fileInfo.permissions = std::stoi(PQgetvalue(res, i, 6));
        fileInfo.size = 0; // This would normally come from version info or storage
        fileInfo.version = PQgetvalue(res, i, 9) ? PQgetvalue(res, i, 9) : "";

        files.push_back(fileInfo);
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::vector<FileInfo>>::ok(files);
}

Result<std::optional<FileInfo>> Database::get_file_by_name_and_parent(const std::string& name, const std::string& parent_uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::optional<FileInfo>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql;
    const char* param_values[2];
    int param_count = 2;

    if (parent_uid.empty()) {
        query_sql = R"(
            SELECT uid, name, path, parent_uid, type, owner, permissions, created_at, modified_at, current_version
            FROM files WHERE name = $1 AND (parent_uid IS NULL OR parent_uid = '') AND is_deleted = FALSE;
        )";
        param_values[0] = name.c_str();
        param_values[1] = nullptr;
    } else {
        query_sql = R"(
            SELECT uid, name, path, parent_uid, type, owner, permissions, created_at, modified_at, current_version
            FROM files WHERE name = $1 AND parent_uid = $2 AND is_deleted = FALSE;
        )";
        param_values[0] = name.c_str();
        param_values[1] = parent_uid.c_str();
    }

    PGresult* res = PQexecParams(pg_conn, query_sql, param_count, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::err("Failed to query file by name and parent: " + std::string(PQerrorMessage(pg_conn)));
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::ok(std::nullopt);
    }

    // Parse result
    FileInfo fileInfo;
    fileInfo.uid = PQgetvalue(res, 0, 0);
    fileInfo.name = PQgetvalue(res, 0, 1);
    fileInfo.path = PQgetvalue(res, 0, 2);
    fileInfo.parent_uid = PQgetvalue(res, 0, 3) ? PQgetvalue(res, 0, 3) : "";
    fileInfo.type = static_cast<FileType>(std::stoi(PQgetvalue(res, 0, 4)));
    fileInfo.owner = PQgetvalue(res, 0, 5);
    fileInfo.permissions = std::stoi(PQgetvalue(res, 0, 6));
    fileInfo.size = 0; // This would normally come from version info or storage
    fileInfo.version = PQgetvalue(res, 0, 9) ? PQgetvalue(res, 0, 9) : "";

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::optional<FileInfo>>::ok(fileInfo);
}

Result<std::optional<FileInfo>> Database::get_file_by_name_and_parent_include_deleted(const std::string& name, const std::string& parent_uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::optional<FileInfo>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql;
    const char* param_values[2];
    int param_count = 2;

    if (parent_uid.empty()) {
        query_sql = R"(
            SELECT uid, name, path, parent_uid, type, owner, permissions, created_at, modified_at, current_version
            FROM files WHERE name = $1 AND (parent_uid IS NULL OR parent_uid = '');
        )";
        param_values[0] = name.c_str();
        param_values[1] = nullptr;
    } else {
        query_sql = R"(
            SELECT uid, name, path, parent_uid, type, owner, permissions, created_at, modified_at, current_version
            FROM files WHERE name = $1 AND parent_uid = $2;
        )";
        param_values[0] = name.c_str();
        param_values[1] = parent_uid.c_str();
    }

    PGresult* res = PQexecParams(pg_conn, query_sql, param_count, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::err("Failed to query file by name and parent (include deleted): " + std::string(PQerrorMessage(pg_conn)));
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::ok(std::nullopt);
    }

    // Parse result
    FileInfo fileInfo;
    fileInfo.uid = PQgetvalue(res, 0, 0);
    fileInfo.name = PQgetvalue(res, 0, 1);
    fileInfo.path = PQgetvalue(res, 0, 2);
    fileInfo.parent_uid = PQgetvalue(res, 0, 3) ? PQgetvalue(res, 0, 3) : "";
    fileInfo.type = static_cast<FileType>(std::stoi(PQgetvalue(res, 0, 4)));
    fileInfo.owner = PQgetvalue(res, 0, 5);
    fileInfo.permissions = std::stoi(PQgetvalue(res, 0, 6));
    fileInfo.size = 0; // This would normally come from version info or storage
    fileInfo.version = PQgetvalue(res, 0, 9) ? PQgetvalue(res, 0, 9) : "";

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::optional<FileInfo>>::ok(fileInfo);
}

Result<int64_t> Database::get_file_size(const std::string& file_uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<int64_t>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // First get the current version of the file
    const char* get_version_sql = "SELECT current_version FROM files WHERE uid = $1;";
    const char* param_values[1] = {file_uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, get_version_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<int64_t>::err("Failed to get file version: " + std::string(PQerrorMessage(pg_conn)));
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<int64_t>::err("File not found");
    }

    std::string version_timestamp = PQgetvalue(res, 0, 0);
    PQclear(res);

    // Now get the size from the versions table
    const char* get_size_sql = "SELECT size FROM versions WHERE file_uid = $1 AND version_timestamp = $2;";
    const char* size_param_values[2] = {file_uid.c_str(), version_timestamp.c_str()};

    res = PQexecParams(pg_conn, get_size_sql, 2, nullptr, size_param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<int64_t>::err("Failed to get file size: " + std::string(PQerrorMessage(pg_conn)));
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<int64_t>::ok(0); // File exists but no version data
    }

    int64_t size = std::stoll(PQgetvalue(res, 0, 0));
    PQclear(res);
    connection_pool_->release(conn);
    return Result<int64_t>::ok(size);
}

Result<int64_t> Database::get_directory_size(const std::string& dir_uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<int64_t>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Get all child files and directories recursively (simplified for this implementation)
    const char* query_sql = R"(
        WITH RECURSIVE children AS (
            SELECT uid, parent_uid, type FROM files WHERE parent_uid = $1 AND is_deleted = FALSE
            UNION ALL
            SELECT f.uid, f.parent_uid, f.type
            FROM files f
            INNER JOIN children c ON f.parent_uid = c.uid
            WHERE f.is_deleted = FALSE
        )
        SELECT SUM(v.size) as total_size
        FROM children c
        JOIN versions v ON c.uid = v.file_uid
        WHERE c.type = 0; -- Only sum sizes of regular files, not directories
    )";

    const char* param_values[1] = {dir_uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<int64_t>::err("Failed to get directory size: " + std::string(PQerrorMessage(pg_conn)));
    }

    if (PQntuples(res) == 0 || PQgetvalue(res, 0, 0) == nullptr) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<int64_t>::ok(0);
    }

    int64_t size = std::stoll(PQgetvalue(res, 0, 0));
    PQclear(res);
    connection_pool_->release(conn);
    return Result<int64_t>::ok(size);
}

Result<std::optional<FileInfo>> Database::get_file_by_uid_include_deleted(const std::string& uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::optional<FileInfo>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = R"(
        SELECT uid, name, path, parent_uid, type, owner, permissions, created_at, modified_at, current_version
        FROM files WHERE uid = $1;
    )";

    const char* param_values[1] = {uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::err("Failed to query file (include deleted): " + std::string(PQerrorMessage(pg_conn)));
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<FileInfo>>::ok(std::nullopt);
    }

    // Parse result
    FileInfo fileInfo;
    fileInfo.uid = PQgetvalue(res, 0, 0);
    fileInfo.name = PQgetvalue(res, 0, 1);
    fileInfo.path = PQgetvalue(res, 0, 2);
    fileInfo.parent_uid = PQgetvalue(res, 0, 3) ? PQgetvalue(res, 0, 3) : "";
    fileInfo.type = static_cast<FileType>(std::stoi(PQgetvalue(res, 0, 4)));
    fileInfo.owner = PQgetvalue(res, 0, 5);
    fileInfo.permissions = std::stoi(PQgetvalue(res, 0, 6));
    fileInfo.size = 0; // This would normally come from version info or storage
    fileInfo.version = PQgetvalue(res, 0, 9) ? PQgetvalue(res, 0, 9) : "";

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::optional<FileInfo>>::ok(fileInfo);
}

Result<std::string> Database::path_to_uid(const std::string& path, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::string>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = "SELECT uid FROM files WHERE path = $1;";
    const char* param_values[1] = {path.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::string>::err("Failed to query path to uid: " + std::string(PQerrorMessage(pg_conn)));
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::string>::err("Path not found");
    }

    std::string uid = PQgetvalue(res, 0, 0);
    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::string>::ok(uid);
}

Result<std::vector<std::string>> Database::uid_to_path(const std::string& uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<std::string>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = "SELECT path FROM files WHERE uid = $1;";
    const char* param_values[1] = {uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<std::string>>::err("Failed to query uid to path: " + std::string(PQerrorMessage(pg_conn)));
    }

    std::vector<std::string> paths;
    int nrows = PQntuples(res);

    for (int i = 0; i < nrows; ++i) {
        if (PQgetvalue(res, i, 0) != nullptr) {
            paths.push_back(PQgetvalue(res, i, 0));
        }
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::vector<std::string>>::ok(paths);
}

Result<int64_t> Database::insert_version(const std::string& file_uid, const std::string& version_timestamp,
                                        int64_t size, const std::string& storage_path, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<int64_t>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* insert_sql = "INSERT INTO versions (file_uid, version_timestamp, size, storage_path) VALUES ($1, $2, $3, $4);";
    const char* param_values[4] = {file_uid.c_str(), version_timestamp.c_str(), std::to_string(size).c_str(), storage_path.c_str()};

    PGresult* res = PQexecParams(pg_conn, insert_sql, 4, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<int64_t>::err("Failed to insert version: " + std::string(PQerrorMessage(pg_conn)));
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<int64_t>::ok(size);
}

Result<std::optional<std::string>> Database::get_version_storage_path(const std::string& file_uid, const std::string& version_timestamp, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::optional<std::string>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = "SELECT storage_path FROM versions WHERE file_uid = $1 AND version_timestamp = $2;";
    const char* param_values[2] = {file_uid.c_str(), version_timestamp.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 2, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<std::string>>::err("Failed to query version storage path: " + std::string(PQerrorMessage(pg_conn)));
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<std::string>>::ok(std::nullopt);
    }

    std::string path = PQgetvalue(res, 0, 0);
    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::optional<std::string>>::ok(path);
}

Result<std::vector<std::string>> Database::list_versions(const std::string& file_uid, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<std::string>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* query_sql = "SELECT version_timestamp FROM versions WHERE file_uid = $1 ORDER BY version_timestamp DESC;";
    const char* param_values[1] = {file_uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 1, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<std::string>>::err("Failed to list versions: " + std::string(PQerrorMessage(pg_conn)));
    }

    std::vector<std::string> versions;
    int nrows = PQntuples(res);

    for (int i = 0; i < nrows; ++i) {
        versions.push_back(PQgetvalue(res, i, 0));
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::vector<std::string>>::ok(versions);
}

Result<void> Database::set_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& value, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* upsert_sql = R"(
        INSERT INTO metadata (file_uid, version_timestamp, key_name, value)
        VALUES ($1, $2, $3, $4)
        ON CONFLICT (file_uid, version_timestamp, key_name)
        DO UPDATE SET value = $4, created_at = CURRENT_TIMESTAMP;
    )";

    const char* param_values[4] = {file_uid.c_str(), version_timestamp.c_str(), key.c_str(), value.c_str()};

    PGresult* res = PQexecParams(pg_conn, upsert_sql, 4, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to set metadata: " + std::string(PQerrorMessage(pg_conn)));
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

    const char* query_sql = "SELECT value FROM metadata WHERE file_uid = $1 AND version_timestamp = $2 AND key_name = $3;";
    const char* param_values[3] = {file_uid.c_str(), version_timestamp.c_str(), key.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 3, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::optional<std::string>>::err("Failed to query metadata: " + std::string(PQerrorMessage(pg_conn)));
    }

    if (PQntuples(res) == 0) {
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

    const char* query_sql = "SELECT key_name, value FROM metadata WHERE file_uid = $1 AND version_timestamp = $2;";
    const char* param_values[2] = {file_uid.c_str(), version_timestamp.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql, 2, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::map<std::string, std::string>>::err("Failed to query all metadata: " + std::string(PQerrorMessage(pg_conn)));
    }

    std::map<std::string, std::string> metadata;
    int nrows = PQntuples(res);

    for (int i = 0; i < nrows; ++i) {
        std::string key = PQgetvalue(res, i, 0);
        std::string value = PQgetvalue(res, i, 1);
        metadata[key] = value;
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::map<std::string, std::string>>::ok(metadata);
}

Result<void> Database::delete_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& tenant) {
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    const char* delete_sql = "DELETE FROM metadata WHERE file_uid = $1 AND version_timestamp = $2 AND key_name = $3;";
    const char* param_values[3] = {file_uid.c_str(), version_timestamp.c_str(), key.c_str()};

    PGresult* res = PQexecParams(pg_conn, delete_sql, 3, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to delete metadata: " + std::string(PQerrorMessage(pg_conn)));
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
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to execute query: " + std::string(PQerrorMessage(pg_conn)));
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
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<std::vector<std::string>>>::err("Failed to execute query: " + std::string(PQerrorMessage(pg_conn)));
    }

    std::vector<std::vector<std::string>> result;
    int nrows = PQntuples(res);
    int nfields = PQnfields(res);

    for (int row = 0; row < nrows; ++row) {
        std::vector<std::string> row_data;
        for (int col = 0; col < nfields; ++col) {
            char* value = PQgetvalue(res, row, col);
            row_data.push_back(value ? std::string(value) : "");
        }
        result.push_back(row_data);
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::vector<std::vector<std::string>>>::ok(result);
}

Result<void> Database::update_file_access_stats(const std::string& uid, const std::string& user, const std::string& tenant) {
    // This is a simplified implementation
    // In a full implementation, we would track access counts, timestamps, etc.
    return Result<void>::ok();
}

Result<std::vector<std::string>> Database::get_least_accessed_files(int limit, const std::string& tenant) {
    // This is a simplified implementation
    // In a full implementation, we would query access statistics
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<std::string>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    std::string query_sql = "SELECT uid FROM files WHERE is_deleted = FALSE ORDER BY modified_at ASC LIMIT " + std::to_string(limit) + ";";

    PGresult* res = PQexec(pg_conn, query_sql.c_str());

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<std::string>>::err("Failed to query least accessed files: " + std::string(PQerrorMessage(pg_conn)));
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
    // This is a simplified implementation
    // In a full implementation, we would track access statistics
    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<std::string>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    std::string query_sql = "SELECT uid FROM files WHERE is_deleted = FALSE AND modified_at < NOW() - INTERVAL '" + std::to_string(days_threshold) + " days' ORDER BY modified_at ASC;";

    PGresult* res = PQexec(pg_conn, query_sql.c_str());

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<std::string>>::err("Failed to query infrequently accessed files: " + std::string(PQerrorMessage(pg_conn)));
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

    const char* query_sql = "SELECT SUM(size) FROM versions;";

    PGresult* res = PQexec(pg_conn, query_sql);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<int64_t>::err("Failed to query storage usage: " + std::string(PQerrorMessage(pg_conn)));
    }

    if (PQntuples(res) == 0 || PQgetvalue(res, 0, 0) == nullptr) {
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
    // This is a placeholder - in a real implementation this would return actual storage capacity
    return Result<int64_t>::ok(1024 * 1024 * 1024); // 1GB placeholder
}

Result<void> Database::create_tenant_schema(const std::string& tenant) {
    if (tenant.empty()) {
        return Result<void>::err("Tenant name cannot be empty");
    }

    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Validate tenant name to prevent SQL injection
    std::string validated_tenant = validate_schema_name(tenant);
    std::string schema_name = "tenant_" + validated_tenant;

    // Create schema
    std::string create_schema_sql = "CREATE SCHEMA IF NOT EXISTS " + schema_name + ";";
    PGresult* res = PQexec(pg_conn, create_schema_sql.c_str());

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to create tenant schema: " + std::string(PQerrorMessage(pg_conn)));
    }
    PQclear(res);

    // Create tenant-specific tables
    std::string create_files_sql = "CREATE TABLE IF NOT EXISTS " + schema_name + ".files ("
                                   "uid VARCHAR(36) PRIMARY KEY,"
                                   "name VARCHAR(255) NOT NULL,"
                                   "path TEXT,"
                                   "parent_uid VARCHAR(36),"
                                   "type INTEGER NOT NULL,"
                                   "owner VARCHAR(255) NOT NULL,"
                                   "permissions INTEGER NOT NULL,"
                                   "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
                                   "modified_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
                                   "current_version VARCHAR(30),"
                                   "is_deleted BOOLEAN DEFAULT FALSE"
                                   ");";

    res = PQexec(pg_conn, create_files_sql.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to create tenant files table: " + std::string(PQerrorMessage(pg_conn)));
    }
    PQclear(res);

    std::string create_versions_sql = "CREATE TABLE IF NOT EXISTS " + schema_name + ".versions ("
                                      "file_uid VARCHAR(36) NOT NULL,"
                                      "version_timestamp VARCHAR(30) NOT NULL,"
                                      "size BIGINT NOT NULL,"
                                      "storage_path TEXT NOT NULL,"
                                      "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
                                      "PRIMARY KEY (file_uid, version_timestamp)"
                                      ");";

    res = PQexec(pg_conn, create_versions_sql.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to create tenant versions table: " + std::string(PQerrorMessage(pg_conn)));
    }
    PQclear(res);

    std::string create_metadata_sql = "CREATE TABLE IF NOT EXISTS " + schema_name + ".metadata ("
                                      "file_uid VARCHAR(36) NOT NULL,"
                                      "version_timestamp VARCHAR(30) NOT NULL,"
                                      "key_name VARCHAR(255) NOT NULL,"
                                      "value TEXT,"
                                      "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
                                      "PRIMARY KEY (file_uid, version_timestamp, key_name)"
                                      ");";

    res = PQexec(pg_conn, create_metadata_sql.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to create tenant metadata table: " + std::string(PQerrorMessage(pg_conn)));
    }
    PQclear(res);

    connection_pool_->release(conn);
    return Result<void>::ok();
}

Result<bool> Database::tenant_schema_exists(const std::string& tenant) {
    if (tenant.empty()) {
        return Result<bool>::ok(false);
    }

    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<bool>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    std::string validated_tenant = validate_schema_name(tenant);
    std::string schema_name = "tenant_" + validated_tenant;

    std::string query_sql = "SELECT schema_name FROM information_schema.schemata WHERE schema_name = '" + schema_name + "';";
    PGresult* res = PQexec(pg_conn, query_sql.c_str());

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<bool>::err("Failed to check if tenant schema exists: " + std::string(PQerrorMessage(pg_conn)));
    }

    bool exists = PQntuples(res) > 0;
    PQclear(res);
    connection_pool_->release(conn);
    return Result<bool>::ok(exists);
}

Result<void> Database::cleanup_tenant_data(const std::string& tenant) {
    if (tenant.empty()) {
        return Result<void>::err("Tenant name cannot be empty");
    }

    auto conn = connection_pool_->acquire();
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    std::string validated_tenant = validate_schema_name(tenant);
    std::string schema_name = "tenant_" + validated_tenant;

    std::string drop_schema_sql = "DROP SCHEMA IF EXISTS " + schema_name + " CASCADE;";
    PGresult* res = PQexec(pg_conn, drop_schema_sql.c_str());

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to cleanup tenant data: " + std::string(PQerrorMessage(pg_conn)));
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<void>::ok();
}

std::string Database::get_connection_info() const {
    return connection_pool_ ? "Connection pool initialized" : "No connection pool";
}

// Helper methods implementation
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