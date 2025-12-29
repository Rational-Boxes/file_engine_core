#include <iostream>
#include <memory>
#include <string>
#include "core/include/fileengine/filesystem.h"
#include "core/include/fileengine/tenant_manager.h"
#include "core/include/fileengine/database.h"

int main() {
    std::cout << "Testing direct filesystem operations with non-empty parent UID..." << std::endl;

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

    std::cout << "First, let's try to create a root directory with a known UID..." << std::endl;
    
    // Test creating a directory with a non-empty parent UID (using root directory UID)
    // First, let's try to create a directory with a dummy parent UID to see if that works
    auto result = filesystem->mkdir("some-parent-uid", "test_child_dir", "root", 0755, "");
    
    if (result.success) {
        std::cout << "SUCCESS: Created child directory with UID: " << result.value << std::endl;
    } else {
        std::cout << "EXPECTED FAILURE: Could not create directory with non-existent parent: " << result.error << std::endl;
        std::cout << "This is expected since the parent doesn't exist" << std::endl;
    }

    // Now let's try to create a directory with empty parent UID but using a different approach
    // Let's first check if the root directory exists in the database
    std::cout << "Checking if root directory exists..." << std::endl;
    auto exists_result = filesystem->exists("", "");  // Check if root exists
    if (exists_result.success) {
        std::cout << "Root directory exists: " << (exists_result.value ? "YES" : "NO") << std::endl;
    } else {
        std::cout << "Could not check root directory existence: " << exists_result.error << std::endl;
    }

    std::cout << "Direct filesystem test with non-empty parent completed!" << std::endl;
    return 0;
}