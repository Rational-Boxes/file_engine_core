#include "fileengine/tenant_manager.h"
#include "fileengine/database.h"
#include "fileengine/storage.h"
#include "fileengine/s3_storage.h"

namespace fileengine {

TenantManager::TenantManager(const TenantConfig& config, std::shared_ptr<IDatabase> shared_db, class StorageTracker* storage_tracker)
    : config_(config), shared_database_(shared_db), storage_tracker_(storage_tracker) {
}

TenantManager::~TenantManager() {
    // Cleanup all tenant contexts
    std::lock_guard<std::mutex> lock(mutex_);
    tenant_contexts_.clear();
}

TenantContext* TenantManager::get_tenant_context(const std::string& tenant_id) {
    std::string actual_tenant_id = tenant_id;
    if (actual_tenant_id.empty()) {
        actual_tenant_id = "default";  // Use "default" tenant when no tenant is specified
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Check if tenant context already exists
    auto it = tenant_contexts_.find(actual_tenant_id);
    if (it != tenant_contexts_.end()) {
        return it->second.get();
    }

    // Create a new tenant context which will ensure database structures exist
    TenantContext* context = create_tenant_context(actual_tenant_id);
    if (context) {
        tenant_contexts_[actual_tenant_id] = std::unique_ptr<TenantContext>(context);
        return context;
    }

    return nullptr;
}

bool TenantManager::initialize_tenant(const std::string& tenant_id) {
    std::string actual_tenant_id = tenant_id;
    if (actual_tenant_id.empty()) {
        actual_tenant_id = "default";  // Use "default" tenant when no tenant is specified
    }

    // Always use the shared database instance - never create new connections outside of the pooling system
    if (shared_database_ == nullptr) {
        return false;  // Cannot initialize without shared database
    }

    // Create tenant-specific schema using the shared database (which uses connection pooling)
    auto result = shared_database_->create_tenant_schema(actual_tenant_id);
    return result.success;
}

bool TenantManager::tenant_exists(const std::string& tenant_id) const {
    std::string actual_tenant_id = tenant_id;
    if (actual_tenant_id.empty()) {
        actual_tenant_id = "default";  // Use "default" tenant when no tenant is specified
    }

    std::lock_guard<std::mutex> lock(mutex_);
    return tenant_contexts_.find(actual_tenant_id) != tenant_contexts_.end();
}

Result<void> TenantManager::remove_tenant(const std::string& tenant_id) {
    std::string actual_tenant_id = tenant_id;
    if (actual_tenant_id.empty()) {
        actual_tenant_id = "default";  // Use "default" tenant when no tenant is specified
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tenant_contexts_.find(actual_tenant_id);
    if (it == tenant_contexts_.end()) {
        return Result<void>::err("Tenant does not exist");
    }

    // Clean up tenant data in the database
    auto context = it->second.get();

    if (context->db) {
        auto db_result = context->db->cleanup_tenant_data(actual_tenant_id);
        if (!db_result.success) {
            return db_result;
        }
    }

    if (context->storage) {
        auto storage_result = context->storage->clear_storage(actual_tenant_id);
        if (!storage_result.success) {
            return storage_result;
        }
    }

    if (context->object_store) {
        auto obj_store_result = context->object_store->clear_storage(actual_tenant_id);
        if (!obj_store_result.success) {
            return obj_store_result;
        }
    }

    // Remove from our contexts map
    tenant_contexts_.erase(it);
    return Result<void>::ok();
}

TenantContext* TenantManager::create_tenant_context(const std::string& tenant_id) {
    try {
        // Create database instance for the tenant
        auto db = std::make_unique<Database>(
            config_.db_host,
            config_.db_port,
            config_.db_name,
            config_.db_user,
            config_.db_password
        );

        // Initialize the database connection
        if (!db->connect()) {
            return nullptr;
        }

        // Ensure tenant schema and tables exist - this will create them if they don't exist
        auto schema_result = db->create_tenant_schema(tenant_id);
        if (!schema_result.success) {
            // Log the error but continue - some operations might still work
        }

        // Create storage instance for the tenant
        auto storage = std::make_unique<Storage>(
            config_.storage_base_path,
            config_.encrypt_data,
            config_.compress_data
        );

        // Create object store instance
        auto object_store = std::make_unique<S3Storage>(
            config_.s3_endpoint,
            config_.s3_region,
            config_.s3_bucket,
            config_.s3_access_key,
            config_.s3_secret_key,
            config_.s3_path_style
        );

        // Initialize the object store (non-fatal if it fails)
        auto init_result = object_store->initialize();
        if (!init_result.success) {
            // Log warning but continue - object store is optional
        }

        // Create the tenant context
        auto context = std::make_unique<TenantContext>();
        context->db = std::move(db);
        context->storage = std::move(storage);
        context->object_store = std::move(object_store);
        context->storage_tracker = storage_tracker_;

        return context.release();  // Transfer ownership to caller who will place in map
    } catch (const std::exception& ex) {
        return nullptr;
    }
}

} // namespace fileengine