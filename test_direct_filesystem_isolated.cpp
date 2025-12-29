#include <iostream>
#include <memory>
#include <string>
#include "core/include/fileengine/filesystem.h"
#include "core/include/fileengine/tenant_manager.h"
#include "core/include/fileengine/database.h"

int main() {
    std::cout << "Testing direct filesystem operations without gRPC..." << std::endl;

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

    std::cout << "Testing mkdir with empty parent UID (root directory creation)..." << std::endl;
    
    // Test creating a directory with empty parent UID (should create root directory)
    auto result = filesystem->mkdir("", "test_root_dir", "root", 0755, "");
    
    if (result.success) {
        std::cout << "SUCCESS: Created directory with UID: " << result.value << std::endl;
        
        // Test creating a file in the root directory
        std::cout << "Testing touch operation in root directory..." << std::endl;
        auto file_result = filesystem->touch(result.value, "test_file.txt", "root", "");
        if (file_result.success) {
            std::cout << "SUCCESS: Created file with UID: " << file_result.value << std::endl;
        } else {
            std::cout << "FAILED: Could not create file: " << file_result.error << std::endl;
        }
    } else {
        std::cout << "FAILED: Could not create directory: " << result.error << std::endl;
        return 1;
    }

    std::cout << "Direct filesystem test completed!" << std::endl;
    return 0;
}