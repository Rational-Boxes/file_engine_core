#pragma once

#include "types.h"
#include "IDatabase.h"
#include "IStorage.h"
#include "IObjectStore.h"
#include "acl_manager.h"
#include "cache_manager.h"
#include "tenant_manager.h"
#include <string>
#include <vector>
#include <memory>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <shared_mutex>

namespace fileengine {

class FileSystem {
public:
    FileSystem(std::shared_ptr<TenantManager> tenant_manager);

    virtual ~FileSystem();

    // Directory operations - now using UUIDs
    virtual Result<std::string> mkdir(const std::string& parent_uid, const std::string& name, 
                                      const std::string& user, int permissions = 0755, 
                                      const std::string& tenant = "");
    virtual Result<void> rmdir(const std::string& dir_uid, const std::string& user, 
                               const std::string& tenant = "");
    virtual Result<std::vector<DirectoryEntry>> listdir(const std::string& dir_uid, 
                                                        const std::string& user, 
                                                        const std::string& tenant = "");
    virtual Result<std::vector<DirectoryEntry>> listdir_with_deleted(const std::string& dir_uid, 
                                                                     const std::string& user, 
                                                                     const std::string& tenant = "");

    // File operations
    virtual Result<std::string> touch(const std::string& parent_uid, const std::string& name, 
                                      const std::string& user, const std::string& tenant = "");
    virtual Result<void> remove(const std::string& file_uid, const std::string& user, 
                                const std::string& tenant = "");
    virtual Result<void> undelete(const std::string& file_uid, const std::string& user, 
                                  const std::string& tenant = "");
    virtual Result<void> put(const std::string& file_uid, const std::vector<uint8_t>& data, 
                             const std::string& user, const std::string& tenant = "");
    virtual Result<std::vector<uint8_t>> get(const std::string& file_uid, 
                                             const std::string& user, 
                                             const std::string& tenant = "");

    // Metadata operations
    virtual Result<FileInfo> stat(const std::string& file_uid, const std::string& user, 
                                  const std::string& tenant = "");
    virtual Result<bool> exists(const std::string& file_uid, const std::string& tenant = "");

    // Path operations
    virtual Result<void> move(const std::string& src_uid, const std::string& dst_uid, 
                              const std::string& user, const std::string& tenant = "");
    virtual Result<void> copy(const std::string& src_uid, const std::string& dst_uid, 
                              const std::string& user, const std::string& tenant = "");
    virtual Result<void> rename(const std::string& uid, const std::string& new_name, 
                                const std::string& user, const std::string& tenant = "");

    // Version operations
    virtual Result<std::vector<std::string>> list_versions(const std::string& file_uid, 
                                                           const std::string& user, 
                                                           const std::string& tenant = "");
    virtual Result<std::vector<uint8_t>> get_version(const std::string& file_uid,
                                                     const std::string& version_timestamp,
                                                     const std::string& user,
                                                     const std::string& tenant = "");

    virtual Result<bool> restore_to_version(const std::string& file_uid,
                                           const std::string& version_timestamp,
                                           const std::string& user,
                                           const std::string& tenant = "");

    // S3 archival operations
    virtual Result<void> backup_to_object_store(const std::string& file_uid, 
                                                const std::string& tenant = "");
    virtual Result<void> purge_old_versions(const std::string& file_uid, int keep_count, 
                                            const std::string& tenant = "");

    // Cache management operations
    virtual void update_cache_threshold(double threshold, const std::string& tenant = "");
    virtual Result<double> get_cache_usage_percentage(const std::string& tenant = "") const;

    // Metadata operations (versioned)
    virtual Result<void> set_metadata(const std::string& file_uid, const std::string& key, 
                                      const std::string& value, const std::string& user, 
                                      const std::string& tenant = "");
    virtual Result<std::string> get_metadata(const std::string& file_uid, const std::string& key, 
                                             const std::string& user, const std::string& tenant = "");
    virtual Result<std::map<std::string, std::string>> get_all_metadata(const std::string& file_uid, 
                                                                        const std::string& user, 
                                                                        const std::string& tenant = "");
    virtual Result<void> delete_metadata(const std::string& file_uid, const std::string& key, 
                                         const std::string& user, const std::string& tenant = "");
    virtual Result<std::string> get_metadata_for_version(const std::string& file_uid, 
                                                         const std::string& version_timestamp, 
                                                         const std::string& key, 
                                                         const std::string& user, 
                                                         const std::string& tenant = "");
    virtual Result<std::map<std::string, std::string>> get_all_metadata_for_version(const std::string& file_uid, 
                                                                                    const std::string& version_timestamp, 
                                                                                    const std::string& user, 
                                                                                    const std::string& tenant = "");

    // ACL operations
    virtual Result<void> grant_permission(const std::string& resource_uid, 
                                          const std::string& principal, 
                                          int permissions, 
                                          const std::string& user, 
                                          const std::string& tenant = "");
    virtual Result<void> revoke_permission(const std::string& resource_uid, 
                                           const std::string& principal, 
                                           int permissions, 
                                           const std::string& user, 
                                           const std::string& tenant = "");
    virtual Result<bool> check_permission(const std::string& resource_uid, 
                                          const std::string& user, 
                                          const std::vector<std::string>& roles, 
                                          int required_permissions, 
                                          const std::string& tenant = "");

    // Cleanup
    virtual void shutdown();

    // Helper method to fetch from object store if missing locally
    virtual Result<std::vector<uint8_t>> fetch_from_object_store_if_missing(const std::string& uid,
                                                                           const std::string& version_timestamp,
                                                                           const std::string& tenant = "");

private:
    std::shared_ptr<TenantManager> tenant_manager_;
    std::shared_ptr<AclManager> acl_manager_;
    std::unique_ptr<CacheManager> cache_manager_;
    
    // Helper to get tenant context for operations
    TenantContext* get_tenant_context(const std::string& tenant);
    
    // Helper to validate permissions
    Result<bool> validate_user_permissions(const std::string& resource_uid,
                                          const std::string& user,
                                          const std::vector<std::string>& roles,
                                          int required_permissions,
                                          const std::string& tenant);

private:
    // Async object store backup functionality
    struct BackupTask {
        std::string file_uid;
        std::string tenant;
    };

    std::queue<BackupTask> backup_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread backup_worker_thread_;
    std::atomic<bool> backup_worker_running_{false};

    // Method to start the async backup worker
    void start_async_backup_worker();

    // Method to stop the async backup worker
    void stop_async_backup_worker();

    // Method run by the backup worker thread
    void backup_worker_loop();
};

} // namespace fileengine