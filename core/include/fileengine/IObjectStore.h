#pragma once

#include "types.h"
#include <string>
#include <vector>
#include <memory>
#include <fstream>

namespace fileengine {

class IObjectStore {
public:
    virtual ~IObjectStore() = default;

    // Upload a file already present on local disk to the object store. The
    // default reads the whole file and delegates to store_file(); S3Storage
    // overrides this to stream the file via multipart upload so large files are
    // never fully buffered in memory. Returns the stored key/path.
    virtual Result<std::string> store_file_from_path(const std::string& virtual_path,
                                                     const std::string& version_timestamp,
                                                     const std::string& local_path,
                                                     const std::string& tenant = "") {
        std::ifstream f(local_path, std::ios::binary | std::ios::ate);
        if (!f.is_open()) return Result<std::string>::err("Cannot open local file: " + local_path);
        std::streamsize sz = f.tellg();
        f.seekg(0, std::ios::beg);
        std::vector<uint8_t> data(sz > 0 ? static_cast<size_t>(sz) : 0);
        if (sz > 0 && !f.read(reinterpret_cast<char*>(data.data()), sz)) {
            return Result<std::string>::err("Cannot read local file: " + local_path);
        }
        return store_file(virtual_path, version_timestamp, data, tenant);
    }

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