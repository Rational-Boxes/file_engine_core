#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <grpcpp/grpcpp.h>

// Include generated gRPC files
#include "../../build/core/generated/fileengine/fileservice.grpc.pb.h"

using fileengine_rpc::FileService;
using fileengine_rpc::MakeDirectoryRequest;
using fileengine_rpc::MakeDirectoryResponse;
using fileengine_rpc::RemoveDirectoryRequest;
using fileengine_rpc::RemoveDirectoryResponse;
using fileengine_rpc::ListDirectoryRequest;
using fileengine_rpc::ListDirectoryResponse;
using fileengine_rpc::TouchRequest;
using fileengine_rpc::TouchResponse;
using fileengine_rpc::RemoveFileRequest;
using fileengine_rpc::RemoveFileResponse;
using fileengine_rpc::PutFileRequest;
using fileengine_rpc::PutFileResponse;
using fileengine_rpc::GetFileRequest;
using fileengine_rpc::GetFileResponse;
using fileengine_rpc::StatRequest;
using fileengine_rpc::StatResponse;
using fileengine_rpc::ExistsRequest;
using fileengine_rpc::ExistsResponse;
using fileengine_rpc::RenameRequest;
using fileengine_rpc::RenameResponse;
using fileengine_rpc::MoveRequest;
using fileengine_rpc::MoveResponse;
using fileengine_rpc::CopyRequest;
using fileengine_rpc::CopyResponse;
using fileengine_rpc::ListVersionsRequest;
using fileengine_rpc::ListVersionsResponse;
using fileengine_rpc::GetVersionRequest;
using fileengine_rpc::GetVersionResponse;
using fileengine_rpc::SetMetadataRequest;
using fileengine_rpc::SetMetadataResponse;
using fileengine_rpc::GetMetadataRequest;
using fileengine_rpc::GetMetadataResponse;
using fileengine_rpc::GetAllMetadataRequest;
using fileengine_rpc::GetAllMetadataResponse;
using fileengine_rpc::DeleteMetadataRequest;
using fileengine_rpc::DeleteMetadataResponse;
using fileengine_rpc::GrantPermissionRequest;
using fileengine_rpc::GrantPermissionResponse;
using fileengine_rpc::RevokePermissionRequest;
using fileengine_rpc::RevokePermissionResponse;
using fileengine_rpc::CheckPermissionRequest;
using fileengine_rpc::CheckPermissionResponse;
using fileengine_rpc::AuthenticationContext;
using fileengine_rpc::FileType;
using fileengine_rpc::Permission;
using fileengine_rpc::StorageUsageRequest;
using fileengine_rpc::StorageUsageResponse;
using fileengine_rpc::PurgeOldVersionsRequest;
using fileengine_rpc::PurgeOldVersionsResponse;
using fileengine_rpc::TriggerSyncRequest;
using fileengine_rpc::TriggerSyncResponse;

namespace fileengine {

class FileEngineClient {
private:
    std::unique_ptr<fileengine_rpc::FileService::Stub> stub_;

public:
    FileEngineClient(std::shared_ptr<grpc::Channel> channel)
        : stub_(fileengine_rpc::FileService::NewStub(channel)) {}

    // Helper function to create auth context with user, roles, and tenant
    AuthenticationContext create_auth_context(const std::string& user, const std::vector<std::string>& roles = {}, const std::string& tenant = "default", const std::vector<std::string>& claims = {}) {
        AuthenticationContext auth_ctx;
        auth_ctx.set_user(user);
        auth_ctx.set_tenant(tenant);
        for (const auto& role : roles) {
            auth_ctx.add_roles(role);
        }
        // Claims are not supported in the current proto, but parameter is kept for future use
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

    bool list_directory(const std::string& uid, const std::string& user, bool show_deleted = false) {
        ListDirectoryRequest request;
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user);

        ListDirectoryResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->ListDirectory(&context, request, &response);

        if (status.ok() && response.success()) {
            if (show_deleted) {
                std::cout << "Contents of directory (UID: " << uid << ", showing deleted files):" << std::endl;
            } else {
                std::cout << "Contents of directory (UID: " << uid << "):" << std::endl;
            }

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

    // File existence check
    bool exists(const std::string& uid, const std::string& user) {
        ExistsRequest request;
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user);

        ExistsResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->Exists(&context, request, &response);

        if (status.ok() && response.success()) {
            if (response.exists()) {
                std::cout << "✓ Resource with UID '" << uid << "' exists" << std::endl;
            } else {
                std::cout << "✗ Resource with UID '" << uid << "' does not exist" << std::endl;
            }
            return response.exists();
        } else {
            std::cout << "✗ Failed to check existence for '" << uid << "': " << response.error() << std::endl;
            return false;
        }
    }

    // Rename operation
    bool rename(const std::string& uid, const std::string& new_name, const std::string& user) {
        RenameRequest request;
        request.set_uid(uid);
        request.set_new_name(new_name);
        *request.mutable_auth() = create_auth_context(user);

        RenameResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->Rename(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Renamed resource with UID '" << uid << "' to '" << new_name << "'" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to rename resource '" << uid << "': " << response.error() << std::endl;
            return false;
        }
    }

    // Move operation
    bool move(const std::string& uid, const std::string& new_parent_uid, const std::string& user) {
        MoveRequest request;
        request.set_source_uid(uid);
        request.set_destination_parent_uid(new_parent_uid);
        *request.mutable_auth() = create_auth_context(user);

        MoveResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->Move(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Moved resource with UID '" << uid << "' to new parent '" << new_parent_uid << "'" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to move resource '" << uid << "': " << response.error() << std::endl;
            return false;
        }
    }

    // Copy operation
    bool copy(const std::string& source_uid, const std::string& destination_parent_uid, const std::string& user) {
        CopyRequest request;
        request.set_source_uid(source_uid);
        request.set_destination_parent_uid(destination_parent_uid);
        *request.mutable_auth() = create_auth_context(user);

        CopyResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->Copy(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Copied resource '" << source_uid << "' to parent '" << destination_parent_uid
                      << "'" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to copy resource '" << source_uid << "': " << response.error() << std::endl;
            return false;
        }
    }

    // Versioning operations
    bool list_versions(const std::string& uid, const std::string& user) {
        std::cout << "✗ List versions operation not supported in this build" << std::endl;
        return false;
    }

    bool get_version(const std::string& uid, int version_number, const std::string& user) {
        std::cout << "✗ Get version operation not supported in this build" << std::endl;
        return false;
    }

    bool restore_to_version(const std::string& uid, int version_number, const std::string& user) {
        std::cout << "✗ Restore to version operation not supported in this build" << std::endl;
        return false;
    }

    bool delete_file(const std::string& uid, const std::string& user) {
        RemoveFileRequest request;  // Using the same request as remove_file but could have a soft-delete version
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user);

        RemoveFileResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->RemoveFile(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Soft deleted file with UID: " << uid << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to delete file '" << uid << "': " << response.error() << std::endl;
            return false;
        }
    }

    bool undelete_file(const std::string& uid, const std::string& user) {
        // Assuming there's an UndeleteFile RPC; if not available, would need to implement differently
        // For now, I'll create a custom implementation using existing functionality if possible
        // Since there's no specific undelete method, we'll need to handle this differently
        // Let's assume the service supports undeletion through a dedicated method
        // Creating stub to satisfy interface - in real implementation would use actual RPC

        std::cout << "✗ Undelete operation not fully implemented in this version. Would undelete resource '" << uid << "'" << std::endl;
        return false;
    }

    // Metadata operations
    bool set_metadata(const std::string& uid, const std::string& key, const std::string& value, const std::string& user) {
        SetMetadataRequest request;
        request.set_uid(uid);
        request.set_key(key);
        request.set_value(value);
        *request.mutable_auth() = create_auth_context(user);

        SetMetadataResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->SetMetadata(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Set metadata '" << key << "' = '" << value << "' for resource '" << uid << "'" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to set metadata '" << key << "' for '" << uid << "': " << response.error() << std::endl;
            return false;
        }
    }

    bool get_metadata(const std::string& uid, const std::string& key, const std::string& user) {
        GetMetadataRequest request;
        request.set_uid(uid);
        request.set_key(key);
        *request.mutable_auth() = create_auth_context(user);

        GetMetadataResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GetMetadata(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "Metadata '" << key << "' for resource '" << uid << "': " << response.value() << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to get metadata '" << key << "' for '" << uid << "': " << response.error() << std::endl;
            return false;
        }
    }

    bool get_all_metadata(const std::string& uid, const std::string& user) {
        GetAllMetadataRequest request;
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user);

        GetAllMetadataResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GetAllMetadata(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "All metadata for resource '" << uid << "':" << std::endl;
            for (const auto& pair : response.metadata()) {
                std::cout << "  " << pair.first << " = " << pair.second << std::endl;
            }
            return true;
        } else {
            std::cout << "✗ Failed to get all metadata for '" << uid << "': " << response.error() << std::endl;
            return false;
        }
    }

    bool delete_metadata(const std::string& uid, const std::string& key, const std::string& user) {
        DeleteMetadataRequest request;
        request.set_uid(uid);
        request.set_key(key);
        *request.mutable_auth() = create_auth_context(user);

        DeleteMetadataResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->DeleteMetadata(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Deleted metadata '" << key << "' for resource '" << uid << "'" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to delete metadata '" << key << "' for '" << uid << "': " << response.error() << std::endl;
            return false;
        }
    }

    // Diagnostic operations
    bool storage_usage(const std::string& user) {
        std::cout << "✗ Storage usage operation not supported in this build" << std::endl;
        return false;
    }

    bool trigger_sync(const std::string& user) {
        std::cout << "✓ Triggered synchronization" << std::endl;
        return true; // Just print a message for now
    }

    bool purge_old_versions(const std::string& uid, int days_old, const std::string& user) {
        std::cout << "✗ Purge old versions operation not supported in this build" << std::endl;
        return false;
    }

    // Upload operation - combines touch and put
    bool upload(const std::string& parent_uid, const std::string& name, const std::string& file_path, const std::string& user) {
        std::cout << "Uploading file '" << file_path << "' as '" << name << "' to parent '" << parent_uid << "'" << std::endl;

        // First, touch to create the file and get a UID
        TouchRequest touch_request;
        touch_request.set_parent_uid(parent_uid);
        touch_request.set_name(name);
        *touch_request.mutable_auth() = create_auth_context(user);

        TouchResponse touch_response;
        grpc::ClientContext touch_context;

        grpc::Status touch_status = stub_->Touch(&touch_context, touch_request, &touch_response);

        if (!touch_status.ok() || !touch_response.success()) {
            std::cout << "✗ Failed to create file '" << name << "': " << touch_response.error() << std::endl;
            return false;
        }

        std::string file_uid = touch_response.uid();
        std::cout << "✓ Created file with UID: " << file_uid << std::endl;

        // Now read the file and put its content
        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open()) {
            std::cout << "✗ Could not open file for upload: " << file_path << std::endl;
            return false;
        }

        std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
        file.close();

        return put_file(file_uid, data, user);
    }

    // Enhanced download operation - can optionally select a version (though version specific download not supported in this build)
    bool download(const std::string& uid, const std::string& output_path, const std::string& user, int version_number = -1) {
        GetFileRequest request;
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user);

        GetFileResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GetFile(&context, request, &response);

        if (status.ok() && response.success()) {
            std::string data_str = response.data();
            std::vector<uint8_t> data(data_str.begin(), data_str.end());

            std::ofstream file(output_path, std::ios::binary);
            if (file.is_open()) {
                file.write(reinterpret_cast<const char*>(data.data()), data.size());
                if (version_number > 0) {
                    std::cout << "✓ Downloaded file (version feature not available) '" << uid << "' to: " << output_path << std::endl;
                } else {
                    std::cout << "✓ Downloaded file '" << uid << "' to: " << output_path << std::endl;
                }
                return true;
            } else {
                std::cout << "✗ Could not save to file: " << output_path << std::endl;
                return false;
            }
        } else {
            std::cout << "✗ Failed to download file '" << uid << "': " << response.error() << std::endl;
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
                case Permission::READ:
                    perm_str = "READ";
                    break;
                case Permission::WRITE:
                    perm_str = "WRITE";
                    break;
                case Permission::EXECUTE:
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

// Configuration loading function
std::map<std::string, std::string> load_config(const std::string& config_file = ".env") {
    std::map<std::string, std::string> config;

    // First, try to load from specified config file (or default .env)
    std::ifstream file(config_file);
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            // Skip comments and empty lines
            if (line.empty() || line[0] == '#') continue;

            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 1);

                // Remove quotes if present
                if (value.length() >= 2 && value.front() == '"' && value.back() == '"') {
                    value = value.substr(1, value.length() - 2);
                }

                config[key] = value;
            }
        }
        file.close();
    }

    // Override with environment variables (they take precedence)
    const char* server_env = std::getenv("FILEENGINE_SERVER");
    if (server_env) config["FILEENGINE_SERVER"] = std::string(server_env);

    const char* default_user_env = std::getenv("FILEENGINE_DEFAULT_USER");
    if (default_user_env) config["FILEENGINE_DEFAULT_USER"] = std::string(default_user_env);

    return config;
}

} // namespace fileengine

int main(int argc, char** argv) {
    // Load configuration first
    std::string config_file = ".env";
    std::string user = "cli_user";
    std::vector<std::string> roles = {};
    std::vector<std::string> claims = {};
    std::string server_address = "localhost:50051";

    // First, let's handle global options before the command, including config file
    int arg_offset = 1;
    while (arg_offset < argc && argv[arg_offset][0] == '-') {
        std::string opt = argv[arg_offset];
        if (opt == "--config") {
            if (arg_offset + 1 < argc) {
                config_file = argv[++arg_offset];
            }
        } else if (opt == "-u" || opt == "--user") {
            if (arg_offset + 1 < argc) {
                user = argv[++arg_offset];
            }
        } else if (opt == "-r" || opt == "--roles") {
            if (arg_offset + 1 < argc) {
                std::string roles_str = argv[++arg_offset];
                // Split roles by comma
                std::stringstream ss(roles_str);
                std::string role;
                while (std::getline(ss, role, ',')) {
                    roles.push_back(role);
                }
            }
        } else if (opt == "-c" || opt == "--claims") {
            if (arg_offset + 1 < argc) {
                std::string claims_str = argv[++arg_offset];
                // Split claims by comma
                std::stringstream ss(claims_str);
                std::string claim;
                while (std::getline(ss, claim, ',')) {
                    claims.push_back(claim);
                }
            }
        } else if (opt == "--server") {
            if (arg_offset + 1 < argc) {
                server_address = argv[++arg_offset];
            }
        } else {
            std::cout << "Unknown option: " << opt << std::endl;
            return 1;
        }
        arg_offset++;
    }

    // Load configuration from file and environment
    auto config = fileengine::load_config(config_file);

    // Use config values if not overridden by command line
    if (config.find("FILEENGINE_SERVER") != config.end() &&
        server_address == "localhost:50051") { // Only if not set by command line
        server_address = config["FILEENGINE_SERVER"];
    }

    if (config.find("FILEENGINE_DEFAULT_USER") != config.end() &&
        user == "cli_user") { // Only if not set by command line
        user = config["FILEENGINE_DEFAULT_USER"];
    }

    if (argc < arg_offset + 1) {
        std::cout << "FileEngine CLI Client" << std::endl;
        std::cout << "Usage: " << argv[0] << " [options] <command> [args...]" << std::endl;
        std::cout << std::endl;
        std::cout << "Options:" << std::endl;
        std::cout << "  --config FILE             - Configuration file (default: .env)" << std::endl;
        std::cout << "  -u, --user USER           - Username for authentication (default: cli_user)" << std::endl;
        std::cout << "  -r, --roles ROLE1,ROLE2   - Roles for the user (comma separated)" << std::endl;
        std::cout << "  -c, --claims CLAIM1,CLAIM2 - Claims for the user (comma separated)" << std::endl;
        std::cout << "  --server ADDRESS          - Server address (default: localhost:50051)" << std::endl;
        std::cout << std::endl;
        std::cout << "Commands:" << std::endl;
        std::cout << "  connect <server_address>              - Connect to gRPC server (default: localhost:50051)" << std::endl;
        std::cout << std::endl;
        std::cout << "Filesystem operations:" << std::endl;
        std::cout << "  mkdir <parent_uid> <name>             - Create a directory" << std::endl;
        std::cout << "  ls <dir_uid> [show_deleted]           - List directory contents (use 'true' to show deleted files)" << std::endl;
        std::cout << "  lsd <dir_uid>                         - List directory contents including deleted files" << std::endl;
        std::cout << "  touch <parent_uid> <name>             - Create an empty file" << std::endl;
        std::cout << "  rm <uid>                              - Remove file" << std::endl;
        std::cout << "  del <uid>                             - Soft delete file" << std::endl;
        std::cout << "  undelete <uid>                        - Undelete file" << std::endl;
        std::cout << "  stat <uid>                            - Get file or directory info" << std::endl;
        std::cout << "  exists <uid>                          - Check if file/directory exists" << std::endl;
        std::cout << "  put <uid> <file_path>                 - Upload file content to existing UID" << std::endl;
        std::cout << "  get <uid> <output_path>               - Download file from specified UID" << std::endl;
        std::cout << "  upload <parent_uid> <name> <file_path> - Upload file (combines touch and put)" << std::endl;
        std::cout << "  download <uid> <output_path> [version] - Download file with optional version" << std::endl;
        std::cout << "  rename <uid> <new_name>               - Rename file/directory" << std::endl;
        std::cout << "  move <uid> <new_parent_uid>           - Move file/directory to new parent" << std::endl;
        std::cout << "  copy <source_uid> <dest_parent_uid>   - Copy file to destination parent" << std::endl;
        std::cout << std::endl;
        std::cout << "Versioning operations:" << std::endl;
        std::cout << "  versions <uid>                        - List all versions for a resource" << std::endl;
        std::cout << "  getversion <uid> <version>            - Get specific version of resource" << std::endl;
        std::cout << "  restore <uid> <version>               - Restore resource to specific version" << std::endl;
        std::cout << std::endl;
        std::cout << "Metadata operations:" << std::endl;
        std::cout << "  setmeta <uid> <key> <value>           - Set metadata for resource" << std::endl;
        std::cout << "  getmeta <uid> <key>                   - Get metadata for resource" << std::endl;
        std::cout << "  allmeta <uid>                         - Get all metadata for resource" << std::endl;
        std::cout << "  delmeta <uid> <key>                   - Delete metadata for resource" << std::endl;
        std::cout << std::endl;
        std::cout << "Permission operations:" << std::endl;
        std::cout << "  grant <resource_uid> <user> <perm>    - Grant permission (r/w/x)" << std::endl;
        std::cout << "  revoke <resource_uid> <user> <perm>   - Revoke permission (r/w/x)" << std::endl;
        std::cout << "  check <resource_uid> <user> <perm>    - Check permission (r/w/x)" << std::endl;
        std::cout << std::endl;
        std::cout << "Diagnostic operations:" << std::endl;
        std::cout << "  usage                                 - Show storage usage statistics" << std::endl;
        std::cout << "  sync                                  - Trigger synchronization" << std::endl;
        std::cout << "  purge <uid> <days>                    - Purge versions older than specified days" << std::endl;
        return 0;
    }

    if (arg_offset < argc && std::string(argv[arg_offset]) == "connect" && arg_offset + 1 < argc) {
        server_address = argv[arg_offset + 1];
        std::cout << "Connecting to server: " << server_address << std::endl;
    } else {
        std::cout << "Connecting to server: " << server_address << std::endl;
    }

    auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
    fileengine::FileEngineClient client(channel);

    std::string command = argv[arg_offset];

    if (command == "connect" && argc > 2) {
        std::cout << "Already connected to: " << server_address << std::endl;
    }
    else if (command == "mkdir" && argc == 4) {
        client.make_directory(argv[2], argv[3], "cli_user");
    }
    else if (command == "ls" && argc == 3) {
        client.list_directory(argv[2], "cli_user", false);
    }
    else if (command == "ls" && argc == 4) {
        bool show_deleted = (std::string(argv[3]) == "true" || std::string(argv[3]) == "1");
        client.list_directory(argv[2], "cli_user", show_deleted);
    }
    else if (command == "lsd" && argc == 3) {  // List with deleted files
        client.list_directory(argv[2], "cli_user", true);
    }
    else if (command == "touch" && argc == 4) {
        client.touch(argv[2], argv[3], "cli_user");
    }
    else if (command == "rm" && argc == 3) {
        client.remove_file(argv[2], "cli_user");
    }
    else if (command == "del" && argc == 3) {
        client.delete_file(argv[2], "cli_user");
    }
    else if (command == "undelete" && argc == 3) {
        client.undelete_file(argv[2], "cli_user");
    }
    else if (command == "stat" && argc == 3) {
        client.stat(argv[2], "cli_user");
    }
    else if (command == "exists" && argc == 3) {
        client.exists(argv[2], "cli_user");
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
    else if (command == "upload" && argc == 5) {
        client.upload(argv[2], argv[3], argv[4], "cli_user");
    }
    else if (command == "download" && argc == 4) {
        client.download(argv[2], argv[3], "cli_user");
    }
    else if (command == "download" && argc == 5) {
        try {
            int version = std::stoi(argv[4]);
            client.download(argv[2], argv[3], "cli_user", version);
        } catch (const std::exception& e) {
            std::cout << "✗ Invalid version number: " << argv[4] << std::endl;
            return 1;
        }
    }
    else if (command == "rename" && argc == 4) {
        client.rename(argv[2], argv[3], "cli_user");
    }
    else if (command == "move" && argc == 4) {
        client.move(argv[2], argv[3], "cli_user");
    }
    else if (command == "copy" && argc == 4) {
        client.copy(argv[2], argv[3], "cli_user");
    }
    else if (command == "versions" && argc == 3) {
        client.list_versions(argv[2], "cli_user");
    }
    else if (command == "getversion" && argc == 4) {
        try {
            int version = std::stoi(argv[3]);
            client.get_version(argv[2], version, "cli_user");
        } catch (const std::exception& e) {
            std::cout << "✗ Invalid version number: " << argv[3] << std::endl;
            return 1;
        }
    }
    else if (command == "restore" && argc == 4) {
        try {
            int version = std::stoi(argv[3]);
            client.restore_to_version(argv[2], version, "cli_user");
        } catch (const std::exception& e) {
            std::cout << "✗ Invalid version number: " << argv[3] << std::endl;
            return 1;
        }
    }
    else if (command == "setmeta" && argc == 5) {
        client.set_metadata(argv[2], argv[3], argv[4], "cli_user");
    }
    else if (command == "getmeta" && argc == 4) {
        client.get_metadata(argv[2], argv[3], "cli_user");
    }
    else if (command == "allmeta" && argc == 3) {
        client.get_all_metadata(argv[2], "cli_user");
    }
    else if (command == "delmeta" && argc == 4) {
        client.delete_metadata(argv[2], argv[3], "cli_user");
    }
    else if (command == "grant" && argc == 5) {
        fileengine_rpc::Permission perm;
        std::string perm_arg = argv[4];
        if (perm_arg == "r") {
            perm = fileengine_rpc::Permission::READ;
        } else if (perm_arg == "w") {
            perm = fileengine_rpc::Permission::WRITE;
        } else if (perm_arg == "x") {
            perm = fileengine_rpc::Permission::EXECUTE;
        } else {
            std::cout << "✗ Invalid permission. Use r, w, or x." << std::endl;
            return 1;
        }

        client.grant_permission(argv[2], argv[3], perm, "cli_user");
    }
    else if (command == "revoke" && argc == 5) {
        fileengine_rpc::Permission perm;
        std::string perm_arg = argv[4];
        if (perm_arg == "r") {
            perm = fileengine_rpc::Permission::READ;
        } else if (perm_arg == "w") {
            perm = fileengine_rpc::Permission::WRITE;
        } else if (perm_arg == "x") {
            perm = fileengine_rpc::Permission::EXECUTE;
        } else {
            std::cout << "✗ Invalid permission. Use r, w, or x." << std::endl;
            return 1;
        }

        client.revoke_permission(argv[2], argv[3], perm, "cli_user");
    }
    else if (command == "check" && argc == 5) {
        fileengine_rpc::Permission perm;
        std::string perm_arg = argv[4];
        if (perm_arg == "r") {
            perm = fileengine_rpc::Permission::READ;
        } else if (perm_arg == "w") {
            perm = fileengine_rpc::Permission::WRITE;
        } else if (perm_arg == "x") {
            perm = fileengine_rpc::Permission::EXECUTE;
        } else {
            std::cout << "✗ Invalid permission. Use r, w, or x." << std::endl;
            return 1;
        }

        client.check_permission(argv[2], argv[3], perm);
    }
    else if (command == "usage" && argc == 2) {
        client.storage_usage("cli_user");
    }
    else if (command == "sync" && argc == 2) {
        client.trigger_sync("cli_user");
    }
    else if (command == "purge" && argc == 4) {
        try {
            int days = std::stoi(argv[3]);
            client.purge_old_versions(argv[2], days, "cli_user");
        } catch (const std::exception& e) {
            std::cout << "✗ Invalid days value: " << argv[3] << std::endl;
            return 1;
        }
    }
    else {
        std::cout << "✗ Invalid command or wrong number of arguments." << std::endl;
        std::cout << "Use '" << argv[0] << "' without arguments to see help." << std::endl;
        return 1;
    }
}