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

#ifndef TENANT_MANAGER_H
#define TENANT_MANAGER_H

#include "IDatabase.h"
#include "accountability.h"
#include "IStorage.h"
#include "IObjectStore.h"
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace fileengine {

struct TenantConfig {
    std::string db_host;
    int db_port;
    std::string db_name;
    std::string db_user;
    std::string db_password;
    std::string storage_base_path;
    std::string s3_endpoint;
    std::string s3_region;
    std::string s3_bucket;
    std::string s3_access_key;
    std::string s3_secret_key;
    bool s3_path_style;
    bool encrypt_data;
    bool compress_data;
    std::string encryption_key;  // Added for encryption support
};

struct TenantContext {
    std::shared_ptr<IDatabase> db;  // Shared across all tenants to reuse connection pool
    std::unique_ptr<IStorage> storage;
    std::unique_ptr<IObjectStore> object_store;
    class StorageTracker* storage_tracker;  // Pointer to shared storage tracker
    TenantConfig config;  // Added to store tenant-specific configuration including encryption key
};

class TenantManager {
public:
    TenantManager(const TenantConfig& config, std::shared_ptr<IDatabase> shared_db = nullptr, class StorageTracker* storage_tracker = nullptr);
    ~TenantManager();

    TenantContext* get_tenant_context(const std::string& tenant_id);

    // `ctx` attributes the tenant's creation in the global accountability
    // record. It defaults to the system identity because that is the honest
    // answer for the two paths that reach here — server startup provisioning
    // "default", and lazy provisioning triggered by the first request for a
    // tenant. Neither is a person deciding to create a tenant; when an
    // administrative path to create one exists, it should pass the operator.
    bool initialize_tenant(const std::string& tenant_id,
                           const AccountabilityContext& ctx = AccountabilityContext::system());
    bool tenant_exists(const std::string& tenant_id) const;

    // The platform's most destructive operation. `ctx` must name a real
    // operator — the global record of the deletion is all that will survive it.
    Result<void> remove_tenant(const std::string& tenant_id,
                               const AccountabilityContext& ctx);
    const TenantConfig& get_config() const { return config_; }

private:
    TenantContext* create_tenant_context(const std::string& tenant_id);

    TenantConfig config_;
    std::shared_ptr<IDatabase> shared_database_;
    class StorageTracker* storage_tracker_;
    std::map<std::string, std::unique_ptr<TenantContext>> tenant_contexts_;
    mutable std::mutex mutex_;
};

} // namespace fileengine

#endif // TENANT_MANAGER_H