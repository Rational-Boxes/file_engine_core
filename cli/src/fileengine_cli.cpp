#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <grpcpp/grpcpp.h>

// Include generated gRPC files (resolved via target_include_directories)
#include "fileservice.grpc.pb.h"

// Include logging functionality
#include "../include/logger.h"

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
using fileengine_rpc::RestoreToVersionRequest;
using fileengine_rpc::RestoreToVersionResponse;
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
// Role management types
using fileengine_rpc::CreateRoleRequest;
using fileengine_rpc::CreateRoleResponse;
using fileengine_rpc::DeleteRoleRequest;
using fileengine_rpc::DeleteRoleResponse;
using fileengine_rpc::AssignUserToRoleRequest;
using fileengine_rpc::AssignUserToRoleResponse;
using fileengine_rpc::RemoveUserFromRoleRequest;
using fileengine_rpc::RemoveUserFromRoleResponse;
using fileengine_rpc::GetRolesForUserRequest;
using fileengine_rpc::GetRolesForUserResponse;
using fileengine_rpc::GetUsersForRoleRequest;
using fileengine_rpc::GetUsersForRoleResponse;
using fileengine_rpc::GetAllRolesRequest;
using fileengine_rpc::GetAllRolesResponse;

namespace fileengine {

// Parse a single-letter permission code into the proto Permission enum.
// Returns false on unknown letters; caller is responsible for the error
// message. Letters were chosen to be mnemonic and pairwise-unique:
//   r READ, w WRITE, x EXECUTE
//   d DELETE, l LIST_DELETED, u UNDELETE
//   v VIEW_VERSIONS, b RETRIEVE_BACK_VERSION (b = "back"), s RESTORE_TO_VERSION
//   m MANAGE_ACL, i ACL_INHERIT
inline bool parse_perm_letter(const std::string& letter, Permission& out) {
    if (letter == "r") { out = Permission::READ; return true; }
    if (letter == "w") { out = Permission::WRITE; return true; }
    if (letter == "x") { out = Permission::EXECUTE; return true; }
    if (letter == "d") { out = Permission::DELETE; return true; }
    if (letter == "l") { out = Permission::LIST_DELETED; return true; }
    if (letter == "u") { out = Permission::UNDELETE; return true; }
    if (letter == "v") { out = Permission::VIEW_VERSIONS; return true; }
    if (letter == "b") { out = Permission::RETRIEVE_BACK_VERSION; return true; }
    if (letter == "s") { out = Permission::RESTORE_TO_VERSION; return true; }
    if (letter == "m") { out = Permission::MANAGE_ACL; return true; }
    if (letter == "i") { out = Permission::ACL_INHERIT; return true; }
    return false;
}

inline const char* perm_name(Permission p) {
    switch (p) {
        case Permission::READ: return "READ";
        case Permission::WRITE: return "WRITE";
        case Permission::EXECUTE: return "EXECUTE";
        case Permission::DELETE: return "DELETE";
        case Permission::LIST_DELETED: return "LIST_DELETED";
        case Permission::UNDELETE: return "UNDELETE";
        case Permission::VIEW_VERSIONS: return "VIEW_VERSIONS";
        case Permission::RETRIEVE_BACK_VERSION: return "RETRIEVE_BACK_VERSION";
        case Permission::RESTORE_TO_VERSION: return "RESTORE_TO_VERSION";
        case Permission::MANAGE_ACL: return "MANAGE_ACL";
        case Permission::ACL_INHERIT: return "ACL_INHERIT";
        default: return "UNKNOWN";
    }
}

class FileEngineClient {
private:
    std::unique_ptr<fileengine_rpc::FileService::Stub> stub_;
    std::vector<std::string> roles_;  // Roles passed from command line

public:
    FileEngineClient(std::shared_ptr<grpc::Channel> channel, const std::vector<std::string>& initial_roles = {})
        : stub_(fileengine_rpc::FileService::NewStub(channel)), roles_(initial_roles) {}

    // Helper function to create auth context with user, roles, and tenant
    AuthenticationContext create_auth_context(const std::string& user, const std::vector<std::string>& roles = {}, const std::string& tenant = "default", const std::vector<std::string>& claims = {}) {
        fileengine::Logger::trace("AuthContext", "Creating auth context for user: ", user, ", tenant: ", tenant);
        AuthenticationContext auth_ctx;
        auth_ctx.set_user(user);
        auth_ctx.set_tenant(tenant);
        for (const auto& role : roles) {
            auth_ctx.add_roles(role);
            fileengine::Logger::detail("AuthContext", "Added role: ", role);
        }
        fileengine::Logger::detail("AuthContext", "Auth context created successfully");
        // Claims are not supported in the current proto, but parameter is kept for future use
        return auth_ctx;
    }

    // Directory operations
    bool make_directory(const std::string& parent_uid, const std::string& name, const std::string& user, const std::string& tenant = "default") {
        fileengine::Logger::debug("Mkdir", "Attempting to create directory '", name, "' in parent '", parent_uid, "' for user '", user, "' in tenant '", tenant, "'");

        MakeDirectoryRequest request;
        request.set_parent_uid(parent_uid);
        request.set_name(name);
        fileengine::Logger::detail("Mkdir", "Set parent UID: ", parent_uid, ", name: ", name);

        *request.mutable_auth() = create_auth_context(user, roles_, tenant);
        request.set_permissions(0755);
        fileengine::Logger::detail("Mkdir", "Set permissions: 0755");

        MakeDirectoryResponse response;
        grpc::ClientContext context;
        fileengine::Logger::trace("Mkdir", "Created request and context, making gRPC call");

        grpc::Status status = stub_->MakeDirectory(&context, request, &response);
        fileengine::Logger::trace("Mkdir", "gRPC call completed with status: ", status.ok() ? "OK" : "ERROR");

        if (status.ok() && response.success()) {
            fileengine::Logger::debug("Mkdir", "Directory created successfully with UID: ", response.uid());
            std::cout << "✓ Created directory '" << name << "' with UID: " << response.uid() << " in tenant '" << tenant << "'" << std::endl;
            return true;
        } else {
            fileengine::Logger::debug("Mkdir", "Failed to create directory '", name, "', error: ", response.error());
            std::cout << "✗ Failed to create directory '" << name << "' in tenant '" << tenant << "': " << response.error();
            if (!status.ok()) {
                fileengine::Logger::detail("Mkdir", "gRPC error code: ", status.error_code(), ", message: ", status.error_message());
                std::cout << " (gRPC Status: " << status.error_code() << " - " << status.error_message() << ")";
            }
            std::cout << std::endl;
            return false;
        }
    }

    bool list_directory(const std::string& uid, const std::string& user, bool show_deleted = false, const std::string& tenant = "default") {
        fileengine::Logger::debug("ListDir", "Attempting to list directory with UID: ", uid, " for user: ", user, ", show_deleted: ", show_deleted ? "true" : "false", ", tenant: ", tenant);

        // Shared rendering for either response type (both expose .entries()).
        auto fmt_epoch = [](int64_t s) -> std::string {
            if (s <= 0) return "-";
            std::time_t t = static_cast<std::time_t>(s);
            std::tm tm{}; gmtime_r(&t, &tm);
            char buf[20]; std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
            return std::string(buf);
        };
        auto render = [&](const auto& response) -> bool {
            fileengine::Logger::debug("ListDir", "Directory listing successful, found ", response.entries_size(), " entries");
            if (show_deleted) {
                std::cout << "Contents of directory (UID: " << uid << ", showing deleted files) in tenant '" << tenant << "':" << std::endl;
            } else {
                std::cout << "Contents of directory (UID: " << uid << ") in tenant '" << tenant << "':" << std::endl;
            }
            for (const auto& entry : response.entries()) {
                std::string type_str = "FILE";
                if (entry.type() == FileType::DIRECTORY) {
                    type_str = "DIR";
                } else if (entry.type() == FileType::SYMLINK) {
                    type_str = "LINK";
                }
                fileengine::Logger::detail("ListDir", "Entry - Name: ", entry.name(), ", UID: ", entry.uid(), ", Type: ", type_str);
                std::cout << "  [" << type_str << "] " << entry.name() << " (UID: " << entry.uid() << ")"
                          << "  created " << fmt_epoch(entry.created_at()) << " by " << entry.created_by()
                          << ", modified " << fmt_epoch(entry.modified_at()) << " by " << entry.modified_by();
                if (entry.deleted()) std::cout << "  [deleted]";
                std::cout << std::endl;
            }
            return true;
        };

        grpc::ClientContext context;
        fileengine::Logger::trace("ListDir", "Created request and context, making gRPC call");

        // show_deleted must hit the dedicated RPC; plain ListDirectory filters
        // out soft-deleted entries server-side.
        if (show_deleted) {
            fileengine_rpc::ListDirectoryWithDeletedRequest request;
            request.set_uid(uid);
            *request.mutable_auth() = create_auth_context(user, roles_, tenant);
            fileengine_rpc::ListDirectoryWithDeletedResponse response;
            grpc::Status status = stub_->ListDirectoryWithDeleted(&context, request, &response);
            if (status.ok() && response.success()) {
                return render(response);
            }
            std::cout << "✗ Failed to list directory '" << uid << "' in tenant '" << tenant << "': " << response.error() << std::endl;
            return false;
        }

        ListDirectoryRequest request;
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);
        ListDirectoryResponse response;
        grpc::Status status = stub_->ListDirectory(&context, request, &response);
        fileengine::Logger::trace("ListDir", "gRPC call completed with status: ", status.ok() ? "OK" : "ERROR");
        if (status.ok() && response.success()) {
            return render(response);
        }
        fileengine::Logger::debug("ListDir", "Failed to list directory '", uid, "', error: ", response.error());
        std::cout << "✗ Failed to list directory '" << uid << "' in tenant '" << tenant << "': " << response.error() << std::endl;
        return false;
    }

    bool remove_directory(const std::string& uid, const std::string& user, const std::string& tenant = "default") {
        RemoveDirectoryRequest request;
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

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
    bool touch(const std::string& parent_uid, const std::string& name, const std::string& user, const std::string& tenant = "default") {
        fileengine::Logger::debug("Touch", "Attempting to create file '", name, "' in parent '", parent_uid, "' for user '", user, "' in tenant '", tenant, "'");

        TouchRequest request;
        request.set_parent_uid(parent_uid);
        request.set_name(name);
        fileengine::Logger::detail("Touch", "Set parent UID: ", parent_uid, ", name: ", name);

        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        TouchResponse response;
        grpc::ClientContext context;
        fileengine::Logger::trace("Touch", "Created request and context, making gRPC call");

        grpc::Status status = stub_->Touch(&context, request, &response);
        fileengine::Logger::trace("Touch", "gRPC call completed with status: ", status.ok() ? "OK" : "ERROR");

        if (status.ok() && response.success()) {
            fileengine::Logger::debug("Touch", "File created successfully with UID: ", response.uid());
            std::cout << "✓ Created file '" << name << "' with UID: " << response.uid() << " in tenant '" << tenant << "'" << std::endl;
            return true;
        } else {
            fileengine::Logger::debug("Touch", "Failed to create file '", name, "', error: ", response.error());
            std::cout << "✗ Failed to create file '" << name << "' in tenant '" << tenant << "': " << response.error() << std::endl;
            return false;
        }
    }

    bool remove_file(const std::string& uid, const std::string& user, const std::string& tenant = "default") {
        RemoveFileRequest request;
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

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

    std::vector<uint8_t> get_file(const std::string& uid, const std::string& user, const std::string& tenant = "default") {
        fileengine::Logger::debug("GetFile", "Attempting to retrieve file with UID: ", uid, " for user: ", user);

        GetFileRequest request;
        request.set_uid(uid);
        fileengine::Logger::detail("GetFile", "Set file UID: ", uid);

        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        GetFileResponse response;
        grpc::ClientContext context;
        fileengine::Logger::trace("GetFile", "Created request and context, making gRPC call");

        grpc::Status status = stub_->GetFile(&context, request, &response);
        fileengine::Logger::trace("GetFile", "gRPC call completed with status: ", status.ok() ? "OK" : "ERROR");

        if (status.ok() && response.success()) {
            std::string data_str = response.data();
            std::vector<uint8_t> data(data_str.begin(), data_str.end());
            fileengine::Logger::debug("GetFile", "File retrieved successfully, size: ", data.size(), " bytes");
            std::cout << "✓ Retrieved file '" << uid << "' (" << data.size() << " bytes)" << std::endl;
            return data;
        } else {
            fileengine::Logger::debug("GetFile", "Failed to retrieve file '", uid, "', error: ", response.error());
            std::cout << "✗ Failed to get file '" << uid << "': " << response.error() << std::endl;
            return std::vector<uint8_t>();  // Return empty vector on error
        }
    }

    bool put_file(const std::string& uid, const std::vector<uint8_t>& data, const std::string& user, const std::string& tenant = "default") {
        fileengine::Logger::debug("PutFile", "Attempting to upload file to UID: ", uid, " for user: ", user, ", size: ", data.size(), " bytes");

        PutFileRequest request;
        request.set_uid(uid);
        std::string data_str(data.begin(), data.end());
        request.set_data(data_str);
        fileengine::Logger::detail("PutFile", "Set file UID: ", uid, ", data size: ", data_str.size());

        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        PutFileResponse response;
        grpc::ClientContext context;
        fileengine::Logger::trace("PutFile", "Created request and context, making gRPC call");

        grpc::Status status = stub_->PutFile(&context, request, &response);
        fileengine::Logger::trace("PutFile", "gRPC call completed with status: ", status.ok() ? "OK" : "ERROR");

        if (status.ok() && response.success()) {
            fileengine::Logger::debug("PutFile", "File uploaded successfully to UID: ", uid, ", size: ", data.size(), " bytes");
            std::cout << "✓ Uploaded file to UID: " << uid << " (" << data.size() << " bytes)" << std::endl;
            return true;
        } else {
            fileengine::Logger::debug("PutFile", "Failed to upload file to UID '", uid, "', error: ", response.error());
            std::cout << "✗ Failed to upload file to '" << uid << "': " << response.error() << std::endl;
            return false;
        }
    }

    // Stat operation
    bool stat(const std::string& uid, const std::string& user, const std::string& tenant = "default") {
        StatRequest request;
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        StatResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->Stat(&context, request, &response);

        if (status.ok() && response.success()) {
            const auto& info = response.info();
            std::cout << "File Info for UID: " << info.uid() << " in tenant '" << tenant << "':" << std::endl;
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
            std::cout << "✗ Failed to get file info for '" << uid << "' in tenant '" << tenant << "': " << response.error() << std::endl;
            return false;
        }
    }

    // File existence check
    bool exists(const std::string& uid, const std::string& user, const std::string& tenant = "default") {
        ExistsRequest request;
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        ExistsResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->Exists(&context, request, &response);

        if (status.ok() && response.success()) {
            if (response.exists()) {
                std::cout << "✓ Resource with UID '" << uid << "' exists in tenant '" << tenant << "'" << std::endl;
            } else {
                std::cout << "✗ Resource with UID '" << uid << "' does not exist in tenant '" << tenant << "'" << std::endl;
            }
            return response.exists();
        } else {
            std::cout << "✗ Failed to check existence for '" << uid << "' in tenant '" << tenant << "': " << response.error() << std::endl;
            return false;
        }
    }

    // Rename operation
    bool rename(const std::string& uid, const std::string& new_name, const std::string& user, const std::string& tenant = "default") {
        RenameRequest request;
        request.set_uid(uid);
        request.set_new_name(new_name);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

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
    bool move(const std::string& uid, const std::string& new_parent_uid, const std::string& user, const std::string& tenant = "default") {
        MoveRequest request;
        request.set_source_uid(uid);
        request.set_destination_parent_uid(new_parent_uid);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

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
    bool copy(const std::string& source_uid, const std::string& destination_parent_uid, const std::string& user, const std::string& tenant = "default") {
        CopyRequest request;
        request.set_source_uid(source_uid);
        request.set_destination_parent_uid(destination_parent_uid);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

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
    bool list_versions(const std::string& uid, const std::string& user, const std::string& tenant = "default") {
        ListVersionsRequest request;
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        ListVersionsResponse response;
        grpc::ClientContext context;
        grpc::Status status = stub_->ListVersions(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "Versions for '" << uid << "':" << std::endl;
            if (response.versions_size() == 0) {
                std::cout << "  (no versions yet)" << std::endl;
            }
            for (const auto& v : response.versions()) {
                std::cout << "  - " << v << std::endl;
            }
            return true;
        }
        std::cout << "✗ Failed to list versions: " << response.error() << std::endl;
        return false;
    }

    bool get_version(const std::string& uid, const std::string& version_timestamp,
                     const std::string& output_path, const std::string& user,
                     const std::string& tenant = "default") {
        GetVersionRequest request;
        request.set_uid(uid);
        request.set_version_timestamp(version_timestamp);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        GetVersionResponse response;
        grpc::ClientContext context;
        grpc::Status status = stub_->GetVersion(&context, request, &response);

        if (!(status.ok() && response.success())) {
            std::cout << "✗ Failed to get version: " << response.error() << std::endl;
            return false;
        }

        std::ofstream out(output_path, std::ios::binary);
        if (!out) {
            std::cout << "✗ Failed to open output file: " << output_path << std::endl;
            return false;
        }
        out.write(response.data().data(), response.data().size());
        out.close();
        std::cout << "✓ Wrote version " << version_timestamp << " (" << response.data().size()
                  << " bytes) to " << output_path << std::endl;
        return true;
    }

    bool restore_to_version(const std::string& uid, const std::string& version_timestamp,
                            const std::string& user, const std::string& tenant = "default") {
        RestoreToVersionRequest request;
        request.set_uid(uid);
        request.set_version_timestamp(version_timestamp);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        RestoreToVersionResponse response;
        grpc::ClientContext context;
        grpc::Status status = stub_->RestoreToVersion(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Restored '" << uid << "' to version " << response.restored_version() << std::endl;
            return true;
        }
        std::cout << "✗ Failed to restore: " << response.error() << std::endl;
        return false;
    }

    bool delete_file(const std::string& uid, const std::string& user, const std::string& tenant = "default") {
        RemoveFileRequest request;  // Using the same request as remove_file but could have a soft-delete version
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

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

    bool undelete_file(const std::string& uid, const std::string& user, const std::string& tenant = "default") {
        fileengine_rpc::UndeleteFileRequest request;
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        fileengine_rpc::UndeleteFileResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->UndeleteFile(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Undeleted resource '" << uid << "' in tenant '" << tenant << "'" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to undelete resource '" << uid << "': " << response.error() << std::endl;
            return false;
        }
    }

    // Metadata operations
    bool set_metadata(const std::string& uid, const std::string& key, const std::string& value, const std::string& user, const std::string& tenant = "default") {
        SetMetadataRequest request;
        request.set_uid(uid);
        request.set_key(key);
        request.set_value(value);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        SetMetadataResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->SetMetadata(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Set metadata '" << key << "' = '" << value << "' for resource '" << uid << "' in tenant '" << tenant << "'" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to set metadata '" << key << "' for '" << uid << "' in tenant '" << tenant << "': " << response.error() << std::endl;
            return false;
        }
    }

    bool get_metadata(const std::string& uid, const std::string& key, const std::string& user, const std::string& tenant = "default") {
        GetMetadataRequest request;
        request.set_uid(uid);
        request.set_key(key);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        GetMetadataResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GetMetadata(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "Metadata '" << key << "' for resource '" << uid << "' in tenant '" << tenant << "': " << response.value() << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to get metadata '" << key << "' for '" << uid << "' in tenant '" << tenant << "': " << response.error() << std::endl;
            return false;
        }
    }

    bool get_all_metadata(const std::string& uid, const std::string& user, const std::string& tenant = "default") {
        GetAllMetadataRequest request;
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        GetAllMetadataResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GetAllMetadata(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "All metadata for resource '" << uid << "' in tenant '" << tenant << "':" << std::endl;
            for (const auto& pair : response.metadata()) {
                std::cout << "  " << pair.first << " = " << pair.second << std::endl;
            }
            return true;
        } else {
            std::cout << "✗ Failed to get all metadata for '" << uid << "' in tenant '" << tenant << "': " << response.error() << std::endl;
            return false;
        }
    }

    bool get_metadata_for_version(const std::string& uid, const std::string& version_timestamp,
                                  const std::string& key, const std::string& user, const std::string& tenant = "default") {
        fileengine_rpc::GetMetadataForVersionRequest request;
        request.set_uid(uid);
        request.set_version_timestamp(version_timestamp);
        request.set_key(key);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        fileengine_rpc::GetMetadataForVersionResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GetMetadataForVersion(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "Metadata '" << key << "' for '" << uid << "' @ version " << version_timestamp
                      << " in tenant '" << tenant << "': " << response.value() << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to get metadata '" << key << "' for '" << uid << "' @ version "
                      << version_timestamp << "': " << response.error() << std::endl;
            return false;
        }
    }

    bool get_all_metadata_for_version(const std::string& uid, const std::string& version_timestamp,
                                      const std::string& user, const std::string& tenant = "default") {
        fileengine_rpc::GetAllMetadataForVersionRequest request;
        request.set_uid(uid);
        request.set_version_timestamp(version_timestamp);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        fileengine_rpc::GetAllMetadataForVersionResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GetAllMetadataForVersion(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "All metadata for '" << uid << "' @ version " << version_timestamp
                      << " in tenant '" << tenant << "':" << std::endl;
            for (const auto& pair : response.metadata()) {
                std::cout << "  " << pair.first << " = " << pair.second << std::endl;
            }
            return true;
        } else {
            std::cout << "✗ Failed to get all metadata for '" << uid << "' @ version "
                      << version_timestamp << "': " << response.error() << std::endl;
            return false;
        }
    }

    bool delete_metadata(const std::string& uid, const std::string& key, const std::string& user, const std::string& tenant = "default") {
        DeleteMetadataRequest request;
        request.set_uid(uid);
        request.set_key(key);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        DeleteMetadataResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->DeleteMetadata(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Deleted metadata '" << key << "' for resource '" << uid << "' in tenant '" << tenant << "'" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to delete metadata '" << key << "' for '" << uid << "' in tenant '" << tenant << "': " << response.error() << std::endl;
            return false;
        }
    }

    // Diagnostic operations
    bool storage_usage(const std::string& user, const std::string& tenant = "default") {
        StorageUsageRequest request;
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);
        request.set_tenant(tenant);

        StorageUsageResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GetStorageUsage(&context, request, &response);

        if (status.ok() && response.success()) {
            // Convert bytes to more readable format
            double total_gb = response.total_space() / (1024.0 * 1024.0 * 1024.0);
            double used_gb = response.used_space() / (1024.0 * 1024.0 * 1024.0);
            double available_gb = response.available_space() / (1024.0 * 1024.0 * 1024.0);

            std::cout << "Storage Usage:" << std::endl;
            std::cout << "  Total Space: " << response.total_space() << " bytes (" << std::fixed << std::setprecision(2) << total_gb << " GB)" << std::endl;
            std::cout << "  Used Space:  " << response.used_space() << " bytes (" << std::fixed << std::setprecision(2) << used_gb << " GB)" << std::endl;
            std::cout << "  Available:   " << response.available_space() << " bytes (" << std::fixed << std::setprecision(2) << available_gb << " GB)" << std::endl;
            std::cout << "  Usage:       " << std::fixed << std::setprecision(2) << (response.usage_percentage() * 100.0) << "%" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to get storage usage: " << response.error();
            if (!status.ok()) {
                std::cout << " (gRPC Status: " << status.error_code() << " - " << status.error_message() << ")";
            }
            std::cout << std::endl;
            return false;
        }
    }

    bool trigger_sync(const std::string& user, const std::string& tenant = "default") {
        TriggerSyncRequest request;
        request.set_tenant(tenant);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        TriggerSyncResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->TriggerSync(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Triggered synchronization for tenant '" << tenant << "'" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to trigger synchronization: "
                      << (status.ok() ? response.error() : status.error_message()) << std::endl;
            return false;
        }
    }

    bool purge_old_versions(const std::string& uid, int keep_count, const std::string& user, const std::string& tenant = "default") {
        PurgeOldVersionsRequest request;
        request.set_uid(uid);
        request.set_keep_count(keep_count);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        PurgeOldVersionsResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->PurgeOldVersions(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Purged old versions of '" << uid << "', keeping the " << keep_count
                      << " most recent, in tenant '" << tenant << "'" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to purge old versions for '" << uid << "': " << response.error() << std::endl;
            return false;
        }
    }

    // Upload operation - combines touch and put
    bool upload(const std::string& parent_uid, const std::string& name, const std::string& file_path, const std::string& user, const std::string& tenant = "default") {
        std::cout << "Uploading file '" << file_path << "' as '" << name << "' to parent '" << parent_uid << "'" << std::endl;

        // First, touch to create the file and get a UID
        TouchRequest touch_request;
        touch_request.set_parent_uid(parent_uid);
        touch_request.set_name(name);
        *touch_request.mutable_auth() = create_auth_context(user, roles_, tenant);

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

        return put_file(file_uid, data, user, tenant);
    }

    // Enhanced download operation - can optionally select a version (though version specific download not supported in this build)
    bool download(const std::string& uid, const std::string& output_path, const std::string& user, const std::string& tenant = "default", int version_number = -1) {
        GetFileRequest request;
        request.set_uid(uid);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

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
    bool grant_permission(const std::string& resource_uid, const std::string& principal, Permission permission, const std::string& user, const std::string& tenant = "default", fileengine_rpc::AclEffect effect = fileengine_rpc::AclEffect::ALLOW) {
        GrantPermissionRequest request;
        request.set_resource_uid(resource_uid);
        request.set_principal(principal);
        request.set_permission(permission);
        request.set_effect(effect);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        GrantPermissionResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GrantPermission(&context, request, &response);

        const char* effect_str = (effect == fileengine_rpc::AclEffect::DENY) ? "DENY" : "ALLOW";
        if (status.ok() && response.success()) {
            std::cout << "✓ Granted " << effect_str << " " << perm_name(permission)
                      << " to '" << principal << "' on resource '" << resource_uid << "'" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to grant permission: " << response.error() << std::endl;
            return false;
        }
    }

    bool revoke_permission(const std::string& resource_uid, const std::string& principal, Permission permission, const std::string& user, const std::string& tenant = "default", fileengine_rpc::AclEffect effect = fileengine_rpc::AclEffect::ALLOW) {
        RevokePermissionRequest request;
        request.set_resource_uid(resource_uid);
        request.set_principal(principal);
        request.set_permission(permission);
        request.set_effect(effect);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        RevokePermissionResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->RevokePermission(&context, request, &response);

        const char* effect_str = (effect == fileengine_rpc::AclEffect::DENY) ? "DENY" : "ALLOW";
        if (status.ok() && response.success()) {
            std::cout << "✓ Revoked " << effect_str << " " << perm_name(permission)
                      << " from '" << principal << "' on resource '" << resource_uid << "'" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to revoke permission: " << response.error() << std::endl;
            return false;
        }
    }

    bool check_permission(const std::string& resource_uid, const std::string& user, Permission required_permission, const std::string& tenant = "default") {
        CheckPermissionRequest request;
        request.set_resource_uid(resource_uid);
        request.set_required_permission(required_permission);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        CheckPermissionResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->CheckPermission(&context, request, &response);

        if (status.ok() && response.success()) {
            const char* perm_str = perm_name(required_permission);
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

    // Role management operations
    bool create_role(const std::string& role, const std::string& user, const std::string& tenant = "default") {
        CreateRoleRequest request;
        request.set_role(role);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        CreateRoleResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->CreateRole(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Created role '" << role << "' in tenant '" << tenant << "'" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to create role '" << role << "' in tenant '" << tenant << "': " << response.error() << std::endl;
            return false;
        }
    }

    bool delete_role(const std::string& role, const std::string& user, const std::string& tenant = "default") {
        DeleteRoleRequest request;
        request.set_role(role);
        *request.mutable_auth() = create_auth_context(user, roles_, tenant);

        DeleteRoleResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->DeleteRole(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Deleted role '" << role << "' in tenant '" << tenant << "'" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to delete role '" << role << "' in tenant '" << tenant << "': " << response.error() << std::endl;
            return false;
        }
    }

    bool assign_user_to_role(const std::string& user, const std::string& role, const std::string& requesting_user, const std::string& tenant = "default") {
        AssignUserToRoleRequest request;
        request.set_user(user);
        request.set_role(role);
        *request.mutable_auth() = create_auth_context(requesting_user, roles_, tenant);

        AssignUserToRoleResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->AssignUserToRole(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Assigned user '" << user << "' to role '" << role << "' in tenant '" << tenant << "'" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to assign user '" << user << "' to role '" << role << "' in tenant '" << tenant << "': " << response.error() << std::endl;
            return false;
        }
    }

    bool remove_user_from_role(const std::string& user, const std::string& role, const std::string& requesting_user, const std::string& tenant = "default") {
        RemoveUserFromRoleRequest request;
        request.set_user(user);
        request.set_role(role);
        *request.mutable_auth() = create_auth_context(requesting_user, roles_, tenant);

        RemoveUserFromRoleResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->RemoveUserFromRole(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "✓ Removed user '" << user << "' from role '" << role << "' in tenant '" << tenant << "'" << std::endl;
            return true;
        } else {
            std::cout << "✗ Failed to remove user '" << user << "' from role '" << role << "' in tenant '" << tenant << "': " << response.error() << std::endl;
            return false;
        }
    }

    bool list_roles_for_user(const std::string& user, const std::string& requesting_user, const std::string& tenant = "default") {
        GetRolesForUserRequest request;
        request.set_user(user);
        *request.mutable_auth() = create_auth_context(requesting_user, roles_, tenant);

        GetRolesForUserResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GetRolesForUser(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "Roles for user '" << user << "' in tenant '" << tenant << "':" << std::endl;
            if (response.roles_size() > 0) {
                for (const auto& role : response.roles()) {
                    std::cout << "  - " << role << std::endl;
                }
            } else {
                std::cout << "  (no roles assigned to this user)" << std::endl;
            }
            return true;
        } else {
            std::cout << "✗ Failed to get roles for user '" << user << "' in tenant '" << tenant << "': " << response.error() << std::endl;
            return false;
        }
    }

    bool list_users_for_role(const std::string& role, const std::string& requesting_user, const std::string& tenant = "default") {
        GetUsersForRoleRequest request;
        request.set_role(role);
        *request.mutable_auth() = create_auth_context(requesting_user, roles_, tenant);

        GetUsersForRoleResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GetUsersForRole(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "Users in role '" << role << "' in tenant '" << tenant << "':" << std::endl;
            if (response.users_size() > 0) {
                for (const auto& user : response.users()) {
                    std::cout << "  - " << user << std::endl;
                }
            } else {
                std::cout << "  (no users assigned to this role)" << std::endl;
            }
            return true;
        } else {
            std::cout << "✗ Failed to get users for role '" << role << "' in tenant '" << tenant << "': " << response.error() << std::endl;
            return false;
        }
    }

    bool list_all_roles(const std::string& requesting_user, const std::string& tenant = "default") {
        GetAllRolesRequest request;
        *request.mutable_auth() = create_auth_context(requesting_user, roles_, tenant);

        GetAllRolesResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GetAllRoles(&context, request, &response);

        if (status.ok() && response.success()) {
            std::cout << "All roles in tenant '" << tenant << "':" << std::endl;
            if (response.roles_size() > 0) {
                for (const auto& role : response.roles()) {
                    std::cout << "  - " << role << std::endl;
                }
            } else {
                std::cout << "  No roles found (roles are provided with each request)" << std::endl;
            }
            return true;
        } else {
            std::cout << "✗ Failed to get all roles in tenant '" << tenant << "': " << response.error() << std::endl;
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
    std::string tenant = "default";
    std::vector<std::string> roles = {};
    std::vector<std::string> claims = {};
    std::string server_address = "localhost:50051";
    fileengine::LogLevel log_level = fileengine::LogLevel::NORMAL;
    // Phase 6: ACL effect for grant/revoke. Default to ALLOW; users opt into
    // DENY rules with --effect deny (or -e deny).
    fileengine_rpc::AclEffect effect = fileengine_rpc::AclEffect::ALLOW;

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
        } else if (opt == "-t" || opt == "--tenant") {
            if (arg_offset + 1 < argc) {
                tenant = argv[++arg_offset];
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
        } else if (opt == "-e" || opt == "--effect") {
            if (arg_offset + 1 < argc) {
                std::string val = argv[++arg_offset];
                if (val == "deny" || val == "DENY") {
                    effect = fileengine_rpc::AclEffect::DENY;
                } else if (val == "allow" || val == "ALLOW") {
                    effect = fileengine_rpc::AclEffect::ALLOW;
                } else {
                    std::cout << "Invalid --effect value: " << val << " (use allow or deny)" << std::endl;
                    return 1;
                }
            }
        } else if (opt == "-v" || opt == "--verbose") {
            log_level = fileengine::LogLevel::VERBOSE;
        } else if (opt == "-vv" || opt == "--very-verbose") {
            log_level = fileengine::LogLevel::VERY_VERBOSE;
        } else if (opt == "-vvv" || opt == "--extremely-verbose") {
            log_level = fileengine::LogLevel::EXTREMELY_VERBOSE;
        } else {
            std::cout << "Unknown option: " << opt << std::endl;
            return 1;
        }
        arg_offset++;
    }

    // Set the logging level
    fileengine::Logger::set_level(log_level);
    fileengine::Logger::debug("Main", "Logging level set to: ", static_cast<int>(log_level));

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
        std::cout << "  -t, --tenant TENANT       - Tenant for operations (default: default)" << std::endl;
        std::cout << "  -r, --roles ROLE1,ROLE2   - Roles for the user (comma separated)" << std::endl;
        std::cout << "  -c, --claims CLAIM1,CLAIM2 - Claims for the user (comma separated)" << std::endl;
        std::cout << "  -e, --effect allow|deny   - For grant/revoke: target the ALLOW (default) or DENY ACL row" << std::endl;
        std::cout << "  --server ADDRESS          - Server address (default: localhost:50051)" << std::endl;
        std::cout << "  -v, --verbose             - Enable verbose logging" << std::endl;
        std::cout << "  -vv, --very-verbose       - Enable very verbose logging" << std::endl;
        std::cout << "  -vvv, --extremely-verbose - Enable extremely verbose logging" << std::endl;
        std::cout << "  (Tenant option applies to all operations)" << std::endl;
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
        std::cout << "  (Use -t or --tenant option to specify tenant)" << std::endl;
        std::cout << std::endl;
        std::cout << "Versioning operations:" << std::endl;
        std::cout << "  versions <uid>                            - List all version timestamps for a resource" << std::endl;
        std::cout << "  getversion <uid> <timestamp> <out_path>   - Read a specific version into out_path" << std::endl;
        std::cout << "  restore <uid> <timestamp>                 - Restore resource to a specific version" << std::endl;
        std::cout << "  (Use -t or --tenant option to specify tenant)" << std::endl;
        std::cout << std::endl;
        std::cout << "Metadata operations:" << std::endl;
        std::cout << "  setmeta <uid> <key> <value>           - Set metadata for resource" << std::endl;
        std::cout << "  getmeta <uid> <key>                   - Get metadata for resource" << std::endl;
        std::cout << "  allmeta <uid>                         - Get all metadata for resource" << std::endl;
        std::cout << "  getmetaversion <uid> <timestamp> <key> - Get metadata for a specific version" << std::endl;
        std::cout << "  allmetaversion <uid> <timestamp>      - Get all metadata for a specific version" << std::endl;
        std::cout << "  delmeta <uid> <key>                   - Delete metadata for resource" << std::endl;
        std::cout << "  (Use -t or --tenant option to specify tenant)" << std::endl;
        std::cout << std::endl;
        std::cout << "Permission operations:" << std::endl;
        std::cout << "  grant <resource_uid> <principal> <perm>  - Grant permission (use -e deny for a DENY rule)" << std::endl;
        std::cout << "  revoke <resource_uid> <principal> <perm> - Revoke permission (use -e deny to revoke a DENY rule)" << std::endl;
        std::cout << "  check <resource_uid> <user> <perm>       - Check whether user has the permission" << std::endl;
        std::cout << "  Permission letters:" << std::endl;
        std::cout << "    r=READ  w=WRITE  x=EXECUTE  d=DELETE  l=LIST_DELETED  u=UNDELETE" << std::endl;
        std::cout << "    v=VIEW_VERSIONS  b=RETRIEVE_BACK_VERSION  s=RESTORE_TO_VERSION" << std::endl;
        std::cout << "    m=MANAGE_ACL  i=ACL_INHERIT" << std::endl;
        std::cout << "  (Use -t or --tenant option to specify tenant)" << std::endl;
        std::cout << std::endl;
        std::cout << "Role management operations:" << std::endl;
        std::cout << "  create_role <role>                    - Create a new role" << std::endl;
        std::cout << "  delete_role <role>                    - Delete a role" << std::endl;
        std::cout << "  assign_role <user> <role>             - Assign user to a role" << std::endl;
        std::cout << "  remove_role <user> <role>             - Remove user from a role" << std::endl;
        std::cout << "  list_roles <user>                     - List roles for a user" << std::endl;
        std::cout << "  list_users <role>                     - List users in a role" << std::endl;
        std::cout << "  list_all_roles                        - List all roles" << std::endl;
        std::cout << "  (Use -t or --tenant option to specify tenant)" << std::endl;
        std::cout << std::endl;
        std::cout << "Diagnostic operations:" << std::endl;
        std::cout << "  usage                                 - Show storage usage statistics" << std::endl;
        std::cout << "  sync                                  - Trigger synchronization" << std::endl;
        std::cout << "  purge <uid> <keep_count>              - Purge old versions, keeping the N most recent" << std::endl;
        std::cout << "  (Use -t or --tenant option to specify tenant)" << std::endl;
        return 0;
    }

    if (arg_offset < argc && std::string(argv[arg_offset]) == "connect" && arg_offset + 1 < argc) {
        server_address = argv[arg_offset + 1];
        std::cout << "Connecting to server: " << server_address << std::endl;
    } else {
        std::cout << "Connecting to server: " << server_address << std::endl;
    }

    auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
    fileengine::FileEngineClient client(channel, roles);

    std::string command = argv[arg_offset];

    if (command == "connect" && argc > 2) {
        std::cout << "Already connected to: " << server_address << std::endl;
    }
    else if (command == "mkdir" && argc - arg_offset == 3) {  // command + 2 args
        client.make_directory(argv[arg_offset + 1], argv[arg_offset + 2], user, tenant);
    }
    else if (command == "ls" && argc - arg_offset == 2) {  // command + 1 arg
        client.list_directory(argv[arg_offset + 1], user, false, tenant);
    }
    else if (command == "ls" && argc - arg_offset == 3) {  // command + 2 args
        bool show_deleted = (std::string(argv[arg_offset + 2]) == "true" || std::string(argv[arg_offset + 2]) == "1");
        client.list_directory(argv[arg_offset + 1], user, show_deleted, tenant);
    }
    else if (command == "lsd" && argc - arg_offset == 2) {  // command + 1 arg
        client.list_directory(argv[arg_offset + 1], user, true, tenant);
    }
    else if (command == "touch" && argc - arg_offset == 3) {  // command + 2 args
        client.touch(argv[arg_offset + 1], argv[arg_offset + 2], user, tenant);
    }
    else if (command == "rm" && argc - arg_offset == 2) {  // command + 1 arg
        client.remove_file(argv[arg_offset + 1], user, tenant);
    }
    else if (command == "del" && argc - arg_offset == 2) {  // command + 1 arg
        client.delete_file(argv[arg_offset + 1], user, tenant);
    }
    else if (command == "undelete" && argc - arg_offset == 2) {  // command + 1 arg
        client.undelete_file(argv[arg_offset + 1], user, tenant);
    }
    else if (command == "stat" && argc - arg_offset == 2) {  // command + 1 arg
        client.stat(argv[arg_offset + 1], user, tenant);
    }
    else if (command == "exists" && argc - arg_offset == 2) {  // command + 1 arg
        client.exists(argv[arg_offset + 1], user, tenant);
    }
    else if (command == "put" && argc - arg_offset == 3) {  // command + 2 args
        // Read file from disk
        std::ifstream file(argv[arg_offset + 2], std::ios::binary);
        if (file.is_open()) {
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                                      std::istreambuf_iterator<char>());
            client.put_file(argv[arg_offset + 1], data, user, tenant);
        } else {
            std::cout << "✗ Could not open file: " << argv[arg_offset + 2] << std::endl;
        }
    }
    else if (command == "get" && argc - arg_offset == 3) {  // command + 2 args
        auto data = client.get_file(argv[arg_offset + 1], user, tenant);
        if (!data.empty()) {
            std::ofstream file(argv[arg_offset + 2], std::ios::binary);
            if (file.is_open()) {
                file.write(reinterpret_cast<const char*>(data.data()), data.size());
                std::cout << "✓ Saved file to: " << argv[arg_offset + 2] << std::endl;
            } else {
                std::cout << "✗ Could not save to file: " << argv[arg_offset + 2] << std::endl;
            }
        }
    }
    else if (command == "upload" && argc - arg_offset == 4) {  // command + 3 args
        client.upload(argv[arg_offset + 1], argv[arg_offset + 2], argv[arg_offset + 3], user, tenant);
    }
    else if (command == "download" && argc - arg_offset == 3) {  // command + 2 args
        client.download(argv[arg_offset + 1], argv[arg_offset + 2], user, tenant);
    }
    else if (command == "download" && argc - arg_offset == 4) {  // command + 3 args
        try {
            int version = std::stoi(argv[arg_offset + 3]);
            client.download(argv[arg_offset + 1], argv[arg_offset + 2], user, tenant, version);
        } catch (const std::exception& e) {
            std::cout << "✗ Invalid version number: " << argv[arg_offset + 3] << std::endl;
            return 1;
        }
    }
    else if (command == "rename" && argc - arg_offset == 3) {  // command + 2 args
        client.rename(argv[arg_offset + 1], argv[arg_offset + 2], user, tenant);
    }
    else if (command == "move" && argc - arg_offset == 3) {  // command + 2 args
        client.move(argv[arg_offset + 1], argv[arg_offset + 2], user, tenant);
    }
    else if (command == "copy" && argc - arg_offset == 3) {  // command + 2 args
        client.copy(argv[arg_offset + 1], argv[arg_offset + 2], user, tenant);
    }
    else if (command == "versions" && argc - arg_offset == 2) {  // command + 1 arg
        client.list_versions(argv[arg_offset + 1], user, tenant);
    }
    else if (command == "getversion" && argc - arg_offset == 4) {  // command + 3 args: uid, timestamp, output_path
        client.get_version(argv[arg_offset + 1], argv[arg_offset + 2], argv[arg_offset + 3], user, tenant);
    }
    else if (command == "restore" && argc - arg_offset == 3) {  // command + 2 args: uid, timestamp
        client.restore_to_version(argv[arg_offset + 1], argv[arg_offset + 2], user, tenant);
    }
    else if (command == "setmeta" && argc - arg_offset == 4) {  // command + 3 args
        client.set_metadata(argv[arg_offset + 1], argv[arg_offset + 2], argv[arg_offset + 3], user, tenant);
    }
    else if (command == "getmeta" && argc - arg_offset == 3) {  // command + 2 args
        client.get_metadata(argv[arg_offset + 1], argv[arg_offset + 2], user, tenant);
    }
    else if (command == "allmeta" && argc - arg_offset == 2) {  // command + 1 arg
        client.get_all_metadata(argv[arg_offset + 1], user, tenant);
    }
    else if (command == "getmetaversion" && argc - arg_offset == 4) {  // command + 3 args: uid, timestamp, key
        client.get_metadata_for_version(argv[arg_offset + 1], argv[arg_offset + 2], argv[arg_offset + 3], user, tenant);
    }
    else if (command == "allmetaversion" && argc - arg_offset == 3) {  // command + 2 args: uid, timestamp
        client.get_all_metadata_for_version(argv[arg_offset + 1], argv[arg_offset + 2], user, tenant);
    }
    else if (command == "delmeta" && argc - arg_offset == 3) {  // command + 2 args
        client.delete_metadata(argv[arg_offset + 1], argv[arg_offset + 2], user, tenant);
    }
    else if (command == "grant" && argc - arg_offset == 4) {  // command + 3 args
        fileengine_rpc::Permission perm;
        if (!fileengine::parse_perm_letter(argv[arg_offset + 3], perm)) {
            std::cout << "✗ Invalid permission letter. Use one of: r w x d l u v b s m i" << std::endl;
            return 1;
        }
        client.grant_permission(argv[arg_offset + 1], argv[arg_offset + 2], perm, user, tenant, effect);
    }
    else if (command == "revoke" && argc - arg_offset == 4) {  // command + 3 args
        fileengine_rpc::Permission perm;
        if (!fileengine::parse_perm_letter(argv[arg_offset + 3], perm)) {
            std::cout << "✗ Invalid permission letter. Use one of: r w x d l u v b s m i" << std::endl;
            return 1;
        }
        client.revoke_permission(argv[arg_offset + 1], argv[arg_offset + 2], perm, user, tenant, effect);
    }
    else if (command == "check" && argc - arg_offset == 4) {  // command + 3 args
        fileengine_rpc::Permission perm;
        if (!fileengine::parse_perm_letter(argv[arg_offset + 3], perm)) {
            std::cout << "✗ Invalid permission letter. Use one of: r w x d l u v b s m i" << std::endl;
            return 1;
        }
        client.check_permission(argv[arg_offset + 1], user, perm, tenant);
    }
    // Role management commands
    else if (command == "create_role" && argc - arg_offset == 2) {  // command + 1 arg
        client.create_role(argv[arg_offset + 1], user, tenant);
    }
    else if (command == "delete_role" && argc - arg_offset == 2) {  // command + 1 arg
        client.delete_role(argv[arg_offset + 1], user, tenant);
    }
    else if (command == "assign_role" && argc - arg_offset == 3) {  // command + 2 args
        client.assign_user_to_role(argv[arg_offset + 1], argv[arg_offset + 2], user, tenant);
    }
    else if (command == "remove_role" && argc - arg_offset == 3) {  // command + 2 args
        client.remove_user_from_role(argv[arg_offset + 1], argv[arg_offset + 2], user, tenant);
    }
    else if (command == "list_roles" && argc - arg_offset == 2) {  // command + 1 arg
        client.list_roles_for_user(argv[arg_offset + 1], user, tenant);
    }
    else if (command == "list_users" && argc - arg_offset == 2) {  // command + 1 arg
        client.list_users_for_role(argv[arg_offset + 1], user, tenant);
    }
    else if (command == "list_all_roles" && argc - arg_offset == 1) {  // command only
        client.list_all_roles(user, tenant);
    }
    else if (command == "usage" && argc - arg_offset == 1) {  // command only
        client.storage_usage(user, tenant);
    }
    else if (command == "sync" && argc - arg_offset == 1) {  // command only
        client.trigger_sync(user, tenant);
    }
    else if (command == "purge" && argc - arg_offset == 3) {  // command + 2 args
        try {
            int keep_count = std::stoi(argv[arg_offset + 2]);
            client.purge_old_versions(argv[arg_offset + 1], keep_count, user, tenant);
        } catch (const std::exception& e) {
            std::cout << "✗ Invalid keep_count value: " << argv[arg_offset + 2] << std::endl;
            return 1;
        }
    }
    else {
        std::cout << "✗ Invalid command or wrong number of arguments." << std::endl;
        std::cout << "Use '" << argv[0] << "' without arguments to see help." << std::endl;
        return 1;
    }
}