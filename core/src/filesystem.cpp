#include "fileengine/filesystem.h"
#include "fileengine/utils.h"
#include "fileengine/server_logger.h"
#include <algorithm>

namespace fileengine {

FileSystem::FileSystem(std::shared_ptr<TenantManager> tenant_manager)
    : tenant_manager_(tenant_manager) {
    // Start the async backup worker thread
    start_async_backup_worker();
}

FileSystem::~FileSystem() {
    shutdown();
}

Result<std::string> FileSystem::mkdir(const std::string& parent_uid, const std::string& name,
                                      const std::string& user, int permissions,
                                      const std::string& tenant) {
    // Detailed debug logging for entry
    SERVER_LOG_DEBUG("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
              "Entering mkdir operation - parent_uid: " + parent_uid +
              ", name: " + name + ", user: " + user + ", tenant: " + tenant);

    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        SERVER_LOG_ERROR("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
                  "Database not available for tenant: " + tenant);
        return Result<std::string>::err("Database not available for tenant: " + tenant);
    }

    SERVER_LOG_DEBUG("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
              "Validating permissions for user: " + user + " on parent: " + parent_uid);

    // Check permissions - the user needs write permission on the parent directory
    if (parent_uid.empty()) {
        // Root directory creation - only for system admin
        SERVER_LOG_DEBUG("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
                  "Attempting root directory creation - only allowed for root user");
        if (user != "root") {
            SERVER_LOG_ERROR("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
                      "Non-root user attempting to create in root directory");
            return Result<std::string>::err("Only root can create in root directory");
        }
    } else {
        auto perm_result = validate_user_permissions(parent_uid, user, {}, static_cast<int>(Permission::WRITE), tenant);
        if (!perm_result.success || !perm_result.value) {
            SERVER_LOG_ERROR("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
                      "User " + user + " does not have permission to create directory in " + parent_uid);
            return Result<std::string>::err("User does not have permission to create directory");
        }
        SERVER_LOG_DEBUG("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
                  "User " + user + " has permission to create directory in " + parent_uid);
    }

    std::string new_uid = Utils::generate_uuid();
    std::string path = parent_uid.empty() ? "/" + name : parent_uid + "/" + name;
    SERVER_LOG_DEBUG("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
              "Generated new UID: " + new_uid + " for directory path: " + path);

    // Insert directory in database
    SERVER_LOG_DEBUG("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
              "Inserting directory in database with UID: " + new_uid);
    auto db_result = context->db->insert_file(new_uid, name, path, parent_uid,
                                              FileType::DIRECTORY, user, permissions, tenant);
    if (!db_result.success) {
        SERVER_LOG_ERROR("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
                  "Failed to create directory in database: " + db_result.error);
        return Result<std::string>::err("Failed to create directory in database: " + db_result.error);
    }

    // Apply default ACLs
    if (acl_manager_) {
        SERVER_LOG_DEBUG("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
                  "Applying default ACLs for new directory: " + new_uid);
        auto acl_result = acl_manager_->apply_default_acls(new_uid, user, tenant);
        if (!acl_result.success) {
            // Log error but don't fail the operation
            SERVER_LOG_WARN("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
                     "Failed to apply default ACLs: " + acl_result.error);
        } else {
            SERVER_LOG_DEBUG("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
                      "Successfully applied default ACLs for: " + new_uid);
        }
    } else {
        SERVER_LOG_WARN("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
                 "ACL manager not available for tenant: " + tenant);
    }

    SERVER_LOG_DEBUG("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
              "Successfully created directory with UID: " + new_uid);
    return Result<std::string>::ok(new_uid);
}

Result<void> FileSystem::rmdir(const std::string& dir_uid, const std::string& user,
                               const std::string& tenant) {
    SERVER_LOG_DEBUG("FileSystem::rmdir", ServerLogger::getInstance().detailed_log_prefix() +
              "Entering rmdir operation - dir_uid: " + dir_uid +
              ", user: " + user + ", tenant: " + tenant);

    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        SERVER_LOG_ERROR("FileSystem::rmdir", ServerLogger::getInstance().detailed_log_prefix() +
                  "Database not available for tenant: " + tenant);
        return Result<void>::err("Database not available for tenant: " + tenant);
    }

    // Check permissions - the user needs write permission on the directory
    SERVER_LOG_DEBUG("FileSystem::rmdir", ServerLogger::getInstance().detailed_log_prefix() +
              "Checking permissions for user: " + user + " on directory: " + dir_uid);
    auto perm_result = validate_user_permissions(dir_uid, user, {}, static_cast<int>(Permission::WRITE), tenant);
    if (!perm_result.success || !perm_result.value) {
        SERVER_LOG_ERROR("FileSystem::rmdir", ServerLogger::getInstance().detailed_log_prefix() +
                  "User " + user + " does not have permission to remove directory " + dir_uid);
        return Result<void>::err("User does not have permission to remove directory");
    }
    SERVER_LOG_DEBUG("FileSystem::rmdir", ServerLogger::getInstance().detailed_log_prefix() +
              "User " + user + " has permission to remove directory " + dir_uid);

    // First, check if directory is empty
    SERVER_LOG_DEBUG("FileSystem::rmdir", ServerLogger::getInstance().detailed_log_prefix() +
              "Checking if directory " + dir_uid + " is empty");
    auto list_result = listdir(dir_uid, user, tenant);
    if (list_result.success && !list_result.value.empty()) {
        SERVER_LOG_ERROR("FileSystem::rmdir", ServerLogger::getInstance().detailed_log_prefix() +
                  "Directory " + dir_uid + " is not empty, contains " +
                  std::to_string(list_result.value.size()) + " items");
        return Result<void>::err("Directory is not empty");
    }
    SERVER_LOG_DEBUG("FileSystem::rmdir", ServerLogger::getInstance().detailed_log_prefix() +
              "Directory " + dir_uid + " is empty, proceeding with removal");

    // Mark directory as deleted in database
    SERVER_LOG_DEBUG("FileSystem::rmdir", ServerLogger::getInstance().detailed_log_prefix() +
              "Marking directory " + dir_uid + " as deleted in database");
    auto db_result = context->db->delete_file(dir_uid, tenant);
    if (!db_result.success) {
        SERVER_LOG_ERROR("FileSystem::rmdir", ServerLogger::getInstance().detailed_log_prefix() +
                  "Failed to remove directory from database: " + db_result.error);
        return Result<void>::err("Failed to remove directory from database: " + db_result.error);
    }
    SERVER_LOG_DEBUG("FileSystem::rmdir", ServerLogger::getInstance().detailed_log_prefix() +
              "Successfully marked directory " + dir_uid + " as deleted");

    return Result<void>::ok();
}

Result<std::vector<DirectoryEntry>> FileSystem::listdir(const std::string& dir_uid, 
                                                        const std::string& user, 
                                                        const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::vector<DirectoryEntry>>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs read permission on the directory
    auto perm_result = validate_user_permissions(dir_uid, user, {}, static_cast<int>(Permission::READ), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<std::vector<DirectoryEntry>>::err("User does not have permission to list directory");
    }
    
    // List files in the directory
    auto db_result = context->db->list_files_in_directory(dir_uid, tenant);
    if (!db_result.success) {
        return Result<std::vector<DirectoryEntry>>::err("Failed to list directory: " + db_result.error);
    }
    
    // Convert FileInfo to DirectoryEntry
    std::vector<DirectoryEntry> entries;
    for (const auto& file_info : db_result.value) {
        DirectoryEntry entry;
        entry.uid = file_info.uid;
        entry.name = file_info.name;
        entry.type = file_info.type;
        entry.size = file_info.size;
        // Set timestamps appropriately
        entry.version_count = 0; // Would be populated from version info in a complete implementation
        
        entries.push_back(entry);
    }
    
    return Result<std::vector<DirectoryEntry>>::ok(entries);
}

Result<std::vector<DirectoryEntry>> FileSystem::listdir_with_deleted(const std::string& dir_uid, 
                                                                     const std::string& user, 
                                                                     const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::vector<DirectoryEntry>>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs read permission on the directory
    auto perm_result = validate_user_permissions(dir_uid, user, {}, static_cast<int>(Permission::READ), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<std::vector<DirectoryEntry>>::err("User does not have permission to list directory");
    }
    
    // List files in the directory including deleted ones
    auto db_result = context->db->list_files_in_directory_with_deleted(dir_uid, tenant);
    if (!db_result.success) {
        return Result<std::vector<DirectoryEntry>>::err("Failed to list directory with deleted: " + db_result.error);
    }
    
    // Convert FileInfo to DirectoryEntry
    std::vector<DirectoryEntry> entries;
    for (const auto& file_info : db_result.value) {
        DirectoryEntry entry;
        entry.uid = file_info.uid;
        entry.name = file_info.name;
        entry.type = file_info.type;
        entry.size = file_info.size;
        // Set timestamps appropriately
        entry.version_count = 0; // Would be populated from version info in a complete implementation
        
        entries.push_back(entry);
    }
    
    return Result<std::vector<DirectoryEntry>>::ok(entries);
}

Result<std::string> FileSystem::touch(const std::string& parent_uid, const std::string& name, 
                                      const std::string& user, const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::string>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs write permission on the parent directory
    if (parent_uid.empty()) {
        // Root directory creation - only for system admin
        if (user != "root") {
            return Result<std::string>::err("Only root can create in root directory");
        }
    } else {
        auto perm_result = validate_user_permissions(parent_uid, user, {}, static_cast<int>(Permission::WRITE), tenant);
        if (!perm_result.success || !perm_result.value) {
            return Result<std::string>::err("User does not have permission to create file");
        }
    }
    
    std::string new_uid = Utils::generate_uuid();
    std::string path = parent_uid.empty() ? "/" + name : parent_uid + "/" + name;
    
    // Insert file in database
    auto db_result = context->db->insert_file(new_uid, name, path, parent_uid, 
                                              FileType::REGULAR_FILE, user, 0644, tenant);
    if (!db_result.success) {
        return Result<std::string>::err("Failed to create file in database: " + db_result.error);
    }
    
    // Apply default ACLs
    if (acl_manager_) {
        auto acl_result = acl_manager_->apply_default_acls(new_uid, user, tenant);
        if (!acl_result.success) {
            // Log error but don't fail the operation
        }
    }
    
    return Result<std::string>::ok(new_uid);
}

Result<void> FileSystem::remove(const std::string& file_uid, const std::string& user, 
                                const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<void>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs write permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, {}, static_cast<int>(Permission::WRITE), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<void>::err("User does not have permission to remove file");
    }
    
    // Mark file as deleted in database
    auto db_result = context->db->delete_file(file_uid, tenant);
    if (!db_result.success) {
        return Result<void>::err("Failed to remove file from database: " + db_result.error);
    }
    
    return Result<void>::ok();
}

Result<void> FileSystem::undelete(const std::string& file_uid, const std::string& user, 
                                  const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<void>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs write permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, {}, static_cast<int>(Permission::WRITE), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<void>::err("User does not have permission to undelete file");
    }
    
    // Mark file as not deleted in database
    auto db_result = context->db->undelete_file(file_uid, tenant);
    if (!db_result.success) {
        return Result<void>::err("Failed to undelete file in database: " + db_result.error);
    }
    
    return Result<void>::ok();
}

Result<void> FileSystem::put(const std::string& file_uid, const std::vector<uint8_t>& data, 
                             const std::string& user, const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db || !context->storage) {
        return Result<void>::err("Database or storage not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs write permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, {}, static_cast<int>(Permission::WRITE), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<void>::err("User does not have permission to write file");
    }
    
    // Get the file info to check if it exists
    auto file_info_result = context->db->get_file_by_uid(file_uid, tenant);
    if (!file_info_result.success || !file_info_result.value.has_value()) {
        return Result<void>::err("File does not exist");
    }
    
    // Generate a new version timestamp
    std::string version_timestamp = Utils::get_timestamp_string();
    
    // Store the file in storage
    auto storage_result = context->storage->store_file(file_uid, version_timestamp, data, tenant);
    if (!storage_result.success) {
        return Result<void>::err("Failed to store file in storage: " + storage_result.error);
    }
    
    // Update the file's current version in the database
    auto update_result = context->db->update_file_current_version(file_uid, version_timestamp, tenant);
    if (!update_result.success) {
        return Result<void>::err("Failed to update current version: " + update_result.error);
    }
    
    // Record the version in the database
    auto insert_version_result = context->db->insert_version(file_uid, version_timestamp, data.size(), 
                                                             storage_result.value, tenant);
    if (!insert_version_result.success) {
        return Result<void>::err("Failed to record version: " + insert_version_result.error);
    }
    
    // Update the modification time
    auto update_time_result = context->db->update_file_modified(file_uid, tenant);
    if (!update_time_result.success) {
        // Log error but don't fail the operation
    }
    
    // If we have an object store, consider backing up asynchronously using the async worker thread
// TODO: Implement async worker thread to save to the object store so that the operation can return as soon as the local write is complete. Make sure to not conflict with the startup or connection recovery sync. Debug log-level needs to be very detailed.
    if (context->object_store) {
        SERVER_LOG_DEBUG("FileSystem::put", ServerLogger::getInstance().detailed_log_prefix() +
                  "[PERFORMANCE ENHANCEMENT] Object store available, scheduling async backup for file_uid: " + file_uid +
                  " [CONCURRENCY WARNING] Ensure this doesn't conflict with startup/connection recovery sync operations");

        // Schedule the backup to happen asynchronously on the backup worker thread
        // This allows the PUT operation to return immediately after local storage is complete
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            // Pass the version timestamp that was used when storing the file
            backup_queue_.push({file_uid, tenant, version_timestamp});
            SERVER_LOG_DEBUG("FileSystem::put", ServerLogger::getInstance().detailed_log_prefix() +
                      "[PERFORMANCE ENHANCEMENT] Backup task queued for file_uid: " + file_uid +
                      " with version: " + version_timestamp);
        }
        queue_cv_.notify_one(); // Wake up the backup worker thread

        SERVER_LOG_DEBUG("FileSystem::put", ServerLogger::getInstance().detailed_log_prefix() +
                  "[PERFORMANCE ENHANCEMENT] Async backup scheduled, returning immediately while backup continues in background");
    } else {
        SERVER_LOG_DEBUG("FileSystem::put", ServerLogger::getInstance().detailed_log_prefix() +
                  "[PERFORMANCE ENHANCEMENT] No object store available, skipping async backup for file_uid: " + file_uid);
    }

    // CRITICAL CONCURRENCY INFO: Operation completed successfully but potential race conditions may occur
    // during the async backup process that happens after this method returns
    SERVER_LOG_DEBUG("FileSystem::put", ServerLogger::getInstance().detailed_log_prefix() +
              "Put operation completed successfully for file_uid: " + file_uid +
              " [CONCURRENCY CRITICAL] NOTE: Race conditions possible during async operations after this point");

    return Result<void>::ok();
}

Result<std::vector<uint8_t>> FileSystem::get(const std::string& file_uid,
                                              const std::string& user,
                                              const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::vector<uint8_t>>::err("Database not available for tenant: " + tenant);
    }

    // Check permissions - the user needs read permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, {}, static_cast<int>(Permission::READ), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<std::vector<uint8_t>>::err("User does not have permission to read file");
    }

    // Get the file info to determine the current version
    auto file_info_result = context->db->get_file_by_uid(file_uid, tenant);
    if (!file_info_result.success || !file_info_result.value.has_value()) {
        return Result<std::vector<uint8_t>>::err("File does not exist");
    }

    std::string current_version = file_info_result.value->version;
    if (current_version.empty()) {
        // If no version is set, get the latest version
        auto versions_result = list_versions(file_uid, user, tenant);
        if (!versions_result.success || versions_result.value.empty()) {
            return Result<std::vector<uint8_t>>::err("No versions available for file");
        }
        current_version = versions_result.value[0]; // Latest version
    }

    // Determine expected locations for the file
    std::string local_storage_path = context->storage->get_storage_path(file_uid, current_version, tenant);
    std::string s3_storage_path = context->object_store ?
        context->object_store->get_storage_path(file_uid, current_version, tenant) : "N/A";

    SERVER_LOG_DEBUG("FileSystem::get", "File UID: " + file_uid);
    SERVER_LOG_DEBUG("FileSystem::get", "Current version: " + current_version);
    SERVER_LOG_DEBUG("FileSystem::get", "Tenant: " + tenant);
    SERVER_LOG_DEBUG("FileSystem::get", "Local storage path: " + local_storage_path);
    SERVER_LOG_DEBUG("FileSystem::get", "S3 storage path: " + s3_storage_path);

    // Check if file exists locally
    bool file_exists_locally = false;
    if (context->storage) {
        SERVER_LOG_DEBUG("FileSystem::get", "Checking if file exists locally at: " + local_storage_path);
        auto exists_result = context->storage->file_exists(local_storage_path, tenant);
        SERVER_LOG_DEBUG("FileSystem::get", "Local file exists result: " + std::string(exists_result.success ? "success" : "error"));
        if (exists_result.success) {
            SERVER_LOG_DEBUG("FileSystem::get", "Local file exists: " + std::string(exists_result.value ? "true" : "false"));
            file_exists_locally = exists_result.value;
        } else {
            SERVER_LOG_DEBUG("FileSystem::get", "Error checking local file existence: " + exists_result.error);
        }
    } else {
        SERVER_LOG_DEBUG("FileSystem::get", "No storage context available");
    }

    // If file doesn't exist locally, attempt to restore from S3
    if (!file_exists_locally && context->object_store) {
        SERVER_LOG_DEBUG("FileSystem::get", "File does not exist locally, attempting to restore from S3");

        // Compose the path to the remote payload in object store
        std::string remote_payload_path = context->object_store->get_storage_path(file_uid, current_version, tenant);
        SERVER_LOG_DEBUG("FileSystem::get", "Remote payload path: " + remote_payload_path);

        // Check if the payload exists in the remote object store
        SERVER_LOG_DEBUG("FileSystem::get", "Checking if file exists in S3 at: " + remote_payload_path);
        auto remote_exists_result = context->object_store->file_exists(remote_payload_path, tenant);
        SERVER_LOG_DEBUG("FileSystem::get", "S3 file exists result: " + std::string(remote_exists_result.success ? "success" : "error"));

        if (remote_exists_result.success && remote_exists_result.value) {
            SERVER_LOG_DEBUG("FileSystem::get", "S3 file exists: true");

            // Read from the remote object store
            SERVER_LOG_DEBUG("FileSystem::get", "Reading file from S3 at: " + remote_payload_path);
            auto object_store_result = context->object_store->read_file(remote_payload_path, tenant);
            SERVER_LOG_DEBUG("FileSystem::get", "S3 read result: " + std::string(object_store_result.success ? "success" : "error"));

            if (object_store_result.success) {
                SERVER_LOG_DEBUG("FileSystem::get", "Successfully read " + std::to_string(object_store_result.value.size()) + " bytes from S3");

                // Store the payload locally at the expected local path (for future access)
                if (context->storage) {
                    SERVER_LOG_DEBUG("FileSystem::get", "Storing file locally at: " + local_storage_path);
                    auto store_result = context->storage->store_file(file_uid, current_version,
                                                                     object_store_result.value, tenant);
                    SERVER_LOG_DEBUG("FileSystem::get", "Local store result: " + std::string(store_result.success ? "success" : "error"));

                    if (store_result.success) {
                        SERVER_LOG_DEBUG("FileSystem::get", "File successfully restored to local storage at: " + store_result.value);
                    } else {
                        SERVER_LOG_WARN("FileSystem::get", "Failed to store file locally: " + store_result.error + " (continuing with S3 data)");
                    }
                } else {
                    SERVER_LOG_WARN("FileSystem::get", "No storage context available for local storage (continuing with S3 data)");
                }

                // Add to cache if available
                if (cache_manager_) {
                    cache_manager_->add_file(local_storage_path, object_store_result.value, tenant);
                    SERVER_LOG_DEBUG("FileSystem::get", "Added file to cache");
                }

                // Return the data directly from S3 (don't re-read from disk)
                SERVER_LOG_DEBUG("FileSystem::get", "Returning file data restored from S3");
                return Result<std::vector<uint8_t>>::ok(object_store_result.value);
            } else {
                SERVER_LOG_ERROR("FileSystem::get", "Failed to read file from S3: " + object_store_result.error);
            }
        } else if (!remote_exists_result.success) {
            SERVER_LOG_ERROR("FileSystem::get", "Error checking S3 file existence: " + remote_exists_result.error);
        } else {
            SERVER_LOG_DEBUG("FileSystem::get", "File does not exist in S3 at the expected path");
        }
    } else if (!context->object_store) {
        SERVER_LOG_DEBUG("FileSystem::get", "No object store context available");
    } else {
        SERVER_LOG_DEBUG("FileSystem::get", "File exists locally, no need to restore from S3");
    }

    // Now read the file from local storage (either it was already there, or restoration from S3 failed)
    if (file_exists_locally && context->storage) {
        SERVER_LOG_DEBUG("FileSystem::get", "Reading file from local storage at: " + local_storage_path);
        auto storage_result = context->storage->read_file(local_storage_path, tenant);
        if (storage_result.success) {
            SERVER_LOG_DEBUG("FileSystem::get", "Successfully read " + std::to_string(storage_result.value.size()) + " bytes from local storage");
            // Add to cache if available
            if (cache_manager_) {
                cache_manager_->add_file(local_storage_path, storage_result.value, tenant);
            }
            return storage_result;
        } else {
            SERVER_LOG_ERROR("FileSystem::get", "Failed to read file from local storage: " + storage_result.error);
            return Result<std::vector<uint8_t>>::err("Failed to read file from local storage: " + storage_result.error);
        }
    }

    SERVER_LOG_ERROR("FileSystem::get", "File content not found in storage or object store");
    return Result<std::vector<uint8_t>>::err("File content not found in storage or object store");
}

Result<FileInfo> FileSystem::stat(const std::string& file_uid, const std::string& user, 
                                  const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<FileInfo>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs read permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, {}, static_cast<int>(Permission::READ), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<FileInfo>::err("User does not have permission to access file info");
    }
    
    auto db_result = context->db->get_file_by_uid(file_uid, tenant);
    if (!db_result.success || !db_result.value.has_value()) {
        return Result<FileInfo>::err("File does not exist");
    }
    
    return Result<FileInfo>::ok(db_result.value.value());
}

Result<bool> FileSystem::exists(const std::string& file_uid, const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<bool>::err("Database not available for tenant: " + tenant);
    }
    
    auto db_result = context->db->get_file_by_uid(file_uid, tenant);
    if (!db_result.success) {
        return Result<bool>::err(db_result.error);
    }
    
    return Result<bool>::ok(db_result.value.has_value());
}

Result<void> FileSystem::move(const std::string& src_uid, const std::string& dst_uid, 
                              const std::string& user, const std::string& tenant) {
    // This is a simplified implementation - in reality, move would involve updating 
    // the parent_uid in the database and potentially moving the actual file data
    return Result<void>::err("Move operation not fully implemented");
}

Result<void> FileSystem::copy(const std::string& src_uid, const std::string& dst_uid, 
                              const std::string& user, const std::string& tenant) {
    // This is a simplified implementation - in reality, copy would involve 
    // creating a new file and copying the content
    return Result<void>::err("Copy operation not fully implemented");
}

Result<void> FileSystem::rename(const std::string& uid, const std::string& new_name, 
                                const std::string& user, const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<void>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs write permission on the file
    auto perm_result = validate_user_permissions(uid, user, {}, static_cast<int>(Permission::WRITE), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<void>::err("User does not have permission to rename file");
    }
    
    auto db_result = context->db->update_file_name(uid, new_name, tenant);
    if (!db_result.success) {
        return Result<void>::err("Failed to rename file: " + db_result.error);
    }
    
    return Result<void>::ok();
}

Result<std::vector<std::string>> FileSystem::list_versions(const std::string& file_uid, 
                                                           const std::string& user, 
                                                           const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::vector<std::string>>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs read permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, {}, static_cast<int>(Permission::READ), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<std::vector<std::string>>::err("User does not have permission to list versions");
    }
    
    auto db_result = context->db->list_versions(file_uid, tenant);
    if (!db_result.success) {
        return Result<std::vector<std::string>>::err("Failed to list versions: " + db_result.error);
    }
    
    return Result<std::vector<std::string>>::ok(db_result.value);
}

Result<std::vector<uint8_t>> FileSystem::get_version(const std::string& file_uid, 
                                                     const std::string& version_timestamp, 
                                                     const std::string& user, 
                                                     const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::vector<uint8_t>>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs read permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, {}, static_cast<int>(Permission::READ), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<std::vector<uint8_t>>::err("User does not have permission to access version");
    }
    
    // Get the storage path for this version
    auto path_result = context->db->get_version_storage_path(file_uid, version_timestamp, tenant);
    if (!path_result.success || !path_result.value.has_value()) {
        return Result<std::vector<uint8_t>>::err("Version storage path not found");
    }
    
    std::string storage_path = path_result.value.value();
    
    // If we have a cache manager, try to get from cache first
    if (cache_manager_) {
        auto cache_result = cache_manager_->get_file(storage_path, tenant);
        if (cache_result.success) {
            return cache_result;
        }
    }
    
    // Get from storage
    if (context->storage) {
        auto storage_result = context->storage->read_file(storage_path, tenant);
        if (storage_result.success) {
            // Add to cache if available
            if (cache_manager_) {
                cache_manager_->add_file(storage_path, storage_result.value, tenant);
            }
            return storage_result;
        }
    }
    
    return Result<std::vector<uint8_t>>::err("Version content not found");
}

Result<bool> FileSystem::restore_to_version(const std::string& file_uid,
                                           const std::string& version_timestamp,
                                           const std::string& user,
                                           const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<bool>::err("Database not available for tenant: " + tenant);
    }

    // Check if user has special permission to restore to version
    // Typically requires WRITE permission or special version management permission
    auto perm_result = validate_user_permissions(file_uid, user, std::vector<std::string>(), static_cast<int>(fileengine::Permission::WRITE), tenant); // WRITE permission
    if (!perm_result.success || !perm_result.value) {
        return Result<bool>::err("User does not have permission to restore to version");
    }

    auto result = context->db->restore_to_version(file_uid, version_timestamp, user, tenant);
    return result;
}

Result<void> FileSystem::backup_to_object_store(const std::string& file_uid,
                                                const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->storage || !context->object_store) {
        return Result<void>::err("Storage or object store not available for tenant: " + tenant);
    }

    // Get the most recent version timestamp from the versions table
    // This ensures we're backing up the latest version that was actually stored
    auto versions_result = context->db->list_versions(file_uid, tenant);
    if (!versions_result.success || versions_result.value.empty()) {
        return Result<void>::err("No versions found for file: " + file_uid);
    }

    // Get the most recent version (list_versions returns versions sorted by timestamp)
    // The last element in the list should be the most recent
    std::string current_version = versions_result.value.back();
    if (current_version.empty()) {
        return Result<void>::err("Version timestamp is empty for file: " + file_uid);
    }

    // Construct the local storage path directly using the desaturation pattern
    // This avoids the need to query the database for storage paths
    std::string local_storage_path = context->storage->get_storage_path(file_uid, current_version, tenant);

    // Read the file from local storage
    auto storage_result = context->storage->read_file(local_storage_path, tenant);
    if (!storage_result.success) {
        return Result<void>::err("Failed to read file from local storage: " + storage_result.error);
    }

    // Store in object store using the file UID and version timestamp
    auto obj_store_result = context->object_store->store_file(file_uid, current_version,
                                                              storage_result.value, tenant);
    if (!obj_store_result.success) {
        return Result<void>::err("Failed to store file in object store: " + obj_store_result.error);
    }

    return Result<void>::ok();
}

Result<void> FileSystem::backup_to_object_store_with_version(const std::string& file_uid,
                                                           const std::string& tenant,
                                                           const std::string& version_timestamp) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->storage || !context->object_store) {
        return Result<void>::err("Storage or object store not available for tenant: " + tenant);
    }

    // Validate that the version timestamp is not empty
    if (version_timestamp.empty()) {
        return Result<void>::err("Version timestamp is empty for file: " + file_uid);
    }

    // Construct the local storage path directly using the desaturation pattern
    // This ensures we're backing up the specific version that was stored
    std::string local_storage_path = context->storage->get_storage_path(file_uid, version_timestamp, tenant);

    // Read the file from local storage
    auto storage_result = context->storage->read_file(local_storage_path, tenant);
    if (!storage_result.success) {
        return Result<void>::err("Failed to read file from local storage: " + storage_result.error);
    }

    // Store in object store using the file UID and version timestamp
    auto obj_store_result = context->object_store->store_file(file_uid, version_timestamp,
                                                              storage_result.value, tenant);
    if (!obj_store_result.success) {
        return Result<void>::err("Failed to store file in object store: " + obj_store_result.error);
    }

    return Result<void>::ok();
}

Result<void> FileSystem::purge_old_versions(const std::string& file_uid, int keep_count,
                                            const std::string& tenant) {
    // This is a simplified implementation - in reality, this would involve
    // deleting old versions from both local storage and object store
    return Result<void>::err("Purge old versions not fully implemented");
}

void FileSystem::update_cache_threshold(double threshold, const std::string& tenant) {
    if (cache_manager_) {
        cache_manager_->set_cache_threshold(threshold);
    }
}

Result<double> FileSystem::get_cache_usage_percentage(const std::string& tenant) const {
    if (cache_manager_) {
        return Result<double>::ok(cache_manager_->get_cache_usage_percentage());
    }
    return Result<double>::err("Cache manager not available");
}

Result<void> FileSystem::set_metadata(const std::string& file_uid, const std::string& key, 
                                      const std::string& value, const std::string& user, 
                                      const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<void>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs write permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, {}, static_cast<int>(Permission::WRITE), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<void>::err("User does not have permission to set metadata");
    }
    
    std::string version_timestamp = "current"; // Would use actual current version in practice
    auto db_result = context->db->set_metadata(file_uid, version_timestamp, key, value, tenant);
    return db_result;
}

Result<std::string> FileSystem::get_metadata(const std::string& file_uid, const std::string& key, 
                                             const std::string& user, const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::string>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs read permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, {}, static_cast<int>(Permission::READ), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<std::string>::err("User does not have permission to get metadata");
    }
    
    std::string version_timestamp = "current"; // Would use actual current version in practice
    auto db_result = context->db->get_metadata(file_uid, version_timestamp, key, tenant);
    if (!db_result.success || !db_result.value.has_value()) {
        return Result<std::string>::err("Metadata key not found");
    }
    
    return Result<std::string>::ok(db_result.value.value());
}

Result<std::map<std::string, std::string>> FileSystem::get_all_metadata(const std::string& file_uid, 
                                                                        const std::string& user, 
                                                                        const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::map<std::string, std::string>>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs read permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, {}, static_cast<int>(Permission::READ), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<std::map<std::string, std::string>>::err("User does not have permission to get metadata");
    }
    
    std::string version_timestamp = "current"; // Would use actual current version in practice
    auto db_result = context->db->get_all_metadata(file_uid, version_timestamp, tenant);
    return db_result;
}

Result<void> FileSystem::delete_metadata(const std::string& file_uid, const std::string& key, 
                                         const std::string& user, const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<void>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs write permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, {}, static_cast<int>(Permission::WRITE), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<void>::err("User does not have permission to delete metadata");
    }
    
    std::string version_timestamp = "current"; // Would use actual current version in practice
    auto db_result = context->db->delete_metadata(file_uid, version_timestamp, key, tenant);
    return db_result;
}

Result<std::string> FileSystem::get_metadata_for_version(const std::string& file_uid, 
                                                         const std::string& version_timestamp, 
                                                         const std::string& key, 
                                                         const std::string& user, 
                                                         const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::string>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs read permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, {}, static_cast<int>(Permission::READ), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<std::string>::err("User does not have permission to get metadata");
    }
    
    auto db_result = context->db->get_metadata(file_uid, version_timestamp, key, tenant);
    if (!db_result.success || !db_result.value.has_value()) {
        return Result<std::string>::err("Metadata key not found");
    }
    
    return Result<std::string>::ok(db_result.value.value());
}

Result<std::map<std::string, std::string>> FileSystem::get_all_metadata_for_version(const std::string& file_uid, 
                                                                                    const std::string& version_timestamp, 
                                                                                    const std::string& user, 
                                                                                    const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::map<std::string, std::string>>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs read permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, {}, static_cast<int>(Permission::READ), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<std::map<std::string, std::string>>::err("User does not have permission to get metadata");
    }
    
    auto db_result = context->db->get_all_metadata(file_uid, version_timestamp, tenant);
    return db_result;
}

Result<void> FileSystem::grant_permission(const std::string& resource_uid, 
                                          const std::string& principal, 
                                          int permissions, 
                                          const std::string& user, 
                                          const std::string& tenant) {
    if (!acl_manager_) {
        return Result<void>::err("ACL manager not available");
    }
    
    // Only allow users with appropriate permissions to grant permissions
    auto perm_result = validate_user_permissions(resource_uid, user, {}, static_cast<int>(Permission::WRITE), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<void>::err("User does not have permission to grant permissions");
    }
    
    auto result = acl_manager_->grant_permission(resource_uid, principal, PrincipalType::USER, 
                                                 permissions, tenant);
    return result;
}

Result<void> FileSystem::revoke_permission(const std::string& resource_uid, 
                                           const std::string& principal, 
                                           int permissions, 
                                           const std::string& user, 
                                           const std::string& tenant) {
    if (!acl_manager_) {
        return Result<void>::err("ACL manager not available");
    }
    
    // Only allow users with appropriate permissions to revoke permissions
    auto perm_result = validate_user_permissions(resource_uid, user, {}, static_cast<int>(Permission::WRITE), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<void>::err("User does not have permission to revoke permissions");
    }
    
    auto result = acl_manager_->revoke_permission(resource_uid, principal, PrincipalType::USER, 
                                                  permissions, tenant);
    return result;
}

Result<bool> FileSystem::check_permission(const std::string& resource_uid, 
                                          const std::string& user, 
                                          const std::vector<std::string>& roles, 
                                          int required_permissions, 
                                          const std::string& tenant) {
    if (!acl_manager_) {
        // If no ACL manager, default to allowing access for basic implementation
        return Result<bool>::ok(true);
    }
    
    auto result = acl_manager_->check_permission(resource_uid, user, roles, 
                                                 required_permissions, tenant);
    return result;
}

void FileSystem::shutdown() {
    // Stop the async backup worker thread
    stop_async_backup_worker();

    // Cleanup operations
    if (cache_manager_) {
        cache_manager_->cleanup_cache();
    }
}

Result<std::vector<uint8_t>> FileSystem::fetch_from_object_store_if_missing(const std::string& uid,
                                                                           const std::string& version_timestamp,
                                                                           const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->object_store) {
        return Result<std::vector<uint8_t>>::err("Object store not available for tenant: " + tenant);
    }

    // Create the storage path for the object store lookup
    auto obj_store_path = context->object_store->get_storage_path(uid, version_timestamp, tenant);

    // Read from object store
    auto object_store_result = context->object_store->read_file(obj_store_path, tenant);
    if (!object_store_result.success) {
        return Result<std::vector<uint8_t>>::err("File not found in object store: " + object_store_result.error);
    }

    // If found in object store, also store in local storage and cache
    if (context->storage) {
        auto store_result = context->storage->store_file(uid, version_timestamp,
                                                         object_store_result.value, tenant);
        if (store_result.success && cache_manager_) {
            cache_manager_->add_file(store_result.value, object_store_result.value, tenant);
        }
    }

    return Result<std::vector<uint8_t>>::ok(object_store_result.value);
}

TenantContext* FileSystem::get_tenant_context(const std::string& tenant) {
    SERVER_LOG_DEBUG("FileSystem::get_tenant_context", ServerLogger::getInstance().detailed_log_prefix() +
              "Called for tenant: " + tenant);
    if (!tenant_manager_) {
        SERVER_LOG_WARN("FileSystem::get_tenant_context", ServerLogger::getInstance().detailed_log_prefix() +
                 "Tenant manager not available.");
        return nullptr;
    }

    // Get the tenant context
    TenantContext* context = tenant_manager_->get_tenant_context(tenant);

    // If context doesn't exist, try to initialize the tenant first
    if (!context) {
        SERVER_LOG_INFO("FileSystem::get_tenant_context", ServerLogger::getInstance().detailed_log_prefix() +
                 "Tenant context not found for " + tenant + ", attempting initialization.");
        // Try to initialize the tenant (this should create the schema)
        if (tenant_manager_->initialize_tenant(tenant)) {
            SERVER_LOG_INFO("FileSystem::get_tenant_context", ServerLogger::getInstance().detailed_log_prefix() +
                     "Tenant " + tenant + " initialized successfully, re-getting context.");
            // Try getting the context again after initialization
            context = tenant_manager_->get_tenant_context(tenant);
        } else {
            SERVER_LOG_ERROR("FileSystem::get_tenant_context", ServerLogger::getInstance().detailed_log_prefix() +
                      "Failed to initialize tenant: " + tenant);
        }
    } else {
        SERVER_LOG_DEBUG("FileSystem::get_tenant_context", ServerLogger::getInstance().detailed_log_prefix() +
                  "Tenant context found for: " + tenant);
    }

    return context;
}

Result<bool> FileSystem::validate_user_permissions(const std::string& resource_uid, 
                                                  const std::string& user,
                                                  const std::vector<std::string>& roles,
                                                  int required_permissions,
                                                  const std::string& tenant) {
    if (!acl_manager_) {
        // If no ACL manager, default to allowing access for basic implementation
        return Result<bool>::ok(true);
    }
    
    auto result = acl_manager_->check_permission(resource_uid, user, roles, 
                                                 required_permissions, tenant);
    return result;
}

void FileSystem::start_async_backup_worker() {
    if (backup_worker_running_.load()) {
        return; // Already running
    }

    SERVER_LOG_DEBUG("FileSystem", ServerLogger::getInstance().detailed_log_prefix() +
              "[PERFORMANCE ENHANCEMENT] Starting async backup worker thread");

    backup_worker_running_.store(true);
    backup_worker_thread_ = std::thread(&FileSystem::backup_worker_loop, this);

    SERVER_LOG_DEBUG("FileSystem", ServerLogger::getInstance().detailed_log_prefix() +
              "[PERFORMANCE ENHANCEMENT] Async backup worker thread started");
}

void FileSystem::stop_async_backup_worker() {
    if (!backup_worker_running_.load()) {
        return; // Already stopped
    }

    SERVER_LOG_DEBUG("FileSystem", ServerLogger::getInstance().detailed_log_prefix() +
              "[PERFORMANCE ENHANCEMENT] Stopping async backup worker thread");

    backup_worker_running_.store(false);
    queue_cv_.notify_all(); // Wake up the worker thread if it's waiting

    if (backup_worker_thread_.joinable()) {
        backup_worker_thread_.join();
    }

    SERVER_LOG_DEBUG("FileSystem", ServerLogger::getInstance().detailed_log_prefix() +
              "[PERFORMANCE ENHANCEMENT] Async backup worker thread stopped");
}

void FileSystem::backup_worker_loop() {
    SERVER_LOG_DEBUG("FileSystem::backup_worker_loop", ServerLogger::getInstance().detailed_log_prefix() +
              "[PERFORMANCE ENHANCEMENT] Backup worker thread loop started");

    while (backup_worker_running_.load()) {
        BackupTask task;
        bool has_task = false;

        std::unique_lock<std::mutex> lock(queue_mutex_);
        SERVER_LOG_DEBUG("FileSystem::backup_worker_loop", ServerLogger::getInstance().detailed_log_prefix() +
                  "[CONCURRENCY] Backup worker entering wait state.");
        queue_cv_.wait(lock, [this] {
            return !backup_queue_.empty() || !backup_worker_running_.load();
        });
        SERVER_LOG_DEBUG("FileSystem::backup_worker_loop", ServerLogger::getInstance().detailed_log_prefix() +
                  "[CONCURRENCY] Backup worker woke up. Queue empty: " + (backup_queue_.empty() ? "true" : "false") +
                  ", Running: " + (backup_worker_running_.load() ? "true" : "false"));

        if (!backup_worker_running_.load() && backup_queue_.empty()) {
            SERVER_LOG_DEBUG("FileSystem::backup_worker_loop", ServerLogger::getInstance().detailed_log_prefix() +
                      "[CONCURRENCY] Backup worker shutting down (no more tasks and not running).");
            break; // Shutting down and no more tasks
        }

        // Get a task from the queue
        if (!backup_queue_.empty()) {
            task = backup_queue_.front();
            backup_queue_.pop();
            has_task = true;
            SERVER_LOG_DEBUG("FileSystem::backup_worker_loop", ServerLogger::getInstance().detailed_log_prefix() +
                      "[CONCURRENCY] Backup worker retrieved task from queue.");
        } else {
            SERVER_LOG_DEBUG("FileSystem::backup_worker_loop", ServerLogger::getInstance().detailed_log_prefix() +
                      "[CONCURRENCY] Backup worker woke up but no task available (likely shutdown signal).");
        }

        lock.unlock(); // Release lock before processing task to allow other threads to push tasks

        if (has_task) {
            SERVER_LOG_DEBUG("FileSystem::backup_worker_loop", ServerLogger::getInstance().detailed_log_prefix() +
                      "[PERFORMANCE ENHANCEMENT] Processing backup task for file: " + task.file_uid +
                      ", tenant: " + task.tenant);

            // Perform the actual backup operation
            auto context = get_tenant_context(task.tenant);
            if (context && context->object_store) {
                SERVER_LOG_DEBUG("FileSystem::backup_worker_loop", ServerLogger::getInstance().detailed_log_prefix() +
                          "[CONCURRENCY CRITICAL] Beginning object store backup for file: " + task.file_uid +
                          " [PERFORMANCE ENHANCEMENT: Async backup operation]");

                // Use the version timestamp from the task to ensure we backup the correct version
                auto backup_result = backup_to_object_store_with_version(task.file_uid, task.tenant, task.version_timestamp);
                if (backup_result.success) {
                    SERVER_LOG_DEBUG("FileSystem::backup_worker_loop", ServerLogger::getInstance().detailed_log_prefix() +
                              "[PERFORMANCE ENHANCEMENT] Successfully backed up file: " + task.file_uid +
                              " to object store");
                } else {
                    SERVER_LOG_ERROR("FileSystem::backup_worker_loop", ServerLogger::getInstance().detailed_log_prefix() +
                              "[PERFORMANCE ENHANCEMENT] Failed to backup file: " + task.file_uid +
                              " to object store, error: " + backup_result.error);
                }
            } else {
                SERVER_LOG_WARN("FileSystem::backup_worker_loop", ServerLogger::getInstance().detailed_log_prefix() +
                         "[PERFORMANCE ENHANCEMENT] No object store available for tenant: " + task.tenant +
                         ", skipping backup for file: " + task.file_uid);
            }
        }
    }

    SERVER_LOG_DEBUG("FileSystem::backup_worker_loop", ServerLogger::getInstance().detailed_log_prefix() +
              "[PERFORMANCE ENHANCEMENT] Backup worker thread loop ended");
}

} // namespace fileengine