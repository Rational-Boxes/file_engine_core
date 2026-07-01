#include "fileengine/filesystem.h"
#include "fileengine/utils.h"
#include "fileengine/server_logger.h"
#include "fileengine/crypto_utils.h"
#include <algorithm>
#include <optional>
#include <fstream>
#include <filesystem>

namespace fileengine {

namespace {
// Convert a system_clock time_point to whole UNIX epoch seconds, matching the
// int64 timestamp fields in the proto DirectoryEntry/FileInfo messages.
int64_t to_epoch_seconds(const std::chrono::system_clock::time_point& tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
}

// Returns true if `candidate` is `ancestor` itself or nested somewhere inside
// it, by walking the parent chain up to the root (empty parent_uid).
//
// Used to reject copying/moving a directory into its own subtree. Without this
// guard a recursive directory copy into a descendant never terminates — each
// level creates a fresh child inside the destination, which then appears in the
// next listing — exhausting the stack and crashing the server (SIGSEGV). For
// move it would splice an unreachable cycle into the tree. kMaxDepth bounds the
// walk so a pre-existing cycle can't hang the request.
bool is_within_subtree(const std::shared_ptr<IDatabase>& db,
                       const std::string& candidate,
                       const std::string& ancestor,
                       const std::string& tenant) {
    constexpr int kMaxDepth = 4096;
    std::string current = candidate;
    for (int depth = 0; depth < kMaxDepth && !current.empty(); ++depth) {
        if (current == ancestor) return true;
        auto info = db->get_file_by_uid(current, tenant);
        if (!info.success || !info.value.has_value()) break;
        current = info.value->parent_uid;
    }
    return false;
}
} // namespace

FileSystem::FileSystem(std::shared_ptr<TenantManager> tenant_manager)
    : tenant_manager_(tenant_manager) {
    // Initialize file culler (cache management) - will be set by the server with proper dependencies
    // The actual initialization will happen when the server sets it up with storage tracker
    // Start the async backup worker thread
    start_async_backup_worker();
}

FileSystem::~FileSystem() {
    shutdown();
}

Result<std::string> FileSystem::mkdir(const std::string& parent_uid, const std::string& name,
                                      const std::string& user,
                                      const std::vector<std::string>& roles,
                                      int permissions,
                                      const std::string& tenant) {
    // Request-scoped cache: the parent ACL is read once by the WRITE check
    // and again by compute_initial_acl_grants — share the read.
    std::optional<AclManager::CacheScope> cache_scope;
    if (acl_manager_) cache_scope.emplace(*acl_manager_);

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

    // Check permissions - the user needs write permission on the parent directory.
    // Creating directly under the filesystem root is restricted to callers
    // holding the system_admin role in their effective roles.
    if (parent_uid.empty()) {
        SERVER_LOG_DEBUG("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
                  "Attempting root directory creation - only allowed for system_admin role");
        if (!acl_manager_ || !acl_manager_->is_system_admin(user, roles, tenant)) {
            SERVER_LOG_ERROR("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
                      "Non-admin user attempting to create in root directory");
            return Result<std::string>::err("Only system_admin can create in root directory");
        }
    } else {
        auto perm_result = validate_user_permissions(parent_uid, user, roles, static_cast<int>(Permission::WRITE), tenant);
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

    // Atomic creation: file row + default/inherited ACLs in one transaction
    // so a crash mid-creation can't leave a directory without ACLs.
    auto grants = compute_initial_acl_grants(parent_uid, user, tenant);
    SERVER_LOG_DEBUG("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
              "Inserting directory atomically with UID: " + new_uid);
    auto db_result = context->db->create_file_with_acls(new_uid, name, path, parent_uid,
                                                       FileType::DIRECTORY, user, permissions,
                                                       grants, tenant);
    if (!db_result.success) {
        SERVER_LOG_ERROR("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
                  "Failed to create directory in database: " + db_result.error);
        return Result<std::string>::err("Failed to create directory in database: " + db_result.error);
    }

    SERVER_LOG_DEBUG("FileSystem::mkdir", ServerLogger::getInstance().detailed_log_prefix() +
              "Successfully created directory with UID: " + new_uid);
    emit_fs_event(tenant, FileEventType::DirCreated, new_uid, user);
    return Result<std::string>::ok(new_uid);
}

Result<void> FileSystem::rmdir(const std::string& dir_uid, const std::string& user,
                               const std::vector<std::string>& roles,
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
    auto perm_result = validate_user_permissions(dir_uid, user, roles, static_cast<int>(Permission::WRITE), tenant);
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
    auto list_result = listdir(dir_uid, user, roles, tenant);
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

    emit_fs_event(tenant, FileEventType::DirDeleted, dir_uid, user);
    return Result<void>::ok();
}

Result<std::vector<DirectoryEntry>> FileSystem::listdir(const std::string& dir_uid,
                                                        const std::string& user,
                                                        const std::vector<std::string>& roles,
                                                        const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::vector<DirectoryEntry>>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs read permission on the directory
    auto perm_result = validate_user_permissions(dir_uid, user, roles, static_cast<int>(Permission::READ), tenant);
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
        // These int64 fields are otherwise read uninitialized by the gRPC layer.
        entry.created_at = to_epoch_seconds(file_info.created_at);
        entry.modified_at = to_epoch_seconds(file_info.modified_at);
        entry.version_count = file_info.version_count;
        entry.rendition_count = file_info.rendition_count;
        entry.deleted = file_info.deleted;
        entry.owner = file_info.owner;
        entry.created_by = file_info.created_by;
        entry.modified_by = file_info.modified_by;

        entries.push_back(entry);
    }

    return Result<std::vector<DirectoryEntry>>::ok(entries);
}

Result<std::vector<DirectoryEntry>> FileSystem::listdir_with_deleted(const std::string& dir_uid,
                                                                     const std::string& user,
                                                                     const std::vector<std::string>& roles,
                                                                     const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::vector<DirectoryEntry>>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs read permission on the directory
    auto perm_result = validate_user_permissions(dir_uid, user, roles, static_cast<int>(Permission::READ), tenant);
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
        // These int64 fields are otherwise read uninitialized by the gRPC layer.
        entry.created_at = to_epoch_seconds(file_info.created_at);
        entry.modified_at = to_epoch_seconds(file_info.modified_at);
        entry.version_count = file_info.version_count;
        entry.rendition_count = file_info.rendition_count;
        entry.deleted = file_info.deleted;
        entry.owner = file_info.owner;
        entry.created_by = file_info.created_by;
        entry.modified_by = file_info.modified_by;

        entries.push_back(entry);
    }

    return Result<std::vector<DirectoryEntry>>::ok(entries);
}

Result<std::string> FileSystem::touch(const std::string& parent_uid, const std::string& name,
                                      const std::string& user,
                                      const std::vector<std::string>& roles,
                                      const std::string& tenant) {
    std::optional<AclManager::CacheScope> cache_scope;
    if (acl_manager_) cache_scope.emplace(*acl_manager_);

    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::string>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs write permission on the parent directory.
    // Creating directly under the filesystem root is restricted to system admins.
    if (parent_uid.empty()) {
        if (!acl_manager_ || !acl_manager_->is_system_admin(user, roles, tenant)) {
            return Result<std::string>::err("Only system_admin can create in root directory");
        }
    } else {
        auto perm_result = validate_user_permissions(parent_uid, user, roles, static_cast<int>(Permission::WRITE), tenant);
        if (!perm_result.success || !perm_result.value) {
            return Result<std::string>::err("User does not have permission to create file");
        }
    }
    
    std::string new_uid = Utils::generate_uuid();
    std::string path = parent_uid.empty() ? "/" + name : parent_uid + "/" + name;
    
    // Atomic creation: file row + default/inherited ACLs in one transaction.
    auto grants = compute_initial_acl_grants(parent_uid, user, tenant);
    auto db_result = context->db->create_file_with_acls(new_uid, name, path, parent_uid,
                                                       FileType::REGULAR_FILE, user, 0644,
                                                       grants, tenant);
    if (!db_result.success) {
        return Result<std::string>::err("Failed to create file in database: " + db_result.error);
    }

    emit_fs_event(tenant, FileEventType::FileCreated, new_uid, user);
    return Result<std::string>::ok(new_uid);
}

Result<void> FileSystem::remove(const std::string& file_uid, const std::string& user,
                                const std::vector<std::string>& roles,
                                const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<void>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs write permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, roles, static_cast<int>(Permission::WRITE), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<void>::err("User does not have permission to remove file");
    }
    
    // Mark file as deleted in database
    auto db_result = context->db->delete_file(file_uid, tenant);
    if (!db_result.success) {
        return Result<void>::err("Failed to remove file from database: " + db_result.error);
    }

    // Cascade to hidden renditions: they are children of the file, so removing
    // the file removes its alternate-format copies too (one level). Best-effort.
    auto rend_list = context->db->list_files_in_directory(file_uid, tenant);
    if (rend_list.success) {
        for (const auto& rend : rend_list.value) {
            auto del = context->db->delete_file(rend.uid, tenant);
            if (!del.success) {
                SERVER_LOG_ERROR("FileSystem::remove", ServerLogger::getInstance().detailed_log_prefix() +
                          "Failed to remove rendition " + rend.uid + ": " + del.error);
            }
        }
    }

    emit_fs_event(tenant, FileEventType::FileDeleted, file_uid, user);
    return Result<void>::ok();
}

Result<void> FileSystem::undelete(const std::string& file_uid, const std::string& user,
                                  const std::vector<std::string>& roles,
                                  const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<void>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs write permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, roles, static_cast<int>(Permission::WRITE), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<void>::err("User does not have permission to undelete file");
    }
    
    // Mark file as not deleted in database
    auto db_result = context->db->undelete_file(file_uid, tenant);
    if (!db_result.success) {
        return Result<void>::err("Failed to undelete file in database: " + db_result.error);
    }

    emit_fs_event(tenant, FileEventType::FileRestored, file_uid, user);
    return Result<void>::ok();
}

Result<void> FileSystem::put(const std::string& file_uid, const std::vector<uint8_t>& data,
                             const std::string& user,
                             const std::vector<std::string>& roles,
                             const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db || !context->storage) {
        return Result<void>::err("Database or storage not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs write permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, roles, static_cast<int>(Permission::WRITE), tenant);
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
    
    // Check if we need to perform cache pruning before storing
    // Only proceed with cache management if object store is available to prevent data loss
    if (file_culler_ && context->object_store) {
        // Verify object store is initialized and available
        bool object_store_available = context->object_store->is_initialized();
        if (object_store_available) {
            // Try to perform culling to free up space for the specific file size
            size_t file_size = data.size();
            auto culling_result = file_culler_->perform_culling_for_space(file_size);
            if (!culling_result.success) {
                // Log the error but continue with the operation
                SERVER_LOG_WARN("FileSystem::put", "Cache culling failed: " + culling_result.error);
            }
        } else {
            // If object store is not available, we cannot safely cull local files
            return Result<void>::err("Object store not available - cannot perform cache culling to make space");
        }
    } else if (file_culler_ && !context->object_store) {
        // If culling is enabled but no object store is configured, return an error to prevent data loss
        return Result<void>::err("Cache culling requires object store configuration to prevent data loss");
    }

    // Process data for storage (compress and encrypt if enabled)
    std::vector<uint8_t> processed_data = data;

    // Check if compression is enabled for this tenant/context
    if (context->storage && context->storage->is_compression_enabled()) {
        try {
            processed_data = fileengine::CryptoUtils::compress_data(processed_data);
            SERVER_LOG_DEBUG("FileSystem::put", "Data compressed from " + std::to_string(data.size()) +
                             " to " + std::to_string(processed_data.size()) + " bytes");
        } catch (const std::exception& e) {
            SERVER_LOG_ERROR("FileSystem::put", "Compression failed: " + std::string(e.what()));
            return Result<void>::err("Failed to compress data: " + std::string(e.what()));
        }
    }

    // Check if encryption is enabled for this tenant/context
    if (context->storage && context->storage->is_encryption_enabled()) {
        try {
            // Get the encryption key from the config
            std::string encryption_key = context->config.encryption_key;
            if (encryption_key.empty()) {
                return Result<void>::err("Encryption key not available");
            }

            processed_data = fileengine::CryptoUtils::encrypt_data(processed_data, encryption_key);
            SERVER_LOG_DEBUG("FileSystem::put", "Data encrypted successfully");
        } catch (const std::exception& e) {
            SERVER_LOG_ERROR("FileSystem::put", "Encryption failed: " + std::string(e.what()));
            return Result<void>::err("Failed to encrypt data: " + std::string(e.what()));
        }
    }

    // Store the processed file in storage
    auto storage_result = context->storage->store_file(file_uid, version_timestamp, processed_data, tenant);
    if (!storage_result.success) {
        return Result<void>::err("Failed to store file in storage: " + storage_result.error);
    }

    // Record file creation in storage tracker for cache management
    if (context->storage_tracker) {
        context->storage_tracker->record_file_creation(storage_result.value, data.size(), tenant);
    }
    
    // Update the file's current version in the database
    auto update_result = context->db->update_file_current_version(file_uid, version_timestamp, tenant);
    if (!update_result.success) {
        return Result<void>::err("Failed to update current version: " + update_result.error);
    }
    
    // Record the version in the database
    auto insert_version_result = context->db->insert_version(file_uid, version_timestamp, data.size(),
                                                             storage_result.value, user, tenant);
    if (!insert_version_result.success) {
        return Result<void>::err("Failed to record version: " + insert_version_result.error);
    }

    // Keep files.size in sync with the current content so stat/listdir report
    // the real byte size (the version table alone is not read by stat).
    auto update_size_result = context->db->update_file_size(file_uid, static_cast<int64_t>(data.size()), tenant);
    if (!update_size_result.success) {
        SERVER_LOG_ERROR("FileSystem::put", ServerLogger::getInstance().detailed_log_prefix() +
                  "Failed to update file size for " + file_uid + ": " + update_size_result.error);
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

    emit_fs_event(tenant, FileEventType::FileUpdated, file_uid, user);
    return Result<void>::ok();
}

Result<std::vector<uint8_t>> FileSystem::get(const std::string& file_uid,
                                              const std::string& user,
                                              const std::vector<std::string>& roles,
                                              const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::vector<uint8_t>>::err("Database not available for tenant: " + tenant);
    }

    // Check permissions - the user needs read permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, roles, static_cast<int>(Permission::READ), tenant);
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
        auto versions_result = list_versions(file_uid, user, roles, tenant);
        if (!versions_result.success || versions_result.value.empty()) {
            return Result<std::vector<uint8_t>>::err("No versions available for file");
        }
        current_version = versions_result.value[0]; // Latest version
    }

    // Look up the actual stored path from the versions table. The schema's
    // storage_path column is the source of truth — important for restored
    // versions where (uid, new_timestamp) doesn't map to a real file but the
    // row points at an older version's file.
    std::string local_storage_path;
    auto path_result = context->db->get_version_storage_path(file_uid, current_version, tenant);
    if (path_result.success && path_result.value.has_value()) {
        local_storage_path = path_result.value.value();
    } else {
        // Fallback: derive from (uid, version_timestamp). Covers callers
        // that wrote bytes before a versions row existed.
        local_storage_path = context->storage->get_storage_path(file_uid, current_version, tenant);
    }
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

            // Process data after reading (decrypt and decompress if needed)
            std::vector<uint8_t> processed_data = storage_result.value;

            // Check if encryption is enabled for this tenant/context
            if (context->storage && context->storage->is_encryption_enabled()) {
                try {
                    // Get the encryption key from the config
                    std::string encryption_key = context->config.encryption_key;
                    if (encryption_key.empty()) {
                        return Result<std::vector<uint8_t>>::err("Encryption key not available");
                    }

                    processed_data = fileengine::CryptoUtils::decrypt_data(processed_data, encryption_key);
                    SERVER_LOG_DEBUG("FileSystem::get", "Data decrypted successfully");
                } catch (const std::exception& e) {
                    SERVER_LOG_ERROR("FileSystem::get", "Decryption failed: " + std::string(e.what()));
                    return Result<std::vector<uint8_t>>::err("Failed to decrypt data: " + std::string(e.what()));
                }
            }

            // Check if compression is enabled for this tenant/context
            if (context->storage && context->storage->is_compression_enabled()) {
                try {
                    processed_data = fileengine::CryptoUtils::decompress_data(processed_data);
                    SERVER_LOG_DEBUG("FileSystem::get", "Data decompressed successfully");
                } catch (const std::exception& e) {
                    SERVER_LOG_ERROR("FileSystem::get", "Decompression failed: " + std::string(e.what()));
                    return Result<std::vector<uint8_t>>::err("Failed to decompress data: " + std::string(e.what()));
                }
            }

            // Add to cache if available
            if (cache_manager_) {
                cache_manager_->add_file(local_storage_path, processed_data, tenant);
            }
            return Result<std::vector<uint8_t>>::ok(processed_data);
        } else {
            SERVER_LOG_ERROR("FileSystem::get", "Failed to read file from local storage: " + storage_result.error);
            return Result<std::vector<uint8_t>>::err("Failed to read file from local storage: " + storage_result.error);
        }
    }

    SERVER_LOG_ERROR("FileSystem::get", "File content not found in storage or object store");
    return Result<std::vector<uint8_t>>::err("File content not found in storage or object store");
}

Result<void> FileSystem::put_stream(const std::string& file_uid,
                                    const std::function<bool(std::vector<uint8_t>&)>& next_chunk,
                                    const std::string& user,
                                    const std::vector<std::string>& roles,
                                    const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db || !context->storage) {
        return Result<void>::err("Database or storage not available for tenant: " + tenant);
    }
    auto perm_result = validate_user_permissions(file_uid, user, roles, static_cast<int>(Permission::WRITE), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<void>::err("User does not have permission to write file");
    }
    auto file_info_result = context->db->get_file_by_uid(file_uid, tenant);
    if (!file_info_result.success || !file_info_result.value.has_value()) {
        return Result<void>::err("File does not exist");
    }
    // Mirror put()'s culling safety guard: without an object store we cannot
    // safely free local space (and a stream has no known size to cull by).
    if (file_culler_ && !context->object_store) {
        return Result<void>::err("Cache culling requires object store configuration to prevent data loss");
    }

    const std::string version_timestamp = Utils::get_timestamp_string();
    const std::string storage_path = context->storage->get_storage_path(file_uid, version_timestamp, tenant);

    const bool do_compress = context->storage->is_compression_enabled();
    const bool do_encrypt = context->storage->is_encryption_enabled();
    std::string encryption_key;
    if (do_encrypt) {
        encryption_key = context->config.encryption_key;
        if (encryption_key.empty()) return Result<void>::err("Encryption key not available");
    }

    try {
        std::filesystem::create_directories(std::filesystem::path(storage_path).parent_path());
    } catch (const std::exception& e) {
        return Result<void>::err("Failed to create storage directory: " + std::string(e.what()));
    }

    std::ofstream ofs(storage_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return Result<void>::err("Failed to open storage file for writing: " + storage_path);
    }

    uint64_t original_size = 0;
    try {
        std::unique_ptr<CompressStream> compressor;
        std::unique_ptr<EncryptStream> encryptor;
        if (do_compress) compressor = std::make_unique<CompressStream>();
        if (do_encrypt) encryptor = std::make_unique<EncryptStream>(encryption_key);

        std::vector<uint8_t> chunk, cbuf, ebuf;
        auto sink = [&](const uint8_t* p, size_t n) {
            if (n > 0) ofs.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
        };
        // Push one plaintext chunk through compress -> encrypt -> disk.
        auto feed = [&](const uint8_t* p, size_t n) {
            if (do_compress) { compressor->update(p, n, cbuf); p = cbuf.data(); n = cbuf.size(); }
            if (n == 0) return;
            if (do_encrypt) { encryptor->update(p, n, ebuf); sink(ebuf.data(), ebuf.size()); }
            else sink(p, n);
        };

        while (next_chunk(chunk)) {
            if (chunk.empty()) continue;
            original_size += chunk.size();
            feed(chunk.data(), chunk.size());
        }

        if (original_size == 0) {
            // Empty content -> empty blob (matches the one-shot convention where
            // compress/encrypt of empty data yields an empty stored blob).
            ofs.close();
        } else {
            if (do_compress) {
                compressor->finish(cbuf);            // flush trailing compressed bytes
                if (!cbuf.empty()) {
                    if (do_encrypt) { encryptor->update(cbuf.data(), cbuf.size(), ebuf); sink(ebuf.data(), ebuf.size()); }
                    else sink(cbuf.data(), cbuf.size());
                }
            }
            if (do_encrypt) {
                encryptor->finish(ebuf);             // appends the 16-byte GCM tag
                sink(ebuf.data(), ebuf.size());
            }
            ofs.flush();
            ofs.close();
        }
        if (ofs.fail()) throw std::runtime_error("write error on " + storage_path);
    } catch (const std::exception& e) {
        if (ofs.is_open()) ofs.close();
        std::error_code ec;
        std::filesystem::remove(storage_path, ec);   // drop the partial file
        return Result<void>::err(std::string("Failed to stream file to storage: ") + e.what());
    }

    // Bookkeeping — identical to put().
    if (context->storage_tracker) {
        context->storage_tracker->record_file_creation(storage_path, original_size, tenant);
    }
    auto update_result = context->db->update_file_current_version(file_uid, version_timestamp, tenant);
    if (!update_result.success) return Result<void>::err("Failed to update current version: " + update_result.error);
    auto insert_version_result = context->db->insert_version(file_uid, version_timestamp,
                                                             static_cast<int64_t>(original_size), storage_path, user, tenant);
    if (!insert_version_result.success) return Result<void>::err("Failed to record version: " + insert_version_result.error);
    auto update_size_result = context->db->update_file_size(file_uid, static_cast<int64_t>(original_size), tenant);
    if (!update_size_result.success) {
        SERVER_LOG_ERROR("FileSystem::put_stream", "Failed to update file size for " + file_uid + ": " + update_size_result.error);
    }
    context->db->update_file_modified(file_uid, tenant);

    if (context->object_store) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            backup_queue_.push({file_uid, tenant, version_timestamp});
        }
        queue_cv_.notify_one();
    }
    emit_fs_event(tenant, FileEventType::FileUpdated, file_uid, user);
    return Result<void>::ok();
}

Result<void> FileSystem::get_stream(const std::string& file_uid,
                                    const std::function<bool(const uint8_t*, size_t)>& on_chunk,
                                    const std::string& user,
                                    const std::vector<std::string>& roles,
                                    const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<void>::err("Database not available for tenant: " + tenant);
    }
    auto perm_result = validate_user_permissions(file_uid, user, roles, static_cast<int>(Permission::READ), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<void>::err("User does not have permission to read file");
    }
    auto file_info_result = context->db->get_file_by_uid(file_uid, tenant);
    if (!file_info_result.success || !file_info_result.value.has_value()) {
        return Result<void>::err("File does not exist");
    }

    std::string current_version = file_info_result.value->version;
    if (current_version.empty()) {
        auto versions_result = list_versions(file_uid, user, roles, tenant);
        if (!versions_result.success || versions_result.value.empty()) {
            return Result<void>::err("No versions available for file");
        }
        current_version = versions_result.value[0];
    }

    std::string local_storage_path;
    auto path_result = context->db->get_version_storage_path(file_uid, current_version, tenant);
    if (path_result.success && path_result.value.has_value()) {
        local_storage_path = path_result.value.value();
    } else {
        local_storage_path = context->storage->get_storage_path(file_uid, current_version, tenant);
    }

    bool file_exists_locally = false;
    if (context->storage) {
        auto exists_result = context->storage->file_exists(local_storage_path, tenant);
        if (exists_result.success) file_exists_locally = exists_result.value;
    }

    // Cold path (not on local disk): reuse the whole-buffer get(), which handles
    // S3 restore + decrypt/decompress, and emit it as a single chunk. This keeps
    // the rare restore path simple; the common local path below streams.
    if (!file_exists_locally) {
        auto r = get(file_uid, user, roles, tenant);
        if (!r.success) return Result<void>::err(r.error);
        if (!r.value.empty()) on_chunk(r.value.data(), r.value.size());
        return Result<void>::ok();
    }

    const bool do_compress = context->storage->is_compression_enabled();
    const bool do_encrypt = context->storage->is_encryption_enabled();
    std::string encryption_key;
    if (do_encrypt) {
        encryption_key = context->config.encryption_key;
        if (encryption_key.empty()) return Result<void>::err("Encryption key not available");
    }

    std::ifstream ifs(local_storage_path, std::ios::binary);
    if (!ifs.is_open()) {
        return Result<void>::err("Failed to open file for reading: " + local_storage_path);
    }

    try {
        std::unique_ptr<DecryptStream> decryptor;
        std::unique_ptr<DecompressStream> decompressor;
        if (do_encrypt) decryptor = std::make_unique<DecryptStream>(encryption_key);
        if (do_compress) decompressor = std::make_unique<DecompressStream>();

        std::vector<char> buf(256 * 1024);
        std::vector<uint8_t> dbuf, ddbuf;
        bool aborted = false;
        // Emit decrypted+decompressed plaintext to the caller.
        auto emit = [&](const uint8_t* p, size_t n) {
            if (n == 0 || aborted) return;
            if (do_compress) {
                decompressor->update(p, n, ddbuf);
                if (!ddbuf.empty() && !on_chunk(ddbuf.data(), ddbuf.size())) aborted = true;
            } else {
                if (!on_chunk(p, n)) aborted = true;
            }
        };
        // disk bytes -> decrypt -> emit
        auto consume = [&](const uint8_t* p, size_t n) {
            if (do_encrypt) { decryptor->update(p, n, dbuf); emit(dbuf.data(), dbuf.size()); }
            else emit(p, n);
        };

        while (!aborted) {
            ifs.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            std::streamsize n = ifs.gcount();
            if (n <= 0) break;
            consume(reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(n));
        }
        if (!aborted) {
            if (do_encrypt) {
                decryptor->finish(dbuf);     // verifies the GCM tag (throws on mismatch)
                if (!dbuf.empty()) emit(dbuf.data(), dbuf.size());
            }
            if (do_compress) {
                decompressor->finish(ddbuf);
                if (!ddbuf.empty()) on_chunk(ddbuf.data(), ddbuf.size());
            }
        }
    } catch (const std::exception& e) {
        return Result<void>::err(std::string("Failed to stream file from storage: ") + e.what());
    }
    return Result<void>::ok();
}

Result<FileInfo> FileSystem::stat(const std::string& file_uid, const std::string& user,
                                  const std::vector<std::string>& roles,
                                  const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<FileInfo>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs read permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, roles, static_cast<int>(Permission::READ), tenant);
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

namespace {
// If `desired` already names a child of `parent_uid`, append " (n)" before the
// extension to make it unique: "report.pdf" -> "report (1).pdf". Used so copy
// and move never create two siblings with the same name.
std::string unique_child_name(const std::shared_ptr<IDatabase>& db,
                              const std::string& parent_uid,
                              const std::string& desired,
                              const std::string& tenant) {
    auto existing = db->list_files_in_directory(parent_uid, tenant);
    if (!existing.success) return desired;
    const auto& files = existing.value;
    auto taken = [&](const std::string& n) {
        return std::any_of(files.begin(), files.end(),
                           [&](const FileInfo& f) { return f.name == n; });
    };
    if (!taken(desired)) return desired;
    const std::size_t dot = desired.rfind('.');
    const std::string base = (dot != std::string::npos && dot != 0) ? desired.substr(0, dot) : desired;
    const std::string ext  = (dot != std::string::npos && dot != 0) ? desired.substr(dot)    : "";
    for (int n = 1; n < 100000; ++n) {
        std::string cand = base + " (" + std::to_string(n) + ")" + ext;
        if (!taken(cand)) return cand;
    }
    return desired;
}
}  // namespace

Result<void> FileSystem::move(const std::string& src_uid, const std::string& dst_uid,
                              const std::string& user,
                              const std::vector<std::string>& roles,
                              const std::string& tenant) {
    std::optional<AclManager::CacheScope> cache_scope;
    if (acl_manager_) cache_scope.emplace(*acl_manager_);

    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<void>::err("Database not available for tenant: " + tenant);
    }

    // Check permissions - the user needs write permission on both source and destination
    auto src_perm_result = validate_user_permissions(src_uid, user, roles, static_cast<int>(Permission::WRITE), tenant);
    if (!src_perm_result.success || !src_perm_result.value) {
        return Result<void>::err("User does not have permission to move source file");
    }

    // For move operation, dst_uid represents the new parent directory
    auto dst_perm_result = validate_user_permissions(dst_uid, user, roles, static_cast<int>(Permission::WRITE), tenant);
    if (!dst_perm_result.success || !dst_perm_result.value) {
        return Result<void>::err("User does not have permission to move to destination directory");
    }

    // Get the source file info to check if it exists and to get its type
    auto src_info_result = context->db->get_file_by_uid(src_uid, tenant);
    if (!src_info_result.success || !src_info_result.value.has_value()) {
        return Result<void>::err("Source file does not exist");
    }

    // Get the destination directory info to ensure it's a directory
    auto dst_info_result = context->db->get_file_by_uid(dst_uid, tenant);
    if (!dst_info_result.success || !dst_info_result.value.has_value()) {
        return Result<void>::err("Destination directory does not exist");
    }

    if (dst_info_result.value->type != FileType::DIRECTORY) {
        return Result<void>::err("Destination is not a directory");
    }

    // Reject moving a directory into itself or its own subtree, which would
    // splice an unreachable cycle into the tree and break later traversals.
    if (src_info_result.value->type == FileType::DIRECTORY &&
        is_within_subtree(context->db, dst_uid, src_uid, tenant)) {
        return Result<void>::err("Cannot move a directory into itself or its own subtree");
    }

    // Compute a collision-free name in the destination BEFORE re-parenting (so
    // the node isn't yet a child of dst and can't collide with itself).
    const std::string move_name =
        unique_child_name(context->db, dst_uid, src_info_result.value->name, tenant);

    // Update the parent_uid in the database to move the file/directory
    auto db_result = context->db->update_file_parent(src_uid, dst_uid, tenant);
    if (!db_result.success) {
        return Result<void>::err("Failed to move file in database: " + db_result.error);
    }

    // De-duplicate the name on collision (best-effort: the move already succeeded).
    if (move_name != src_info_result.value->name) {
        auto rn = context->db->update_file_name(src_uid, move_name, tenant);
        if (!rn.success) {
            SERVER_LOG_ERROR("FileSystem::move", ServerLogger::getInstance().detailed_log_prefix() +
                      "Moved but failed to de-duplicate name for " + src_uid + ": " + rn.error);
        }
    }

    emit_fs_event(tenant, FileEventType::FileMoved, src_uid, user);
    return Result<void>::ok();
}

Result<void> FileSystem::copy(const std::string& src_uid, const std::string& dst_uid,
                              const std::string& user,
                              const std::vector<std::string>& roles,
                              const std::string& tenant) {
    // copy is the heaviest beneficiary of the cache — recursive directory
    // copies re-validate the same ancestors as they walk the tree.
    std::optional<AclManager::CacheScope> cache_scope;
    if (acl_manager_) cache_scope.emplace(*acl_manager_);

    auto context = get_tenant_context(tenant);
    if (!context || !context->db || !context->storage) {
        return Result<void>::err("Database or storage not available for tenant: " + tenant);
    }

    // Check permissions - the user needs read permission on source and write permission on destination
    auto src_perm_result = validate_user_permissions(src_uid, user, roles, static_cast<int>(Permission::READ), tenant);
    if (!src_perm_result.success || !src_perm_result.value) {
        return Result<void>::err("User does not have permission to read source file");
    }

    // For copy operation, dst_uid represents the new parent directory
    auto dst_perm_result = validate_user_permissions(dst_uid, user, roles, static_cast<int>(Permission::WRITE), tenant);
    if (!dst_perm_result.success || !dst_perm_result.value) {
        return Result<void>::err("User does not have permission to write to destination directory");
    }

    // Get the source file info to check if it exists and to get its type
    auto src_info_result = context->db->get_file_by_uid(src_uid, tenant);
    if (!src_info_result.success || !src_info_result.value.has_value()) {
        return Result<void>::err("Source file does not exist");
    }

    auto src_info = src_info_result.value.value();

    // Get the destination directory info to ensure it's a directory
    auto dst_info_result = context->db->get_file_by_uid(dst_uid, tenant);
    if (!dst_info_result.success || !dst_info_result.value.has_value()) {
        return Result<void>::err("Destination directory does not exist");
    }

    if (dst_info_result.value->type != FileType::DIRECTORY) {
        return Result<void>::err("Destination is not a directory");
    }

    // Reject copying a directory into itself or its own subtree. This would
    // recurse without bound (each created child reappears in the next listing)
    // and crash the server, so guard before doing any work.
    if (src_info.type == FileType::DIRECTORY &&
        is_within_subtree(context->db, dst_uid, src_uid, tenant)) {
        return Result<void>::err("Cannot copy a directory into itself or its own subtree");
    }

    // Generate a new UID + a collision-free name for the copy (so copying into a
    // folder that already has "x.txt" yields "x (1).txt", not a duplicate).
    std::string new_uid = Utils::generate_uuid();
    std::string copy_name = unique_child_name(context->db, dst_uid, src_info.name, tenant);
    std::string new_path = dst_uid + "/" + copy_name;

    // If it's a directory, we need to recursively copy all contents
    if (src_info.type == FileType::DIRECTORY) {
        // Atomic creation: directory row + default/inherited ACLs from dst.
        auto grants = compute_initial_acl_grants(dst_uid, user, tenant);
        auto db_result = context->db->create_file_with_acls(new_uid, copy_name, new_path, dst_uid,
                                                           FileType::DIRECTORY, user, src_info.permissions,
                                                           grants, tenant);
        if (!db_result.success) {
            return Result<void>::err("Failed to create directory in database: " + db_result.error);
        }

        // List all entries in the source directory
        auto list_result = listdir(src_uid, user, roles, tenant);
        if (list_result.success) {
            // Recursively copy each entry in the source directory
            for (const auto& entry : list_result.value) {
                Result<void> copy_result = copy(entry.uid, new_uid, user, roles, tenant);
                if (!copy_result.success) {
                    return Result<void>::err("Failed to copy directory contents: " + copy_result.error);
                }
            }
        }

        emit_fs_event(tenant, FileEventType::DirCreated, new_uid, user);
        return Result<void>::ok();
    } else {
        // It's a regular file — copy the full version history so the copy is a
        // faithful duplicate (history preserved). list_versions() returns
        // NEWEST-first, so the latest is front(); the previous code used back()
        // (the OLDEST version), which copied stale/empty content — e.g. the empty
        // initial version of a touch-then-write file — and dropped history.
        auto versions_result = list_versions(src_uid, user, roles, tenant);
        if (!versions_result.success || versions_result.value.empty()) {
            return Result<void>::err("No versions available for source file");
        }
        const auto& versions = versions_result.value;        // newest-first
        const std::string current_version = versions.front();

        // Create the new file row first (atomic with default/inherited ACLs).
        auto grants = compute_initial_acl_grants(dst_uid, user, tenant);
        auto db_result = context->db->create_file_with_acls(new_uid, copy_name, new_path, dst_uid,
                                                           FileType::REGULAR_FILE, user, src_info.permissions,
                                                           grants, tenant);
        if (!db_result.success) {
            return Result<void>::err("Failed to create file in database: " + db_result.error);
        }

        // Copy every version's content (oldest -> newest) under the new UID.
        int64_t current_size = 0;
        for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
            const std::string& ver = *it;
            std::string src_storage_path = context->storage->get_storage_path(src_uid, ver, tenant);
            auto storage_result = context->storage->read_file(src_storage_path, tenant);
            if (!storage_result.success) {
                return Result<void>::err("Failed to read source version " + ver + ": " + storage_result.error);
            }
            auto store_result = context->storage->store_file(new_uid, ver, storage_result.value, tenant);
            if (!store_result.success) {
                return Result<void>::err("Failed to store copied version " + ver + ": " + store_result.error);
            }
            if (context->storage_tracker) {
                context->storage_tracker->record_file_creation(store_result.value, storage_result.value.size(), tenant);
            }
            auto insert_version_result = context->db->insert_version(new_uid, ver, storage_result.value.size(),
                                                                     store_result.value, user, tenant);
            if (!insert_version_result.success) {
                return Result<void>::err("Failed to record version " + ver + ": " + insert_version_result.error);
            }
            if (ver == current_version) current_size = static_cast<int64_t>(storage_result.value.size());
            // Schedule an object-store backup for this version.
            if (context->object_store) {
                {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    backup_queue_.push({new_uid, tenant, ver});
                }
                queue_cv_.notify_one(); // Wake up the backup worker thread
            }
        }

        // Point the new file at the newest version + mirror its size.
        auto update_result = context->db->update_file_current_version(new_uid, current_version, tenant);
        if (!update_result.success) {
            return Result<void>::err("Failed to update current version: " + update_result.error);
        }
        auto copy_size_result = context->db->update_file_size(new_uid, current_size, tenant);
        if (!copy_size_result.success) {
            SERVER_LOG_ERROR("FileSystem::copy", ServerLogger::getInstance().detailed_log_prefix() +
                      "Failed to update file size for " + new_uid + ": " + copy_size_result.error);
        }

        // Update the modification time
        auto update_time_result = context->db->update_file_modified(new_uid, tenant);
        if (!update_time_result.success) {
            // Log error but don't fail the operation
        }

        // Deep-copy hidden renditions: the source file's children become children
        // of the new copy (one level; names are preserved per convention). copy()
        // rejects a file as a destination parent, so copy each rendition's content
        // and row directly here. Best-effort: a rendition failure doesn't fail the
        // file copy (renditions are regenerable by the re-rendition service).
        auto rend_list = context->db->list_files_in_directory(src_uid, tenant);
        if (rend_list.success) {
            for (const auto& rend : rend_list.value) {
                if (rend.type == FileType::DIRECTORY) continue;  // one level: files only
                auto rv = list_versions(rend.uid, user, roles, tenant);
                if (!rv.success || rv.value.empty()) {
                    SERVER_LOG_ERROR("FileSystem::copy", ServerLogger::getInstance().detailed_log_prefix() +
                              "Skipping rendition with no version: " + rend.uid);
                    continue;
                }
                std::string r_ver = rv.value.back();
                auto r_read = context->storage->read_file(
                    context->storage->get_storage_path(rend.uid, r_ver, tenant), tenant);
                if (!r_read.success) continue;
                std::string r_new_uid = Utils::generate_uuid();
                auto r_store = context->storage->store_file(r_new_uid, r_ver, r_read.value, tenant);
                if (!r_store.success) continue;
                if (context->storage_tracker) {
                    context->storage_tracker->record_file_creation(r_store.value, r_read.value.size(), tenant);
                }
                auto r_grants = compute_initial_acl_grants(new_uid, user, tenant);
                auto r_db = context->db->create_file_with_acls(
                    r_new_uid, rend.name, new_uid + "/" + rend.name, new_uid,
                    FileType::REGULAR_FILE, user, rend.permissions, r_grants, tenant);
                if (!r_db.success) {
                    SERVER_LOG_ERROR("FileSystem::copy", ServerLogger::getInstance().detailed_log_prefix() +
                              "Failed to copy rendition " + rend.name + ": " + r_db.error);
                    continue;
                }
                context->db->update_file_current_version(r_new_uid, r_ver, tenant);
                context->db->insert_version(r_new_uid, r_ver, r_read.value.size(), r_store.value, user, tenant);
                context->db->update_file_size(r_new_uid, static_cast<int64_t>(r_read.value.size()), tenant);
                context->db->update_file_modified(r_new_uid, tenant);
            }
        }

        emit_fs_event(tenant, FileEventType::FileCreated, new_uid, user);
        return Result<void>::ok();
    }
}

Result<void> FileSystem::rename(const std::string& uid, const std::string& new_name,
                                const std::string& user,
                                const std::vector<std::string>& roles,
                                const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<void>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs write permission on the file
    auto perm_result = validate_user_permissions(uid, user, roles, static_cast<int>(Permission::WRITE), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<void>::err("User does not have permission to rename file");
    }
    
    auto db_result = context->db->update_file_name(uid, new_name, tenant);
    if (!db_result.success) {
        return Result<void>::err("Failed to rename file: " + db_result.error);
    }

    emit_fs_event(tenant, FileEventType::FileRenamed, uid, user);
    return Result<void>::ok();
}

Result<std::vector<std::string>> FileSystem::list_versions(const std::string& file_uid,
                                                           const std::string& user,
                                                           const std::vector<std::string>& roles,
                                                           const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::vector<std::string>>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs read permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, roles, static_cast<int>(Permission::READ), tenant);
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
                                                     const std::vector<std::string>& roles,
                                                     const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::vector<uint8_t>>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs read permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, roles, static_cast<int>(Permission::READ), tenant);
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
            // Process data after reading (decrypt and decompress if needed)
            std::vector<uint8_t> processed_data = storage_result.value;

            // Check if encryption is enabled for this tenant/context
            if (context->storage && context->storage->is_encryption_enabled()) {
                try {
                    // Get the encryption key from the config
                    std::string encryption_key = context->config.encryption_key;
                    if (encryption_key.empty()) {
                        return Result<std::vector<uint8_t>>::err("Encryption key not available");
                    }

                    processed_data = fileengine::CryptoUtils::decrypt_data(processed_data, encryption_key);
                    SERVER_LOG_DEBUG("FileSystem::get_version", "Version data decrypted successfully");
                } catch (const std::exception& e) {
                    SERVER_LOG_ERROR("FileSystem::get_version", "Decryption failed: " + std::string(e.what()));
                    return Result<std::vector<uint8_t>>::err("Failed to decrypt version data: " + std::string(e.what()));
                }
            }

            // Check if compression is enabled for this tenant/context
            if (context->storage && context->storage->is_compression_enabled()) {
                try {
                    processed_data = fileengine::CryptoUtils::decompress_data(processed_data);
                    SERVER_LOG_DEBUG("FileSystem::get_version", "Version data decompressed successfully");
                } catch (const std::exception& e) {
                    SERVER_LOG_ERROR("FileSystem::get_version", "Decompression failed: " + std::string(e.what()));
                    return Result<std::vector<uint8_t>>::err("Failed to decompress version data: " + std::string(e.what()));
                }
            }

            // Add to cache if available
            if (cache_manager_) {
                cache_manager_->add_file(storage_path, processed_data, tenant);
            }
            return Result<std::vector<uint8_t>>::ok(processed_data);
        }
    }
    
    return Result<std::vector<uint8_t>>::err("Version content not found");
}

Result<bool> FileSystem::restore_to_version(const std::string& file_uid,
                                           const std::string& version_timestamp,
                                           const std::string& user,
                                           const std::vector<std::string>& roles,
                                           const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<bool>::err("Database not available for tenant: " + tenant);
    }

    // Check if user has special permission to restore to version
    // Typically requires WRITE permission or special version management permission
    auto perm_result = validate_user_permissions(file_uid, user, roles, static_cast<int>(fileengine::Permission::WRITE), tenant); // WRITE permission
    if (!perm_result.success || !perm_result.value) {
        return Result<bool>::err("User does not have permission to restore to version");
    }

    auto result = context->db->restore_to_version(file_uid, version_timestamp, user, tenant);
    if (result.success && result.value) {
        emit_fs_event(tenant, FileEventType::FileUpdated, file_uid, user);
    }
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

    // Stream the local file to the object store (multipart for large files), so
    // the backup never holds the whole payload in memory.
    auto obj_store_result = context->object_store->store_file_from_path(file_uid, version_timestamp,
                                                                        local_storage_path, tenant);
    if (!obj_store_result.success) {
        return Result<void>::err("Failed to store file in object store: " + obj_store_result.error);
    }

    return Result<void>::ok();
}

Result<void> FileSystem::purge_old_versions(const std::string& file_uid, int keep_count,
                                            const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<void>::err("Database not available for tenant: " + tenant);
    }

    // Always retain at least the current (newest) version so the file stays
    // readable, regardless of the requested keep_count.
    int keep = keep_count > 1 ? keep_count : 1;

    // list_versions returns newest-first.
    auto versions_result = context->db->list_versions(file_uid, tenant);
    if (!versions_result.success) {
        return Result<void>::err("Failed to list versions: " + versions_result.error);
    }
    const auto& versions = versions_result.value;
    if (static_cast<int>(versions.size()) <= keep) {
        return Result<void>::ok();  // nothing old enough to purge
    }

    // Drop everything older than the newest `keep` versions. The local storage
    // payload is removed best-effort; object-store copies are immutable by
    // design and are intentionally left untouched.
    for (size_t i = keep; i < versions.size(); ++i) {
        const std::string& vts = versions[i];
        if (context->storage) {
            std::string path = context->storage->get_storage_path(file_uid, vts, tenant);
            context->storage->delete_file(path, tenant);  // best-effort
        }
        auto del = context->db->delete_version(file_uid, vts, tenant);
        if (!del.success) {
            return Result<void>::err("Failed to purge version " + vts + ": " + del.error);
        }
    }

    return Result<void>::ok();
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
                                      const std::vector<std::string>& roles,
                                      const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<void>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs write permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, roles, static_cast<int>(Permission::WRITE), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<void>::err("User does not have permission to set metadata");
    }
    
    std::string version_timestamp = "current"; // Would use actual current version in practice
    auto db_result = context->db->set_metadata(file_uid, version_timestamp, key, value, tenant);
    return db_result;
}

Result<std::string> FileSystem::get_metadata(const std::string& file_uid, const std::string& key,
                                             const std::string& user,
                                             const std::vector<std::string>& roles,
                                             const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::string>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs read permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, roles, static_cast<int>(Permission::READ), tenant);
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
                                                                        const std::vector<std::string>& roles,
                                                                        const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::map<std::string, std::string>>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs read permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, roles, static_cast<int>(Permission::READ), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<std::map<std::string, std::string>>::err("User does not have permission to get metadata");
    }
    
    std::string version_timestamp = "current"; // Would use actual current version in practice
    auto db_result = context->db->get_all_metadata(file_uid, version_timestamp, tenant);
    return db_result;
}

Result<void> FileSystem::delete_metadata(const std::string& file_uid, const std::string& key,
                                         const std::string& user,
                                         const std::vector<std::string>& roles,
                                         const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<void>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs write permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, roles, static_cast<int>(Permission::WRITE), tenant);
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
                                                         const std::vector<std::string>& roles,
                                                         const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::string>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs read permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, roles, static_cast<int>(Permission::READ), tenant);
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
                                                                                    const std::vector<std::string>& roles,
                                                                                    const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<std::map<std::string, std::string>>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs read permission on the file
    auto perm_result = validate_user_permissions(file_uid, user, roles, static_cast<int>(Permission::READ), tenant);
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
                                          const std::vector<std::string>& roles,
                                          const std::string& tenant) {
    if (!acl_manager_) {
        return Result<void>::err("ACL manager not available");
    }
    
    // Grant requires MANAGE_ACL on the resource — see plan §4.3 / Phase 5.
    auto perm_result = validate_user_permissions(resource_uid, user, roles, static_cast<int>(Permission::MANAGE_ACL), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<void>::err("User does not have permission to grant permissions");
    }

    auto result = acl_manager_->grant_permission(resource_uid, principal, PrincipalType::USER,
                                                 permissions, tenant, user);
    if (result.success) {
        emit_acl_event(tenant, resource_uid, principal, permissions, user);
    }
    return result;
}

Result<void> FileSystem::revoke_permission(const std::string& resource_uid,
                                           const std::string& principal,
                                           int permissions,
                                           const std::string& user,
                                           const std::vector<std::string>& roles,
                                           const std::string& tenant) {
    if (!acl_manager_) {
        return Result<void>::err("ACL manager not available");
    }

    // Revoke requires MANAGE_ACL on the resource — see plan §4.3 / Phase 5.
    auto perm_result = validate_user_permissions(resource_uid, user, roles, static_cast<int>(Permission::MANAGE_ACL), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<void>::err("User does not have permission to revoke permissions");
    }
    
    auto result = acl_manager_->revoke_permission(resource_uid, principal, PrincipalType::USER,
                                                  permissions, tenant, user);
    if (result.success) {
        emit_acl_event(tenant, resource_uid, principal, permissions, user);
    }
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

    // Drain and stop the event sink (joins its worker). Safe if unset.
    if (event_sink_) {
        event_sink_->stop();
    }

    // Cleanup operations
    if (cache_manager_) {
        cache_manager_->cleanup_cache();
    }
}

void FileSystem::emit_fs_event(const std::string& tenant, FileEventType type,
                               const std::string& uid, const std::string& user) noexcept {
    if (!event_sink_) return;  // events disabled — cheap no-op, no DB work
    try {
        FileEvent ev;
        ev.event_id = Utils::generate_uuid();
        ev.type = type;
        ev.tenant = tenant.empty() ? "default" : tenant;
        ev.file_uid = uid;
        ev.actor = user;
        ev.ts = Utils::get_timestamp_string();

        // Best-effort enrichment. include_deleted so delete/rmdir events still
        // resolve metadata for the row that was just soft-deleted.
        auto context = get_tenant_context(tenant);
        if (context && context->db) {
            auto info = context->db->get_file_by_uid_include_deleted(uid, tenant);
            if (info.success && info.value.has_value()) {
                const auto& fi = info.value.value();
                ev.name = fi.name;
                ev.parent_uid = fi.parent_uid;
                ev.path = fi.path;
                ev.size = fi.size;
                ev.version = fi.version;
                ev.is_folder = (fi.type == FileType::DIRECTORY);
            }
            // A rendition is a hidden child of a *file*: detect via parent type
            // so consumers can ignore the conversion service's own output.
            if (!ev.parent_uid.empty()) {
                auto parent = context->db->get_file_by_uid_include_deleted(ev.parent_uid, tenant);
                if (parent.success && parent.value.has_value()) {
                    ev.is_rendition = (parent.value.value().type == FileType::REGULAR_FILE);
                }
            }
        }
        event_sink_->publish(ev);
    } catch (...) {
        // fail-open: event emission must never disturb the filesystem operation
    }
}

void FileSystem::emit_acl_event(const std::string& tenant, const std::string& resource_uid,
                                const std::string& principal, int permissions,
                                const std::string& user) noexcept {
    if (!event_sink_) return;
    try {
        FileEvent ev;
        ev.event_id = Utils::generate_uuid();
        ev.type = FileEventType::AclChanged;
        ev.tenant = tenant.empty() ? "default" : tenant;
        ev.file_uid = resource_uid;
        ev.principal = principal;
        ev.permissions = permissions;
        ev.actor = user;
        ev.ts = Utils::get_timestamp_string();

        auto context = get_tenant_context(tenant);
        if (context && context->db) {
            auto info = context->db->get_file_by_uid_include_deleted(resource_uid, tenant);
            if (info.success && info.value.has_value()) {
                const auto& fi = info.value.value();
                ev.name = fi.name;
                ev.parent_uid = fi.parent_uid;
                ev.path = fi.path;
                ev.is_folder = (fi.type == FileType::DIRECTORY);
            }
        }
        event_sink_->publish(ev);
    } catch (...) {
        // fail-open
    }
}

void FileSystem::emit_role_event(const std::string& tenant, FileEventType type,
                                 const std::string& role, const std::string& member,
                                 const std::string& user) noexcept {
    if (!event_sink_) return;
    try {
        FileEvent ev;
        ev.event_id = Utils::generate_uuid();
        ev.type = type;
        ev.tenant = tenant.empty() ? "default" : tenant;
        ev.role = role;
        ev.member = member;
        ev.actor = user;
        ev.ts = Utils::get_timestamp_string();
        event_sink_->publish(ev);
    } catch (...) {
        // fail-open
    }
}

void FileSystem::apply_acls_for_new_resource(const std::string& parent_uid,
                                             const std::string& new_uid,
                                             const std::string& user,
                                             const std::string& tenant) {
    if (!acl_manager_) {
        return;
    }
    // Creator always gets default USER bits (READ|WRITE|EXECUTE|MANAGE_ACL|
    // ACL_INHERIT) so they can manage what they just created — regardless of
    // what cascades down from the parent.
    auto default_result = acl_manager_->apply_default_acls(new_uid, user, tenant);
    if (!default_result.success) {
        SERVER_LOG_WARN("FileSystem::apply_acls_for_new_resource",
                        "Failed to apply default ACLs for " + new_uid + ": " + default_result.error);
    }

    // Inherit parent rules marked ACL_INHERIT on top. Empty parent_uid (the
    // filesystem root) skips inheritance.
    if (!parent_uid.empty() && acl_manager_->parent_has_inheritable_acls(parent_uid, tenant)) {
        auto inherit_result = acl_manager_->inherit_acls(parent_uid, new_uid, tenant, user);
        if (!inherit_result.success) {
            SERVER_LOG_WARN("FileSystem::apply_acls_for_new_resource",
                            "Inheritance failed for " + new_uid + " from parent " + parent_uid
                            + ": " + inherit_result.error);
        }
    }
}

std::vector<IDatabase::AclGrant> FileSystem::compute_initial_acl_grants(
        const std::string& parent_uid,
        const std::string& creator,
        const std::string& tenant) {
    std::vector<IDatabase::AclGrant> grants;
    if (!acl_manager_) {
        return grants;
    }

    // Creator's default USER bits. Mirrors apply_default_acls semantics.
    IDatabase::AclGrant creator_grant;
    creator_grant.principal = creator;
    creator_grant.type = static_cast<int>(PrincipalType::USER);
    creator_grant.permissions =
        static_cast<int>(Permission::READ)
        | static_cast<int>(Permission::WRITE)
        | static_cast<int>(Permission::EXECUTE)
        | static_cast<int>(Permission::MANAGE_ACL)
        | static_cast<int>(Permission::ACL_INHERIT);
    creator_grant.performed_by = creator;
    creator_grant.effect = static_cast<int>(AclEffect::ALLOW);
    grants.push_back(creator_grant);

    // Optional world-readable default.
    if (acl_manager_->default_world_readable()) {
        IDatabase::AclGrant other_grant;
        other_grant.principal = "other";
        other_grant.type = static_cast<int>(PrincipalType::OTHER);
        other_grant.permissions = static_cast<int>(Permission::READ);
        other_grant.performed_by = creator;
        other_grant.effect = static_cast<int>(AclEffect::ALLOW);
        grants.push_back(other_grant);
    }

    // Inheritable parent rules.
    if (!parent_uid.empty()) {
        auto parent_acls = acl_manager_->get_acls_for_resource(parent_uid, tenant);
        if (parent_acls.success) {
            const int inherit_bit = static_cast<int>(Permission::ACL_INHERIT);
            for (const auto& rule : parent_acls.value) {
                if ((rule.permissions & inherit_bit) == 0) continue;
                IDatabase::AclGrant g;
                g.principal = rule.principal;
                g.type = static_cast<int>(rule.type);
                g.permissions = rule.permissions;
                g.performed_by = creator;
                g.effect = static_cast<int>(rule.effect);
                grants.push_back(g);
            }
        }
    }

    return grants;
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

    // Special rule: The filesystem root (empty UID) is always readable by all users
    // This allows users to list the root directory contents regardless of specific ACLs
    if (resource_uid.empty() && (required_permissions & static_cast<int>(Permission::READ))) {
        SERVER_LOG_DEBUG("FileSystem::validate_user_permissions",
                         ServerLogger::getInstance().detailed_log_prefix() +
                         "Allowing READ access to filesystem root for user: " + user);
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