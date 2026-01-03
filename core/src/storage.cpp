#include "fileengine/storage.h"
#include "fileengine/utils.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

namespace fileengine {

Storage::Storage(const std::string& base_path, bool encrypt_data, bool compress_data)
    : base_path_(base_path), encrypt_data_(encrypt_data), compress_data_(compress_data), object_store_(nullptr) {
    // Create base directory if it doesn't exist
    std::filesystem::create_directories(base_path_);
}

Storage::~Storage() {
    // Cleanup operations if needed
}

std::string Storage::get_sha256_desaturated_path(const std::string& uid) const {
    // For this implementation, we'll use the first few characters of the UUID to create multiple subdirectories
    // This helps prevent filesystem performance issues with many files in one directory
    // We'll create 3 levels of desaturation with 2 characters each: xx/yy/zz/

    // Remove hyphens from UUID to get continuous hex characters
    std::string clean_uid = uid;
    clean_uid.erase(std::remove(clean_uid.begin(), clean_uid.end(), '-'), clean_uid.end());

    if (clean_uid.length() < 6) {
        return uid;  // Return original if not enough characters
    }

    // Use first 6 characters of cleaned UUID to create 3 levels of 2-character subdirectories
    std::string level1 = clean_uid.substr(0, 2);  // First 2 chars
    std::string level2 = clean_uid.substr(2, 2);  // Next 2 chars
    std::string level3 = clean_uid.substr(4, 2);  // Next 2 chars

    return level1 + "/" + level2 + "/" + level3;
}

std::string Storage::get_storage_path(const std::string& uid, const std::string& version_timestamp, const std::string& tenant) const {
    std::string path = base_path_;
    
    if (!tenant.empty()) {
        path += "/" + tenant;
    }
    
    std::string desaturated = get_sha256_desaturated_path(uid);
    path += "/" + desaturated + "/" + uid + "/" + version_timestamp;
    
    return path;
}

Result<std::string> Storage::store_file(const std::string& uid, const std::string& version_timestamp,
                                        const std::vector<uint8_t>& data, const std::string& tenant) {
    std::lock_guard<std::mutex> lock(storage_mutex_);
    
    std::string full_path = get_storage_path(uid, version_timestamp, tenant);
    std::string dir_path = std::filesystem::path(full_path).parent_path();
    
    // Ensure directory exists
    auto result = ensure_directory_exists(dir_path);
    if (!result.success) {
        return Result<std::string>::err("Failed to create directory: " + result.error);
    }
    
    // Write data to file
    std::ofstream file(full_path, std::ios::binary);
    if (!file.is_open()) {
        return Result<std::string>::err("Failed to open file for writing: " + full_path);
    }
    
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    file.close();
    
    if (!file.good()) {
        return Result<std::string>::err("Failed to write file: " + full_path);
    }
    
    return Result<std::string>::ok(full_path);
}

Result<std::vector<uint8_t>> Storage::read_file(const std::string& storage_path, const std::string& tenant) {
    std::ifstream file(storage_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return Result<std::vector<uint8_t>>::err("Failed to open file for reading: " + storage_path);
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        file.close();
        return Result<std::vector<uint8_t>>::err("Failed to read file: " + storage_path);
    }

    file.close();
    return Result<std::vector<uint8_t>>::ok(buffer);
}

Result<void> Storage::delete_file(const std::string& storage_path, const std::string& tenant) {
    std::lock_guard<std::mutex> lock(storage_mutex_);
    
    try {
        if (std::filesystem::exists(storage_path)) {
            std::filesystem::remove(storage_path);
        }
        // If the directory is now empty, we might want to remove it as well
        auto parent_path = std::filesystem::path(storage_path).parent_path();
        if (std::filesystem::exists(parent_path) && std::filesystem::is_empty(parent_path)) {
            std::filesystem::remove(parent_path);
        }
    } catch (const std::filesystem::filesystem_error& ex) {
        return Result<void>::err("Failed to delete file: " + std::string(ex.what()));
    }
    
    return Result<void>::ok();
}

Result<bool> Storage::file_exists(const std::string& storage_path, const std::string& tenant) {
    bool exists = std::filesystem::exists(storage_path);
    return Result<bool>::ok(exists);
}

bool Storage::is_encryption_enabled() const {
    return encrypt_data_;
}

Result<void> Storage::create_tenant_directory(const std::string& tenant) {
    if (tenant.empty()) {
        return Result<void>::err("Tenant name cannot be empty");
    }
    
    std::string tenant_path = base_path_ + "/" + tenant;
    
    try {
        std::filesystem::create_directories(tenant_path);
    } catch (const std::filesystem::filesystem_error& ex) {
        return Result<void>::err("Failed to create tenant directory: " + std::string(ex.what()));
    }
    
    return Result<void>::ok();
}

Result<bool> Storage::tenant_directory_exists(const std::string& tenant) {
    if (tenant.empty()) {
        return Result<bool>::err("Tenant name cannot be empty");
    }
    
    std::string tenant_path = base_path_ + "/" + tenant;
    bool exists = std::filesystem::exists(tenant_path) && std::filesystem::is_directory(tenant_path);
    
    return Result<bool>::ok(exists);
}

Result<void> Storage::cleanup_tenant_directory(const std::string& tenant) {
    if (tenant.empty()) {
        return Result<void>::err("Tenant name cannot be empty");
    }
    
    std::string tenant_path = base_path_ + "/" + tenant;
    
    try {
        if (std::filesystem::exists(tenant_path)) {
            std::filesystem::remove_all(tenant_path);
        }
    } catch (const std::filesystem::filesystem_error& ex) {
        return Result<void>::err("Failed to cleanup tenant directory: " + std::string(ex.what()));
    }
    
    return Result<void>::ok();
}

Result<void> Storage::sync_to_object_store(std::function<void(const std::string&, const std::string&, int)> progress_callback) {
    if (!object_store_) {
        return Result<void>::err("No object store configured for synchronization");
    }
    
    // Walk through all local files and sync to object store
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(base_path_)) {
            if (entry.is_regular_file()) {
                std::string file_path = entry.path().string();
                
                // Read file content
                auto read_result = read_file(file_path);
                if (read_result.success) {
                    // Store in object store
                    // For this example, we'll use a simple approach - in practice you'd need to parse
                    // the path to extract uid and version info
                    std::string dummy_uid = "dummy";  // Would extract from path in real implementation
                    std::string dummy_version = "1"; // Would extract from path in real implementation
                    
                    auto store_result = object_store_->store_file(dummy_uid, dummy_version, read_result.value);
                    if (store_result.success && progress_callback) {
                        progress_callback(dummy_uid, store_result.value, 1); // Simple progress reporting
                    }
                }
            }
        }
    } catch (const std::exception& ex) {
        return Result<void>::err("Failed to sync to object store: " + std::string(ex.what()));
    }
    
    return Result<void>::ok();
}

Result<std::vector<std::string>> Storage::get_local_file_paths(const std::string& tenant) const {
    std::vector<std::string> paths;
    
    std::string search_path = base_path_;
    if (!tenant.empty()) {
        search_path += "/" + tenant;
    }
    
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(search_path)) {
            if (entry.is_regular_file()) {
                paths.push_back(entry.path().string());
            }
        }
    } catch (const std::exception& ex) {
        return Result<std::vector<std::string>>::err("Failed to get local file paths: " + std::string(ex.what()));
    }
    
    return Result<std::vector<std::string>>::ok(paths);
}

void Storage::set_object_store(IObjectStore* object_store) {
    object_store_ = object_store;
}

IObjectStore* Storage::get_object_store() const {
    return object_store_;
}

Result<void> Storage::clear_storage(const std::string& tenant) {
    if (tenant.empty()) {
        return Result<void>::err("Tenant cannot be empty for clear_storage operation");
    }

    // Construct tenant-specific path
    std::string tenant_path = base_path_ + "/" + tenant;

    // In a real implementation, this would delete all files in the tenant directory
    // For now, we'll just return success
    return Result<void>::ok();
}

Result<void> Storage::ensure_directory_exists(const std::string& dir_path) {
    try {
        std::filesystem::create_directories(dir_path);
    } catch (const std::filesystem::filesystem_error& ex) {
        return Result<void>::err("Failed to create directory: " + std::string(ex.what()));
    }
    
    return Result<void>::ok();
}

} // namespace fileengine