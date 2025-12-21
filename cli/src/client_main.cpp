#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>
#include <vector>
#include <grpcpp/grpcpp.h>

// Include generated gRPC files
#include "../../build/core/generated/fileengine/fileservice.grpc.pb.h"

using fileengine_service::FileService;
using fileengine_service::MakeDirectoryRequest;
using fileengine_service::MakeDirectoryResponse;
using fileengine_service::RemoveDirectoryRequest;
using fileengine_service::RemoveDirectoryResponse;
using fileengine_service::ListDirectoryRequest;
using fileengine_service::ListDirectoryResponse;
using fileengine_service::TouchRequest;
using fileengine_service::TouchResponse;
using fileengine_service::RemoveFileRequest;
using fileengine_service::RemoveFileResponse;
using fileengine_service::PutFileRequest;
using fileengine_service::PutFileResponse;
using fileengine_service::GetFileRequest;
using fileengine_service::GetFileResponse;
using fileengine_service::StatRequest;
using fileengine_service::StatResponse;
using fileengine_service::ExistsRequest;
using fileengine_service::ExistsResponse;
using fileengine_service::RenameRequest;
using fileengine_service::RenameResponse;
using fileengine_service::MoveRequest;
using fileengine_service::MoveResponse;
using fileengine_service::CopyRequest;
using fileengine_service::CopyResponse;
using fileengine_service::ListVersionsRequest;
using fileengine_service::ListVersionsResponse;
using fileengine_service::GetVersionRequest;
using fileengine_service::GetVersionResponse;
using fileengine_service::SetMetadataRequest;
using fileengine_service::SetMetadataResponse;
using fileengine_service::GetMetadataRequest;
using fileengine_service::GetMetadataResponse;
using fileengine_service::GetAllMetadataRequest;
using fileengine_service::GetAllMetadataResponse;
using fileengine_service::DeleteMetadataRequest;
using fileengine_service::DeleteMetadataResponse;
using fileengine_service::GrantPermissionRequest;
using fileengine_service::GrantPermissionResponse;
using fileengine_service::RevokePermissionRequest;
using fileengine_service::RevokePermissionResponse;
using fileengine_service::CheckPermissionRequest;
using fileengine_service::CheckPermissionResponse;
using fileengine_service::AuthenticationContext;
using fileengine_service::FileType;
using fileengine_service::Permission;
using fileengine_service::StorageUsageRequest;
using fileengine_service::StorageUsageResponse;
using fileengine_service::PurgeOldVersionsRequest;
using fileengine_service::PurgeOldVersionsResponse;
using fileengine_service::TriggerSyncRequest;
using fileengine_service::TriggerSyncResponse;

namespace fileengine {

class FileEngineClient {
private:
    std::unique_ptr<fileengine_service::FileService::Stub> stub_;

public:
    FileEngineClient(std::shared_ptr<grpc::Channel> channel)
        : stub_(fileengine_service::FileService::NewStub(channel)) {}

    // Helper function to create auth context
    AuthenticationContext create_auth_context(const std::string& user, const std::vector<std::string>& roles = {}, const std::string& tenant = "default") {
        AuthenticationContext auth_ctx;
        auth_ctx.set_user(user);
        auth_ctx.set_tenant(tenant);
        for (const auto& role : roles) {
            auth_ctx.add_roles(role);
        }
        return auth_ctx;
    }

    // Directory operations
    bool make_directory(const std::string& parent_uid, const std::string& name, const std::string& user) {
        MakeDirectoryRequest request;
        request.set_parent_uid(parent_uid);
        request.set_name(name);
        *request.mutable_auth() = create_auth_context(user);
        request.set_permissions(0755);

        MakeDirectoryResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->MakeDirectory(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Created directory '" << name << "' with UID: " << response.uid() << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to create directory '" << name << "': " << response.error() << std::endl;
            return false;
        }
    }

    bool remove_directory(const std::string& uid, const std::string& user) {
        RemoveDirectoryRequest request;
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user);

        RemoveDirectoryResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->RemoveDirectory(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Removed directory with UID: " << uid << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to remove directory '" << uid << "': " << response.error() << std::endl;
            return false;
        }
    }

    bool list_directory(const std::string& uid, const std::string& user) {
        ListDirectoryRequest request;
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user);

        ListDirectoryResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->ListDirectory(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "Contents of directory (UID: " << uid << "):" << std::endl;
            for (const auto& entry : response.entries()) {
                std::string type_str = "FILE";
                if (entry.type() == FileType::REGULAR_FILE) {
                    type_str = "FILE";
                } else if (entry.type() == FileType::DIRECTORY) {
                    type_str = "DIR";
                } else if (entry.type() == FileType::SYMLINK) {
                    type_str = "LINK";
                }
                
                std::cout << "  [" << type_str << "] " << entry.name() << " (UID: " << entry.uid() << ")" << std::endl;
            }
            return true;
        } else {
            std::cout << "✗ Failed to list directory '" << uid << "': " << response.error() << std::endl;
            return false;
        }
    }

    // File operations
    bool touch(const std::string& parent_uid, const std::string& name, const std::string& user) {
        TouchRequest request;
        request.set_parent_uid(parent_uid);
        request.set_name(name);
        *request.mutable_auth() = create_auth_context(user);

        TouchResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->Touch(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Created file '" << name << "' with UID: " << response.uid() << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to create file '" << name << "': " << response.error() << std::endl;
            return false;
        }
    }

    bool remove_file(const std::string& uid, const std::string& user) {
        RemoveFileRequest request;
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user);

        RemoveFileResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->RemoveFile(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Removed file with UID: " << uid << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to remove file '" << uid << "': " << response.error() << std::endl;
            return false;
        }
    }

    std::vector<uint8_t> get_file(const std::string& uid, const std::string& user) {
        GetFileRequest request;
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user);

        GetFileResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GetFile(&context, request, &response);

        if (status.ok() && response.success()) {
            std::string data_str = response.data();
            std::vector<uint8_t> data(data_str.begin(), data_str.end());
            std::cout << "✓ Retrieved file '" << uid << "' (" << data.size() << " bytes)" << std::endl;
            return data;
        } else {
            std::cout << "✗ Failed to get file '" << uid << "': " << response.error() << std::endl;
            return std::vector<uint8_t>();  // Return empty vector on error
        }
    }

    bool put_file(const std::string& uid, const std::vector<uint8_t>& data, const std::string& user) {
        PutFileRequest request;
        request.set_uid(uid);
        std::string data_str(data.begin(), data.end());
        request.set_data(data_str);
        *request.mutable_auth() = create_auth_context(user);

        PutFileResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->PutFile(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Uploaded file to UID: " << uid << " (" << data.size() << " bytes)" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to upload file to '" << uid << "': " << response.error() << std::endl;
            return false;
        }
    }

    // Stat operation
    bool stat(const std::string& uid, const std::string& user) {
        StatRequest request;
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user);

        StatResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->Stat(&context, request, &response);

        if (status.ok() && response.success()) {
            const auto& info = response.info();
            std::cout << "File Info for UID: " << info.uid() << std::endl;
            std::cout << "  Name: " << info.name() << std::endl;
            std::cout << "  Type: ";
            
            switch(info.type()) {
                case FileType::REGULAR_FILE:
                    std::cout << "REGULAR_FILE";
                    break;
                case FileType::DIRECTORY:
                    std::cout << "DIRECTORY";
                    break;
                case FileType::SYMLINK:
                    std::cout << "SYMLINK";
                    break;
                default:
                    std::cout << "UNKNOWN";
                    break;
            }
            
            std::cout << std::endl;
            std::cout << "  Size: " << info.size() << " bytes" << std::endl;
            std::cout << "  Owner: " << info.owner() << std::endl;
            std::cout << "  Permissions: " << info.permissions() << std::endl;
            
            // For timestamp values
            std::cout << "  Created: " << info.created_at() << std::endl;
            std::cout << "  Modified: " << info.modified_at() << std::endl;
            std::cout << "  Version: " << info.version() << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to get file info for '" << uid << "': " << response.error() << std::endl;
            return false;
        }
    }

    // ACL operations
    bool grant_permission(const std::string& resource_uid, const std::string& principal, Permission permission, const std::string& user) {
        GrantPermissionRequest request;
        request.set_resource_uid(resource_uid);
        request.set_principal(principal);
        request.set_permission(permission);
        *request.mutable_auth() = create_auth_context(user);

        GrantPermissionResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GrantPermission(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Granted permission to '" << principal << "' on resource '" << resource_uid << "'" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to grant permission: " << response.error() << std::endl;
            return false;
        }
    }

    bool revoke_permission(const std::string& resource_uid, const std::string& principal, Permission permission, const std::string& user) {
        RevokePermissionRequest request;
        request.set_resource_uid(resource_uid);
        request.set_principal(principal);
        request.set_permission(permission);
        *request.mutable_auth() = create_auth_context(user);

        RevokePermissionResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->RevokePermission(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Revoked permission from '" << principal << "' on resource '" << resource_uid << "'" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to revoke permission: " << response.error() << std::endl;
            return false;
        }
    }

    bool check_permission(const std::string& resource_uid, const std::string& user, Permission required_permission) {
        CheckPermissionRequest request;
        request.set_resource_uid(resource_uid);
        request.set_required_permission(required_permission);
        *request.mutable_auth() = create_auth_context(user);

        CheckPermissionResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->CheckPermission(&context, request, &response);

        if (status.ok() && response.success()) {
            std::string perm_str = "UNKNOWN";
            switch(required_permission) {
                case Permission::READ_PERMISSION:
                    perm_str = "READ";
                    break;
                case Permission::WRITE_PERMISSION:
                    perm_str = "WRITE";
                    break;
                case Permission::EXECUTE_PERMISSION:
                    perm_str = "EXECUTE";
                    break;
            }
            
            if (response.has_permission()) {
                std::cout << "✓ User '" << user << "' has " << perm_str << " permission on resource '" << resource_uid << "'" << std::endl;
            } else {
                std::cout << "✗ User '" << user << "' does not have " << perm_str << " permission on resource '" << resource_uid << "'" << std::endl;
            }
            return response.success();
        } else {
            std::cout << "✗ Failed to check permission: " << response.error() << std::endl;
            return false;
        }
    }
};

} // namespace fileengine

int main(int argc, char** argv) {
    // Parse command line arguments
    if (argc < 2) {
        std::cout << "FileEngine CLI Client" << std::endl;
        std::cout << "Usage: " << argv[0] << " <command> [args...]" << std::endl;
        std::cout << std::endl;
        std::cout << "Commands:" << std::endl;
        std::cout << "  connect <server_address>    - Connect to gRPC server (default: localhost:50051)" << std::endl;
        std::cout << "  mkdir <parent_uid> <name>   - Create a directory" << std::endl;
        std::cout << "  ls <dir_uid>               - List directory contents" << std::endl;
        std::cout << "  touch <parent_uid> <name>  - Create an empty file" << std::endl;
        std::cout << "  rm <uid>                   - Remove file" << std::endl;
        std::cout << "  stat <uid>                 - Get file or directory info" << std::endl;
        std::cout << "  put <uid> <file_path>      - Upload file to specified UID" << std::endl;
        std::cout << "  get <uid> <output_path>    - Download file from specified UID" << std::endl;
        std::cout << "  grant <resource_uid> <user> <perm> - Grant permission (r/w/x)" << std::endl;
        std::cout << "  revoke <resource_uid> <user> <perm> - Revoke permission (r/w/x)" << std::endl;
        std::cout << "  check <resource_uid> <user> <perm> - Check permission (r/w/x)" << std::endl;
        return 0;
    }

    std::string server_address = "localhost:50051";
    // Allow overriding server address if first argument is "connect"
    if (argc > 2 && std::string(argv[1]) == "connect") {
        server_address = argv[2];
        std::cout << "Connecting to server: " << server_address << std::endl;
    } else {
        std::cout << "Connecting to server: " << server_address << std::endl;
    }

    auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
    fileengine::FileEngineClient client(channel);

    std::string command = argv[1];

    if (command == "connect" && argc > 2) {
        std::cout << "Already connected to: " << server_address << std::endl;
    }
    else if (command == "mkdir" && argc == 4) {
        client.make_directory(argv[2], argv[3], "cli_user");
    }
    else if (command == "ls" && argc == 3) {
        client.list_directory(argv[2], "cli_user");
    }
    else if (command == "touch" && argc == 4) {
        client.touch(argv[2], argv[3], "cli_user");
    }
    else if (command == "rm" && argc == 3) {
        client.remove_file(argv[2], "cli_user");
    }
    else if (command == "stat" && argc == 3) {
        client.stat(argv[2], "cli_user");
    }
    else if (command == "put" && argc == 4) {
        // Read file from disk
        std::ifstream file(argv[3], std::ios::binary);
        if (file.is_open()) {
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                                      std::istreambuf_iterator<char>());
            client.put_file(argv[2], data, "cli_user");
        } else {
            std::cout << "✗ Could not open file: " << argv[3] << std::endl;
        }
    }
    else if (command == "get" && argc == 4) {
        auto data = client.get_file(argv[2], "cli_user");
        if (!data.empty()) {
            std::ofstream file(argv[3], std::ios::binary);
            if (file.is_open()) {
                file.write(reinterpret_cast<const char*>(data.data()), data.size());
                std::cout << "✓ Saved file to: " << argv[3] << std::endl;
            } else {
                std::cout << "✗ Could not save to file: " << argv[3] << std::endl;
            }
        }
    }
    else if (command == "grant" && argc == 5) {
        fileengine_service::Permission perm;
        std::string perm_arg = argv[4];
        if (perm_arg == "r") {
            perm = fileengine_service::Permission::READ_PERMISSION;
        } else if (perm_arg == "w") {
            perm = fileengine_service::Permission::WRITE_PERMISSION;
        } else if (perm_arg == "x") {
            perm = fileengine_service::Permission::EXECUTE_PERMISSION;
        } else {
            std::cout << "✗ Invalid permission. Use r, w, or x." << std::endl;
            return 1;
        }

        client.grant_permission(argv[2], argv[3], perm, "cli_user");
    }
    else if (command == "revoke" && argc == 5) {
        fileengine_service::Permission perm;
        std::string perm_arg = argv[4];
        if (perm_arg == "r") {
            perm = fileengine_service::Permission::READ_PERMISSION;
        } else if (perm_arg == "w") {
            perm = fileengine_service::Permission::WRITE_PERMISSION;
        } else if (perm_arg == "x") {
            perm = fileengine_service::Permission::EXECUTE_PERMISSION;
        } else {
            std::cout << "✗ Invalid permission. Use r, w, or x." << std::endl;
            return 1;
        }

        client.revoke_permission(argv[2], argv[3], perm, "cli_user");
    }
    else if (command == "check" && argc == 5) {
        fileengine_service::Permission perm;
        std::string perm_arg = argv[4];
        if (perm_arg == "r") {
            perm = fileengine_service::Permission::READ_PERMISSION;
        } else if (perm_arg == "w") {
            perm = fileengine_service::Permission::WRITE_PERMISSION;
        } else if (perm_arg == "x") {
            perm = fileengine_service::Permission::EXECUTE_PERMISSION;
        } else {
            std::cout << "✗ Invalid permission. Use r, w, or x." << std::endl;
            return 1;
        }

        client.check_permission(argv[2], argv[3], perm);
    }
    else {
        std::cout << "✗ Invalid command or wrong number of arguments." << std::endl;
        std::cout << "Use '" << argv[0] << "' without arguments to see help." << std::endl;
        return 1;
    }

    return 0;
}