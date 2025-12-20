#pragma once

#include "types.h"
#include "IObjectStore.h"  // Include IObjectStore for type definition
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace fileengine {

class IStorage {
public:
    virtual ~IStorage() = default;

    // File storage operations (automatically compress and encrypt)
    virtual Result<std::string> store_file(const std::string& uid, const std::string& version_timestamp,
                                            const std::vector<uint8_t>& data, const std::string& tenant = "") = 0;
    virtual Result<std::vector<uint8_t>> read_file(const std::string& storage_path, const std::string& tenant = "") = 0;
    virtual Result<void> delete_file(const std::string& storage_path, const std::string& tenant = "") = 0;
    virtual Result<bool> file_exists(const std::string& storage_path, const std::string& tenant = "") = 0;

    // Get storage path for a file by UUID and timestamp
    virtual std::string get_storage_path(const std::string& uid, const std::string& version_timestamp, const std::string& tenant = "") const = 0;

    // Check if encryption is enabled
    virtual bool is_encryption_enabled() const = 0;

    // Tenant management operations
    virtual Result<void> create_tenant_directory(const std::string& tenant) = 0;
    virtual Result<bool> tenant_directory_exists(const std::string& tenant) = 0;
    virtual Result<void> cleanup_tenant_directory(const std::string& tenant) = 0;

    // Synchronization operations
    virtual Result<void> sync_to_object_store(std::function<void(const std::string&, const std::string&, int)> progress_callback = nullptr) = 0;
    virtual Result<std::vector<std::string>> get_local_file_paths(const std::string& tenant = "") const = 0;

    // Object store access for caching functionality
    virtual void set_object_store(IObjectStore* object_store) = 0;
    virtual IObjectStore* get_object_store() const = 0;
};

} // namespace fileengine