#ifndef TENANT_MANAGER_H
#define TENANT_MANAGER_H

#include "IDatabase.h"
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
};

struct TenantContext {
    std::unique_ptr<IDatabase> db;
    std::unique_ptr<IStorage> storage;
    std::unique_ptr<IObjectStore> object_store;
    class StorageTracker* storage_tracker;  // Pointer to shared storage tracker
};

class TenantManager {
public:
    TenantManager(const TenantConfig& config, std::shared_ptr<IDatabase> shared_db = nullptr, class StorageTracker* storage_tracker = nullptr);
    ~TenantManager();

    TenantContext* get_tenant_context(const std::string& tenant_id);
    bool initialize_tenant(const std::string& tenant_id);
    bool tenant_exists(const std::string& tenant_id) const;
    Result<void> remove_tenant(const std::string& tenant_id);
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