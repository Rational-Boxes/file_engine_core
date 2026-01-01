#ifndef FILEENGINE_DATABASE_H
#define FILEENGINE_DATABASE_H

#include "types.h"
#include "IDatabase.h"
#include "connection_pool.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>
#include <libpq-fe.h>
#include <regex>
#include <thread>
#include <atomic>
#include <mutex>

namespace fileengine {

class Database : public IDatabase {
public:
    Database(const std::string& host, int port, const std::string& dbname,
             const std::string& user, const std::string& password, int pool_size = 10);
    ~Database();

    // Connection management
    bool connect() override;
    void disconnect() override;
    bool is_connected() const override;

    // Schema management
    Result<void> create_schema() override;
    Result<void> create_global_schema();
    Result<void> create_tenant_schema(const std::string& tenant) override;
    Result<bool> tenant_schema_exists(const std::string& tenant) override;
    Result<void> drop_schema() override;

    // File metadata operations (using UUIDs instead of paths/ids) - now tenant-specific
    Result<std::string> insert_file(const std::string& uid, const std::string& name,
                                    const std::string& path, const std::string& parent_uid,
                                    FileType type, const std::string& owner,
                                    int permissions, const std::string& tenant) override;
    Result<void> update_file_modified(const std::string& uid, const std::string& tenant) override;
    Result<void> update_file_current_version(const std::string& uid, const std::string& version_timestamp, const std::string& tenant) override;
    Result<bool> delete_file(const std::string& uid, const std::string& tenant) override;
    Result<bool> undelete_file(const std::string& uid, const std::string& tenant) override;
    Result<std::optional<FileInfo>> get_file_by_uid(const std::string& uid, const std::string& tenant) override;
    Result<std::optional<FileInfo>> get_file_by_path(const std::string& path, const std::string& tenant) override;  // Keep for backward compatibility
    Result<void> update_file_name(const std::string& uid, const std::string& new_name, const std::string& tenant) override;
    Result<std::vector<FileInfo>> list_files_in_directory(const std::string& parent_uid, const std::string& tenant) override;
    Result<std::vector<FileInfo>> list_files_in_directory_with_deleted(const std::string& parent_uid, const std::string& tenant) override;
    Result<std::vector<FileInfo>> list_all_files(const std::string& tenant) override;
    Result<std::optional<FileInfo>> get_file_by_name_and_parent(const std::string& name, const std::string& parent_uid, const std::string& tenant) override;
    Result<std::optional<FileInfo>> get_file_by_name_and_parent_include_deleted(const std::string& name, const std::string& parent_uid, const std::string& tenant) override;
    Result<int64_t> get_file_size(const std::string& file_uid, const std::string& tenant) override;
    Result<int64_t> get_directory_size(const std::string& dir_uid, const std::string& tenant) override;
    Result<std::optional<FileInfo>> get_file_by_uid_include_deleted(const std::string& uid, const std::string& tenant) override;

    // Path-to-UUID mapping (for backward compatibility)
    Result<std::string> path_to_uid(const std::string& path, const std::string& tenant) override;
    Result<std::vector<std::string>> uid_to_path(const std::string& uid, const std::string& tenant) override;

    // Version operations (using UUIDs and timestamp strings)
    Result<int64_t> insert_version(const std::string& file_uid, const std::string& version_timestamp,
                                    int64_t size, const std::string& storage_path, const std::string& tenant) override;
    Result<std::optional<std::string>> get_version_storage_path(const std::string& file_uid, const std::string& version_timestamp, const std::string& tenant) override;
    Result<std::vector<std::string>> list_versions(const std::string& file_uid, const std::string& tenant) override;

    // Version restoration operations
    Result<bool> restore_to_version(const std::string& file_uid, const std::string& version_timestamp, const std::string& user, const std::string& tenant = "") override;

    // Metadata operations (versioned by timestamp)
    Result<void> set_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& value, const std::string& tenant) override;
    Result<std::optional<std::string>> get_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& tenant) override;
    Result<std::map<std::string, std::string>> get_all_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& tenant) override;
    Result<void> delete_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& tenant) override;

    // Direct SQL execution for testing
    Result<void> execute(const std::string& sql, const std::string& tenant) override;
    Result<std::vector<std::vector<std::string>>> query(const std::string& sql, const std::string& tenant) override;

    // Cache tracking operations (global schema)
    Result<void> update_file_access_stats(const std::string& uid, const std::string& user, const std::string& tenant = "") override;
    Result<std::vector<std::string>> get_least_accessed_files(int limit, const std::string& tenant = "") override;
    Result<std::vector<std::string>> get_infrequently_accessed_files(int days_threshold, const std::string& tenant = "") override;
    Result<int64_t> get_storage_usage(const std::string& tenant = "") override;
    Result<int64_t> get_storage_capacity(const std::string& tenant = "") override;

    // Tenant management operations
    Result<void> cleanup_tenant_data(const std::string& tenant) override;

    // ACL operations
    Result<void> add_acl(const std::string& resource_uid, const std::string& principal,
                         int type, int permissions, const std::string& tenant = "") override;
    Result<void> remove_acl(const std::string& resource_uid, const std::string& principal,
                            int type, const std::string& tenant = "") override;
    Result<std::vector<AclEntry>> get_acls_for_resource(const std::string& resource_uid,
                                                        const std::string& tenant = "") override;
    Result<std::vector<AclEntry>> get_user_acls(const std::string& resource_uid,
                                                const std::string& principal,
                                                const std::string& tenant = "") override;

    // Connection info access for administrative operations
    std::string get_connection_info() const;

    // Methods for configuring and managing primary-secondary database connections
    void configure_secondary_connection(const std::string& host, int port, const std::string& database_name,
                                        const std::string& user, const std::string& password);

    // Connection monitoring methods
    bool is_primary_available() const { return primary_available_.load(); }
    bool is_using_secondary() const { return using_secondary_.load(); }
    void start_connection_monitoring();
    void stop_connection_monitoring();

private:
    // Primary connection pool (main database)
    std::shared_ptr<ConnectionPool> connection_pool_;
    std::string hostname_;

    // Secondary/local database connection for read-only operations when primary is unavailable
    std::shared_ptr<DatabaseConnection> secondary_connection_;
    std::string secondary_conn_info_;
    std::atomic<bool> using_secondary_{false};

    // Connection health monitoring
    std::atomic<bool> primary_available_{true};
    std::atomic<bool> monitoring_active_{false};
    std::thread connection_monitor_thread_;
    std::mutex connection_mutex_;
    int retry_interval_seconds_{30}; // Default retry interval

    Result<void> check_connection() const;
    std::string escape_string(const std::string& str, PGconn* conn) const;
    std::string validate_schema_name(const std::string& schema_name) const;
    std::string get_hostname() const;
    std::string get_schema_prefix(const std::string& tenant) const;
    Result<PGresult*> execute_query_with_params(PGconn* conn, const std::string& sql_template,
                                                const std::vector<std::string>& params) const;
};

} // namespace fileengine

#endif // FILEENGINE_DATABASE_H