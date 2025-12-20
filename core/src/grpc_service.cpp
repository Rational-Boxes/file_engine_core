#include "grpc_service.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <google/protobuf/empty.pb.h>
#include <chrono>
#include <ctime>

#include "fileengine/logger.h"


namespace fileengine_service {
    // Include the generated types
}

namespace fileengine {

GRPCFileService::GRPCFileService(std::shared_ptr<FileSystem> filesystem,
                                 std::shared_ptr<TenantManager> tenant_manager,
                                 std::shared_ptr<AclManager> acl_manager,
                                 bool root_user_enabled)
    : filesystem_(filesystem), tenant_manager_(tenant_manager),
      acl_manager_(acl_manager), root_user_enabled_(root_user_enabled) {
}

// Directory operations
grpc::Status GRPCFileService::MakeDirectory(grpc::ServerContext* context,
                                            const fileengine_service::MakeDirectoryRequest* request,
                                            fileengine_service::MakeDirectoryResponse* response) {
    // Get the authentication context
    std::string parent_uid = request->parent_uid();
    std::string name = request->name();
    auto auth_context = request->auth();

    // Log the request details
    LOG_DEBUG("GRPC-Service", "MakeDirectory called: parent_uid=" + parent_uid + ", name=" + name);

    // Determine tenant from auth context
    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check if this is a root user before validating permissions
    if (root_user_enabled_ && user == "root") {
        LOG_DEBUG("GRPC-Service", "Root user bypassing permission check for mkdir");
    } else {
        // Validate permissions for non-root users
        if (!validate_user_permissions(parent_uid, auth_context, 0200)) { // WRITE permission
            LOG_WARN("GRPC-Service", "User " + user + " denied permission to create directory in " + parent_uid);
            response->set_success(false);
            response->set_uid("");
            response->set_error("User does not have permission to create directory");
            return grpc::Status::OK;
        }
    }

    // Call the filesystem to create the directory
    auto result = filesystem_->mkdir(parent_uid, name, user, request->permissions(), tenant);

    if (result.success) {
        response->set_success(true);
        response->set_uid(result.value);
        response->set_error("");
        LOG_INFO("GRPC-Service", "Directory created successfully: " + result.value);
    } else {
        response->set_success(false);
        response->set_uid("");
        response->set_error(result.error);
        LOG_ERROR("GRPC-Service", "Failed to create directory: " + result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::RemoveDirectory(grpc::ServerContext* context,
                                              const fileengine_service::RemoveDirectoryRequest* request,
                                              fileengine_service::RemoveDirectoryResponse* response) {
    std::string dir_uid = request->uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions before removing
    if (!validate_user_permissions(dir_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to remove directory");
        return grpc::Status::OK;
    }

    auto result = filesystem_->rmdir(dir_uid, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::ListDirectory(grpc::ServerContext* context,
                                            const fileengine_service::ListDirectoryRequest* request,
                                            fileengine_service::ListDirectoryResponse* response) {
    std::string dir_uid = request->uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check read permissions on the directory
    if (!validate_user_permissions(dir_uid, auth_context, 0400)) { // READ permission
        response->set_success(false);
        response->set_error("User does not have permission to list directory");
        return grpc::Status::OK;
    }

    auto result = filesystem_->listdir(dir_uid, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
    } else {
        for (const auto& entry : result.value) {
            auto* dir_entry = response->add_entries();
            dir_entry->set_uid(entry.uid);
            dir_entry->set_name(entry.name);
            
            // Convert internal file type to gRPC file type
            fileengine_service::FileType grpc_file_type;
            switch (entry.type) {
                case fileengine::FileType::REGULAR_FILE:
                    grpc_file_type = fileengine_service::FileType::REGULAR_FILE;
                    break;
                case fileengine::FileType::DIRECTORY:
                    grpc_file_type = fileengine_service::FileType::DIRECTORY;
                    break;
                case fileengine::FileType::SYMLINK:
                    grpc_file_type = fileengine_service::FileType::SYMLINK;
                    break;
                default:
                    grpc_file_type = fileengine_service::FileType::REGULAR_FILE; // default
                    break;
            }
            dir_entry->set_type(grpc_file_type);
            
            dir_entry->set_size(entry.size);
            // DirectoryEntry already has int64_t values
            dir_entry->set_created_at(entry.created_at);
            dir_entry->set_modified_at(entry.modified_at);
            dir_entry->set_version_count(entry.version_count);
        }
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::ListDirectoryWithDeleted(grpc::ServerContext* context,
                                                       const fileengine_service::ListDirectoryWithDeletedRequest* request,
                                                       fileengine_service::ListDirectoryWithDeletedResponse* response) {
    // For now, implement the same as ListDirectory but indicate this functionality would return deleted items too
    std::string dir_uid = request->uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check read permissions on the directory
    if (!validate_user_permissions(dir_uid, auth_context, 0400)) { // READ permission
        response->set_success(false);
        response->set_error("User does not have permission to list directory with deleted items");
        return grpc::Status::OK;
    }

    auto result = filesystem_->listdir(dir_uid, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
    } else {
        for (const auto& entry : result.value) {
            auto* dir_entry = response->add_entries();
            dir_entry->set_uid(entry.uid);
            dir_entry->set_name(entry.name);
            
            // Convert internal file type to gRPC file type
            fileengine_service::FileType grpc_file_type;
            switch (entry.type) {
                case fileengine::FileType::REGULAR_FILE:
                    grpc_file_type = fileengine_service::FileType::REGULAR_FILE;
                    break;
                case fileengine::FileType::DIRECTORY:
                    grpc_file_type = fileengine_service::FileType::DIRECTORY;
                    break;
                case fileengine::FileType::SYMLINK:
                    grpc_file_type = fileengine_service::FileType::SYMLINK;
                    break;
                default:
                    grpc_file_type = fileengine_service::FileType::REGULAR_FILE; // default
                    break;
            }
            dir_entry->set_type(grpc_file_type);
            
            dir_entry->set_size(entry.size);
            // DirectoryEntry already has int64_t values
            dir_entry->set_created_at(entry.created_at);
            dir_entry->set_modified_at(entry.modified_at);
            dir_entry->set_version_count(entry.version_count);
        }
    }

    return grpc::Status::OK;
}

// File operations
grpc::Status GRPCFileService::Touch(grpc::ServerContext* context,
                                   const fileengine_service::TouchRequest* request,
                                   fileengine_service::TouchResponse* response) {
    std::string parent_uid = request->parent_uid();
    std::string name = request->name();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions on parent directory
    if (!validate_user_permissions(parent_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to create file in this directory");
        return grpc::Status::OK;
    }

    auto result = filesystem_->touch(parent_uid, name, user, tenant);

    response->set_success(result.success);
    if (result.success) {
        response->set_uid(result.value);
        response->set_error("");
    } else {
        response->set_uid("");
        response->set_error(result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::RemoveFile(grpc::ServerContext* context,
                                        const fileengine_service::RemoveFileRequest* request,
                                        fileengine_service::RemoveFileResponse* response) {
    std::string file_uid = request->uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions on the file
    if (!validate_user_permissions(file_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to remove file");
        return grpc::Status::OK;
    }

    auto result = filesystem_->remove(file_uid, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::UndeleteFile(grpc::ServerContext* context,
                                          const fileengine_service::UndeleteFileRequest* request,
                                          fileengine_service::UndeleteFileResponse* response) {
    std::string file_uid = request->uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // For now, we'll just check if the user has permission to write in the directory
    // In a real implementation, there would be a special undelete permission check
    if (!validate_user_permissions(file_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to undelete file");
        return grpc::Status::OK;
    }

    // In a real implementation, this would undelete the file
    // For now, we'll return an error to indicate it's not supported
    response->set_success(false);
    response->set_error("Undelete functionality not implemented in this version");

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::PutFile(grpc::ServerContext* context,
                                     const fileengine_service::PutFileRequest* request,
                                     fileengine_service::PutFileResponse* response) {
    std::string file_uid = request->uid();
    auto file_data = request->data();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs write access to the file
    if (!validate_user_permissions(file_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to write to file");
        return grpc::Status::OK;
    }

    // Convert bytes to std::vector<uint8_t>
    std::vector<uint8_t> data_vec(file_data.begin(), file_data.end());

    auto result = filesystem_->put(file_uid, data_vec, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::GetFile(grpc::ServerContext* context,
                                     const fileengine_service::GetFileRequest* request,
                                     fileengine_service::GetFileResponse* response) {
    std::string file_uid = request->uid();
    std::string version_timestamp = request->version_timestamp();
    auto auth_context = request->auth();

    LOG_DEBUG("GRPC-Service", "GetFile called for file: " + file_uid + ", version: " + version_timestamp);

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check if this is a root user before validating permissions
    if (root_user_enabled_ && user == "root") {
        LOG_DEBUG("GRPC-Service", "Root user bypassing permission check for GetFile");
    } else {
        // Check permissions - user needs read access to the file
        if (!validate_user_permissions(file_uid, auth_context, 0400)) { // READ permission
            LOG_WARN("GRPC-Service", "User " + user + " denied permission to read file " + file_uid);
            response->set_success(false);
            response->set_error("User does not have permission to read file");
            return grpc::Status::OK;
        }
    }

    auto result = filesystem_->get(file_uid, user, tenant);

    response->set_success(result.success);
    if (result.success) {
        response->set_data(std::string(result.value.begin(), result.value.end()));
        response->set_error("");
        LOG_DEBUG("GRPC-Service", "GetFile succeeded for file: " + file_uid);
    } else {
        response->set_error(result.error);
        LOG_ERROR("GRPC-Service", "GetFile failed for file: " + file_uid + ", error: " + result.error);
    }

    return grpc::Status::OK;
}

// File information operations
grpc::Status GRPCFileService::Stat(grpc::ServerContext* context,
                                  const fileengine_service::StatRequest* request,
                                  fileengine_service::StatResponse* response) {
    std::string file_uid = request->uid();
    auto auth_context = request->auth();

    LOG_DEBUG("GRPC-Service", "Stat called for file: " + file_uid);

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check if this is a root user before validating permissions
    if (root_user_enabled_ && user == "root") {
        LOG_DEBUG("GRPC-Service", "Root user bypassing permission check for stat");
    } else {
        // Check permissions - user needs read access to stat the file
        if (!validate_user_permissions(file_uid, auth_context, 0400)) { // READ permission
            LOG_WARN("GRPC-Service", "User " + user + " denied permission to stat file " + file_uid);
            response->set_success(false);
            response->set_error("User does not have permission to access file information");
            return grpc::Status::OK;
        }
    }

    auto result = filesystem_->stat(file_uid, user, tenant);

    response->set_success(result.success);
    if (result.success) {
        auto* info = response->mutable_info();
        info->set_uid(result.value.uid);
        info->set_name(result.value.name);
        info->set_parent_uid(result.value.parent_uid);

        // Convert internal file type to gRPC file type
        fileengine_service::FileType grpc_file_type;
        switch (result.value.type) {
            case fileengine::FileType::REGULAR_FILE:
                grpc_file_type = fileengine_service::FileType::REGULAR_FILE;
                break;
            case fileengine::FileType::DIRECTORY:
                grpc_file_type = fileengine_service::FileType::DIRECTORY;
                break;
            case fileengine::FileType::SYMLINK:
                grpc_file_type = fileengine_service::FileType::SYMLINK;
                break;
            default:
                grpc_file_type = fileengine_service::FileType::REGULAR_FILE; // default
                break;
        }
        info->set_type(grpc_file_type);

        info->set_size(result.value.size);
        info->set_owner(result.value.owner);
        info->set_permissions(result.value.permissions);
        // Convert from time_point to int64_t timestamp
        info->set_created_at(std::chrono::duration_cast<std::chrono::seconds>(
            result.value.created_at.time_since_epoch()).count());
        info->set_modified_at(std::chrono::duration_cast<std::chrono::seconds>(
            result.value.modified_at.time_since_epoch()).count());
        info->set_version(result.value.version);

        LOG_DEBUG("GRPC-Service", "Stat succeeded for file: " + file_uid);
    } else {
        response->set_error(result.error);
        LOG_ERROR("GRPC-Service", "Stat failed for file: " + file_uid + ", error: " + result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::Exists(grpc::ServerContext* context,
                                    const fileengine_service::ExistsRequest* request,
                                    fileengine_service::ExistsResponse* response) {
    std::string file_uid = request->uid();
    auto auth_context = request->auth();

    LOG_DEBUG("GRPC-Service", "Exists called for file: " + file_uid);

    std::string tenant = get_tenant_from_auth_context(auth_context);

    auto result = filesystem_->exists(file_uid, tenant);

    response->set_success(result.success);
    if (result.success) {
        response->set_exists(result.value);
        LOG_DEBUG("GRPC-Service", "Exists check for file " + file_uid + " returned: " + (result.value ? "true" : "false"));
    } else {
        response->set_error(result.error);
        LOG_ERROR("GRPC-Service", "Exists check failed for file " + file_uid + ", error: " + result.error);
    }

    return grpc::Status::OK;
}

// File manipulation operations
grpc::Status GRPCFileService::Rename(grpc::ServerContext* context,
                                    const fileengine_service::RenameRequest* request,
                                    fileengine_service::RenameResponse* response) {
    std::string uid = request->uid();
    std::string new_name = request->new_name();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs write access to the file
    if (!validate_user_permissions(uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to rename file");
        return grpc::Status::OK;
    }

    auto result = filesystem_->rename(uid, new_name, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::Move(grpc::ServerContext* context,
                                  const fileengine_service::MoveRequest* request,
                                  fileengine_service::MoveResponse* response) {
    std::string source_uid = request->source_uid();
    std::string dest_uid = request->destination_parent_uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions on source and destination
    if (!validate_user_permissions(source_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to move source file");
        return grpc::Status::OK;
    }

    if (!validate_user_permissions(dest_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to move to destination directory");
        return grpc::Status::OK;
    }

    auto result = filesystem_->move(source_uid, dest_uid, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::Copy(grpc::ServerContext* context,
                                  const fileengine_service::CopyRequest* request,
                                  fileengine_service::CopyResponse* response) {
    std::string source_uid = request->source_uid();
    std::string dest_uid = request->destination_parent_uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions on source and destination
    if (!validate_user_permissions(source_uid, auth_context, 0400)) { // READ permission
        response->set_success(false);
        response->set_error("User does not have permission to read source file");
        return grpc::Status::OK;
    }

    if (!validate_user_permissions(dest_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to write to destination directory");
        return grpc::Status::OK;
    }

    auto result = filesystem_->copy(source_uid, dest_uid, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
    }

    return grpc::Status::OK;
}

// Version operations
grpc::Status GRPCFileService::ListVersions(grpc::ServerContext* context,
                                          const fileengine_service::ListVersionsRequest* request,
                                          fileengine_service::ListVersionsResponse* response) {
    std::string file_uid = request->uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs read access to the file
    if (!validate_user_permissions(file_uid, auth_context, 0400)) { // READ permission
        response->set_success(false);
        response->set_error("User does not have permission to list file versions");
        return grpc::Status::OK;
    }

    auto result = filesystem_->list_versions(file_uid, user, tenant);

    response->set_success(result.success);
    if (result.success) {
        for (const auto& version : result.value) {
            response->add_versions(version);
        }
    } else {
        response->set_error(result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::GetVersion(grpc::ServerContext* context,
                                        const fileengine_service::GetVersionRequest* request,
                                        fileengine_service::GetVersionResponse* response) {
    std::string file_uid = request->uid();
    std::string version_timestamp = request->version_timestamp();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs read access to the file
    if (!validate_user_permissions(file_uid, auth_context, 0400)) { // READ permission
        response->set_success(false);
        response->set_error("User does not have permission to access file version");
        return grpc::Status::OK;
    }

    auto result = filesystem_->get_version(file_uid, version_timestamp, user, tenant);

    response->set_success(result.success);
    if (result.success) {
        response->set_data(std::string(result.value.begin(), result.value.end()));
    } else {
        response->set_error(result.error);
    }

    return grpc::Status::OK;
}

// Metadata operations
grpc::Status GRPCFileService::SetMetadata(grpc::ServerContext* context,
                                         const fileengine_service::SetMetadataRequest* request,
                                         fileengine_service::SetMetadataResponse* response) {
    std::string file_uid = request->uid();
    std::string key = request->key();
    std::string value = request->value();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs write access to set metadata
    if (!validate_user_permissions(file_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to set metadata");
        return grpc::Status::OK;
    }

    auto result = filesystem_->set_metadata(file_uid, key, value, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::GetMetadata(grpc::ServerContext* context,
                                         const fileengine_service::GetMetadataRequest* request,
                                         fileengine_service::GetMetadataResponse* response) {
    std::string file_uid = request->uid();
    std::string key = request->key();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs read access to get metadata
    if (!validate_user_permissions(file_uid, auth_context, 0400)) { // READ permission
        response->set_success(false);
        response->set_error("User does not have permission to get metadata");
        return grpc::Status::OK;
    }

    auto result = filesystem_->get_metadata(file_uid, key, user, tenant);

    response->set_success(result.success);
    if (result.success) {
        response->set_value(result.value);
    } else {
        response->set_error(result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::GetAllMetadata(grpc::ServerContext* context,
                                            const fileengine_service::GetAllMetadataRequest* request,
                                            fileengine_service::GetAllMetadataResponse* response) {
    std::string file_uid = request->uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs read access to get metadata
    if (!validate_user_permissions(file_uid, auth_context, 0400)) { // READ permission
        response->set_success(false);
        response->set_error("User does not have permission to get metadata");
        return grpc::Status::OK;
    }

    auto result = filesystem_->get_all_metadata(file_uid, user, tenant);

    response->set_success(result.success);
    if (result.success) {
        for (const auto& [k, v] : result.value) {
            (*response->mutable_metadata())[k] = v;
        }
    } else {
        response->set_error(result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::DeleteMetadata(grpc::ServerContext* context,
                                            const fileengine_service::DeleteMetadataRequest* request,
                                            fileengine_service::DeleteMetadataResponse* response) {
    std::string file_uid = request->uid();
    std::string key = request->key();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs write access to delete metadata
    if (!validate_user_permissions(file_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to delete metadata");
        return grpc::Status::OK;
    }

    auto result = filesystem_->delete_metadata(file_uid, key, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::GetMetadataForVersion(grpc::ServerContext* context,
                                                   const fileengine_service::GetMetadataForVersionRequest* request,
                                                   fileengine_service::GetMetadataForVersionResponse* response) {
    std::string file_uid = request->uid();
    std::string version_timestamp = request->version_timestamp();
    std::string key = request->key();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs read access to get metadata
    if (!validate_user_permissions(file_uid, auth_context, 0400)) { // READ permission
        response->set_success(false);
        response->set_error("User does not have permission to get metadata for version");
        return grpc::Status::OK;
    }

    // In a real implementation, this would get metadata for a specific version
    // For now, we'll return an error to indicate it's not supported
    response->set_success(false);
    response->set_error("Get metadata for version functionality not implemented in this version");

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::GetAllMetadataForVersion(grpc::ServerContext* context,
                                                      const fileengine_service::GetAllMetadataForVersionRequest* request,
                                                      fileengine_service::GetAllMetadataForVersionResponse* response) {
    std::string file_uid = request->uid();
    std::string version_timestamp = request->version_timestamp();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs read access to get metadata
    if (!validate_user_permissions(file_uid, auth_context, 0400)) { // READ permission
        response->set_success(false);
        response->set_error("User does not have permission to get metadata for version");
        return grpc::Status::OK;
    }

    // In a real implementation, this would get all metadata for a specific version
    // For now, we'll return an error to indicate it's not supported
    response->set_success(false);
    response->set_error("Get all metadata for version functionality not implemented in this version");

    return grpc::Status::OK;
}

// ACL operations
grpc::Status GRPCFileService::GrantPermission(grpc::ServerContext* context,
                                             const fileengine_service::GrantPermissionRequest* request,
                                             fileengine_service::GrantPermissionResponse* response) {
    std::string resource_uid = request->resource_uid();
    std::string principal = request->principal();
    fileengine_service::Permission permission = request->permission();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Only admins or users with grant permission can grant permissions
    if (user != "root" && !validate_user_permissions(resource_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to grant permissions");
        return grpc::Status::OK;
    }

    // Convert gRPC Permission to our internal representation
    int converted_permissions = static_cast<int>(permission);

    auto result = acl_manager_->grant_permission(resource_uid, principal,
                                                 PrincipalType::USER,  // Simplified for this example
                                                 converted_permissions, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::RevokePermission(grpc::ServerContext* context,
                                              const fileengine_service::RevokePermissionRequest* request,
                                              fileengine_service::RevokePermissionResponse* response) {
    std::string resource_uid = request->resource_uid();
    std::string principal = request->principal();
    fileengine_service::Permission permission = request->permission();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Only admins or users with appropriate permissions can revoke permissions
    if (user != "root" && !validate_user_permissions(resource_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to revoke permissions");
        return grpc::Status::OK;
    }

    int converted_permissions = static_cast<int>(permission);

    auto result = acl_manager_->revoke_permission(resource_uid, principal,
                                                  PrincipalType::USER,  // Simplified for this example
                                                  converted_permissions, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::CheckPermission(grpc::ServerContext* context,
                                             const fileengine_service::CheckPermissionRequest* request,
                                             fileengine_service::CheckPermissionResponse* response) {
    std::string resource_uid = request->resource_uid();
    fileengine_service::Permission required_permission = request->required_permission();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Convert roles from gRPC context to internal representation
    std::vector<std::string> roles = get_roles_from_auth_context(auth_context);

    int required_permissions_int = static_cast<int>(required_permission);

    auto result = acl_manager_->check_permission(resource_uid, user, roles,
                                                 required_permissions_int, tenant);

    response->set_success(result.success);
    if (result.success) {
        response->set_has_permission(result.value);
    } else {
        response->set_error(result.error);
    }

    return grpc::Status::OK;
}

// Streaming operations for large files
grpc::Status GRPCFileService::StreamFileUpload(grpc::ServerContext* context,
                                              grpc::ServerReader<fileengine_service::PutFileRequest>* reader,
                                              fileengine_service::PutFileResponse* response) {
    fileengine_service::PutFileRequest request;
    std::vector<uint8_t> full_data;
    std::string file_uid;
    fileengine_service::AuthenticationContext auth_context;
    bool first_chunk = true;

    while (reader->Read(&request)) {
        if (first_chunk) {
            file_uid = request.uid();
            auth_context = request.auth();
            first_chunk = false;
        }

        // Append chunk data to full data
        auto chunk_data = request.data();
        full_data.insert(full_data.end(), chunk_data.begin(), chunk_data.end());
    }

    // Now process the full data
    if (!file_uid.empty()) {
        std::string tenant = get_tenant_from_auth_context(auth_context);
        std::string user = get_user_from_auth_context(auth_context);

        // Check permissions - user needs write access to the file
        if (!validate_user_permissions(file_uid, auth_context, 0200)) { // WRITE permission
            response->set_success(false);
            response->set_error("User does not have permission to write to file");
            return grpc::Status::OK;
        }

        auto result = filesystem_->put(file_uid, full_data, user, tenant);

        response->set_success(result.success);
        if (!result.success) {
            response->set_error(result.error);
        }
    } else {
        response->set_success(false);
        response->set_error("No file data received");
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::StreamFileDownload(grpc::ServerContext* context,
                                                const fileengine_service::GetFileRequest* request,
                                                grpc::ServerWriter<fileengine_service::GetFileResponse>* writer) {
    std::string file_uid = request->uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs read access to the file
    if (!validate_user_permissions(file_uid, auth_context, 0400)) { // READ permission
        fileengine_service::GetFileResponse response;
        response.set_success(false);
        response.set_error("User does not have permission to read file");
        writer->Write(response);
        return grpc::Status::OK;
    }

    auto result = filesystem_->get(file_uid, user, tenant);

    if (result.success) {
        // Simulate streaming by creating chunks
        const size_t chunk_size = 1024 * 64; // 64KB chunks
        const auto& data = result.value;
        size_t offset = 0;

        while (offset < data.size()) {
            fileengine_service::GetFileResponse response;
            size_t current_chunk_size = std::min(chunk_size, data.size() - offset);
            std::string chunk_data(reinterpret_cast<const char*>(&data[offset]), current_chunk_size);
            
            response.set_data(chunk_data);
            response.set_success(true);
            response.set_error("");

            if (!writer->Write(response)) {
                break; // Client disconnected
            }

            offset += current_chunk_size;
        }
    } else {
        fileengine_service::GetFileResponse response;
        response.set_success(false);
        response.set_error(result.error);
        writer->Write(response);
    }

    return grpc::Status::OK;
}

// Administrative operations
grpc::Status GRPCFileService::GetStorageUsage(grpc::ServerContext* context,
                                             const fileengine_service::StorageUsageRequest* request,
                                             fileengine_service::StorageUsageResponse* response) {
    auto auth_context = request->auth();
    std::string tenant = request->tenant();

    // In a real implementation, this would check storage usage
    // For now, we'll return some simulated values
    response->set_success(true);
    response->set_error("");
    response->set_total_space(1024 * 1024 * 1024);  // 1GB
    response->set_used_space(512 * 1024 * 1024);    // 512MB
    response->set_available_space(512 * 1024 * 1024); // 512MB
    response->set_usage_percentage(0.5); // 50%

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::PurgeOldVersions(grpc::ServerContext* context,
                                              const fileengine_service::PurgeOldVersionsRequest* request,
                                              fileengine_service::PurgeOldVersionsResponse* response) {
    std::string file_uid = request->uid();
    int keep_count = request->keep_count();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs write access to the file
    if (user != "root" && !validate_user_permissions(file_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to purge old versions");
        return grpc::Status::OK;
    }

    // In a real implementation, this would remove old versions
    // For now, we'll return an error to indicate it's not supported
    response->set_success(false);
    response->set_error("Purge old versions functionality not implemented in this version");

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::TriggerSync(grpc::ServerContext* context,
                                         const fileengine_service::TriggerSyncRequest* request,
                                         fileengine_service::TriggerSyncResponse* response) {
    std::string tenant = request->tenant();
    auto auth_context = request->auth();

    // In a real implementation, we would trigger a sync operation here
    // For this example, we'll just simulate success
    response->set_success(true);
    response->set_error("");

    return grpc::Status::OK;
}

// Helper functions
std::string GRPCFileService::get_tenant_from_auth_context(const fileengine_service::AuthenticationContext& auth_ctx) {
    return auth_ctx.tenant().empty() ? "default" : auth_ctx.tenant();
}

std::string GRPCFileService::get_user_from_auth_context(const fileengine_service::AuthenticationContext& auth_ctx) {
    return auth_ctx.user();
}

std::vector<std::string> GRPCFileService::get_roles_from_auth_context(const fileengine_service::AuthenticationContext& auth_ctx) {
    std::vector<std::string> roles;
    for (const auto& role : auth_ctx.roles()) {
        roles.push_back(role);
    }
    return roles;
}

bool GRPCFileService::validate_user_permissions(const std::string& resource_uid,
                                               const fileengine_service::AuthenticationContext& auth_ctx,
                                               int required_permissions) {
    // Check if root user is enabled and if the current user is root
    if (root_user_enabled_ && auth_ctx.user() == "root") {
        return true;  // Root user has all permissions when enabled
    }

    // Convert roles from gRPC context to internal representation
    std::vector<std::string> roles = get_roles_from_auth_context(auth_ctx);

    std::string tenant = get_tenant_from_auth_context(auth_ctx);
    std::string user = auth_ctx.user();

    auto result = acl_manager_->check_permission(resource_uid, user, roles, required_permissions, tenant);
    return result.success && result.value;
}

} // namespace fileengine