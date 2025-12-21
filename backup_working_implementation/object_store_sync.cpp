#include "fileengine/object_store_sync.h"
#include <algorithm>
#include <thread>
#include <chrono>

namespace fileengine {

ObjectStoreSync::ObjectStoreSync(std::shared_ptr<IDatabase> db, IStorage* storage, IObjectStore* object_store)
    : db_(db), storage_(storage), object_store_(object_store), running_(false), 
      sync_in_progress_(false), synced_file_count_(0), failed_sync_count_(0) {
    // Default configuration
    config_.enabled = true;
    config_.retry_seconds = 60;
    config_.sync_on_startup = true;
    config_.sync_on_demand = true;
    config_.sync_pattern = "all";
    config_.bidirectional = false;
}

void ObjectStoreSync::configure(const SyncConfig& config) {
    std::lock_guard<std::mutex> lock(sync_mutex_);
    config_ = config;
}

Result<void> ObjectStoreSync::start_sync_service() {
    if (running_.load()) {
        return Result<void>::ok();  // Already running
    }
    
    running_ = true;
    
    // Start the monitoring thread
    sync_thread_ = std::thread(&ObjectStoreSync::monitoring_loop, this);
    
    // If sync on startup is enabled, perform it in a background thread to not delay service initialization
    if (config_.sync_on_startup) {
        std::thread startup_sync_thread([this]() {
            auto result = perform_startup_sync();
            if (!result.success) {
                // Log error but don't affect the main service startup
            }
        });
        startup_sync_thread.detach(); // Detach the thread to run independently
    }
    
    return Result<void>::ok();
}

void ObjectStoreSync::stop_sync_service() {
    if (running_.load()) {
        running_ = false;
        
        if (sync_thread_.joinable()) {
            sync_thread_.join();
        }
        
        if (recovery_thread_.joinable()) {
            recovery_thread_.join();
        }
    }
}

Result<void> ObjectStoreSync::perform_sync(std::function<void(const std::string&, int, int)> progress_callback) {
    if (!config_.enabled) {
        return Result<void>::ok();
    }
    
    if (sync_in_progress_.exchange(true)) {
        return Result<void>::err("Sync already in progress");
    }
    
    Result<void> result = Result<void>::ok();
    
    try {
        // Get tenant list for multi-tenant sync
        auto tenant_result = get_tenant_list();
        if (tenant_result.success) {
            int total_tenants = tenant_result.value.size();
            int current_tenant = 0;
            
            for (const auto& tenant : tenant_result.value) {
                auto sync_result = perform_tenant_sync(tenant);
                if (!sync_result.success) {
                    result = sync_result;
                    failed_sync_count_++;  // Increment overall failure counter
                }
                
                current_tenant++;
                if (progress_callback) {
                    progress_callback("Tenant sync", current_tenant, total_tenants);
                }
            }
        } else {
            // Sync default tenant
            result = sync_files();
        }
    } catch (const std::exception& e) {
        result = Result<void>::err(std::string("Sync failed: ") + e.what());
    }
    
    sync_in_progress_ = false;
    return result;
}

Result<void> ObjectStoreSync::perform_startup_sync() {
    if (!config_.enabled) {
        return Result<void>::ok();
    }
    
    // Perform sync for all tenants
    auto tenant_result = get_tenant_list();
    if (tenant_result.success) {
        for (const auto& tenant : tenant_result.value) {
            auto result = perform_tenant_sync(tenant);
            if (!result.success) {
                return result;  // Return the first error
            }
        }
    } else {
        // Just sync the default
        return sync_files();
    }
    
    return Result<void>::ok();
}

Result<void> ObjectStoreSync::perform_tenant_sync(const std::string& tenant) {
    if (!config_.enabled) {
        return Result<void>::ok();
    }
    
    return sync_files(tenant);
}

bool ObjectStoreSync::is_connection_healthy() const {
    if (!object_store_) {
        return false;
    }
    
    // In a real implementation, this would check the actual connection
    // For now, return true if object store is accessible
    return true;
}

Result<void> ObjectStoreSync::attempt_recovery() {
    // This would contain logic to recover from connection failures
    // For example, attempting to reconnect to the object store
    if (!is_connection_healthy()) {
        if (object_store_) {
            auto init_result = object_store_->initialize();
            return init_result;
        }
    }
    
    return Result<void>::ok();
}

size_t ObjectStoreSync::get_synced_file_count() const {
    return synced_file_count_.load();
}

size_t ObjectStoreSync::get_failed_sync_count() const {
    return failed_sync_count_.load();
}

bool ObjectStoreSync::is_sync_running() const {
    return sync_in_progress_.load();
}

void ObjectStoreSync::monitoring_loop() {
    while (running_.load()) {
        // Check connection health
        if (!is_connection_healthy()) {
            attempt_recovery();
        }

        // Sleep for a while before checking again
        std::this_thread::sleep_for(std::chrono::seconds(30));  // Check every 30 seconds
    }
}

bool ObjectStoreSync::is_connection_healthy() {
    if (!object_store_) {
        return false;
    }

    // Try a simple object store operation to test connection health
    // For S3 storage, we could attempt to list buckets or check connection status
    try {
        // In a real implementation, this would check if we can communicate with the object store
        // For now, we'll just check if the object store instance exists and is initialized
        return object_store_->is_initialized();
    } catch (...) {
        // If there's an exception during the health check, connection is not healthy
        return false;
    }
}

void ObjectStoreSync::attempt_recovery() {
    if (!object_store_) {
        return;
    }

    std::cout << "Attempting to reconnect to object store..." << std::endl;

    // Try to re-initialize the object store connection
    auto init_result = object_store_->initialize();
    if (init_result.success) {
        std::cout << "Object store connection restored successfully." << std::endl;
        // Optionally trigger a full sync after recovery
        if (config_.sync_on_demand) {
            // Start a background sync operation to reconcile any missed changes
            std::thread recovery_sync_thread([this]() {
                auto sync_result = perform_startup_sync(); // Use startup sync to resync everything
                if (sync_result.success) {
                    std::cout << "Recovery sync completed successfully after connection restoration." << std::endl;
                } else {
                    std::cout << "Recovery sync failed after connection restoration: " << sync_result.error << std::endl;
                }
            });
            recovery_sync_thread.detach(); // Run in background
        }
    } else {
        std::cout << "Failed to reconnect to object store, will retry in " << config_.retry_seconds << " seconds." << std::endl;
    }
}

Result<void> ObjectStoreSync::sync_files(const std::string& tenant) {
    // Get list of files to sync
    auto files_result = get_files_to_sync(tenant);
    if (!files_result.success) {
        return files_result.error.empty() ? Result<void>::err("No files to sync") : 
                                          Result<void>::err(files_result.error);
    }
    
    int total_files = files_result.value.size();
    int synced_files = 0;
    int failed_files = 0;
    
    for (const auto& [uid, version_timestamp] : files_result.value) {
        auto sync_result = sync_file(uid, version_timestamp, tenant);
        if (sync_result.success) {
            synced_files++;
            synced_file_count_++;
        } else {
            failed_files++;
            failed_sync_count_++;
        }
    }
    
    return Result<void>::ok();
}

Result<void> ObjectStoreSync::sync_file(const std::string& uid, const std::string& version_timestamp, 
                                       const std::string& tenant) {
    if (!storage_ || !object_store_) {
        return Result<void>::err("Storage or object store not available");
    }
    
    // Get the local storage path for this file version
    std::string storage_path = storage_->get_storage_path(uid, version_timestamp, tenant);
    
    // Check if the file exists locally
    auto exists_result = storage_->file_exists(storage_path, tenant);
    if (!exists_result.success || !exists_result.value) {
        return Result<void>::err("Local file does not exist: " + storage_path);
    }
    
    // Read the file from local storage
    auto read_result = storage_->read_file(storage_path, tenant);
    if (!read_result.success) {
        return Result<void>::err("Failed to read local file: " + read_result.error);
    }
    
    // Store the file in object store
    auto store_result = object_store_->store_file(uid, version_timestamp, read_result.value, tenant);
    if (!store_result.success) {
        return Result<void>::err("Failed to store file in object store: " + store_result.error);
    }
    
    return Result<void>::ok();
}

Result<std::vector<std::pair<std::string, std::string>>> ObjectStoreSync::get_files_to_sync(const std::string& tenant) {
    std::vector<std::pair<std::string, std::string>> files_to_sync;
    
    if (!db_) {
        return Result<std::vector<std::pair<std::string, std::string>>>::err("Database not available");
    }
    
    // Get all files from database that may need syncing
    // This is a simplified approach - in reality, we'd want to be more selective
    // about which files need to be synced based on modification time, etc.
    
    // For this implementation, we'll get all files that have versions
    auto all_files_result = db_->list_files_in_directory("", tenant);
    if (!all_files_result.success) {
        return Result<std::vector<std::pair<std::string, std::string>>>::err("Failed to get files list");
    }
    
    for (const auto& file_info : all_files_result.value) {
        // Get all versions of this file
        auto versions_result = db_->list_versions(file_info.uid, tenant);
        if (versions_result.success) {
            for (const auto& version : versions_result.value) {
                // Check if this version needs sync
                auto needs_sync_result = needs_sync(file_info.uid, version, tenant);
                if (needs_sync_result.success && needs_sync_result.value) {
                    files_to_sync.push_back({file_info.uid, version});
                }
            }
        }
    }
    
    return Result<std::vector<std::pair<std::string, std::string>>>::ok(files_to_sync);
}

Result<bool> ObjectStoreSync::needs_sync(const std::string& uid, const std::string& version_timestamp, 
                                        const std::string& tenant) {
    // For this implementation, we'll assume all files need sync
    // In a real system, we'd compare metadata between local and remote storage
    return Result<bool>::ok(true);
}

Result<std::vector<std::string>> ObjectStoreSync::get_tenant_list() {
    // This would query the database for a list of tenants
    // For this simplified implementation, return an empty list
    // (meaning only the default tenant exists)
    return Result<std::vector<std::string>>::ok(std::vector<std::string>());
}

Result<void> ObjectStoreSync::verify_sync_completion() {
    // This would verify that all expected files are present in both local and remote storage
    return Result<void>::ok();
}

ObjectStoreSync::~ObjectStoreSync() {
    stop_sync_service();
}

} // namespace fileengine