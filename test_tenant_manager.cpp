#include <iostream>
#include <memory>
#include <string>
#include "core/include/fileengine/tenant_manager.h"
#include "core/include/fileengine/database.h"

int main() {
    std::cout << "Testing tenant manager with empty tenant string..." << std::endl;

    // Create a TenantConfig with the actual database settings from .env
    fileengine::TenantConfig config;
    config.db_host = "localhost";
    config.db_port = 5434;  // Using the correct port
    config.db_name = "fileengine";
    config.db_user = "postgres";
    config.db_password = "postgres";
    config.storage_base_path = "/tmp/test_storage";
    config.s3_endpoint = "http://localhost:9000";
    config.s3_region = "us-east-1";
    config.s3_bucket = "test_bucket";
    config.s3_access_key = "minioadmin";
    config.s3_secret_key = "minioadmin";
    config.s3_path_style = true;
    config.encrypt_data = false;
    config.compress_data = false;

    // Create a shared database instance
    auto shared_db = std::make_shared<fileengine::Database>(
        config.db_host, config.db_port, config.db_name,
        config.db_user, config.db_password
    );

    // Connect to the database
    if (!shared_db->connect()) {
        std::cout << "Failed to connect to database" << std::endl;
        return 1;
    }

    // Create tenant manager with shared database
    auto tenant_manager = std::make_shared<fileengine::TenantManager>(config, shared_db);

    std::cout << "Testing tenant context creation for empty tenant string..." << std::endl;
    
    // Test getting the tenant context for an empty tenant (should map to "default")
    auto context = tenant_manager->get_tenant_context("");
    if (context != nullptr) {
        std::cout << "SUCCESS: Got tenant context for empty tenant string!" << std::endl;
        std::cout << "Database available: " << (context->db ? "yes" : "no") << std::endl;
        std::cout << "Storage available: " << (context->storage ? "yes" : "no") << std::endl;
        std::cout << "Object store available: " << (context->object_store ? "yes" : "no") << std::endl;
    } else {
        std::cout << "FAILED: Could not get tenant context for empty tenant string" << std::endl;
        return 1;
    }

    // Test getting the tenant context for a named tenant
    std::cout << "Testing tenant context creation for named tenant..." << std::endl;
    auto named_context = tenant_manager->get_tenant_context("test");
    if (named_context != nullptr) {
        std::cout << "SUCCESS: Got tenant context for named tenant!" << std::endl;
    } else {
        std::cout << "FAILED: Could not get tenant context for named tenant" << std::endl;
        return 1;
    }

    std::cout << "Tenant manager test completed successfully!" << std::endl;
    return 0;
}