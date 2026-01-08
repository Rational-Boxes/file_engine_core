#include "fileengine/object_store_sync.h"
#include <algorithm>
#include <thread>
#include <chrono>
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>

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

    // First, sync files that are in the database
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
        auto result = sync_files();
        if (!result.success) {
            return result;
        }
    }

    // Additionally, perform a comprehensive sync of all local files to ensure nothing is missed
    // This handles the case where files exist in local storage but are not tracked in the database
    // For multi-tenant systems, we need to sync each tenant separately
    if (tenant_result.success) {
        for (const auto& tenant : tenant_result.value) {
            auto comprehensive_result = perform_comprehensive_local_sync(tenant);
            if (!comprehensive_result.success) {
                // Log the error but don't fail the entire startup sync
                // This is because local files might be corrupted or inaccessible
            }
        }
    } else {
        // Just sync the default tenant
        auto comprehensive_result = perform_comprehensive_local_sync();
        if (!comprehensive_result.success) {
            // Log the error but don't fail the entire startup sync
            // This is because local files might be corrupted or inaccessible
        }
    }

    return Result<void>::ok();
}

Result<void> ObjectStoreSync::perform_comprehensive_local_sync(const std::string& tenant) {
    if (!storage_ || !object_store_) {
        return Result<void>::err("Storage or object store not available");
    }

    std::string actual_tenant = tenant.empty() ? "default" : tenant;

    // Get all local file paths for the specified tenant
    auto local_paths_result = storage_->get_local_file_paths(actual_tenant);
    if (!local_paths_result.success) {
        // Try with empty tenant name as well
        local_paths_result = storage_->get_local_file_paths("");
        if (!local_paths_result.success) {
            return Result<void>::err("Failed to get local file paths: " + local_paths_result.error);
        }
    }

    // Process each local file to check if it needs to be synced
    for (const auto& path : local_paths_result.value) {
        // Parse the path to extract tenant, uid, and version timestamp
        // Path format: base_path/tenant/xx/yy/zz/uid/version_timestamp
        size_t base_pos = path.find(actual_tenant);
        if (base_pos == std::string::npos) {
            // Try looking for the base path structure
            std::string base_path = storage_->get_storage_path("", "", actual_tenant); // Get a sample path to understand structure
            // We'll use a simpler approach: find the last 2 segments (uid/version)
            size_t last_slash = path.find_last_of('/');
            if (last_slash == std::string::npos) continue;

            std::string version_timestamp = path.substr(last_slash + 1);

            // Validate that this looks like a timestamp (basic check)
            if (version_timestamp.length() < 15 || version_timestamp[8] != '_') continue; // Basic timestamp format check

            size_t second_last_slash = path.substr(0, last_slash).find_last_of('/');
            if (second_last_slash == std::string::npos) continue;

            std::string uid = path.substr(second_last_slash + 1, last_slash - second_last_slash - 1);

            // Basic UUID format validation (contains hyphens in expected positions)
            if (uid.length() != 36 || uid[8] != '-' || uid[13] != '-' || uid[18] != '-' || uid[23] != '-') continue;

            // Check if this file exists in the object store
            std::string obj_store_path = object_store_->get_storage_path(uid, version_timestamp, actual_tenant);
            auto exists_result = object_store_->file_exists(obj_store_path, actual_tenant);

            if (!exists_result.success || !exists_result.value) {
                // File doesn't exist in object store, sync it
                std::string local_storage_path = storage_->get_storage_path(uid, version_timestamp, actual_tenant);
                auto exists_local = storage_->file_exists(local_storage_path, actual_tenant);

                if (exists_local.success && exists_local.value) {
                    auto read_result = storage_->read_file(local_storage_path, actual_tenant);
                    if (read_result.success) {
                        auto store_result = object_store_->store_file(uid, version_timestamp, read_result.value, actual_tenant);
                        if (store_result.success) {
                            synced_file_count_++;
                        } else {
                            failed_sync_count_++;
                        }
                    } else {
                        failed_sync_count_++;
                    }
                }
            }
        } else {
            // Found the tenant in path, extract uid and version
            size_t last_slash = path.find_last_of('/');
            if (last_slash == std::string::npos) continue;

            std::string version_timestamp = path.substr(last_slash + 1);

            // Validate that this looks like a timestamp (basic check)
            if (version_timestamp.length() < 15 || version_timestamp[8] != '_') continue; // Basic timestamp format check

            size_t second_last_slash = path.substr(0, last_slash).find_last_of('/');
            if (second_last_slash == std::string::npos) continue;

            std::string uid = path.substr(second_last_slash + 1, last_slash - second_last_slash - 1);

            // Basic UUID format validation
            if (uid.length() != 36 || uid[8] != '-' || uid[13] != '-' || uid[18] != '-' || uid[23] != '-') continue;

            // Check if this file exists in the object store
            std::string obj_store_path = object_store_->get_storage_path(uid, version_timestamp, actual_tenant);
            auto exists_result = object_store_->file_exists(obj_store_path, actual_tenant);

            if (!exists_result.success || !exists_result.value) {
                // File doesn't exist in object store, sync it
                std::string local_storage_path = storage_->get_storage_path(uid, version_timestamp, actual_tenant);
                auto exists_local = storage_->file_exists(local_storage_path, actual_tenant);

                if (exists_local.success && exists_local.value) {
                    auto read_result = storage_->read_file(local_storage_path, actual_tenant);
                    if (read_result.success) {
                        auto store_result = object_store_->store_file(uid, version_timestamp, read_result.value, actual_tenant);
                        if (store_result.success) {
                            synced_file_count_++;
                        } else {
                            failed_sync_count_++;
                        }
                    } else {
                        failed_sync_count_++;
                    }
                }
            }
        }
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
    if (!object_store_ || !object_store_->is_initialized()) {
        return false;
    }

    // Perform a connectivity test by checking if the bucket exists
    auto result = object_store_->bucket_exists();
    return result.success && result.value;
}

Result<void> ObjectStoreSync::attempt_recovery() {
    // This contains logic to recover from connection failures
    // For example, attempting to reconnect to the object store
    if (object_store_) {
        // Try to reinitialize the object store connection
        auto init_result = object_store_->initialize();
        if (init_result.success) {
            // Log success if needed
            return Result<void>::ok();
        } else {
            // Log error if needed
            return init_result;
        }
    } else {
        return Result<void>::err("Object store not available");
    }
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
    bool connection_lost = false;

    while (running_.load()) {
        // Check connection health
        bool is_healthy = is_connection_healthy();

        if (!is_healthy) {
            if (!connection_lost) {
                // First time we detect connection loss
                connection_lost = true;
            }
            // Attempt recovery
            attempt_recovery();

            // If still not healthy after recovery attempt, wait for retry interval
            if (!is_connection_healthy()) {
                std::this_thread::sleep_for(std::chrono::seconds(config_.retry_seconds));
                continue; // Skip sync if connection is still down
            } else {
                // Connection was restored, reset the flag
                connection_lost = false;
            }
        }

        // Only perform sync if connection is healthy
        if (is_connection_healthy()) {
            // Perform periodic sync of files that need synchronization
            // This ensures files are synced periodically, not just on-demand
            auto sync_result = perform_sync();
            if (!sync_result.success) {
                // Log error but continue monitoring
            }
        }

        // Sleep for the configured retry interval before checking again
        std::this_thread::sleep_for(std::chrono::seconds(config_.retry_seconds));
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

    if (!db_ || !storage_ || !object_store_) {
        return Result<std::vector<std::pair<std::string, std::string>>>::err("Database, storage, or object store not available");
    }

    // Get ALL files from database that may need syncing
    // This ensures we catch all files, not just those in the root directory
    auto all_files_result = db_->list_all_files(tenant);
    if (!all_files_result.success) {
        return Result<std::vector<std::pair<std::string, std::string>>>::err("Failed to get all files list: " + all_files_result.error);
    }

    for (const auto& file_info : all_files_result.value) {
        // Skip directories since they don't have content to sync
        if (file_info.type == FileType::DIRECTORY) {
            continue;
        }

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
        } else {
            // If we can't get versions, try to sync the file anyway
            // This handles cases where version records might be missing
            files_to_sync.push_back({file_info.uid, file_info.version});
        }
    }

    // Additionally, scan local storage for files that might not be in the database
    // This handles the case where files exist in local storage but are not tracked in the database
    auto local_paths_result = storage_->get_local_file_paths(tenant);
    if (local_paths_result.success) {
        // Process the local file paths to identify files that need to be synced
        // This is important for startup sync to ensure all local files are backed up to S3
        for (const auto& path : local_paths_result.value) {
            // Extract file UID and version timestamp from the path
            // The path format is: base_path/tenant/xx/yy/zz/uid/version_timestamp
            size_t tenant_pos = path.find(tenant.empty() ? "default" : tenant);
            if (tenant_pos != std::string::npos) {
                // Extract uid and version from path
                // Path format: base_path/tenant/xx/yy/zz/uid/version_timestamp
                size_t last_slash = path.find_last_of('/');
                if (last_slash != std::string::npos) {
                    std::string version_timestamp = path.substr(last_slash + 1);
                    size_t second_last_slash = path.substr(0, last_slash).find_last_of('/');
                    if (second_last_slash != std::string::npos) {
                        std::string uid = path.substr(second_last_slash + 1, last_slash - second_last_slash - 1);

                        // Check if this file/version combination is already in our sync list
                        bool already_in_list = false;
                        for (const auto& file_pair : files_to_sync) {
                            if (file_pair.first == uid && file_pair.second == version_timestamp) {
                                already_in_list = true;
                                break;
                            }
                        }

                        if (!already_in_list) {
                            // Check if this file exists in the object store
                            std::string obj_store_path = object_store_->get_storage_path(uid, version_timestamp, tenant);
                            auto exists_result = object_store_->file_exists(obj_store_path, tenant);

                            if (!exists_result.success || !exists_result.value) {
                                // File doesn't exist in object store, add to sync list
                                files_to_sync.push_back({uid, version_timestamp});
                            }
                        }
                    }
                }
            }
        }
    }

    return Result<std::vector<std::pair<std::string, std::string>>>::ok(files_to_sync);
}

Result<bool> ObjectStoreSync::needs_sync(const std::string& uid, const std::string& version_timestamp,
                                        const std::string& tenant) {
    if (!object_store_) {
        return Result<bool>::err("Object store not available");
    }

    // Generate the storage path for this file version in the object store
    // This should use the object store's path format, not the local storage path
    std::string obj_store_path = object_store_->get_storage_path(uid, version_timestamp, tenant);

    // Check if the file exists in the object store
    auto exists_result = object_store_->file_exists(obj_store_path, tenant);
    if (!exists_result.success) {
        // If we can't check if it exists, assume it needs sync
        return Result<bool>::ok(true);
    }

    // If the file doesn't exist in the object store, it needs sync
    // If it does exist in the object store, it doesn't need sync (for this basic implementation)
    return Result<bool>::ok(!exists_result.value);
}

Result<std::vector<std::string>> ObjectStoreSync::get_tenant_list() {
    // Query the database for a list of tenants
    if (!db_) {
        return Result<std::vector<std::string>>::err("Database not available");
    }

    // Query the global tenants table for all registered tenants
    return db_->list_tenants();
}

Result<void> ObjectStoreSync::verify_sync_completion() {
    // This would verify that all expected files are present in both local and remote storage
    return Result<void>::ok();
}

ObjectStoreSync::~ObjectStoreSync() {
    stop_sync_service();
}

} // namespace fileengine