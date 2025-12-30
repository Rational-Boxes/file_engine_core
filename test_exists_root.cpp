#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "core/include/fileengine/filesystem.h"
#include "core/include/fileengine/tenant_manager.h"
#include "core/include/fileengine/database.h"

int main() {
    std::cout << "Testing exists operation on root directory..." << std::endl;

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

    // Create filesystem with the tenant manager
    auto filesystem = std::make_unique<fileengine::FileSystem>(tenant_manager);

    std::cout << "Testing exists on root directory (empty UID)..." << std::endl;
    auto exists_result = filesystem->exists("", "");
    if (exists_result.success) {
        std::cout << "EXISTS RESULT: " << (exists_result.value ? "EXISTS" : "DOES NOT EXIST") << std::endl;
    } else {
        std::cout << "EXISTS CHECK FAILED: " << exists_result.error << std::endl;
    }

    std::cout << "Exists test completed!" << std::endl;
    return 0;
}