#include <iostream>
#include <string>
#include <vector>
#include <getopt.h>
#include "fileengine/filesystem.h"
#include "fileengine/database.h"
#include "fileengine/storage.h"
#include "fileengine/s3_storage.h"
#include "fileengine/tenant_manager.h"
#include "fileengine/acl_manager.h"

namespace fileengine {

class FileEngineCLI {
public:
    FileEngineCLI() = default;
    
    int run(int argc, char* argv[]) {
        if (argc < 2) {
            print_usage();
            return 1;
        }
        
        std::string command = argv[1];
        
        if (command == "help" || command == "--help" || command == "-h") {
            print_usage();
            return 0;
        }
        
        // Initialize the system components
        initialize_system();
        
        if (command == "mkdir") {
            return handle_mkdir(argc, argv);
        } else if (command == "ls") {
            return handle_ls(argc, argv);
        } else if (command == "touch") {
            return handle_touch(argc, argv);
        } else if (command == "rm") {
            return handle_rm(argc, argv);
        } else if (command == "stat") {
            return handle_stat(argc, argv);
        } else if (command == "tenant-create") {
            return handle_tenant_create(argc, argv);
        } else if (command == "acl-grant") {
            return handle_acl_grant(argc, argv);
        } else if (command == "acl-list") {
            return handle_acl_list(argc, argv);
        } else {
            std::cerr << "Unknown command: " << command << std::endl;
            print_usage();
            return 1;
        }
        
        return 0;
    }

private:
    std::shared_ptr<TenantManager> tenant_manager_;
    std::shared_ptr<FileSystem> file_system_;
    
    void print_usage() {
        std::cout << "FileEngine CLI - Administrative tool" << std::endl;
        std::cout << "Usage: fileengine_cli <command> [options]" << std::endl;
        std::cout << std::endl;
        std::cout << "Commands:" << std::endl;
        std::cout << "  mkdir <parent_uid> <name>     Create a directory" << std::endl;
        std::cout << "  ls <dir_uid>                  List directory contents" << std::endl;
        std::cout << "  touch <parent_uid> <name>     Create an empty file" << std::endl;
        std::cout << "  rm <file_uid>                 Remove a file or directory" << std::endl;
        std::cout << "  stat <file_uid>               Get file information" << std::endl;
        std::cout << "  tenant-create <tenant_id>     Create a new tenant" << std::endl;
        std::cout << "  acl-grant <resource_uid> <user> <permissions>  Grant ACL permissions" << std::endl;
        std::cout << "  acl-list <resource_uid>       List ACLs for a resource" << std::endl;
        std::cout << "  help                          Show this help message" << std::endl;
    }
    
    void initialize_system() {
        // Create a basic tenant configuration
        TenantConfig config;
        config.db_host = "localhost";
        config.db_port = 5432;
        config.db_name = "fileengine_test";
        config.db_user = "testuser";
        config.db_password = "testpass";
        config.storage_base_path = "/tmp/fileengine_storage";
        config.s3_endpoint = "http://localhost:9000";
        config.s3_region = "us-east-1";
        config.s3_bucket = "fileengine-test";
        config.s3_access_key = "minioadmin";
        config.s3_secret_key = "minioadmin";
        config.s3_path_style = true;
        config.encrypt_data = false;
        config.compress_data = false;
        
        tenant_manager_ = std::make_shared<TenantManager>(config);
        file_system_ = std::make_shared<FileSystem>(tenant_manager_);
    }
    
    int handle_mkdir(int argc, char* argv[]) {
        if (argc != 4) {
            std::cerr << "Usage: fileengine_cli mkdir <parent_uid> <name>" << std::endl;
            return 1;
        }
        
        std::string parent_uid = argv[2];
        std::string name = argv[3];
        std::string user = "admin"; // Default user for CLI
        
        auto result = file_system_->mkdir(parent_uid, name, user);
        if (result.success) {
            std::cout << "Directory created with UID: " << result.value << std::endl;
            return 0;
        } else {
            std::cerr << "Error creating directory: " << result.error << std::endl;
            return 1;
        }
    }
    
    int handle_ls(int argc, char* argv[]) {
        if (argc != 3) {
            std::cerr << "Usage: fileengine_cli ls <dir_uid> [tenant]" << std::endl;
            return 1;
        }
        
        std::string dir_uid = argv[2];
        std::string tenant = (argc > 3) ? argv[3] : "";
        std::string user = "admin"; // Default user for CLI
        
        auto result = file_system_->listdir(dir_uid, user, tenant);
        if (result.success) {
            std::cout << "Contents of directory " << dir_uid << ":" << std::endl;
            for (const auto& entry : result.value) {
                std::cout << entry.uid << " " << entry.name << " " << (entry.type == FileType::DIRECTORY ? "DIR" : "FILE") << std::endl;
            }
            return 0;
        } else {
            std::cerr << "Error listing directory: " << result.error << std::endl;
            return 1;
        }
    }
    
    int handle_touch(int argc, char* argv[]) {
        if (argc != 4) {
            std::cerr << "Usage: fileengine_cli touch <parent_uid> <name>" << std::endl;
            return 1;
        }
        
        std::string parent_uid = argv[2];
        std::string name = argv[3];
        std::string user = "admin"; // Default user for CLI
        
        auto result = file_system_->touch(parent_uid, name, user);
        if (result.success) {
            std::cout << "File created with UID: " << result.value << std::endl;
            return 0;
        } else {
            std::cerr << "Error creating file: " << result.error << std::endl;
            return 1;
        }
    }
    
    int handle_rm(int argc, char* argv[]) {
        if (argc != 3) {
            std::cerr << "Usage: fileengine_cli rm <file_uid> [tenant]" << std::endl;
            return 1;
        }
        
        std::string file_uid = argv[2];
        std::string tenant = (argc > 3) ? argv[3] : "";
        std::string user = "admin"; // Default user for CLI
        
        auto result = file_system_->remove(file_uid, user, tenant);
        if (result.success) {
            std::cout << "File/directory removed successfully" << std::endl;
            return 0;
        } else {
            std::cerr << "Error removing file/directory: " << result.error << std::endl;
            return 1;
        }
    }
    
    int handle_stat(int argc, char* argv[]) {
        if (argc != 3) {
            std::cerr << "Usage: fileengine_cli stat <file_uid> [tenant]" << std::endl;
            return 1;
        }
        
        std::string file_uid = argv[2];
        std::string tenant = (argc > 3) ? argv[3] : "";
        std::string user = "admin"; // Default user for CLI
        
        auto result = file_system_->stat(file_uid, user, tenant);
        if (result.success) {
            std::cout << "File information:" << std::endl;
            std::cout << "  UID: " << result.value.uid << std::endl;
            std::cout << "  Name: " << result.value.name << std::endl;
            std::cout << "  Type: " << (result.value.type == FileType::DIRECTORY ? "DIRECTORY" : 
                                        result.value.type == FileType::REGULAR_FILE ? "REGULAR_FILE" : "SYMLINK") << std::endl;
            std::cout << "  Size: " << result.value.size << std::endl;
            std::cout << "  Owner: " << result.value.owner << std::endl;
            std::cout << "  Permissions: " << result.value.permissions << std::endl;
            return 0;
        } else {
            std::cerr << "Error getting file info: " << result.error << std::endl;
            return 1;
        }
    }
    
    int handle_tenant_create(int argc, char* argv[]) {
        if (argc != 3) {
            std::cerr << "Usage: fileengine_cli tenant-create <tenant_id>" << std::endl;
            return 1;
        }
        
        std::string tenant_id = argv[2];
        
        bool success = tenant_manager_->initialize_tenant(tenant_id);
        if (success) {
            std::cout << "Tenant '" << tenant_id << "' created successfully" << std::endl;
            return 0;
        } else {
            std::cerr << "Error creating tenant: " << tenant_id << std::endl;
            return 1;
        }
    }
    
    int handle_acl_grant(int argc, char* argv[]) {
        if (argc != 5) {
            std::cerr << "Usage: fileengine_cli acl-grant <resource_uid> <user> <permissions>" << std::endl;
            std::cerr << "Permissions: 4=read, 2=write, 1=execute (can be combined, e.g., 6=read+write)" << std::endl;
            return 1;
        }
        
        std::string resource_uid = argv[2];
        std::string user = argv[3];
        int permissions = std::stoi(argv[4]);
        
        auto result = file_system_->grant_permission(resource_uid, user, permissions, "admin");
        if (result.success) {
            std::cout << "Permissions granted successfully" << std::endl;
            return 0;
        } else {
            std::cerr << "Error granting permissions: " << result.error << std::endl;
            return 1;
        }
    }
    
    int handle_acl_list(int argc, char* argv[]) {
        if (argc != 3) {
            std::cerr << "Usage: fileengine_cli acl-list <resource_uid>" << std::endl;
            return 1;
        }
        
        std::string resource_uid = argv[2];
        
        // For this example, we'll just show that the functionality would exist
        // An actual implementation would require an ACL manager in the filesystem
        std::cout << "ACL listing for resource: " << resource_uid << std::endl;
        std::cout << "Note: Detailed ACL listing not fully implemented in this example" << std::endl;
        return 0;
    }
};

} // namespace fileengine

int main(int argc, char* argv[]) {
    fileengine::FileEngineCLI cli;
    return cli.run(argc, argv);
}