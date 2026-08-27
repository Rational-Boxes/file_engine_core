// Copyright (C) 2026 James Hickman
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

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
#include <cstdlib>
#include <ctime>

namespace fileengine {

// Include the Database class implementation methods in full
Database::Database(const std::string& host, int port, const std::string& dbname,
                   const std::string& user, const std::string& password, int pool_size)
    : connection_pool_(std::make_shared<ConnectionPool>(host, port, dbname, user, password, pool_size)),
      hostname_(host),
      pool_size_(pool_size),
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
    auto conn = acquire(DbOp::Write);
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

        -- Global audit log (usage_logging_and_auditing.md §8): tenant
        -- create/drop and cross-tenant admin actions, readable only by
        -- system_admin. Same shape as the per-tenant audit_log plus a nullable
        -- tenant column (this table is not schema-isolated). Range-partitioned
        -- by day like the per-tenant table; partitions and the hash chain are
        -- owned by the single audit-consumer.
        CREATE TABLE IF NOT EXISTS audit_log_global (
            seq          BIGSERIAL,
            event_id     UUID         NOT NULL,
            ts           TIMESTAMPTZ  NOT NULL DEFAULT now(),
            category     SMALLINT     NOT NULL,
            action       VARCHAR(32)  NOT NULL,
            outcome      SMALLINT     NOT NULL,
            actor        VARCHAR(255) NOT NULL,
            actor_roles  TEXT,
            target_uid   VARCHAR(64),
            -- Retained as a nullable column so existing chains still verify
            -- (it is part of the hash's canonical row form), but it is never
            -- populated any more: filenames are party data and this log is
            -- immutable, so a name stored here is one the platform cannot
            -- remove on request. Emitters send the uid; viewers resolve the
            -- current name at read time (§5.4.7).
            target_name  VARCHAR(1024),
            target_type  SMALLINT,
            detail       JSONB,
            source_iface VARCHAR(16),
            source_addr  VARCHAR(64),
            request_id   VARCHAR(64),
            tenant       VARCHAR(255),
            -- Two times, deliberately (PROPOSAL_accountability_record.md §4.3.4).
            -- `ts` is when the event OCCURRED, as reported by its source. This
            -- column is when it was RECORDED into the chain. The chain is
            -- ordered by the latter, and the former can legitimately run
            -- slightly backwards between adjacent entries from different
            -- sources, because cross-source ordering is only accurate to within
            -- the queue latency. Recording both makes that visible and
            -- explicable instead of looking like clock corruption to a later
            -- reader — or worse, prompting someone to "fix" it by reordering a
            -- tamper-evident structure. Not part of the hash: it describes
            -- delivery, not the event.
            recorded_at  TIMESTAMPTZ  NOT NULL DEFAULT now(),
            prev_hash    BYTEA,
            row_hash     BYTEA,
            PRIMARY KEY (seq, ts),
            UNIQUE (event_id, ts)
        ) PARTITION BY RANGE (ts);

        ALTER TABLE audit_log_global
            ADD COLUMN IF NOT EXISTS recorded_at TIMESTAMPTZ NOT NULL DEFAULT now();

        CREATE INDEX IF NOT EXISTS idx_audit_global_actor  ON audit_log_global(actor, ts);
        CREATE INDEX IF NOT EXISTS idx_audit_global_action ON audit_log_global(category, action, ts);
        CREATE INDEX IF NOT EXISTS idx_audit_global_tenant ON audit_log_global(tenant, ts);

        -- Global accountability record (PROPOSAL_accountability_record.md §7.3).
        -- A tenant's history is tenant data and is destroyed with the tenant —
        -- DROP SCHEMA CASCADE takes its accountability_record along with
        -- everything else. One thing cannot live there, for a mechanical
        -- reason: the record of the deletion itself would delete itself. So
        -- tenant lifecycle (create + delete) is recorded HERE, outside every
        -- tenant schema, where whoever deletes a tenant cannot reach it.
        --
        -- Without this, deleting a tenant is the cleanest way to destroy
        -- evidence: it would erase the entire record of what happened inside,
        -- including the deleter's own actions. It also closes the §2.1 finding
        -- that tenant deletion — the most destructive operation in the system —
        -- currently emits no event at all.
        --
        -- Contents are deliberately limited to the fact of the lifecycle event.
        -- Nothing about what the tenant held ever appears here.
        CREATE TABLE IF NOT EXISTS accountability_record_global (
            seq          BIGINT       PRIMARY KEY,
            ts           TIMESTAMPTZ  NOT NULL,
            actor        VARCHAR(255) NOT NULL,
            actor_roles  TEXT[],
            source_iface VARCHAR(32),
            source_addr  VARCHAR(64),
            category     VARCHAR(32)  NOT NULL,
            action       VARCHAR(64)  NOT NULL,
            target_uid   VARCHAR(64),
            target_type  VARCHAR(32),
            principal    VARCHAR(255),
            tenant       VARCHAR(255),
            detail       JSONB        NOT NULL DEFAULT '{}'::jsonb,
            prev_hash    BYTEA,
            hash         BYTEA        NOT NULL
        );

        CREATE INDEX IF NOT EXISTS idx_accountability_global_ts     ON accountability_record_global(ts);
        CREATE INDEX IF NOT EXISTS idx_accountability_global_actor  ON accountability_record_global(actor, ts);
        CREATE INDEX IF NOT EXISTS idx_accountability_global_tenant ON accountability_record_global(tenant, ts);

        -- The global chain's head. Same table shape as the per-tenant one so the
        -- append path is literally the same code with a different schema; the
        -- single row is keyed by the reserved '*global*' name, which no real
        -- tenant can take (validate_schema_name strips '*').
        CREATE TABLE IF NOT EXISTS accountability_chain_head_global (
            tenant     VARCHAR(255) PRIMARY KEY,
            last_seq   BIGINT NOT NULL DEFAULT 0,
            last_ts    TIMESTAMPTZ,
            last_hash  BYTEA
        );

        INSERT INTO accountability_chain_head_global (tenant) VALUES ('*global*')
        ON CONFLICT (tenant) DO NOTHING;
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

    auto conn = acquire(DbOp::Write);
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
            "is_container = EXCLUDED.is_container, "
            "updated_at = CURRENT_TIMESTAMP "        // bump mtime on any metadata change
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
    auto conn = acquire(DbOp::Write);
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
    auto conn = acquire(DbOp::Write);
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
    auto conn = acquire(DbOp::Write);
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
    auto conn = acquire(DbOp::Write);
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

// Defined below (with the listing queries); forward-declared so the single-file
// lookups above can share the exact same timestamp/provenance derivation.
static void apply_listing_provenance(FileInfo& info,
                                     int64_t files_created_epoch, int64_t files_updated_epoch,
                                     const char* first_vts, const char* first_by,
                                     const char* last_vts, const char* last_by);
// A folder's mtime = the newest file anywhere beneath it (recursive). Forward-
// declared so builders above the definition can apply it to directory rows.
static void apply_folder_recursive_mtime(FileInfo& info, PGconn* conn, const std::string& schema);

Result<std::optional<FileInfo>> Database::get_file_by_uid(const std::string& uid, const std::string& tenant) {
    auto conn = acquire(DbOp::Read);
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

    // created/modified are derived from the file's version-name timestamps (first
    // = ctime, latest = mtime), falling back to files.created_at/updated_at — the
    // SAME provenance the directory listing uses (apply_listing_provenance), so a
    // single-file Stat and the parent's listing report identical timestamps for
    // the same file. Emitting a fresh now() here made every PROPFIND look freshly
    // modified, which drove WebDAV editors into a "file changed on disk" loop.
    std::string query_sql = "SELECT f.name, f.parent_uid, f.size, f.owner, f.permission_map, f.is_container, f.deleted, "
                            "FLOOR(EXTRACT(EPOCH FROM f.created_at))::bigint AS created_epoch, "
                            "FLOOR(EXTRACT(EPOCH FROM f.updated_at))::bigint AS updated_epoch, "
                            "(SELECT v.version_timestamp FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp ASC LIMIT 1) AS first_vts, "
                            "(SELECT v.revised_by FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp ASC LIMIT 1) AS first_by, "
                            "(SELECT v.version_timestamp FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp DESC LIMIT 1) AS last_vts, "
                            "(SELECT v.revised_by FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp DESC LIMIT 1) AS last_by "
                            "FROM \"" + schema_name + "\".files f "
                            "WHERE f.uid = $1 AND f.deleted = FALSE "
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
            (void)is_deleted;  // row is already filtered to deleted = FALSE

            FileInfo info;
            info.uid = uid;
            info.name = name;
            info.path = "/" + name;  // Simple path calculation - in a real system this would be more complex
            info.parent_uid = parent_uid;
            info.type = is_container ? FileType::DIRECTORY : FileType::REGULAR_FILE;
            info.size = size;
            info.owner = owner;
            info.permissions = permissions;
            // Timestamps + provenance from version names, DB columns as fallback —
            // identical to the directory-listing path so Stat and ListDirectory
            // agree (owner is set above so the created_by/modified_by fallback
            // resolves). Never now() (see the query comment above): a fresh now()
            // made every PROPFIND report a new mtime, driving WebDAV editors into a
            // "file changed on disk" loop.
            const char* last_vts = PQgetisnull(res, 0, 11) ? nullptr : PQgetvalue(res, 0, 11);
            apply_listing_provenance(info,
                std::stoll(PQgetvalue(res, 0, 7)), std::stoll(PQgetvalue(res, 0, 8)),
                PQgetisnull(res, 0, 9)  ? nullptr : PQgetvalue(res, 0, 9),
                PQgetisnull(res, 0, 10) ? nullptr : PQgetvalue(res, 0, 10),
                last_vts,
                PQgetisnull(res, 0, 12) ? nullptr : PQgetvalue(res, 0, 12));
            // For a folder, override mtime with the newest file anywhere beneath it.
            apply_folder_recursive_mtime(info, pg_conn, schema_name);
            // Current version = the latest version-name timestamp (empty if the
            // file has no versions yet, e.g. a freshly touched 0-byte file).
            info.version = last_vts ? std::string(last_vts) : "";
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
    auto conn = acquire(DbOp::Read);
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
    auto conn = acquire(DbOp::Write);
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
    auto conn = acquire(DbOp::Write);
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

// Parse a version_timestamp ("YYYYMMDD_HHMMSS.mmm", UTC — see Utils::get_timestamp_string)
// into epoch seconds. Returns false for null/empty/unparseable values.
// NOTE: this TRUNCATES the sub-second part (strptime stops at %S). The DB-column
// fallback (files.created_at/updated_at) MUST match that convention, so those
// queries use FLOOR(EXTRACT(EPOCH ...)) — a plain ::bigint cast ROUNDS, which
// would make a just-touched 0-byte file (mtime = rounded updated_at) and its
// first revision (mtime = truncated version) differ by up to a second, and the
// mtime could even move backwards across the first save — a "changed/modified on
// disk" trigger for WebDAV editors.
static bool parse_vts_epoch(const char* vts, int64_t& out) {
    if (vts == nullptr || vts[0] == '\0') return false;
    std::tm tm{};
    if (strptime(vts, "%Y%m%d_%H%M%S", &tm) == nullptr) return false;
    out = static_cast<int64_t>(timegm(&tm));  // input is UTC (gmtime)
    return true;
}

// Newest version_timestamp among all NON-rendition descendant files of a folder,
// recursively. The CTE walks only through sub*folders* (is_container), so it
// collects files that live under a folder but never descends into a file — hence
// renditions (hidden children of files) can never bump a folder's mtime. Returns
// "" when the subtree has no versioned files.
static std::pair<std::string, std::string> subtree_newest_version(
        PGconn* conn, const std::string& schema, const std::string& dir_uid) {
    // The single newest version across all non-rendition descendant files: returns
    // (version_timestamp, revised_by) so a folder can report BOTH the newest file's
    // mtime AND who made that change (modified_by), consistently with the timestamp.
    // UNION (not UNION ALL) on the folder UID: dedups so the recursion TERMINATES
    // even if the folder graph has a cycle (a corrupt parent_uid pointing back into
    // the subtree) — a revisited uid is a duplicate and adds no new rows, so the
    // walk stops. UNION ALL recurses forever on a cycle; and because that SELECT
    // holds an ACCESS SHARE lock on `files`, the runaway blocks the ACCESS EXCLUSIVE
    // `ALTER TABLE` a new core runs during tenant init — hanging startup. Folder
    // UIDs are unique, so UNION yields the same set as UNION ALL for a valid tree.
    const std::string sql =
        "WITH RECURSIVE folders(uid) AS ("
        "  SELECT $1::text"
        "  UNION"
        "  SELECT f.uid FROM \"" + schema + "\".files f"
        "    JOIN folders fo ON f.parent_uid = fo.uid"
        "    WHERE f.is_container = TRUE AND f.deleted = FALSE"
        ") "
        "SELECT v.version_timestamp, v.revised_by"
        "  FROM \"" + schema + "\".files fi"
        "  JOIN folders fo ON fi.parent_uid = fo.uid"
        "  JOIN \"" + schema + "\".versions v ON v.file_uid = fi.uid"
        " WHERE fi.is_container = FALSE AND fi.deleted = FALSE"
        " ORDER BY v.version_timestamp DESC LIMIT 1;";
    const char* params[1] = { dir_uid.c_str() };
    PGresult* res = PQexecParams(conn, sql.c_str(), 1, nullptr, params, nullptr, nullptr, 0);
    std::pair<std::string, std::string> out;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        if (!PQgetisnull(res, 0, 0)) out.first = PQgetvalue(res, 0, 0);   // version_timestamp
        if (!PQgetisnull(res, 0, 1)) out.second = PQgetvalue(res, 0, 1);  // revised_by
    }
    PQclear(res);
    return out;
}

// A directory's mtime is the newest file anywhere in its subtree (recursively),
// per the "folder mtime = newest contained file" rule. No-op for non-directories
// or a file-less subtree (an empty folder keeps its own updated_at from
// apply_listing_provenance). Truncated via parse_vts_epoch so a folder and the
// file that set its mtime report the identical second on every surface.
static void apply_folder_recursive_mtime(FileInfo& info, PGconn* conn, const std::string& schema) {
    if (info.type != FileType::DIRECTORY) return;
    auto nv = subtree_newest_version(conn, schema, info.uid);   // (version_timestamp, revised_by)
    int64_t e;
    if (parse_vts_epoch(nv.first.c_str(), e)) {
        // A folder's mtime AND its "modified by" both come from the newest file in
        // its subtree — the file that set the timestamp and who last revised it.
        info.modified_at = std::chrono::system_clock::time_point(std::chrono::seconds(e));
        if (!nv.second.empty()) info.modified_by = nv.second;
    }
}

// Provenance from the revision history: ctime + creator = first revision,
// mtime + last reviser = latest revision (per the versioning model). Falls back
// to the metadata-DB files.created_at/updated_at + files.owner for rows with no
// revisions (directories, freshly-touched files). Sets info.created_at/modified_at
// and info.created_by/modified_by. `info.owner` must already be set.
static void apply_listing_provenance(FileInfo& info,
                                     int64_t files_created_epoch, int64_t files_updated_epoch,
                                     const char* first_vts, const char* first_by,
                                     const char* last_vts, const char* last_by) {
    int64_t created = files_created_epoch, modified = files_updated_epoch, e;
    if (parse_vts_epoch(first_vts, e)) created = e;
    if (parse_vts_epoch(last_vts, e)) modified = e;
    info.created_at  = std::chrono::system_clock::time_point(std::chrono::seconds(created));
    info.modified_at = std::chrono::system_clock::time_point(std::chrono::seconds(modified));
    info.created_by  = (first_by && first_by[0]) ? std::string(first_by) : info.owner;
    info.modified_by = (last_by  && last_by[0])  ? std::string(last_by)  : info.owner;
}

Result<std::vector<FileInfo>> Database::list_files_in_directory(const std::string& parent_uid, const std::string& tenant) {
    auto conn = acquire(DbOp::Read);
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
                            "WHERE c.parent_uid = f.uid AND c.deleted = FALSE) END AS rendition_count, "
                            "FLOOR(EXTRACT(EPOCH FROM f.created_at))::bigint AS created_epoch, "
                            "FLOOR(EXTRACT(EPOCH FROM f.updated_at))::bigint AS updated_epoch, "
                            "(SELECT v.version_timestamp FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp ASC LIMIT 1) AS first_vts, "
                            "(SELECT v.revised_by FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp ASC LIMIT 1) AS first_by, "
                            "(SELECT v.version_timestamp FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp DESC LIMIT 1) AS last_vts, "
                            "(SELECT v.revised_by FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp DESC LIMIT 1) AS last_by "
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
            // Provenance from revisions (ctime/creator = first, mtime/reviser =
            // latest), falling back to files.created_at/updated_at + owner. info.owner
            // is set above (index 3) so the fallback resolves.
            apply_listing_provenance(info,
                std::stoll(PQgetvalue(res, i, 7)), std::stoll(PQgetvalue(res, i, 8)),
                PQgetisnull(res, i, 9)  ? nullptr : PQgetvalue(res, i, 9),
                PQgetisnull(res, i, 10) ? nullptr : PQgetvalue(res, i, 10),
                PQgetisnull(res, i, 11) ? nullptr : PQgetvalue(res, i, 11),
                PQgetisnull(res, i, 12) ? nullptr : PQgetvalue(res, i, 12));
            // A folder entry's mtime = the newest file anywhere in its subtree.
            apply_folder_recursive_mtime(info, pg_conn, schema_name);
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
    auto conn = acquire(DbOp::Read);
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
                            "WHERE c.parent_uid = f.uid AND c.deleted = FALSE) END AS rendition_count, "
                            "FLOOR(EXTRACT(EPOCH FROM f.created_at))::bigint AS created_epoch, "
                            "FLOOR(EXTRACT(EPOCH FROM f.updated_at))::bigint AS updated_epoch, "
                            "(SELECT v.version_timestamp FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp ASC LIMIT 1) AS first_vts, "
                            "(SELECT v.revised_by FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp ASC LIMIT 1) AS first_by, "
                            "(SELECT v.version_timestamp FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp DESC LIMIT 1) AS last_vts, "
                            "(SELECT v.revised_by FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp DESC LIMIT 1) AS last_by "
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
            info.deleted = (strcmp(PQgetvalue(res, i, 6), "t") == 0 || strcmp(PQgetvalue(res, i, 6), "1") == 0);  // index 6: f.deleted
            info.rendition_count = std::stoi(PQgetvalue(res, i, 7));  // index 7: after deleted (6)
            // Provenance from revisions; files columns (8/9) + owner are the
            // fallback. VTS/by subqueries are 10..13.
            apply_listing_provenance(info,
                std::stoll(PQgetvalue(res, i, 8)), std::stoll(PQgetvalue(res, i, 9)),
                PQgetisnull(res, i, 10) ? nullptr : PQgetvalue(res, i, 10),
                PQgetisnull(res, i, 11) ? nullptr : PQgetvalue(res, i, 11),
                PQgetisnull(res, i, 12) ? nullptr : PQgetvalue(res, i, 12),
                PQgetisnull(res, i, 13) ? nullptr : PQgetvalue(res, i, 13));
            // A folder entry's mtime = the newest file anywhere in its subtree.
            apply_folder_recursive_mtime(info, pg_conn, schema_name);
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
    auto conn = acquire(DbOp::Read);
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
    auto conn = acquire(DbOp::Read);
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

    // Timestamps/provenance from version names (DB columns as fallback), matching
    // the listing + get_file_by_uid paths — never now() (see get_file_by_uid).
    std::string query_sql = "SELECT f.uid, f.size, f.owner, f.permission_map, f.is_container, "
                            "FLOOR(EXTRACT(EPOCH FROM f.created_at))::bigint AS created_epoch, "
                            "FLOOR(EXTRACT(EPOCH FROM f.updated_at))::bigint AS updated_epoch, "
                            "(SELECT v.version_timestamp FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp ASC LIMIT 1) AS first_vts, "
                            "(SELECT v.revised_by FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp ASC LIMIT 1) AS first_by, "
                            "(SELECT v.version_timestamp FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp DESC LIMIT 1) AS last_vts, "
                            "(SELECT v.revised_by FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp DESC LIMIT 1) AS last_by "
                            "FROM \"" + schema_name + "\".files f "
                            "WHERE f.name = $1 AND f.parent_uid = $2 AND f.deleted = FALSE "
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
            // Version-name/DB-derived timestamps + provenance (never now()).
            const char* last_vts = PQgetisnull(res, 0, 9) ? nullptr : PQgetvalue(res, 0, 9);
            apply_listing_provenance(info,
                std::stoll(PQgetvalue(res, 0, 5)), std::stoll(PQgetvalue(res, 0, 6)),
                PQgetisnull(res, 0, 7)  ? nullptr : PQgetvalue(res, 0, 7),
                PQgetisnull(res, 0, 8)  ? nullptr : PQgetvalue(res, 0, 8),
                last_vts,
                PQgetisnull(res, 0, 10) ? nullptr : PQgetvalue(res, 0, 10));
            apply_folder_recursive_mtime(info, pg_conn, schema_name);  // folder mtime = newest descendant file
            info.version = last_vts ? std::string(last_vts) : "";
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
    auto conn = acquire(DbOp::Read);
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

    // Timestamps/provenance from version names (DB columns as fallback), matching
    // the listing + get_file_by_uid paths — never now() (see get_file_by_uid).
    std::string query_sql = "SELECT f.uid, f.size, f.owner, f.permission_map, f.is_container, "
                            "FLOOR(EXTRACT(EPOCH FROM f.created_at))::bigint AS created_epoch, "
                            "FLOOR(EXTRACT(EPOCH FROM f.updated_at))::bigint AS updated_epoch, "
                            "(SELECT v.version_timestamp FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp ASC LIMIT 1) AS first_vts, "
                            "(SELECT v.revised_by FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp ASC LIMIT 1) AS first_by, "
                            "(SELECT v.version_timestamp FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp DESC LIMIT 1) AS last_vts, "
                            "(SELECT v.revised_by FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp DESC LIMIT 1) AS last_by "
                            "FROM \"" + schema_name + "\".files f "
                            "WHERE f.name = $1 AND f.parent_uid = $2 "
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
            // Version-name/DB-derived timestamps + provenance (never now()).
            const char* last_vts = PQgetisnull(res, 0, 9) ? nullptr : PQgetvalue(res, 0, 9);
            apply_listing_provenance(info,
                std::stoll(PQgetvalue(res, 0, 5)), std::stoll(PQgetvalue(res, 0, 6)),
                PQgetisnull(res, 0, 7)  ? nullptr : PQgetvalue(res, 0, 7),
                PQgetisnull(res, 0, 8)  ? nullptr : PQgetvalue(res, 0, 8),
                last_vts,
                PQgetisnull(res, 0, 10) ? nullptr : PQgetvalue(res, 0, 10));
            apply_folder_recursive_mtime(info, pg_conn, schema_name);  // folder mtime = newest descendant file
            info.version = last_vts ? std::string(last_vts) : "";
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
    auto conn = acquire(DbOp::Read);
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
    auto conn = acquire(DbOp::Read);
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
    auto conn = acquire(DbOp::Read);
    if (!conn || !conn->is_valid()) {
        return Result<std::optional<FileInfo>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    // Timestamps/provenance from version names (DB columns as fallback), matching
    // the listing + get_file_by_uid paths — never now() (see get_file_by_uid).
    std::string query_sql = "SELECT f.name, f.parent_uid, f.size, f.owner, f.permission_map, f.is_container, f.deleted, "
                            "FLOOR(EXTRACT(EPOCH FROM f.created_at))::bigint AS created_epoch, "
                            "FLOOR(EXTRACT(EPOCH FROM f.updated_at))::bigint AS updated_epoch, "
                            "(SELECT v.version_timestamp FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp ASC LIMIT 1) AS first_vts, "
                            "(SELECT v.revised_by FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp ASC LIMIT 1) AS first_by, "
                            "(SELECT v.version_timestamp FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp DESC LIMIT 1) AS last_vts, "
                            "(SELECT v.revised_by FROM \"" + schema_name + "\".versions v WHERE v.file_uid = f.uid ORDER BY v.version_timestamp DESC LIMIT 1) AS last_by "
                            "FROM \"" + schema_name + "\".files f "
                            "WHERE f.uid = $1 "
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

            // This is the *include_deleted* lookup: unlike get_file_by_uid (which
            // filters WHERE deleted = FALSE) it MUST return soft-deleted rows too,
            // with `deleted` set — otherwise callers can't resolve a just-deleted
            // row's metadata (delete/rmdir event enrichment) and reachability checks
            // can't detect a deleted ANCESTOR. Returning nullopt here was a latent
            // bug that made both silently no-op.
            FileInfo info;
            info.uid = uid;
            info.name = name;
            info.path = "/" + name;  // Simple path calculation - in a real system this would be more complex
            info.parent_uid = parent_uid;
            info.type = is_container ? FileType::DIRECTORY : FileType::REGULAR_FILE;
            info.size = size;
            info.owner = owner;
            info.permissions = permissions;
            info.deleted = is_deleted;
            // Version-name/DB-derived timestamps + provenance (never now()).
            const char* last_vts = PQgetisnull(res, 0, 11) ? nullptr : PQgetvalue(res, 0, 11);
            apply_listing_provenance(info,
                std::stoll(PQgetvalue(res, 0, 7)), std::stoll(PQgetvalue(res, 0, 8)),
                PQgetisnull(res, 0, 9)  ? nullptr : PQgetvalue(res, 0, 9),
                PQgetisnull(res, 0, 10) ? nullptr : PQgetvalue(res, 0, 10),
                last_vts,
                PQgetisnull(res, 0, 12) ? nullptr : PQgetvalue(res, 0, 12));
            apply_folder_recursive_mtime(info, pg_conn, schema_name);  // folder mtime = newest descendant file
            info.version = last_vts ? std::string(last_vts) : "";
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
    auto conn = acquire(DbOp::Write);
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
    auto conn = acquire(DbOp::Read);
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
    auto conn = acquire(DbOp::Read);
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
                                          int64_t size, const std::string& storage_path,
                                          const std::string& revised_by, const std::string& tenant) {
    auto conn = acquire(DbOp::Write);
    if (!conn || !conn->is_valid()) {
        return Result<int64_t>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Get the schema name for this tenant
    std::string schema_name = get_schema_prefix(tenant);

    // Insert the version into the versions table with its storage path
    // Keep the version_timestamp as a string to avoid conversion issues
    std::string insert_sql = "INSERT INTO \"" + schema_name + "\".versions (file_uid, version_timestamp, size, storage_path, revised_by) "
                             "VALUES ($1, $2, $3, $4, $5) "
                             "ON CONFLICT (file_uid, version_timestamp) DO UPDATE SET "
                             "size = EXCLUDED.size, storage_path = EXCLUDED.storage_path, revised_by = EXCLUDED.revised_by "
                             "RETURNING id;";
    // Own the size string so param_values doesn't point at a freed temporary.
    std::string size_str = std::to_string(size);
    const char* param_values[5] = {
        file_uid.c_str(),
        version_timestamp.c_str(),  // Keep as string
        size_str.c_str(),
        storage_path.c_str(),
        revised_by.c_str()          // acting user who wrote this revision
    };

    PGresult* res = PQexecParams(pg_conn, insert_sql.c_str(), 5, nullptr, param_values, nullptr, nullptr, 0);

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
    auto conn = acquire(DbOp::Read);
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

Result<std::vector<VersionInfo>> Database::list_versions_detailed(const std::string& file_uid, const std::string& tenant) {
    auto conn = acquire(DbOp::Read);
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<VersionInfo>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();
    std::string schema_name = get_schema_prefix(tenant);

    // revised_by has always been stored; it simply was never returned here.
    std::string query_sql = "SELECT version_timestamp, revised_by FROM \"" + schema_name +
                            "\".versions WHERE file_uid = $1 ORDER BY version_timestamp DESC;";
    const char* param_values[1] = {file_uid.c_str()};

    PGresult* res = PQexecParams(pg_conn, query_sql.c_str(), 1, nullptr, param_values, nullptr, nullptr, 0);

    std::vector<VersionInfo> versions;
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int nrows = PQntuples(res);
        versions.reserve(static_cast<size_t>(nrows));
        for (int i = 0; i < nrows; ++i) {
            VersionInfo v;
            v.version_timestamp = PQgetvalue(res, i, 0);
            // Rows written before the column was populated are NULL. An empty
            // uploader is honest; inventing one would be worse than admitting it
            // is unknown.
            if (!PQgetisnull(res, i, 1)) {
                v.revised_by = PQgetvalue(res, i, 1);
            }
            versions.push_back(std::move(v));
        }
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<VersionInfo>>::ok(versions);
    }

    std::string error = PQerrorMessage(pg_conn);
    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::vector<VersionInfo>>::err("Failed to list versions: " + error);
}

// Timestamp-only form, kept for callers that do not need the uploader. Delegates
// so there is one query and one ordering to reason about.
Result<std::vector<std::string>> Database::list_versions(const std::string& file_uid, const std::string& tenant) {
    auto detailed = list_versions_detailed(file_uid, tenant);
    if (!detailed.success) {
        return Result<std::vector<std::string>>::err(detailed.error);
    }
    std::vector<std::string> timestamps;
    timestamps.reserve(detailed.value.size());
    for (const auto& v : detailed.value) {
        timestamps.push_back(v.version_timestamp);
    }
    return Result<std::vector<std::string>>::ok(timestamps);
}

Result<bool> Database::delete_version(const std::string& file_uid, const std::string& version_timestamp, const std::string& tenant) {
    auto conn = acquire(DbOp::Write);
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
    // `user` is the acting principal (FS layer already enforced RESTORE_TO_VERSION);
    // it is recorded as revised_by on the new head so the restore is attributed
    // to whoever performed it rather than falling back to the 'unknown' default.
    std::string revised_by = user.empty() ? "unknown" : user;
    auto conn = acquire(DbOp::Write);
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
                             "(file_uid, version_timestamp, size, storage_path, revised_by) "
                             "VALUES ($1, $2, $3, $4, $5);";
    const char* insert_params[5] = {
        file_uid.c_str(), new_ts.c_str(), size_str.c_str(), source_path.c_str(), revised_by.c_str()
    };
    PGresult* ins = PQexecParams(pg_conn, insert_sql.c_str(), 5, nullptr, insert_params, nullptr, nullptr, 0);
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
    auto conn = acquire(DbOp::Write);
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
    auto conn = acquire(DbOp::Read);
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
    auto conn = acquire(DbOp::Read);
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
    auto conn = acquire(DbOp::Write);
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
    auto conn = acquire(DbOp::Write);
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
    auto conn = acquire(DbOp::Read);
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
    auto conn = acquire(DbOp::Read);
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
    auto conn = acquire(DbOp::Read);
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
    auto conn = acquire(DbOp::Read);
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


// ═══════════════════════════════════════════════════════════════════════════
// The guaranteed accountability record (PROPOSAL_accountability_record.md)
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// EXTRACT(EPOCH FROM ts) returns double precision on PostgreSQL below 14, and a
// microsecond of a 2020s epoch is right at the edge of what a double represents
// exactly — so the obvious expression can silently round the very field the
// consumer's cursor and the chain hash both depend on. Splitting whole seconds
// (always exact) from the sub-second field keeps it exact on every version.
constexpr const char* kEpochMicrosExpr =
    "(EXTRACT(EPOCH FROM date_trunc('second', last_ts))::bigint * 1000000 "
    " + (EXTRACT(MICROSECONDS FROM last_ts)::bigint % 1000000))";

// Round-trip the timestamp as TEXT rather than as a float for the same reason.
constexpr const char* kIsoTextExpr =
    "to_char(last_ts AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS.US')";

// A Postgres text[] literal. Roles go in as a real array rather than a joined
// string so "which records name role X" is an index-able question instead of a
// pattern match over a CSV.
std::string text_array_literal(const std::vector<std::string>& values) {
    std::string out = "{";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += ",";
        out += "\"";
        for (char c : values[i]) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        out += "\"";
    }
    out += "}";
    return out;
}

}  // namespace

// Append one accountability record INSIDE the caller's open transaction.
//
// This is the whole guarantee: the record and the operation commit together or
// not at all (§4.2). Every caller must treat a failure here as fatal to the
// operation — roll back and fail — because the alternative is a committed
// authorization change or destruction with nothing anywhere recording it, which
// is the exact failure this table exists to prevent.
//
// The single locked head row aligns three orderings that were otherwise
// independent: assignment order == commit order == chain order (§5.3.2). That
// is what lets a consumer read forward by cursor with no snapshot watermark, no
// lag heuristic, and no risk of skipping a record that commits late — and what
// makes a gap in `seq` an unambiguous integrity alarm rather than routine noise.
Result<Database::AccountabilityCommit> Database::append_accountability(
        PGconn* conn, const std::string& tenant, const AccountabilityRecord& rec) {
    auto fail = [&tenant](const std::string& msg) {
        // Reaching here means an operation is about to be rolled back because
        // its record could not be written. That is the fail-closed guarantee
        // doing its job — and the single most important thing an operator can
        // know, because the alternative design would have let the operation
        // through unrecorded and said nothing at all.
        SERVER_LOG_SECURITY("Accountability",
                            "Accountability write FAILED for tenant '" + tenant +
                            "' — the operation will be refused: " + msg);
        return Result<AccountabilityCommit>::err("accountability: " + msg);
    };

    auto validated = validate_record(rec);
    if (!validated.success) {
        // A record the code could not honestly write — no actor, an action out
        // of scope, a detail field no schema enumerates. Every one of those is a
        // programming error on a security path, and the operation is about to be
        // refused because of it, so it goes on the unsuppressable channel rather
        // than being returned as a bare string nobody reads.
        SERVER_LOG_SECURITY("Accountability",
                            "Refusing to write an invalid accountability record for tenant '" +
                            tenant + "' (action=" + rec.action + "): " + validated.error);
        return fail(validated.error);
    }

    const bool is_global = (tenant == kGlobalChainKey);
    if (is_global && rec.global_tenant.empty()) {
        return fail("a global record must name the tenant it is about");
    }
    // Same statements either way; only the table names differ. The global chain
    // lives outside every tenant schema precisely so a tenant deletion cannot
    // reach it (§7.3).
    const std::string schema       = is_global ? std::string() : get_schema_prefix(tenant);
    const std::string head_table   = is_global ? std::string("accountability_chain_head_global")
                                               : "\"" + schema + "\".accountability_chain_head";
    const std::string record_table = is_global ? std::string("accountability_record_global")
                                               : "\"" + schema + "\".accountability_record";
    const std::string chain_key    = is_global ? std::string(kGlobalChainKey)
                                               : (tenant.empty() ? std::string("default") : tenant);

    // Bootstrap. Normally a no-op — create_tenant_schema seeds the row — but a
    // tenant provisioned before this table existed would otherwise fail its
    // first authorization change, which is exactly the moment the guarantee is
    // supposed to start holding.
    {
        const std::string sql = "INSERT INTO " + head_table + " (tenant) VALUES ($1) "
                                "ON CONFLICT (tenant) DO NOTHING;";
        const char* params[1] = { chain_key.c_str() };
        PGresult* res = PQexecParams(conn, sql.c_str(), 1, nullptr, params, nullptr, nullptr, 0);
        const bool ok = PQresultStatus(res) == PGRES_COMMAND_OK;
        const std::string err = ok ? "" : PQerrorMessage(conn);
        PQclear(res);
        if (!ok) return fail("could not ensure chain head: " + err);
    }

    // Claim this record's position, under the head row's lock, in one statement.
    // UPDATE ... RETURNING holds that lock until commit — that is what serializes
    // appends per tenant, and what makes a rolled-back transaction release the
    // lock WITHOUT advancing last_seq (so numbers are never burned and gaps
    // cannot occur naturally).
    //
    // clock_timestamp(), NOT now(): now() / CURRENT_TIMESTAMP /
    // transaction_timestamp() all return TRANSACTION START time, so records
    // written in one transaction would share a timestamp taken before the lock
    // was even acquired — reintroducing the ordering ambiguity the lock exists
    // to remove. clock_timestamp() reads the wall clock inside the lock, so it
    // falls in lock order.
    //
    // GREATEST(..., last_ts + 1µs) forces STRICT monotonicity. A clock can step
    // backwards — NTP correction, VM migration, a leap-second smear — and a
    // non-monotonic ts silently breaks any newer-than cursor. With the guard, ts
    // within a tenant is strictly increasing whatever the system clock does,
    // which is what makes `ts > recorded_until` exact: no two records share a
    // ts, so the cursor needs no composite key and no tiebreak, and can neither
    // skip a record nor return one twice.
    std::int64_t seq = 0;
    std::int64_t ts_micros = 0;
    std::string  ts_text;
    std::vector<std::uint8_t> prev_hash;
    {
        const std::string sql =
            "UPDATE " + head_table + " SET "
            "  last_seq = last_seq + 1, "
            "  last_ts  = GREATEST(clock_timestamp(), last_ts + interval '1 microsecond') "
            "WHERE tenant = $1 "
            "RETURNING last_seq, " + kEpochMicrosExpr + ", " + kIsoTextExpr + ", last_hash;";
        const char* params[1] = { chain_key.c_str() };
        PGresult* res = PQexecParams(conn, sql.c_str(), 1, nullptr, params, nullptr, nullptr, 0);
        if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1) {
            const std::string err = PQerrorMessage(conn);
            PQclear(res);
            return fail("could not claim chain position: " + err);
        }
        seq       = std::strtoll(PQgetvalue(res, 0, 0), nullptr, 10);
        ts_micros = std::strtoll(PQgetvalue(res, 0, 1), nullptr, 10);
        ts_text   = PQgetvalue(res, 0, 2);
        if (!PQgetisnull(res, 0, 3)) prev_hash = parse_bytea_hex(PQgetvalue(res, 0, 3));
        PQclear(res);
    }

    const std::string canonical = canonical_record(seq, ts_micros, rec);
    const std::vector<std::uint8_t> hash = chain_hash(prev_hash, canonical);
    if (hash.empty()) return fail("chain hash could not be computed");

    const std::string seq_param       = std::to_string(seq);
    const std::string ts_param        = ts_text + "+00";   // text, not a float — see kIsoTextExpr
    const std::string roles_param     = text_array_literal(rec.ctx.actor_roles);
    const std::string category_param  = to_string(rec.category);
    const std::string detail_param    = rec.detail.canonical_json();
    const std::string prev_hash_param = bytea_hex(prev_hash);
    const std::string hash_param      = bytea_hex(hash);

    {
        std::string sql =
            "INSERT INTO " + record_table + " ("
            "  seq, ts, actor, actor_roles, source_iface, source_addr, category, action,"
            "  target_uid, target_type, principal, detail, prev_hash, hash"
            + std::string(is_global ? ", tenant" : "") + ") VALUES ("
            "  $1::bigint, $2::timestamptz, $3, NULLIF($4, '{}')::text[],"
            "  NULLIF($5, ''), NULLIF($6, ''), $7, $8,"
            "  NULLIF($9, ''), NULLIF($10, ''), NULLIF($11, ''), $12::jsonb,"
            "  NULLIF($13, '\\x')::bytea, $14::bytea"
            + std::string(is_global ? ", $15" : "") + ");";
        std::vector<const char*> params = {
            seq_param.c_str(), ts_param.c_str(), rec.ctx.actor.c_str(), roles_param.c_str(),
            rec.ctx.source_iface.c_str(), rec.ctx.source_addr.c_str(), category_param.c_str(),
            rec.action.c_str(), rec.target_uid.c_str(), rec.target_type.c_str(),
            rec.principal.c_str(), detail_param.c_str(), prev_hash_param.c_str(),
            hash_param.c_str()
        };
        if (is_global) params.push_back(rec.global_tenant.c_str());
        PGresult* res = PQexecParams(conn, sql.c_str(), static_cast<int>(params.size()),
                                     nullptr, params.data(), nullptr, nullptr, 0);
        const bool ok = PQresultStatus(res) == PGRES_COMMAND_OK;
        const std::string err = ok ? "" : PQerrorMessage(conn);
        PQclear(res);
        if (!ok) return fail("could not write record: " + err);
    }

    {
        const std::string sql = "UPDATE " + head_table + " SET last_hash = $2::bytea WHERE tenant = $1;";
        const char* params[2] = { chain_key.c_str(), hash_param.c_str() };
        PGresult* res = PQexecParams(conn, sql.c_str(), 2, nullptr, params, nullptr, nullptr, 0);
        const bool ok = PQresultStatus(res) == PGRES_COMMAND_OK;
        const std::string err = ok ? "" : PQerrorMessage(conn);
        PQclear(res);
        if (!ok) return fail("could not advance chain head: " + err);
    }

    AccountabilityCommit commit;
    commit.seq = seq;
    commit.ts_micros = ts_micros;
    return Result<AccountabilityCommit>::ok(commit);
}

void Database::fire_accountability_hint(const std::string& tenant, std::int64_t seq) noexcept {
    if (seq <= 0 || !accountability_hint_) return;
    try {
        accountability_hint_(tenant, seq);
    } catch (...) {
        // Deliberately swallowed. The record is already committed; the hint only
        // shortens the consumer's latency, so a failure here must never surface
        // as a failed operation.
    }
}

// Cull a file's old versions and record the cull, in one transaction (§5.2).
//
// The accountability record is NOT culled with the content. Culling erases
// history by design — that is the bargain — and a record that vanished with it
// would erase the evidence *of* the cull. So the table survives PurgeOldVersions
// and records it: actor, target, keep-count, and the resulting cut.
Result<int> Database::purge_versions(const std::string& file_uid,
                                     const std::vector<std::string>& version_timestamps,
                                     const std::string& cut_version_timestamp,
                                     int keep_count,
                                     const std::string& tenant,
                                     const AccountabilityContext& ctx) {
    if (!ctx.valid()) {
        return Result<int>::err("Refusing version cull with no actor");
    }
    if (version_timestamps.empty()) {
        return Result<int>::ok(0);   // nothing destroyed, so nothing to record
    }

    auto conn = acquire(DbOp::Write);
    if (!conn || !conn->is_valid()) {
        return Result<int>::err("Failed to acquire database connection");
    }
    PGconn* pg_conn = conn->get_connection();
    std::string schema = get_schema_prefix(tenant);

    PGresult* begin_res = PQexec(pg_conn, "BEGIN;");
    if (PQresultStatus(begin_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to BEGIN purge_versions transaction: " + std::string(PQerrorMessage(pg_conn));
        PQclear(begin_res);
        connection_pool_->release(conn);
        return Result<int>::err(error);
    }
    PQclear(begin_res);

    auto rollback_and_fail = [&](const std::string& msg) {
        PGresult* rb = PQexec(pg_conn, "ROLLBACK;");
        if (rb) PQclear(rb);
        connection_pool_->release(conn);
        return Result<int>::err(msg);
    };

    // One statement for the whole batch: = ANY($2::text[]) rather than a loop,
    // so the deletion is as atomic as the record that describes it.
    const std::string list_literal = text_array_literal(version_timestamps);
    const std::string sql =
        "DELETE FROM " + schema + ".versions "
        "WHERE file_uid = $1 AND version_timestamp = ANY($2::text[]);";
    const char* params[2] = { file_uid.c_str(), list_literal.c_str() };
    PGresult* res = PQexecParams(pg_conn, sql.c_str(), 2, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to purge versions: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        return rollback_and_fail(error);
    }
    const int removed = std::atoi(PQcmdTuples(res));
    PQclear(res);

    std::int64_t hint_seq = 0;
    {
        AccountabilityRecord rec;
        rec.ctx         = ctx;
        rec.category    = AccountabilityCategory::Destruction;
        rec.action      = accountability_action::kCullVersions;
        rec.target_uid  = file_uid;
        rec.target_type = "file";
        rec.detail.set("keep_count", keep_count);
        rec.detail.set("versions_removed", removed);
        rec.detail.set("cut_ts", cut_version_timestamp);
        auto recorded = append_accountability(pg_conn, tenant, rec);
        if (!recorded.success) {
            return rollback_and_fail("Version cull refused: " + recorded.error);
        }
        hint_seq = recorded.value.seq;
    }

    PGresult* commit_res = PQexec(pg_conn, "COMMIT;");
    if (PQresultStatus(commit_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to COMMIT purge_versions: " + std::string(PQerrorMessage(pg_conn));
        PQclear(commit_res);
        return rollback_and_fail(error);
    }
    PQclear(commit_res);

    connection_pool_->release(conn);
    fire_accountability_hint(tenant, hint_seq);
    return Result<int>::ok(removed);
}

// The pull surface (§4.3.1). Reads forward from the consumer's watermark in ts
// order — which, under the chain lock, is also seq order.
//
// Deliberately reads the PRIMARY, not a replica: the consumer treats a gap or a
// missing hinted seq as an integrity alarm rather than a retry (§4.3.2), so
// serving it lagging replica state would manufacture security alarms out of
// ordinary replication delay.
Result<std::vector<StoredAccountabilityRecord>> Database::list_accountability_records(
        const std::string& tenant, std::int64_t newer_than_micros, int limit, bool& has_more) {
    has_more = false;
    if (limit <= 0) limit = 500;
    if (limit > 5000) limit = 5000;

    auto conn = acquire(DbOp::Write);
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<StoredAccountabilityRecord>>::err(
            "Failed to acquire database connection");
    }
    PGconn* pg_conn = conn->get_connection();

    const bool is_global = (tenant == kGlobalChainKey);
    const std::string record_table = is_global
        ? std::string("accountability_record_global")
        : "\"" + get_schema_prefix(tenant) + "\".accountability_record";

    // Fetch one more than asked so has_more is a fact, not an estimate.
    const std::string micros_expr =
        "(EXTRACT(EPOCH FROM date_trunc('second', ts))::bigint * 1000000 "
        " + (EXTRACT(MICROSECONDS FROM ts)::bigint % 1000000))";
    const std::string sql =
        "SELECT seq, " + micros_expr + ", "
        "       to_char(ts AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), "
        "       actor, COALESCE(array_to_string(actor_roles, ','), ''), "
        "       COALESCE(source_iface, ''), COALESCE(source_addr, ''), "
        "       category, action, COALESCE(target_uid, ''), COALESCE(target_type, ''), "
        "       COALESCE(principal, ''), detail::text, prev_hash, hash"
        + std::string(is_global ? ", COALESCE(tenant, '')" : "") +
        "  FROM " + record_table +
        " WHERE " + micros_expr + " > $1::bigint"
        " ORDER BY seq ASC LIMIT $2::int;";

    const std::string cursor_param = std::to_string(newer_than_micros);
    const std::string limit_param  = std::to_string(limit + 1);
    const char* params[2] = { cursor_param.c_str(), limit_param.c_str() };

    PGresult* res = PQexecParams(pg_conn, sql.c_str(), 2, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        const std::string err = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::vector<StoredAccountabilityRecord>>::err(
            "Failed to read accountability records: " + err);
    }

    int rows = PQntuples(res);
    if (rows > limit) {
        has_more = true;
        rows = limit;
    }

    std::vector<StoredAccountabilityRecord> out;
    out.reserve(rows);
    for (int i = 0; i < rows; ++i) {
        StoredAccountabilityRecord r;
        r.seq       = std::strtoll(PQgetvalue(res, i, 0), nullptr, 10);
        r.ts_micros = std::strtoll(PQgetvalue(res, i, 1), nullptr, 10);
        r.ts_iso    = PQgetvalue(res, i, 2);
        r.record.ctx.actor        = PQgetvalue(res, i, 3);
        const std::string roles_csv = PQgetvalue(res, i, 4);
        if (!roles_csv.empty()) {
            size_t start = 0;
            while (start <= roles_csv.size()) {
                size_t comma = roles_csv.find(',', start);
                if (comma == std::string::npos) {
                    r.record.ctx.actor_roles.push_back(roles_csv.substr(start));
                    break;
                }
                r.record.ctx.actor_roles.push_back(roles_csv.substr(start, comma - start));
                start = comma + 1;
            }
        }
        r.record.ctx.source_iface = PQgetvalue(res, i, 5);
        r.record.ctx.source_addr  = PQgetvalue(res, i, 6);
        const std::string category = PQgetvalue(res, i, 7);
        r.record.category = (category == "identity")    ? AccountabilityCategory::Identity
                          : (category == "destruction") ? AccountabilityCategory::Destruction
                          : (category == "lifecycle")   ? AccountabilityCategory::Lifecycle
                                                        : AccountabilityCategory::Authorization;
        r.record.action      = PQgetvalue(res, i, 8);
        r.record.target_uid  = PQgetvalue(res, i, 9);
        r.record.target_type = PQgetvalue(res, i, 10);
        r.record.principal   = PQgetvalue(res, i, 11);
        // Re-canonicalize: Postgres normalizes JSONB into its own key order and
        // spacing, so handing back its text form would give the consumer bytes
        // that cannot reproduce the stored hash.
        auto detail = detail_from_json(PQgetisnull(res, i, 12) ? "" : PQgetvalue(res, i, 12));
        if (!detail.success) {
            PQclear(res);
            connection_pool_->release(conn);
            return Result<std::vector<StoredAccountabilityRecord>>::err(
                "Accountability record seq " + std::to_string(r.seq) + ": " + detail.error);
        }
        r.record.detail = detail.value;
        if (!PQgetisnull(res, i, 13)) r.prev_hash = parse_bytea_hex(PQgetvalue(res, i, 13));
        r.hash = parse_bytea_hex(PQgetvalue(res, i, 14));
        if (is_global) r.record.global_tenant = PQgetvalue(res, i, 15);
        out.push_back(std::move(r));
    }

    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::vector<StoredAccountabilityRecord>>::ok(out);
}

// The seq a freshness hint asserts. Without it a consumer cannot tell "no new
// records" apart from "I am reading state that has not caught up" — and the
// second silently looks exactly like the first (§4.3).
Result<std::int64_t> Database::accountability_head_seq(const std::string& tenant) {
    auto conn = acquire(DbOp::Write);
    if (!conn || !conn->is_valid()) {
        return Result<std::int64_t>::err("Failed to acquire database connection");
    }
    PGconn* pg_conn = conn->get_connection();

    const bool is_global = (tenant == kGlobalChainKey);
    const std::string head_table = is_global
        ? std::string("accountability_chain_head_global")
        : "\"" + get_schema_prefix(tenant) + "\".accountability_chain_head";
    const std::string chain_key = is_global ? std::string(kGlobalChainKey)
                                            : (tenant.empty() ? std::string("default") : tenant);

    const std::string sql = "SELECT last_seq FROM " + head_table + " WHERE tenant = $1;";
    const char* params[1] = { chain_key.c_str() };
    PGresult* res = PQexecParams(pg_conn, sql.c_str(), 1, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        const std::string err = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<std::int64_t>::err("Failed to read accountability head: " + err);
    }
    std::int64_t seq = (PQntuples(res) == 1) ? std::strtoll(PQgetvalue(res, 0, 0), nullptr, 10) : 0;
    PQclear(res);
    connection_pool_->release(conn);
    return Result<std::int64_t>::ok(seq);
}

Result<void> Database::create_tenant_schema(const std::string& tenant,
                                           const AccountabilityContext& ctx) {
    auto conn = acquire(DbOp::Write);
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
        "deleted BOOLEAN NOT NULL DEFAULT FALSE, "
        "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "  // fallback ctime (no revisions)
        "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP"    // fallback mtime; bumped on change
        ");";

    // Migration for tenants whose files table predates these columns. ADD COLUMN
    // IF NOT EXISTS backfills existing rows via the CURRENT_TIMESTAMP default.
    std::string migrate_files_timestamps =
        "ALTER TABLE \"" + escaped_schema + "\".files "
        "ADD COLUMN IF NOT EXISTS created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
        "ADD COLUMN IF NOT EXISTS updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP;";

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
        "revised_by VARCHAR(255) NOT NULL DEFAULT 'unknown', "  // acting user who wrote the revision
        "UNIQUE (file_uid, version_timestamp) "
        ");";

    // Migration for tenants created before revised_by existed: ADD COLUMN IF NOT
    // EXISTS backfills every existing revision row with the 'unknown' placeholder
    // (the column DEFAULT). New revisions carry the real acting user.
    std::string migrate_versions_revised_by =
        "ALTER TABLE \"" + escaped_schema + "\".versions "
        "ADD COLUMN IF NOT EXISTS revised_by VARCHAR(255) NOT NULL DEFAULT 'unknown';";

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

    // Idempotent backfill migration for pre-existing tenants (non-critical).
    res = PQexec(pg_conn, migrate_files_timestamps.c_str());
    PQclear(res);  // columns may already exist; status is irrelevant

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

    // Idempotent backfill migration for pre-existing tenants (non-critical).
    res = PQexec(pg_conn, migrate_versions_revised_by.c_str());
    PQclear(res);  // column may already exist; status is irrelevant

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

    // ── Full audit log (design_documents/usage_logging_and_auditing.md §4) ────
    // Complete, append-only, tamper-evident record of who did what to what,
    // when, from where, and whether it was allowed. Supersedes acl_audit for the
    // permission category (§1) and is distinct from the fail-open event stream.
    // Written ONLY by the single out-of-process audit-consumer (§5): the core and
    // the other emitters publish tenant-tagged entries to the aggregating Redis
    // sink; the consumer demultiplexes by tenant and appends here, owning the
    // per-tenant hash chain.
    //
    // RANGE-partitioned by day on ts for the 30-day rolling window + daily
    // encrypted archival (§7). Postgres requires the partition key in every
    // unique constraint, so the primary key is (seq, ts) rather than seq alone,
    // and the idempotency key (at-least-once Redis redelivery must not duplicate
    // a row or corrupt the hash chain) is UNIQUE (event_id, ts). Daily child
    // partitions are created on demand by the consumer; none are created here,
    // since nothing else ever writes this table. row_hash is nullable until the
    // hash chain lands (Phase 7); the single-writer discipline is the Phase 1
    // append-only guarantee (separate append-only DB role is also Phase 7).
    std::string create_audit_log_table =
        "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".audit_log ("
        "    seq          BIGSERIAL,"
        "    event_id     UUID         NOT NULL,"
        "    ts           TIMESTAMPTZ  NOT NULL DEFAULT now(),"
        "    category     SMALLINT     NOT NULL,"   // access|mutate|permission|user|auth|admin
        "    action       VARCHAR(32)  NOT NULL,"
        "    outcome      SMALLINT     NOT NULL,"   // ok|denied|error
        "    actor        VARCHAR(255) NOT NULL,"
        "    actor_roles  TEXT,"
        "    target_uid   VARCHAR(64),"
        // Never populated any more — see the audit_log_global comment
        // in create_schema and §5.4.7. Kept nullable so existing rows
        // and the hash chain's canonical form are unaffected.
        "    target_name  VARCHAR(1024),"
        "    target_type  SMALLINT,"
        "    detail       JSONB,"
        "    source_iface VARCHAR(16),"
        "    source_addr  VARCHAR(64),"
        "    request_id   VARCHAR(64),"
        // `ts` is when the event OCCURRED; recorded_at is when it was appended
        // to the chain. The chain is ordered by the latter; the former can run
        // slightly backwards between adjacent entries from different sources
        // (§4.3.4). Not part of the hash — it describes delivery, not the event.
        "    recorded_at  TIMESTAMPTZ  NOT NULL DEFAULT now(),"
        "    prev_hash    BYTEA,"
        "    row_hash     BYTEA,"
        "    PRIMARY KEY (seq, ts),"
        "    UNIQUE (event_id, ts)"
        ") PARTITION BY RANGE (ts);";
    res = PQexec(pg_conn, create_audit_log_table.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to create tenant audit_log table: " + error);
    }
    PQclear(res);

    // Query indexes for the console/export API (§9). Indexes on a partitioned
    // parent are themselves partitioned and propagate to every child partition.
    // Names are suffixed with the schema per the repo convention (index names
    // are unique per-schema). Index creation is non-critical.
    // Backfill migration for tenants created before recorded_at existed. On a
    // partitioned parent this propagates to every child partition.
    std::string migrate_audit_recorded_at =
        "ALTER TABLE \"" + escaped_schema + "\".audit_log "
        "ADD COLUMN IF NOT EXISTS recorded_at TIMESTAMPTZ NOT NULL DEFAULT now();";
    res = PQexec(pg_conn, migrate_audit_recorded_at.c_str());
    PQclear(res);  // column may already exist; status is irrelevant

    std::string create_idx_audit_actor =
        "CREATE INDEX IF NOT EXISTS idx_audit_actor_" + escaped_schema +
        " ON \"" + escaped_schema + "\".audit_log(actor, ts);";
    res = PQexec(pg_conn, create_idx_audit_actor.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); }

    std::string create_idx_audit_target =
        "CREATE INDEX IF NOT EXISTS idx_audit_target_" + escaped_schema +
        " ON \"" + escaped_schema + "\".audit_log(target_uid, ts);";
    res = PQexec(pg_conn, create_idx_audit_target.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); }

    std::string create_idx_audit_action =
        "CREATE INDEX IF NOT EXISTS idx_audit_action_" + escaped_schema +
        " ON \"" + escaped_schema + "\".audit_log(category, action, ts);";
    res = PQexec(pg_conn, create_idx_audit_action.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); }

    // ── The guaranteed accountability record (PROPOSAL_accountability_record.md)
    // Append-only, written in the SAME TRANSACTION as the operation it
    // describes, never culled, and not disableable by configuration. It exists
    // because the audit path above cannot *guarantee* a record: it is
    // best-effort for mutations, silently absent when unconfigured,
    // non-transactional and node-local (§3.2). This table closes all four.
    //
    // Deliberately NOT partitioned and NOT range-keyed on ts like audit_log:
    // retention here is "never delete" (§5.2), so there is nothing for a
    // partition boundary to do, and the consumer reads it as a cursor-ordered
    // outbox rather than a time window.
    //
    // seq is a plain BIGINT, NOT a BIGSERIAL. That is the load-bearing choice
    // (§5.3.1): sequence values are assigned at INSERT and become visible at
    // COMMIT, so two concurrent writers can take 9 and 10 and 10 can commit
    // first — a cursor reader would advance past 9 and lose it permanently, for
    // exactly the record type that must never be lost. Rolled-back transactions
    // also burn values, making gaps routine and gap detection useless. Instead
    // seq is assigned under the chain-head lock (see accountability_chain_head),
    // which aligns assignment order, commit order and chain order.
    std::string create_accountability_table =
        "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".accountability_record ("
        "    seq          BIGINT       PRIMARY KEY,"      // gap-free, commit-ordered
        "    ts           TIMESTAMPTZ  NOT NULL,"          // clock_timestamp() under the lock, strictly monotonic
        "    actor        VARCHAR(255) NOT NULL,"          // never empty; 'system' is explicit
        "    actor_roles  TEXT[],"                         // roles as presented at the time
        "    source_iface VARCHAR(32),"
        "    source_addr  VARCHAR(64),"
        "    category     VARCHAR(32)  NOT NULL,"          // authorization|identity|destruction|lifecycle
        "    action       VARCHAR(64)  NOT NULL,"
        "    target_uid   VARCHAR(64),"                    // the uid ONLY — names resolve at read time (§5.4.7)
        "    target_type  VARCHAR(32),"
        "    principal    VARCHAR(255),"                   // whose access changed
        "    detail       JSONB        NOT NULL DEFAULT '{}'::jsonb,"  // schema-constrained per action
        "    prev_hash    BYTEA,"
        "    hash         BYTEA        NOT NULL"
        ");";
    res = PQexec(pg_conn, create_accountability_table.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to create tenant accountability_record table: " + error);
    }
    PQclear(res);

    // The chain head is both the tamper-evidence link AND the sequencer (§5.3.2).
    // Every append locks this one row, so assignment order == commit order ==
    // chain order: no gaps (a rollback releases the lock without advancing
    // last_seq), no out-of-order visibility, and a consumer that checks
    // contiguity is checking delivery integrity at the same time. The cost is
    // that accountability writes serialize per tenant — affordable only because
    // the scope is rare operations (§4.1), never a content path.
    std::string create_accountability_head =
        "CREATE TABLE IF NOT EXISTS \"" + escaped_schema + "\".accountability_chain_head ("
        "    tenant     VARCHAR(255) PRIMARY KEY,"
        "    last_seq   BIGINT NOT NULL DEFAULT 0,"
        "    last_ts    TIMESTAMPTZ,"
        "    last_hash  BYTEA"
        ");";
    res = PQexec(pg_conn, create_accountability_head.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to create tenant accountability_chain_head table: " + error);
    }
    PQclear(res);

    // Seed the head row here so the write path's bootstrap is normally a no-op.
    // ON CONFLICT DO NOTHING because tenant provisioning is not serialized —
    // idempotent DDL is not the same thing as concurrency-safe DDL, and two
    // simultaneous provisioning calls must both succeed.
    std::string seed_accountability_head =
        "INSERT INTO \"" + escaped_schema + "\".accountability_chain_head (tenant) "
        "VALUES (" + escape_string(tenant.empty() ? "default" : tenant, pg_conn) + ") "
        "ON CONFLICT (tenant) DO NOTHING;";
    res = PQexec(pg_conn, seed_accountability_head.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); }
    else { PQclear(res); }

    // §4.2 guarantee 5: the accountability question must be answerable from the
    // core alone, with no audit_service and no Redis. (ts) is the consumer
    // cursor; the other two answer "everything actor A did between T1 and T2"
    // and "every change affecting this resource".
    for (const auto& idx : std::vector<std::string>{
             "CREATE INDEX IF NOT EXISTS idx_accountability_ts_" + escaped_schema +
                 " ON \"" + escaped_schema + "\".accountability_record(ts);",
             "CREATE INDEX IF NOT EXISTS idx_accountability_actor_" + escaped_schema +
                 " ON \"" + escaped_schema + "\".accountability_record(actor, ts);",
             "CREATE INDEX IF NOT EXISTS idx_accountability_target_" + escaped_schema +
                 " ON \"" + escaped_schema + "\".accountability_record(target_uid, ts);",
             "CREATE INDEX IF NOT EXISTS idx_accountability_principal_" + escaped_schema +
                 " ON \"" + escaped_schema + "\".accountability_record(principal, ts);"}) {
        res = PQexec(pg_conn, idx.c_str());
        if (PQresultStatus(res) != PGRES_COMMAND_OK) { PQclear(res); }
        else { PQclear(res); }
    }

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

    // Register the tenant in the global tenants table, and — if this call is
    // what actually created it — record the fact globally (§7.3).
    //
    // The registry INSERT is the arbiter of "was this tenant new?", not a
    // separate existence check: this function is called lazily on nearly every
    // tenant context lookup and is not serialized between callers, so an
    // exists-then-create pair would let two simultaneous provisioning calls
    // both conclude "new" and write two creation records. ON CONFLICT DO
    // NOTHING RETURNING answers it atomically — exactly one caller gets a row.
    std::string tenant_id_to_register = tenant.empty() ? "default" : tenant;

    PGresult* begin_reg = PQexec(pg_conn, "BEGIN;");
    if (PQresultStatus(begin_reg) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(begin_reg);
        connection_pool_->release(conn);
        return Result<void>::err("Failed to begin tenant registration transaction: " + error);
    }
    PQclear(begin_reg);

    auto registration_failed = [&](const std::string& msg) {
        PGresult* rb = PQexec(pg_conn, "ROLLBACK;");
        if (rb) PQclear(rb);
        connection_pool_->release(conn);
        return Result<void>::err(msg);
    };

    std::string register_tenant_sql =
        "INSERT INTO tenants (tenant_id, schema_name, created_at, updated_at) "
        "VALUES ($1, $2, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP) "
        "ON CONFLICT (tenant_id) DO NOTHING RETURNING tenant_id;";
    const char* register_params[2] = { tenant_id_to_register.c_str(), escaped_schema.c_str() };
    res = PQexecParams(pg_conn, register_tenant_sql.c_str(), 2, nullptr, register_params,
                       nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(res);
        return registration_failed("Failed to register tenant '" + tenant_id_to_register +
                                   "' in public.tenants: " + error);
    }
    const bool tenant_is_new = PQntuples(res) > 0;
    PQclear(res);

    std::int64_t hint_seq = 0;
    if (tenant_is_new) {
        AccountabilityRecord rec;
        rec.ctx           = ctx;
        rec.category      = AccountabilityCategory::Lifecycle;
        rec.action        = accountability_action::kTenantCreate;
        rec.target_type   = "tenant";
        rec.global_tenant = tenant_id_to_register;
        rec.detail.set("schema", escaped_schema);
        auto recorded = append_accountability(pg_conn, kGlobalChainKey, rec);
        if (!recorded.success) {
            // Fail-closed. The schema itself already exists at this point, but
            // reporting success would leave a tenant the platform cannot say
            // was ever created, by whom. Returning an error makes the caller
            // retry, and the retry completes the registration and the record —
            // the DDL above is all idempotent.
            return registration_failed("Tenant creation refused: " + recorded.error);
        }
        hint_seq = recorded.value.seq;
    } else {
        std::string touch_sql = "UPDATE tenants SET updated_at = CURRENT_TIMESTAMP "
                                "WHERE tenant_id = $1;";
        const char* touch_params[1] = { tenant_id_to_register.c_str() };
        res = PQexecParams(pg_conn, touch_sql.c_str(), 1, nullptr, touch_params, nullptr, nullptr, 0);
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            SERVER_LOG_WARN("Database::create_tenant_schema",
                            "Failed to touch tenant '" + tenant_id_to_register +
                            "' in public.tenants: " + std::string(PQerrorMessage(pg_conn)));
        }
        PQclear(res);
    }

    PGresult* commit_reg = PQexec(pg_conn, "COMMIT;");
    if (PQresultStatus(commit_reg) != PGRES_COMMAND_OK) {
        std::string error = PQerrorMessage(pg_conn);
        PQclear(commit_reg);
        return registration_failed("Failed to commit tenant registration: " + error);
    }
    PQclear(commit_reg);

    connection_pool_->release(conn);
    // Hint on the GLOBAL chain: that is where the lifecycle record lives.
    fire_accountability_hint(kGlobalChainKey, hint_seq);

    return Result<void>::ok();
}

Result<bool> Database::tenant_schema_exists(const std::string& tenant) {
    if (tenant.empty()) {
        return Result<bool>::err("Cannot check existence for empty tenant name");
    }

    auto conn = acquire(DbOp::Read);
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

Result<void> Database::cleanup_tenant_data(const std::string& tenant,
                                          const AccountabilityContext& ctx) {
    auto conn = acquire(DbOp::Write);
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

    std::int64_t hint_seq = 0;
    // Record the destruction BEFORE destroying anything, in the global table,
    // in this same transaction (§7.3).
    //
    // Global, because a record inside the schema about to be dropped would
    // delete itself — and if deleting a tenant erased every trace of it, tenant
    // deletion would become the cleanest way to destroy evidence: whoever can
    // delete a tenant could erase the whole record of what happened inside it,
    // including their own actions. The contents genuinely go; the fact that they
    // existed and who removed them lives where the deleter cannot reach.
    //
    // First, because if the record cannot be written the transaction rolls back
    // with the tenant intact. The other order would risk the most destructive
    // operation in the system succeeding silently — which §2.1 found is exactly
    // what it does today: DROP SCHEMA CASCADE emits no event at all.
    {
        std::string tenant_id = tenant.empty() ? "default" : tenant;
        AccountabilityRecord rec;
        rec.ctx           = ctx;
        rec.category      = AccountabilityCategory::Destruction;
        rec.action        = accountability_action::kTenantDelete;
        rec.target_type   = "tenant";
        rec.global_tenant = tenant_id;
        rec.detail.set("schema", schema);
        auto recorded = append_accountability(pg_conn, kGlobalChainKey, rec);
        if (!recorded.success) {
            PGresult* rb = PQexec(pg_conn, "ROLLBACK;");
            if (rb) PQclear(rb);
            connection_pool_->release(conn);
            return Result<void>::err("Tenant deletion refused: " + recorded.error);
        }
        hint_seq = recorded.value.seq;
    }

    // Drop the tenant schema (CASCADE will remove all tables in it) — including
    // the tenant's own accountability_record, by design (§5.2): the record
    // outlives every destruction of the thing it describes EXCEPT the
    // destruction of the tenant that owns it.
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
    fire_accountability_hint(kGlobalChainKey, hint_seq);
    return Result<void>::ok();
}

Result<std::vector<std::string>> Database::list_tenants() {
    auto conn = acquire(DbOp::Read);
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

    // A dedicated pool for the read-only standby. Reads route here while failed
    // over (see acquire()). Initialized eagerly; if the standby is down now it can
    // still be acquired (and retried) later.
    secondary_pool_ = std::make_shared<ConnectionPool>(host, port, database_name, user, password, pool_size_);
    if (!secondary_pool_->initialize()) {
        std::cerr << "Secondary database pool failed to initialize (will retry on use): "
                  << host << ":" << port << std::endl;
    }
}

std::shared_ptr<DatabaseConnection> Database::acquire(DbOp op) {
    // Writes always use the primary; reads use the replica only while failed over.
    ConnectionPool* pool = select_pool(op, connection_pool_.get(),
                                       secondary_pool_.get(), using_secondary_.load());
    return pool ? pool->acquire() : nullptr;
}

void Database::start_connection_monitoring() {
    if (monitoring_active_.load()) {
        return; // Already running
    }

    monitoring_active_.store(true);

    connection_monitor_thread_ = std::thread([this]() {
        SERVER_LOG_INFO("Database",
                        "connection watchdog started (probe every " +
                        std::to_string(retry_interval_seconds_) + "s)");
        while (monitoring_active_.load()) {
            // Probe the primary: a cheap health check while up, a reconnect attempt
            // while down. The disconnect/recovery transition is the pure, unit-tested
            // next_failover_state() (REPLICATION_FAILOVER.md).
            const auto probe_start = std::chrono::steady_clock::now();
            monitor_probe_in_flight_.store(true);
            const bool was_up = primary_available_.load();
            const bool reachable = was_up ? is_connected() : connect();
            monitor_probe_in_flight_.store(false);

            ConnectionPoolManager& mgr = ConnectionPoolManager::get_instance();
            FailoverState cur{was_up, using_secondary_.load(), mgr.is_server_in_readonly_mode()};
            FailoverState next = next_failover_state(cur, reachable, !secondary_conn_info_.empty());

            if (next != cur) {
                monitor_transitions_.fetch_add(1);
                primary_available_.store(next.primary_available);
                using_secondary_.store(next.using_secondary);
                mgr.set_server_in_readonly_mode(next.readonly_mode);
                if (!next.primary_available) {
                    SERVER_LOG_ERROR("Database",
                                     "primary unavailable; entering read-only fallback mode");
                    std::cerr << "Database primary unavailable; entering read-only fallback mode."
                              << std::endl;
                } else {
                    SERVER_LOG_INFO("Database",
                                    "primary restored; resuming normal operation");
                    std::cout << "Database connection to primary restored; resuming normal operation."
                              << std::endl;
                }
            }

            // Record the probe AFTER acting on it, so a consumer that sees a fresh
            // timestamp knows the whole iteration completed rather than just started.
            monitor_last_probe_ms_.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - probe_start).count());
            monitor_last_probe_epoch_.store(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            monitor_probes_.fetch_add(1);

            // Sleep until the next probe — but wake immediately if asked to stop.
            // The poll interval is the design; an uninterruptible sleep was not.
            std::unique_lock<std::mutex> wait_lock(monitor_wait_mutex_);
            monitor_wait_cv_.wait_for(wait_lock,
                                      std::chrono::seconds(retry_interval_seconds_),
                                      [this]() { return !monitoring_active_.load(); });
        }
        SERVER_LOG_INFO("Database", "connection watchdog stopped");
    });
}

void Database::stop_connection_monitoring() {
    if (!monitoring_active_.load()) {
        return;
    }

    {
        // Flip the flag under the wait mutex so the watchdog cannot miss the
        // wakeup in the window between testing it and blocking on the CV.
        std::lock_guard<std::mutex> wait_lock(monitor_wait_mutex_);
        monitoring_active_.store(false);
    }
    monitor_wait_cv_.notify_all();

    if (connection_monitor_thread_.joinable()) {
        connection_monitor_thread_.join();
    }
}

ConnectionPoolStats Database::pool_stats() const {
    return connection_pool_ ? connection_pool_->stats() : ConnectionPoolStats{};
}

ConnectionPoolStats Database::secondary_pool_stats() const {
    return secondary_pool_ ? secondary_pool_->stats() : ConnectionPoolStats{};
}

Database::WatchdogStats Database::watchdog_stats() const {
    WatchdogStats w;
    w.running                = monitoring_active_.load();
    w.probe_in_flight        = monitor_probe_in_flight_.load();
    w.primary_available      = primary_available_.load();
    w.using_secondary        = using_secondary_.load();
    w.interval_seconds       = retry_interval_seconds_;
    w.probes_total           = monitor_probes_.load();
    w.transitions_total      = monitor_transitions_.load();
    w.last_probe_epoch       = monitor_last_probe_epoch_.load();
    w.last_probe_duration_ms = monitor_last_probe_ms_.load();
    if (w.last_probe_epoch > 0) {
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        w.last_probe_age_seconds = now - w.last_probe_epoch;
    }
    return w;
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

    auto conn = acquire(DbOp::Write);
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
            "is_container = EXCLUDED.is_container, "
            "updated_at = CURRENT_TIMESTAMP "        // bump mtime on any metadata change
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
                               const AccountabilityContext& ctx,
                               int effect) {
    // §5.1: refuse before touching anything. An authorization change that cannot
    // name who made it is a bug, and recording "" would be worse than recording
    // nothing because it looks like coverage.
    if (!ctx.valid()) {
        return Result<void>::err("Refusing ACL grant with no actor");
    }
    const std::string& performed_by = ctx.actor;

    auto conn = acquire(DbOp::Write);
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();
    // The acls table is created at tenant init time (see create_tenant_schema).
    std::string schema = get_schema_prefix(tenant);

    // One transaction covering the ACL row, its acl_audit row and the
    // accountability record — so the operation and its record commit together
    // or not at all (§4.2 guarantee 1).
    PGresult* begin_res = PQexec(pg_conn, "BEGIN;");
    if (PQresultStatus(begin_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to BEGIN add_acl transaction: " + std::string(PQerrorMessage(pg_conn));
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

    std::string type_str = std::to_string(type);
    std::string perms_str = std::to_string(permissions);
    std::string effect_str = std::to_string(effect);

    // Capture the prior permissions bitmask for the (principal, type, effect)
    // row so the audit log can record before/after deltas.
    //
    // A FAILURE here must abort, not be shrugged off. This read now runs inside
    // the operation's transaction, and a failed statement poisons the whole
    // transaction — so ignoring its status would leave every subsequent
    // statement reporting "current transaction is aborted" and bury the real
    // cause (an unprovisioned tenant, say) behind a message that explains
    // nothing.
    std::string select_before_sql =
        "SELECT permissions FROM " + schema + ".acls "
        "WHERE resource_uid = $1 AND principal = $2 AND principal_type = ($3::int) AND effect = ($4::int);";
    const char* before_params[4] = {
        resource_uid.c_str(), principal.c_str(), type_str.c_str(), effect_str.c_str() };
    int permissions_before = 0;
    bool had_row = false;
    PGresult* before_res = PQexecParams(pg_conn, select_before_sql.c_str(), 4,
                                        nullptr, before_params, nullptr, nullptr, 0);
    if (PQresultStatus(before_res) != PGRES_TUPLES_OK) {
        std::string error = "Failed to read the existing ACL row: " +
                            std::string(PQerrorMessage(pg_conn));
        PQclear(before_res);
        return rollback_and_fail(error);
    }
    if (PQntuples(before_res) > 0) {
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
        return rollback_and_fail(error);
    }
    PQclear(res);

    // acl_audit row. Still best-effort — it is a convenience view of the same
    // facts, superseded for accountability purposes by the record below.
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

    // The guaranteed record. PartOfCreation writes skip it deliberately — see
    // AccountabilityMode: creation-time default and inherited ACLs are already
    // attributed by the resource's own row, and chaining them would put the
    // per-tenant serializing lock on the content-creation path.
    std::int64_t hint_seq = 0;
    if (ctx.mode == AccountabilityMode::Record) {
        AccountabilityRecord rec;
        rec.ctx         = ctx;
        rec.category    = AccountabilityCategory::Authorization;
        rec.action      = accountability_action::kAclGrant;
        rec.target_uid  = resource_uid;
        rec.target_type = "acl";
        rec.principal   = principal;
        rec.detail.set("principal_type", type);
        rec.detail.set("effect", std::string(effect == 1 ? "deny" : "allow"));
        rec.detail.set("mask", permissions);
        rec.detail.set("mask_before", before_value);
        rec.detail.set("mask_after", after_value);
        auto recorded = append_accountability(pg_conn, tenant, rec);
        if (!recorded.success) {
            // Fail-closed (§4.2 guarantee 2). This deliberately inverts the
            // best-effort posture of the audit path, and is affordable only
            // because the scope is rare operations.
            return rollback_and_fail("Permission change refused: " + recorded.error);
        }
        hint_seq = recorded.value.seq;
    }

    PGresult* commit_res = PQexec(pg_conn, "COMMIT;");
    if (PQresultStatus(commit_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to COMMIT add_acl: " + std::string(PQerrorMessage(pg_conn));
        PQclear(commit_res);
        return rollback_and_fail(error);
    }
    PQclear(commit_res);

    connection_pool_->release(conn);
    fire_accountability_hint(tenant, hint_seq);
    return Result<void>::ok();
}

Result<void> Database::remove_acl(const std::string& resource_uid, const std::string& principal,
                                  int type, int permissions,
                                  const std::string& tenant,
                                  const AccountabilityContext& ctx,
                                  int effect) {
    // §5.1 again: a revoke is the operation §2.1 flagged as destroying its own
    // evidence — the row disappears when the remaining bitmask is zero — so an
    // unattributed one is the worst case of all.
    if (!ctx.valid()) {
        return Result<void>::err("Refusing ACL revoke with no actor");
    }
    const std::string& performed_by = ctx.actor;

    auto conn = acquire(DbOp::Write);
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();
    std::string schema = get_schema_prefix(tenant);

    std::string type_str = std::to_string(type);
    std::string perms_str = std::to_string(permissions);
    std::string effect_str = std::to_string(effect);

    // Capture the prior permissions bitmask for the audit row. As in add_acl, a
    // failure here is fatal to the operation rather than ignored — otherwise the
    // real cause is replaced by "current transaction is aborted" further down.
    std::string select_before_sql =
        "SELECT permissions FROM " + schema + ".acls "
        "WHERE resource_uid = $1 AND principal = $2 AND principal_type = ($3::int) AND effect = ($4::int);";
    const char* before_params[4] = {
        resource_uid.c_str(), principal.c_str(), type_str.c_str(), effect_str.c_str() };
    int permissions_before = 0;
    bool had_row = false;
    PGresult* before_res = PQexecParams(pg_conn, select_before_sql.c_str(), 4,
                                        nullptr, before_params, nullptr, nullptr, 0);
    if (PQresultStatus(before_res) != PGRES_TUPLES_OK) {
        std::string error = "Failed to read the existing ACL row: " +
                            std::string(PQerrorMessage(pg_conn));
        PQclear(before_res);
        connection_pool_->release(conn);
        return Result<void>::err(error);
    }
    if (PQntuples(before_res) > 0) {
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
    const bool row_removed = had_row && ((permissions_before & ~permissions) == 0);
    PQclear(res);

    // The audit + accountability writes moved INSIDE this transaction. They used
    // to run after COMMIT, which meant a crash in the gap left a committed
    // revoke with no record of it anywhere — precisely §3.2's gap 3.
    //
    // Only record if there was a row to act on: a revoke of permissions the
    // principal did not hold changed nothing, and a chain entry for it would be
    // a claim that something was revoked when nothing was.
    std::int64_t hint_seq = 0;
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

        if (ctx.mode == AccountabilityMode::Record) {
            AccountabilityRecord rec;
            rec.ctx         = ctx;
            rec.category    = AccountabilityCategory::Authorization;
            rec.action      = accountability_action::kAclRevoke;
            rec.target_uid  = resource_uid;
            rec.target_type = "acl";
            rec.principal   = principal;
            rec.detail.set("principal_type", type);
            rec.detail.set("effect", std::string(effect == 1 ? "deny" : "allow"));
            rec.detail.set("mask", permissions);
            rec.detail.set("mask_before", permissions_before);
            rec.detail.set("mask_after", permissions_before & ~permissions);
            // The §2.1 finding made visible: this is the flag that says the ACL
            // row itself was destroyed, not merely narrowed. Without the record,
            // nothing anywhere would show that the access ever existed.
            rec.detail.set("row_removed", row_removed);
            auto recorded = append_accountability(pg_conn, tenant, rec);
            if (!recorded.success) {
                return rollback_and_fail("Permission change refused: " + recorded.error);
            }
            hint_seq = recorded.value.seq;
        }
    }

    PGresult* commit_res = PQexec(pg_conn, "COMMIT;");
    if (PQresultStatus(commit_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to COMMIT remove_acl: " + std::string(PQerrorMessage(pg_conn));
        PQclear(commit_res);
        return rollback_and_fail(error);
    }
    PQclear(commit_res);

    connection_pool_->release(conn);
    fire_accountability_hint(tenant, hint_seq);
    return Result<void>::ok();
}

Result<std::vector<IDatabase::AclEntry>> Database::get_acls_for_resource(const std::string& resource_uid,
                                                                         const std::string& tenant) {
    auto conn = acquire(DbOp::Read);
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
    auto conn = acquire(DbOp::Read);
    if (!conn || !conn->is_valid()) {
        return Result<std::vector<IDatabase::AclEntry>>::err("Failed to acquire database connection");
    }

    PGconn* pg_conn = conn->get_connection();

    // Get tenant-specific schema prefix
    std::string schema = get_schema_prefix(tenant);

    // Filter by principal_type so user/role/claim names in the same namespace
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
    auto conn = acquire(DbOp::Read);
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
// The four role operations below share one shape: BEGIN, do the work, write the
// accountability record, COMMIT — so the record cannot be lost by a crash in
// between, and a record that cannot be written takes the operation down with it.
//
// role.create and role.assign only record when they actually changed something
// (ON CONFLICT DO NOTHING can make them no-ops). Recording a no-op would put a
// claim in an immutable, never-culled chain that a membership was granted when
// it already existed.
Result<void> Database::create_role(const std::string& role, const std::string& tenant,
                                   const AccountabilityContext& ctx) {
    if (role.empty()) {
        return Result<void>::err("Role name cannot be empty");
    }
    if (!ctx.valid()) {
        return Result<void>::err("Refusing role creation with no actor");
    }
    auto conn = acquire(DbOp::Write);
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }
    PGconn* pg_conn = conn->get_connection();
    std::string schema = get_schema_prefix(tenant);

    PGresult* begin_res = PQexec(pg_conn, "BEGIN;");
    if (PQresultStatus(begin_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to BEGIN create_role transaction: " + std::string(PQerrorMessage(pg_conn));
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

    std::string sql = "INSERT INTO " + schema + ".roles (role_name) VALUES ($1) "
                      "ON CONFLICT (role_name) DO NOTHING RETURNING role_name;";
    const char* params[1] = { role.c_str() };
    PGresult* res = PQexecParams(pg_conn, sql.c_str(), 1, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = "Failed to create role: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        return rollback_and_fail(error);
    }
    const bool created = PQntuples(res) > 0;
    PQclear(res);

    std::int64_t hint_seq = 0;
    if (created) {
        AccountabilityRecord rec;
        rec.ctx         = ctx;
        rec.category    = AccountabilityCategory::Identity;
        rec.action      = accountability_action::kRoleCreate;
        rec.target_type = "role";
        rec.principal   = role;
        rec.detail.set("role", role);
        auto recorded = append_accountability(pg_conn, tenant, rec);
        if (!recorded.success) {
            return rollback_and_fail("Role creation refused: " + recorded.error);
        }
        hint_seq = recorded.value.seq;
    }

    PGresult* commit_res = PQexec(pg_conn, "COMMIT;");
    if (PQresultStatus(commit_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to COMMIT create_role: " + std::string(PQerrorMessage(pg_conn));
        PQclear(commit_res);
        return rollback_and_fail(error);
    }
    PQclear(commit_res);
    connection_pool_->release(conn);
    fire_accountability_hint(tenant, hint_seq);
    return Result<void>::ok();
}

Result<void> Database::delete_role(const std::string& role, const std::string& tenant,
                                   const AccountabilityContext& ctx) {
    if (role.empty()) {
        return Result<void>::err("Role name cannot be empty");
    }
    if (!ctx.valid()) {
        return Result<void>::err("Refusing role deletion with no actor");
    }
    auto conn = acquire(DbOp::Write);
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }
    PGconn* pg_conn = conn->get_connection();
    std::string schema = get_schema_prefix(tenant);

    // §2.1 site 3: this destroys every membership of the role, outside culling
    // and with no history of its own. Wrapping it in a transaction with the
    // record is what makes the destruction accountable — the memberships are
    // still gone, but who removed them and how many there were is not.
    PGresult* begin_res = PQexec(pg_conn, "BEGIN;");
    if (PQresultStatus(begin_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to BEGIN delete_role transaction: " + std::string(PQerrorMessage(pg_conn));
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
        return rollback_and_fail(error);
    }
    const int memberships_removed = std::atoi(PQcmdTuples(res));
    PQclear(res);

    res = PQexecParams(pg_conn, sql_roles.c_str(), 1, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to delete role: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        return rollback_and_fail(error);
    }
    PQclear(res);

    std::int64_t hint_seq = 0;
    {
        AccountabilityRecord rec;
        rec.ctx         = ctx;
        rec.category    = AccountabilityCategory::Identity;
        rec.action      = accountability_action::kRoleDelete;
        rec.target_type = "role";
        rec.principal   = role;
        rec.detail.set("role", role);
        // One record describing the batch, not one per membership — both for
        // contention and because it is one logical act (§5.3.2).
        rec.detail.set("memberships_removed", memberships_removed);
        auto recorded = append_accountability(pg_conn, tenant, rec);
        if (!recorded.success) {
            return rollback_and_fail("Role deletion refused: " + recorded.error);
        }
        hint_seq = recorded.value.seq;
    }

    PGresult* commit_res = PQexec(pg_conn, "COMMIT;");
    if (PQresultStatus(commit_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to COMMIT delete_role: " + std::string(PQerrorMessage(pg_conn));
        PQclear(commit_res);
        return rollback_and_fail(error);
    }
    PQclear(commit_res);
    connection_pool_->release(conn);
    fire_accountability_hint(tenant, hint_seq);
    return Result<void>::ok();
}

Result<void> Database::assign_user_to_role(const std::string& user, const std::string& role,
                                           const std::string& tenant,
                                           const AccountabilityContext& ctx) {
    if (user.empty() || role.empty()) {
        return Result<void>::err("User and role names cannot be empty");
    }
    if (!ctx.valid()) {
        return Result<void>::err("Refusing role assignment with no actor");
    }
    auto conn = acquire(DbOp::Write);
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }
    PGconn* pg_conn = conn->get_connection();
    std::string schema = get_schema_prefix(tenant);

    PGresult* begin_res = PQexec(pg_conn, "BEGIN;");
    if (PQresultStatus(begin_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to BEGIN assign_user_to_role transaction: " + std::string(PQerrorMessage(pg_conn));
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

    std::string sql = "INSERT INTO " + schema + ".user_roles (user_name, role_name) VALUES ($1, $2) "
                      "ON CONFLICT (user_name, role_name) DO NOTHING RETURNING id;";
    const char* params[2] = { user.c_str(), role.c_str() };
    PGresult* res = PQexecParams(pg_conn, sql.c_str(), 2, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string error = "Failed to assign user to role: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        return rollback_and_fail(error);
    }
    const bool assigned = PQntuples(res) > 0;
    PQclear(res);

    std::int64_t hint_seq = 0;
    if (assigned) {
        AccountabilityRecord rec;
        rec.ctx         = ctx;
        rec.category    = AccountabilityCategory::Identity;
        rec.action      = accountability_action::kRoleAssign;
        rec.target_type = "principal";
        rec.principal   = user;
        rec.detail.set("role", role);
        auto recorded = append_accountability(pg_conn, tenant, rec);
        if (!recorded.success) {
            return rollback_and_fail("Role assignment refused: " + recorded.error);
        }
        hint_seq = recorded.value.seq;
    }

    PGresult* commit_res = PQexec(pg_conn, "COMMIT;");
    if (PQresultStatus(commit_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to COMMIT assign_user_to_role: " + std::string(PQerrorMessage(pg_conn));
        PQclear(commit_res);
        return rollback_and_fail(error);
    }
    PQclear(commit_res);
    connection_pool_->release(conn);
    fire_accountability_hint(tenant, hint_seq);
    return Result<void>::ok();
}

Result<void> Database::remove_user_from_role(const std::string& user, const std::string& role,
                                             const std::string& tenant,
                                             const AccountabilityContext& ctx) {
    if (user.empty() || role.empty()) {
        return Result<void>::err("User and role names cannot be empty");
    }
    if (!ctx.valid()) {
        return Result<void>::err("Refusing role removal with no actor");
    }
    auto conn = acquire(DbOp::Write);
    if (!conn || !conn->is_valid()) {
        return Result<void>::err("Failed to acquire database connection");
    }
    PGconn* pg_conn = conn->get_connection();
    std::string schema = get_schema_prefix(tenant);

    // §2.1 site 4: the membership row is destroyed in place. After this, nothing
    // in the current state says the access ever existed — which is the whole
    // reason the record has to commit with it.
    PGresult* begin_res = PQexec(pg_conn, "BEGIN;");
    if (PQresultStatus(begin_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to BEGIN remove_user_from_role transaction: " + std::string(PQerrorMessage(pg_conn));
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

    std::string sql = "DELETE FROM " + schema + ".user_roles WHERE user_name = $1 AND role_name = $2;";
    const char* params[2] = { user.c_str(), role.c_str() };
    PGresult* res = PQexecParams(pg_conn, sql.c_str(), 2, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to remove user from role: " + std::string(PQerrorMessage(pg_conn));
        PQclear(res);
        return rollback_and_fail(error);
    }
    const bool removed = std::atoi(PQcmdTuples(res)) > 0;
    PQclear(res);

    std::int64_t hint_seq = 0;
    if (removed) {
        AccountabilityRecord rec;
        rec.ctx         = ctx;
        rec.category    = AccountabilityCategory::Identity;
        rec.action      = accountability_action::kRoleRemove;
        rec.target_type = "principal";
        rec.principal   = user;
        rec.detail.set("role", role);
        auto recorded = append_accountability(pg_conn, tenant, rec);
        if (!recorded.success) {
            return rollback_and_fail("Role removal refused: " + recorded.error);
        }
        hint_seq = recorded.value.seq;
    }

    PGresult* commit_res = PQexec(pg_conn, "COMMIT;");
    if (PQresultStatus(commit_res) != PGRES_COMMAND_OK) {
        std::string error = "Failed to COMMIT remove_user_from_role: " + std::string(PQerrorMessage(pg_conn));
        PQclear(commit_res);
        return rollback_and_fail(error);
    }
    PQclear(commit_res);
    connection_pool_->release(conn);
    fire_accountability_hint(tenant, hint_seq);
    return Result<void>::ok();
}

Result<std::vector<std::string>> Database::get_roles_for_user(const std::string& user, const std::string& tenant) {
    auto conn = acquire(DbOp::Read);
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
    auto conn = acquire(DbOp::Read);
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
    auto conn = acquire(DbOp::Read);
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