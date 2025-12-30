#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "core/include/fileengine/filesystem.h"
#include "core/include/fileengine/tenant_manager.h"
#include "core/include/fileengine/database.h"

int main() {
    std::cout << "Testing put operation to trigger desaturated directory creation..." << std::endl;

    // Create a TenantConfig with the actual database settings from .env
    fileengine::TenantConfig config;
    config.db_host = "localhost";
    config.db_port = 5434;  // Using the correct port
    config.db_name = "fileengine";
    config.db_user = "postgres";
    config.db_password = "postgres";
    config.storage_base_path = "/home/telendry/temp/fileengine";
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

    std::cout << "Creating a test file..." << std::endl;
    auto file_result = filesystem->touch("", "test_file_for_put.txt", "root", "");
    if (!file_result.success) {
        std::cout << "Failed to create test file: " << file_result.error << std::endl;
        return 1;
    }
    
    std::string file_uid = file_result.value;
    std::cout << "Created test file with UID: " << file_uid << std::endl;

    // Now try to put content to the file to trigger physical storage creation
    std::cout << "Putting content to file to trigger desaturated directory creation..." << std::endl;
    std::vector<uint8_t> content = {'T', 'e', 's', 't', ' ', 'c', 'o', 'n', 't', 'e', 'n', 't'};
    auto put_result = filesystem->put(file_uid, content, "root", "");
    
    if (put_result.success) {
        std::cout << "Successfully put content to file!" << std::endl;
    } else {
        std::cout << "Failed to put content to file: " << put_result.error << std::endl;
        return 1;
    }

    std::cout << "Put operation test completed successfully!" << std::endl;
    return 0;
}