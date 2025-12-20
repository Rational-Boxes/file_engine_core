#include "fileengine/tenant_manager.h"
#include "fileengine/database.h"
#include "fileengine/storage.h"
#include "fileengine/s3_storage.h"

namespace fileengine {

TenantManager::TenantManager(const TenantConfig& config) : config_(config) {
}

TenantManager::~TenantManager() {
    // Cleanup all tenant contexts
    std::lock_guard<std::mutex> lock(mutex_);
    tenant_contexts_.clear();
}

TenantContext* TenantManager::get_tenant_context(const std::string& tenant_id) {
    if (tenant_id.empty()) {
        return nullptr;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check if tenant context already exists
    auto it = tenant_contexts_.find(tenant_id);
    if (it != tenant_contexts_.end()) {
        return it->second.get();
    }
    
    // Create a new tenant context
    TenantContext* context = create_tenant_context(tenant_id);
    if (context) {
        tenant_contexts_[tenant_id] = std::unique_ptr<TenantContext>(context);
        return context;
    }
    
    return nullptr;
}

bool TenantManager::initialize_tenant(const std::string& tenant_id) {
    if (tenant_id.empty()) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Get or create the tenant context
    TenantContext* context = get_tenant_context(tenant_id);
    if (!context) {
        return false;
    }
    
    // Initialize database schema for the tenant
    if (context->db) {
        auto result = context->db->create_tenant_schema(tenant_id);
        if (!result.success) {
            return false;
        }
    }
    
    // Create tenant directory for storage
    if (context->storage) {
        auto result = context->storage->create_tenant_directory(tenant_id);
        if (!result.success) {
            return false;
        }
    }
    
    // Create tenant bucket for object store if available
    if (context->object_store) {
        auto result = context->object_store->create_tenant_bucket(tenant_id);
        if (!result.success) {
            return false;
        }
    }
    
    return true;
}

bool TenantManager::tenant_exists(const std::string& tenant_id) const {
    if (tenant_id.empty()) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    return tenant_contexts_.find(tenant_id) != tenant_contexts_.end();
}

Result<void> TenantManager::remove_tenant(const std::string& tenant_id) {
    if (tenant_id.empty()) {
        return Result<void>::err("Tenant ID cannot be empty");
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = tenant_contexts_.find(tenant_id);
    if (it == tenant_contexts_.end()) {
        return Result<void>::err("Tenant does not exist");
    }
    
    // Clean up tenant data in all components
    TenantContext* context = it->second.get();
    
    if (context->db) {
        auto db_result = context->db->cleanup_tenant_data(tenant_id);
        if (!db_result.success) {
            return db_result;
        }
    }
    
    if (context->storage) {
        auto storage_result = context->storage->cleanup_tenant_directory(tenant_id);
        if (!storage_result.success) {
            return storage_result;
        }
    }
    
    if (context->object_store) {
        auto object_store_result = context->object_store->cleanup_tenant_bucket(tenant_id);
        if (!object_store_result.success) {
            return object_store_result;
        }
    }
    
    // Remove from the map
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
        
        // Create storage instance for the tenant
        auto storage = std::make_unique<Storage>(
            config_.storage_base_path,
            config_.encrypt_data,
            config_.compress_data
        );
        
        // Create S3 storage instance
        auto object_store = std::make_unique<S3Storage>(
            config_.s3_endpoint,
            config_.s3_region,
            config_.s3_bucket,
            config_.s3_access_key,
            config_.s3_secret_key,
            config_.s3_path_style
        );
        
        // Initialize the S3 storage
        auto init_result = object_store->initialize();
        if (!init_result.success) {
            return nullptr;
        }
        
        // Create the tenant context
        auto context = std::make_unique<TenantContext>();
        context->db = std::move(db);
        context->storage = std::move(storage);
        context->object_store = std::move(object_store);
        
        // Return a pointer to the context (it will be managed by the unique_ptr in the map)
        // We need to release the unique_ptr to return a raw pointer, but we need to be careful about ownership
        // The proper approach is to return a raw pointer but store the unique_ptr in the map
        TenantContext* raw_context = context.get();
        tenant_contexts_[tenant_id] = std::move(context);
        
        return raw_context;
    } catch (const std::exception& ex) {
        return nullptr;
    }
}

} // namespace fileengine