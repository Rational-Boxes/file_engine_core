#pragma once

#include "types.h"
#include "IStorage.h"
#include "IObjectStore.h"
#include "IDatabase.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>

namespace fileengine {

struct SyncConfig {
    bool enabled;
    int retry_seconds;           // Number of seconds to wait before retrying failed operations
    bool sync_on_startup;        // Whether to sync on startup
    bool sync_on_demand;         // Whether to support on-demand sync
    std::string sync_pattern;    // Pattern of files to sync (e.g., all, recent, etc.)
    bool bidirectional;          // Whether sync is bidirectional
};

class ObjectStoreSync {
public:
    ObjectStoreSync(std::shared_ptr<IDatabase> db, IStorage* storage, IObjectStore* object_store);
    
    // Configure sync parameters
    void configure(const SyncConfig& config);
    
    // Start the sync service (including background thread for recovery)
    Result<void> start_sync_service();
    
    // Stop the sync service
    void stop_sync_service();
    
    // Perform a one-time sync operation
    Result<void> perform_sync(std::function<void(const std::string&, int, int)> progress_callback = nullptr);
    
    // Perform startup sync to synchronize existing files
    Result<void> perform_startup_sync();
    
    // Perform tenant-specific sync
    Result<void> perform_tenant_sync(const std::string& tenant);
    
    // Check connection health
    bool is_connection_healthy() const;
    
    // Attempt connection recovery
    Result<void> attempt_recovery();
    
    // Get sync statistics
    size_t get_synced_file_count() const;
    size_t get_failed_sync_count() const;
    
    // Check if the sync service is running
    bool is_sync_running() const;

    // Perform comprehensive sync of all local files (for startup)
    Result<void> perform_comprehensive_local_sync(const std::string& tenant = "");

    ~ObjectStoreSync();

private:
    std::shared_ptr<IDatabase> db_;
    IStorage* storage_;
    IObjectStore* object_store_;
    SyncConfig config_;

    std::thread sync_thread_;
    std::thread recovery_thread_;
    std::atomic<bool> running_;
    std::atomic<bool> sync_in_progress_;
    mutable std::mutex sync_mutex_;

    std::atomic<size_t> synced_file_count_;
    std::atomic<size_t> failed_sync_count_;

    // Background thread for monitoring and recovery
    void monitoring_loop();

    // Internal sync method
    Result<void> sync_files(const std::string& tenant = "");

    // Sync a specific file
    Result<void> sync_file(const std::string& uid, const std::string& version_timestamp,
                          const std::string& tenant = "");

    // Get list of files to sync
    Result<std::vector<std::pair<std::string, std::string>>> get_files_to_sync(const std::string& tenant = "");

    // Check if file needs sync (compare local and remote versions)
    Result<bool> needs_sync(const std::string& uid, const std::string& version_timestamp,
                           const std::string& tenant = "");

    // Get tenant list for multi-tenant sync
    Result<std::vector<std::string>> get_tenant_list();

    // Verify sync completion
    Result<void> verify_sync_completion();
};

} // namespace fileengine