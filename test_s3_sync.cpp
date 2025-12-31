#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "core/include/fileengine/filesystem.h"
#include "core/include/fileengine/tenant_manager.h"
#include "core/include/fileengine/database.h"
#include "core/include/fileengine/s3_storage.h"

int main() {
    std::cout << "Testing direct filesystem operations with S3 sync..." << std::endl;

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
    config.s3_bucket = "fileservicecpp";
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

    std::cout << "Creating a file directly..." << std::endl;
    auto file_result = filesystem->touch("", "direct_s3_test_file.txt", "root", "");
    if (!file_result.success) {
        std::cout << "Failed to create file: " << file_result.error << std::endl;
        return 1;
    }
    
    std::string file_uid = file_result.value;
    std::cout << "Created file with UID: " << file_uid << std::endl;

    // Now put content to the file to trigger S3 sync
    std::cout << "Putting content to file to trigger S3 sync..." << std::endl;
    std::vector<uint8_t> content = {'T', 'e', 's', 't', ' ', 'c', 'o', 'n', 't', 'e', 'n', 't', ' ', 'f', 'o', 'r', ' ', 'S', '3'};
    auto put_result = filesystem->put(file_uid, content, "root", "");
    
    if (put_result.success) {
        std::cout << "Successfully put content to file! S3 sync should be triggered in background." << std::endl;
    } else {
        std::cout << "Failed to put content to file: " << put_result.error << std::endl;
        return 1;
    }

    // Wait a bit for the async S3 sync to potentially complete
    std::cout << "Waiting for potential S3 sync..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::cout << "Direct filesystem test with S3 sync completed!" << std::endl;
    return 0;
}