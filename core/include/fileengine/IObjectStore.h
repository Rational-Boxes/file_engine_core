#pragma once

#include "types.h"
#include <string>
#include <vector>
#include <memory>

namespace fileengine {

class IObjectStore {
public:
    virtual ~IObjectStore() = default;

    // Check if the object store is initialized
    virtual bool is_initialized() const = 0;

    // Initialize the object store
    virtual Result<void> initialize() = 0;

    // File storage operations
    virtual Result<std::string> store_file(const std::string& virtual_path, const std::string& version_timestamp,
                                           const std::vector<uint8_t>& data, const std::string& tenant = "") = 0;
    virtual Result<std::vector<uint8_t>> read_file(const std::string& storage_path, const std::string& tenant = "") = 0;
    virtual Result<void> delete_file(const std::string& storage_path, const std::string& tenant = "") = 0;
    virtual Result<bool> file_exists(const std::string& storage_path, const std::string& tenant = "") = 0;

    // Get storage path for a virtual file
    virtual std::string get_storage_path(const std::string& virtual_path, const std::string& version_timestamp, const std::string& tenant = "") const = 0;

    // Bucket operations
    virtual Result<void> create_bucket_if_not_exists(const std::string& tenant = "") = 0;
    virtual Result<bool> bucket_exists(const std::string& tenant = "") = 0;

    // Check if encryption is enabled
    virtual bool is_encryption_enabled() const = 0;

    // Tenant management operations
    virtual Result<void> create_tenant_bucket(const std::string& tenant) = 0;
    virtual Result<bool> tenant_bucket_exists(const std::string& tenant) = 0;
    virtual Result<void> cleanup_tenant_bucket(const std::string& tenant) = 0;
    virtual Result<void> clear_storage(const std::string& tenant = "") = 0;
};

} // namespace fileengine