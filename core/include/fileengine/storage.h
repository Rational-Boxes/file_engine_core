// Copyright (C) 2026 James Hickman
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include "types.h"
#include "IStorage.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>

namespace fileengine {

class IObjectStore;

class Storage : public IStorage {
public:
    Storage(const std::string& base_path, bool encrypt_data = false, bool compress_data = false);
    ~Storage();

    // File storage operations (automatically compress and encrypt)
    Result<std::string> store_file(const std::string& uid, const std::string& version_timestamp,
                                   const std::vector<uint8_t>& data, const std::string& tenant = "") override;
    Result<std::vector<uint8_t>> read_file(const std::string& storage_path, const std::string& tenant = "") override;
    Result<void> delete_file(const std::string& storage_path, const std::string& tenant = "") override;
    Result<bool> file_exists(const std::string& storage_path, const std::string& tenant = "") override;

    // Get storage path for a file by UUID and timestamp
    std::string get_storage_path(const std::string& uid, const std::string& version_timestamp, const std::string& tenant = "") const override;

    // Check if encryption is enabled
    bool is_encryption_enabled() const override;

    // Check if compression is enabled
    bool is_compression_enabled() const override;

    // Tenant management operations
    Result<void> create_tenant_directory(const std::string& tenant) override;
    Result<bool> tenant_directory_exists(const std::string& tenant) override;
    Result<void> cleanup_tenant_directory(const std::string& tenant) override;

    // Synchronization operations
    Result<void> sync_to_object_store(std::function<void(const std::string&, const std::string&, int)> progress_callback = nullptr) override;
    Result<std::vector<std::string>> get_local_file_paths(const std::string& tenant = "") const override;

    // Object store access for caching functionality
    void set_object_store(IObjectStore* object_store) override;
    IObjectStore* get_object_store() const override;

    // Storage clearing operation
    Result<void> clear_storage(const std::string& tenant = "") override;

private:
    std::string base_path_;
    bool encrypt_data_;
    bool compress_data_;
    mutable std::mutex storage_mutex_;
    IObjectStore* object_store_;

    // Helper to create SHA256-based directory structure (desaturation)
    std::string get_sha256_desaturated_path(const std::string& uid) const;
    
    // Helper to ensure directory exists
    Result<void> ensure_directory_exists(const std::string& dir_path);
};

} // namespace fileengine