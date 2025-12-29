#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "core/include/fileengine/filesystem.h"
#include "core/include/fileengine/tenant_manager.h"
#include "core/include/fileengine/database.h"

int main() {
    std::cout << "Testing direct filesystem operations with root UID as parent..." << std::endl;

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

    std::cout << "Testing mkdir with root UID (empty string) as parent..." << std::endl;
    
    // Test creating a directory with empty parent UID (root directory) - this is where the crash occurs
    std::cout << "About to call mkdir with empty parent UID..." << std::endl;
    auto result = filesystem->mkdir("", "test_subdir", "root", 0755, "");
    std::cout << "mkdir call completed." << std::endl;
    
    if (result.success) {
        std::cout << "SUCCESS: Created subdirectory with UID: " << result.value << std::endl;
    } else {
        std::cout << "FAILED: Could not create subdirectory: " << result.error << std::endl;
    }

    // Let's also try to stat the root directory
    std::cout << "Testing stat on root directory..." << std::endl;
    auto stat_result = filesystem->stat("", "root", "");
    if (stat_result.success) {
        std::cout << "SUCCESS: Root directory stat succeeded" << std::endl;
        std::cout << "  UID: " << stat_result.value.uid << std::endl;
        std::cout << "  Name: " << stat_result.value.name << std::endl;
        std::cout << "  Type: " << (stat_result.value.type == fileengine::FileType::DIRECTORY ? "DIRECTORY" : "FILE") << std::endl;
    } else {
        std::cout << "FAILED: Could not stat root directory: " << stat_result.error << std::endl;
    }

    std::cout << "Test completed!" << std::endl;
    return 0;
}