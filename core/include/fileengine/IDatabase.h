#pragma once

#include "types.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>

namespace fileengine {

struct FileInfo;
enum class FileType;

class IDatabase {
public:
    virtual ~IDatabase() = default;

    // Connection management
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;

    // Schema management
    virtual Result<void> create_schema() = 0;
    virtual Result<void> drop_schema() = 0;

    // File metadata operations (using UUIDs instead of paths/ids)
    virtual Result<std::string> insert_file(const std::string& uid, const std::string& name,
                                            const std::string& path, const std::string& parent_uid,
                                            FileType type, const std::string& owner,
                                            int permissions, const std::string& tenant = "") = 0;
    virtual Result<void> update_file_modified(const std::string& uid, const std::string& tenant = "") = 0;
    virtual Result<void> update_file_current_version(const std::string& uid, const std::string& version_timestamp, const std::string& tenant = "") = 0;
    virtual Result<bool> delete_file(const std::string& uid, const std::string& tenant = "") = 0;
    virtual Result<bool> undelete_file(const std::string& uid, const std::string& tenant = "") = 0;
    virtual Result<std::optional<FileInfo>> get_file_by_uid(const std::string& uid, const std::string& tenant = "") = 0;
    virtual Result<std::optional<FileInfo>> get_file_by_path(const std::string& path, const std::string& tenant = "") = 0;  // Keep for backward compatibility
    virtual Result<void> update_file_name(const std::string& uid, const std::string& new_name, const std::string& tenant = "") = 0;
    virtual Result<std::vector<FileInfo>> list_files_in_directory(const std::string& parent_uid, const std::string& tenant = "") = 0;
    virtual Result<std::vector<FileInfo>> list_files_in_directory_with_deleted(const std::string& parent_uid, const std::string& tenant = "") = 0;
    virtual Result<std::optional<FileInfo>> get_file_by_name_and_parent(const std::string& name, const std::string& parent_uid, const std::string& tenant = "") = 0;
    virtual Result<std::optional<FileInfo>> get_file_by_name_and_parent_include_deleted(const std::string& name, const std::string& parent_uid, const std::string& tenant = "") = 0;
    virtual Result<int64_t> get_file_size(const std::string& file_uid, const std::string& tenant = "") = 0;
    virtual Result<int64_t> get_directory_size(const std::string& dir_uid, const std::string& tenant = "") = 0;
    virtual Result<std::optional<FileInfo>> get_file_by_uid_include_deleted(const std::string& uid, const std::string& tenant = "") = 0;

    // Path-to-UUID mapping (for backward compatibility)
    virtual Result<std::string> path_to_uid(const std::string& path, const std::string& tenant = "") = 0;
    virtual Result<std::vector<std::string>> uid_to_path(const std::string& uid, const std::string& tenant = "") = 0;

    // Version operations (using UUIDs and timestamp strings)
    virtual Result<int64_t> insert_version(const std::string& file_uid, const std::string& version_timestamp,
                                            int64_t size, const std::string& storage_path, const std::string& tenant = "") = 0;
    virtual Result<std::optional<std::string>> get_version_storage_path(const std::string& file_uid, const std::string& version_timestamp, const std::string& tenant = "") = 0;
    virtual Result<std::vector<std::string>> list_versions(const std::string& file_uid, const std::string& tenant = "") = 0;

    // Metadata operations (versioned by timestamp)
    virtual Result<void> set_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& value, const std::string& tenant = "") = 0;
    virtual Result<std::optional<std::string>> get_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& tenant = "") = 0;
    virtual Result<std::map<std::string, std::string>> get_all_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& tenant = "") = 0;
    virtual Result<void> delete_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& tenant = "") = 0;

    // Direct SQL execution for testing
    virtual Result<void> execute(const std::string& sql, const std::string& tenant = "") = 0;
    virtual Result<std::vector<std::vector<std::string>>> query(const std::string& sql, const std::string& tenant = "") = 0;

    // Cache tracking operations
    virtual Result<void> update_file_access_stats(const std::string& uid, const std::string& user, const std::string& tenant = "") = 0;
    virtual Result<std::vector<std::string>> get_least_accessed_files(int limit = 10, const std::string& tenant = "") = 0;
    virtual Result<std::vector<std::string>> get_infrequently_accessed_files(int days_threshold = 30, const std::string& tenant = "") = 0;
    virtual Result<int64_t> get_storage_usage(const std::string& tenant = "") = 0;
    virtual Result<int64_t> get_storage_capacity(const std::string& tenant = "") = 0;

    // Tenant management operations
    virtual Result<void> create_tenant_schema(const std::string& tenant) = 0;
    virtual Result<bool> tenant_schema_exists(const std::string& tenant) = 0;
    virtual Result<void> cleanup_tenant_data(const std::string& tenant) = 0;
};

} // namespace fileengine