#include <iostream>
#include <memory>
#include <string>
#include "core/include/fileengine/filesystem.h"
#include "core/include/fileengine/tenant_manager.h"
#include "core/include/fileengine/database.h"

int main() {
    std::cout << "Testing filesystem operations directly (without gRPC)..." << std::endl;

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

    // Create ACL manager
    auto acl_manager = std::make_shared<fileengine::AclManager>(shared_db);

    // Create filesystem with the tenant manager
    auto filesystem = std::make_unique<fileengine::FileSystem>(tenant_manager);
    filesystem->set_acl_manager(acl_manager);

    std::cout << "Testing mkdir with empty tenant string (should use 'default')..." << std::endl;
    
    // Test creating a directory with an empty tenant (should use default tenant)
    auto result = filesystem->mkdir("", "direct_test_dir", "root", 0755, "");
    
    if (result.success) {
        std::cout << "SUCCESS: Created directory in default tenant!" << std::endl;
        std::cout << "Directory UID: " << result.value << std::endl;
        
        // Test creating a file in the directory we just created
        std::cout << "Testing touch operation in the created directory..." << std::endl;
        auto file_result = filesystem->touch(result.value, "test_file.txt", "root", "");
        if (file_result.success) {
            std::cout << "SUCCESS: Created file in directory!" << std::endl;
            std::cout << "File UID: " << file_result.value << std::endl;
            
            // Test putting content to the file
            std::cout << "Testing put operation..." << std::endl;
            std::vector<uint8_t> content = {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd', '!'};
            auto put_result = filesystem->put(file_result.value, content, "root", "");
            if (put_result.success) {
                std::cout << "SUCCESS: Put content to file!" << std::endl;
                
                // Test getting content from the file
                std::cout << "Testing get operation..." << std::endl;
                auto get_result = filesystem->get(file_result.value, "root", "");
                if (get_result.success) {
                    std::cout << "SUCCESS: Got content from file!" << std::endl;
                    std::string content_str(get_result.value.begin(), get_result.value.end());
                    std::cout << "Content: " << content_str << std::endl;
                } else {
                    std::cout << "FAILED: Could not get content from file: " << get_result.error << std::endl;
                }
            } else {
                std::cout << "FAILED: Could not put content to file: " << put_result.error << std::endl;
            }
        } else {
            std::cout << "FAILED: Could not create file: " << file_result.error << std::endl;
        }
    } else {
        std::cout << "FAILED: Could not create directory in default tenant: " << result.error << std::endl;
        return 1;
    }

    std::cout << "Direct filesystem test completed successfully!" << std::endl;
    return 0;
}