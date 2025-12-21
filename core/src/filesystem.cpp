#include "fileengine/filesystem.h"
#include "fileengine/utils.h"
#include <algorithm>

namespace fileengine {

FileSystem::FileSystem(std::shared_ptr<TenantManager> tenant_manager) 
    : tenant_manager_(tenant_manager) {
}

FileSystem::~FileSystem() {
    shutdown();
}

Result<std::string> FileSystem::mkdir(const std::string& parent_uid, const std::string& name, 
                                      const std::string& user, int permissions, 
                                      const std::string& tenant) {
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
            return Result<std::string>::err("User does not have permission to create directory");
        }
    }
    
    std::string new_uid = Utils::generate_uuid();
    std::string path = parent_uid.empty() ? "/" + name : parent_uid + "/" + name;
    
    // Insert directory in database
    auto db_result = context->db->insert_file(new_uid, name, path, parent_uid, 
                                              FileType::DIRECTORY, user, permissions, tenant);
    if (!db_result.success) {
        return Result<std::string>::err("Failed to create directory in database: " + db_result.error);
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

Result<void> FileSystem::rmdir(const std::string& dir_uid, const std::string& user, 
                               const std::string& tenant) {
    auto context = get_tenant_context(tenant);
    if (!context || !context->db) {
        return Result<void>::err("Database not available for tenant: " + tenant);
    }
    
    // Check permissions - the user needs write permission on the directory
    auto perm_result = validate_user_permissions(dir_uid, user, {}, static_cast<int>(Permission::WRITE), tenant);
    if (!perm_result.success || !perm_result.value) {
        return Result<void>::err("User does not have permission to remove directory");
    }
    
    // First, check if directory is empty
    auto list_result = listdir(dir_uid, user, tenant);
    if (list_result.success && !list_result.value.empty()) {
        return Result<void>::err("Directory is not empty");
    }
    
    // Mark directory as deleted in database
    auto db_result = context->db->delete_file(dir_uid, tenant);
    if (!db_result.success) {
        return Result<void>::err("Failed to remove directory from database: " + db_result.error);
    }
    
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
    if (!parent_uid.empty()) {
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
    
    // If we have an object store, consider backing up asynchronously
    if (context->object_store) {
        // In a real implementation, this would be done asynchronously
        // backup_to_object_store(file_uid, tenant);
    }
    
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
    
    // Get the file info
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
    
    // Get the storage path for this version
    auto path_result = context->db->get_version_storage_path(file_uid, current_version, tenant);
    if (!path_result.success || !path_result.value.has_value()) {
        // If not in local storage, try to fetch from object store
        if (context->object_store) {
            auto fetch_result = fetch_from_object_store_if_missing(file_uid, current_version, tenant);
            if (fetch_result.success) {
                return fetch_result;
            }
        }
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
    
    // If not in storage, try object store
    if (context->object_store) {
        auto obj_store_path = context->object_store->get_storage_path(file_uid, current_version, tenant);
        auto object_store_result = context->object_store->read_file(obj_store_path, tenant);
        if (object_store_result.success) {
            // Add to local storage and cache
            if (context->storage) {
                auto store_result = context->storage->store_file(file_uid, current_version, 
                                                                 object_store_result.value, tenant);
                if (store_result.success && cache_manager_) {
                    cache_manager_->add_file(store_result.value, object_store_result.value, tenant);
                }
            }
            return object_store_result;
        }
    }
    
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
    if (!context || !context->db || !context->storage || !context->object_store) {
        return Result<void>::err("Database, storage or object store not available for tenant: " + tenant);
    }
    
    // Get the file info
    auto file_info_result = context->db->get_file_by_uid(file_uid, tenant);
    if (!file_info_result.success || !file_info_result.value.has_value()) {
        return Result<void>::err("File does not exist");
    }
    
    // Get the current version
    std::string current_version = file_info_result.value->version;
    if (current_version.empty()) {
        // If no version is set, get the latest version
        auto versions_result = list_versions(file_uid, file_info_result.value->owner, tenant);
        if (!versions_result.success || versions_result.value.empty()) {
            return Result<void>::err("No versions available for file");
        }
        current_version = versions_result.value[0]; // Latest version
    }
    
    // Get the storage path for this version
    auto path_result = context->db->get_version_storage_path(file_uid, current_version, tenant);
    if (!path_result.success || !path_result.value.has_value()) {
        return Result<void>::err("Version storage path not found");
    }
    
    std::string storage_path = path_result.value.value();
    
    // Read the file from local storage
    auto storage_result = context->storage->read_file(storage_path, tenant);
    if (!storage_result.success) {
        return Result<void>::err("Failed to read file from storage: " + storage_result.error);
    }
    
    // Store in object store
    auto obj_store_result = context->object_store->store_file(file_uid, current_version, 
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
    if (!tenant_manager_) {
        return nullptr;
    }
    return tenant_manager_->get_tenant_context(tenant);
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

} // namespace fileengine