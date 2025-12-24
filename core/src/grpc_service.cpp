#include "grpc_service.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <google/protobuf/empty.pb.h>
#include <chrono>
#include <ctime>
#include "fileengine/connection_pool_manager.h"
#include "fileengine/logger.h"

namespace fileengine {

GRPCFileService::GRPCFileService(std::shared_ptr<FileSystem> filesystem,
                                 std::shared_ptr<TenantManager> tenant_manager,
                                 std::shared_ptr<AclManager> acl_manager)
    : filesystem_(filesystem), tenant_manager_(tenant_manager), acl_manager_(acl_manager) {
}

// Directory operations
grpc::Status GRPCFileService::MakeDirectory(grpc::ServerContext* context,
                                            const fileengine_rpc::MakeDirectoryRequest* request,
                                            fileengine_rpc::MakeDirectoryResponse* response) {
    std::cout << "MakeDirectory called" << std::endl;
    LOG_DEBUG("GRPCService", "MakeDirectory called for parent_uid: " + request->parent_uid() + ", name: " + request->name());
    // Check if server is in read-only mode
    if (is_server_in_readonly_mode()) {
        response->set_success(false);
        response->set_error("Server is in read-only mode due to database disconnection");
        LOG_ERROR("GRPCService", "MakeDirectory failed: Server is in read-only mode");
        return grpc::Status::OK;
    }

    // Get the authentication context
    std::string parent_uid = request->parent_uid();
    std::string name = request->name();
    auto auth_context = request->auth();

    // Determine tenant from auth context
    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Call the filesystem to create the directory
    auto result = filesystem_->mkdir(parent_uid, name, user, request->permissions(), tenant);

    if (result.success) {
        response->set_success(true);
        response->set_uid(result.value);
        response->set_error("");
        LOG_INFO("GRPCService", "MakeDirectory successful for parent_uid: " + request->parent_uid() + ", name: " + request->name());
    } else {
        response->set_success(false);
        response->set_uid("");
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "Failed to create directory: " + result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::RemoveDirectory(grpc::ServerContext* context,
                                              const fileengine_rpc::RemoveDirectoryRequest* request,
                                              fileengine_rpc::RemoveDirectoryResponse* response) {
    LOG_DEBUG("GRPCService", "RemoveDirectory called for uid: " + request->uid());
    std::string dir_uid = request->uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions before removing
    if (!validate_user_permissions(dir_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to remove directory");
        LOG_ERROR("GRPCService", "RemoveDirectory failed: User " + user + " does not have permission to remove directory " + dir_uid);
        return grpc::Status::OK;
    }

    auto result = filesystem_->rmdir(dir_uid, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "RemoveDirectory failed for uid: " + dir_uid + " with error: " + result.error);
    } else {
        LOG_INFO("GRPCService", "RemoveDirectory successful for uid: " + dir_uid);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::ListDirectory(grpc::ServerContext* context,
                                            const fileengine_rpc::ListDirectoryRequest* request,
                                            fileengine_rpc::ListDirectoryResponse* response) {
    LOG_DEBUG("GRPCService", "ListDirectory called for uid: " + request->uid());
    std::string dir_uid = request->uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check read permissions on the directory
    if (!validate_user_permissions(dir_uid, auth_context, 0400)) { // READ permission
        response->set_success(false);
        response->set_error("User does not have permission to list directory");
        LOG_ERROR("GRPCService", "ListDirectory failed: User " + user + " does not have permission to list directory " + dir_uid);
        return grpc::Status::OK;
    }

    auto result = filesystem_->listdir(dir_uid, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "ListDirectory failed for uid: " + dir_uid + " with error: " + result.error);
    } else {
        for (const auto& entry : result.value) {
            auto* dir_entry = response->add_entries();
            dir_entry->set_uid(entry.uid);
            dir_entry->set_name(entry.name);
            // Convert internal file type to gRPC file type
            fileengine_rpc::FileType grpc_file_type;
            switch (entry.type) {
                case fileengine::FileType::REGULAR_FILE:
                    grpc_file_type = fileengine_rpc::FileType::REGULAR_FILE;
                    break;
                case fileengine::FileType::DIRECTORY:
                    grpc_file_type = fileengine_rpc::FileType::DIRECTORY;
                    break;
                case fileengine::FileType::SYMLINK:
                    grpc_file_type = fileengine_rpc::FileType::SYMLINK;
                    break;
                default:
                    grpc_file_type = fileengine_rpc::FileType::REGULAR_FILE; // default
                    break;
            }
            dir_entry->set_type(grpc_file_type);
            dir_entry->set_size(entry.size);
            // Internal DirectoryEntry already has int64_t values, so assign directly
            dir_entry->set_created_at(entry.created_at);
            dir_entry->set_modified_at(entry.modified_at);
            dir_entry->set_version_count(entry.version_count);
        }
        LOG_INFO("GRPCService", "ListDirectory successful for uid: " + dir_uid);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::ListDirectoryWithDeleted(grpc::ServerContext* context,
                                                       const fileengine_rpc::ListDirectoryWithDeletedRequest* request,
                                                       fileengine_rpc::ListDirectoryWithDeletedResponse* response) {
    LOG_DEBUG("GRPCService", "ListDirectoryWithDeleted called for uid: " + request->uid());
    // For now, implement the same as ListDirectory but indicate this functionality would return deleted items too
    std::string dir_uid = request->uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check read permissions on the directory
    if (!validate_user_permissions(dir_uid, auth_context, 0400)) { // READ permission
        response->set_success(false);
        response->set_error("User does not have permission to list directory with deleted items");
        LOG_ERROR("GRPCService", "ListDirectoryWithDeleted failed: User " + user + " does not have permission to list directory " + dir_uid);
        return grpc::Status::OK;
    }

    auto result = filesystem_->listdir_with_deleted(dir_uid, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "ListDirectoryWithDeleted failed for uid: " + dir_uid + " with error: " + result.error);
    } else {
        for (const auto& entry : result.value) {
            auto* dir_entry = response->add_entries();
            dir_entry->set_uid(entry.uid);
            dir_entry->set_name(entry.name);
            // Convert internal file type to gRPC file type
            fileengine_rpc::FileType grpc_file_type;
            switch (entry.type) {
                case fileengine::FileType::REGULAR_FILE:
                    grpc_file_type = fileengine_rpc::FileType::REGULAR_FILE;
                    break;
                case fileengine::FileType::DIRECTORY:
                    grpc_file_type = fileengine_rpc::FileType::DIRECTORY;
                    break;
                case fileengine::FileType::SYMLINK:
                    grpc_file_type = fileengine_rpc::FileType::SYMLINK;
                    break;
                default:
                    grpc_file_type = fileengine_rpc::FileType::REGULAR_FILE; // default
                    break;
            }
            dir_entry->set_type(grpc_file_type);
            dir_entry->set_size(entry.size);
            // Internal DirectoryEntry already has int64_t values, so assign directly
            dir_entry->set_created_at(entry.created_at);
            dir_entry->set_modified_at(entry.modified_at);
            dir_entry->set_version_count(entry.version_count);
        }
        LOG_INFO("GRPCService", "ListDirectoryWithDeleted successful for uid: " + dir_uid);
    }

    return grpc::Status::OK;
}

// File operations
grpc::Status GRPCFileService::Touch(grpc::ServerContext* context,
                                   const fileengine_rpc::TouchRequest* request,
                                   fileengine_rpc::TouchResponse* response) {
    LOG_DEBUG("GRPCService", "Touch called for parent_uid: " + request->parent_uid() + ", name: " + request->name());
    // Check if server is in read-only mode
    if (is_server_in_readonly_mode()) {
        response->set_success(false);
        response->set_error("Server is in read-only mode due to database disconnection");
        LOG_ERROR("GRPCService", "Touch failed: Server is in read-only mode");
        return grpc::Status::OK;
    }

    std::string parent_uid = request->parent_uid();
    std::string name = request->name();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions on parent directory
    if (!validate_user_permissions(parent_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to create file in this directory");
        LOG_ERROR("GRPCService", "Touch failed: User " + user + " does not have permission to create file in directory " + parent_uid);
        return grpc::Status::OK;
    }

    auto result = filesystem_->touch(parent_uid, name, user, tenant);

    response->set_success(result.success);
    if (result.success) {
        response->set_uid(result.value);
        response->set_error("");
        LOG_INFO("GRPCService", "Touch successful for parent_uid: " + parent_uid + ", name: " + name);
    } else {
        response->set_uid("");
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "Touch failed for parent_uid: " + parent_uid + ", name: " + name + " with error: " + result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::RemoveFile(grpc::ServerContext* context,
                                        const fileengine_rpc::RemoveFileRequest* request,
                                        fileengine_rpc::RemoveFileResponse* response) {
    LOG_DEBUG("GRPCService", "RemoveFile called for uid: " + request->uid());
    // Check if server is in read-only mode
    if (is_server_in_readonly_mode()) {
        response->set_success(false);
        response->set_error("Server is in read-only mode due to database disconnection");
        LOG_ERROR("GRPCService", "RemoveFile failed: Server is in read-only mode");
        return grpc::Status::OK;
    }

    std::string file_uid = request->uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions on the file
    if (!validate_user_permissions(file_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to remove file");
        LOG_ERROR("GRPCService", "RemoveFile failed: User " + user + " does not have permission to remove file " + file_uid);
        return grpc::Status::OK;
    }

    auto result = filesystem_->remove(file_uid, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "RemoveFile failed for uid: " + file_uid + " with error: " + result.error);
    } else {
        LOG_INFO("GRPCService", "RemoveFile successful for uid: " + file_uid);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::UndeleteFile(grpc::ServerContext* context,
                                          const fileengine_rpc::UndeleteFileRequest* request,
                                          fileengine_rpc::UndeleteFileResponse* response) {
    LOG_DEBUG("GRPCService", "UndeleteFile called for uid: " + request->uid());
    std::string file_uid = request->uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // For now, we'll just check if the user has permission to write in the directory
    // In a real implementation, there would be a special undelete permission check
    if (!validate_user_permissions(file_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to undelete file");
        LOG_ERROR("GRPCService", "UndeleteFile failed: User " + user + " does not have permission to undelete file " + file_uid);
        return grpc::Status::OK;
    }

    // In a real implementation, this would undelete the file
    // For now, we'll return an error to indicate it's not supported
    response->set_success(false);
    response->set_error("Undelete functionality not implemented in this version");
    LOG_ERROR("GRPCService", "UndeleteFile failed: Not implemented");

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::PutFile(grpc::ServerContext* context,
                                     const fileengine_rpc::PutFileRequest* request,
                                     fileengine_rpc::PutFileResponse* response) {
    LOG_DEBUG("GRPCService", "PutFile called for uid: " + request->uid());
    // Check if server is in read-only mode
    if (is_server_in_readonly_mode()) {
        response->set_success(false);
        response->set_error("Server is in read-only mode due to database disconnection");
        LOG_ERROR("GRPCService", "PutFile failed: Server is in read-only mode");
        return grpc::Status::OK;
    }

    std::string file_uid = request->uid();
    auto file_data = request->data();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs write access to the file
    if (!validate_user_permissions(file_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to write to file");
        LOG_ERROR("GRPCService", "PutFile failed: User " + user + " does not have permission to write to file " + file_uid);
        return grpc::Status::OK;
    }

    // Convert bytes to std::vector<uint8_t>
    std::vector<uint8_t> data_vec(file_data.begin(), file_data.end());

    auto result = filesystem_->put(file_uid, data_vec, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "PutFile failed for uid: " + file_uid + " with error: " + result.error);
    } else {
        LOG_INFO("GRPCService", "PutFile successful for uid: " + file_uid);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::GetFile(grpc::ServerContext* context,
                                     const fileengine_rpc::GetFileRequest* request,
                                     fileengine_rpc::GetFileResponse* response) {
    LOG_DEBUG("GRPCService", "GetFile called for uid: " + request->uid());
    std::string file_uid = request->uid();
    std::string version_timestamp = request->version_timestamp();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs read access to the file
    if (!validate_user_permissions(file_uid, auth_context, 0400)) { // READ permission
        response->set_success(false);
        response->set_error("User does not have permission to read file");
        LOG_ERROR("GRPCService", "GetFile failed: User " + user + " does not have permission to read file " + file_uid);
        return grpc::Status::OK;
    }

    auto result = filesystem_->get(file_uid, user, tenant);

    response->set_success(result.success);
    if (result.success) {
        response->set_data(std::string(result.value.begin(), result.value.end()));
        response->set_error("");
        LOG_INFO("GRPCService", "GetFile successful for uid: " + file_uid);
    } else {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "GetFile failed for uid: " + file_uid + " with error: " + result.error);
    }

    return grpc::Status::OK;
}

// File information operations
grpc::Status GRPCFileService::Stat(grpc::ServerContext* context,
                                  const fileengine_rpc::StatRequest* request,
                                  fileengine_rpc::StatResponse* response) {
    LOG_DEBUG("GRPCService", "Stat called for uid: " + request->uid());
    std::string file_uid = request->uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs read access to stat the file
    if (!validate_user_permissions(file_uid, auth_context, 0400)) { // READ permission
        response->set_success(false);
        response->set_error("User does not have permission to access file information");
        LOG_ERROR("GRPCService", "Stat failed: User " + user + " does not have permission to access file information for " + file_uid);
        return grpc::Status::OK;
    }

    auto result = filesystem_->stat(file_uid, user, tenant);

    response->set_success(result.success);
    if (result.success) {
        auto* info = response->mutable_info();
        info->set_uid(result.value.uid);
        info->set_name(result.value.name);
        info->set_parent_uid(result.value.parent_uid);
        // Convert internal file type to gRPC file type
        fileengine_rpc::FileType grpc_file_type;
        switch (result.value.type) {
            case fileengine::FileType::REGULAR_FILE:
                grpc_file_type = fileengine_rpc::FileType::REGULAR_FILE;
                break;
            case fileengine::FileType::DIRECTORY:
                grpc_file_type = fileengine_rpc::FileType::DIRECTORY;
                break;
            case fileengine::FileType::SYMLINK:
                grpc_file_type = fileengine_rpc::FileType::SYMLINK;
                break;
            default:
                grpc_file_type = fileengine_rpc::FileType::REGULAR_FILE; // default
                break;
        }
        info->set_type(grpc_file_type);
        info->set_size(result.value.size);
        info->set_owner(result.value.owner);
        info->set_permissions(result.value.permissions);
        // Convert time point to timestamp
        info->set_created_at(std::chrono::duration_cast<std::chrono::seconds>(
            result.value.created_at.time_since_epoch()).count());
        info->set_modified_at(std::chrono::duration_cast<std::chrono::seconds>(
            result.value.modified_at.time_since_epoch()).count());
        info->set_version(result.value.version);
        LOG_INFO("GRPCService", "Stat successful for uid: " + file_uid);
    } else {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "Stat failed for uid: " + file_uid + " with error: " + result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::Exists(grpc::ServerContext* context,
                                    const fileengine_rpc::ExistsRequest* request,
                                    fileengine_rpc::ExistsResponse* response) {
    LOG_DEBUG("GRPCService", "Exists called for uid: " + request->uid());
    std::string file_uid = request->uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);

    auto result = filesystem_->exists(file_uid, tenant);

    response->set_success(result.success);
    if (result.success) {
        response->set_exists(result.value);
        LOG_INFO("GRPCService", "Exists successful for uid: " + file_uid);
    } else {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "Exists failed for uid: " + file_uid + " with error: " + result.error);
    }

    return grpc::Status::OK;
}

// File manipulation operations
grpc::Status GRPCFileService::Rename(grpc::ServerContext* context,
                                    const fileengine_rpc::RenameRequest* request,
                                    fileengine_rpc::RenameResponse* response) {
    LOG_DEBUG("GRPCService", "Rename called for uid: " + request->uid() + " to " + request->new_name());
    // Check if server is in read-only mode
    if (is_server_in_readonly_mode()) {
        response->set_success(false);
        response->set_error("Server is in read-only mode due to database disconnection");
        LOG_ERROR("GRPCService", "Rename failed: Server is in read-only mode");
        return grpc::Status::OK;
    }

    std::string uid = request->uid();
    std::string new_name = request->new_name();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs write access to rename the file
    if (!validate_user_permissions(uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to rename file");
        LOG_ERROR("GRPCService", "Rename failed: User " + user + " does not have permission to rename file " + uid);
        return grpc::Status::OK;
    }

    auto result = filesystem_->rename(uid, new_name, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "Rename failed for uid: " + uid + " with error: " + result.error);
    } else {
        LOG_INFO("GRPCService", "Rename successful for uid: " + uid);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::Move(grpc::ServerContext* context,
                                  const fileengine_rpc::MoveRequest* request,
                                  fileengine_rpc::MoveResponse* response) {
    LOG_DEBUG("GRPCService", "Move called for source_uid: " + request->source_uid() + " to " + request->destination_parent_uid());
    std::string source_uid = request->source_uid();
    std::string dest_uid = request->destination_parent_uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions on source and destination
    if (!validate_user_permissions(source_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to move source file");
        LOG_ERROR("GRPCService", "Move failed: User " + user + " does not have permission to move source file " + source_uid);
        return grpc::Status::OK;
    }

    if (!validate_user_permissions(dest_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to move to destination directory");
        LOG_ERROR("GRPCService", "Move failed: User " + user + " does not have permission to move to destination directory " + dest_uid);
        return grpc::Status::OK;
    }

    auto result = filesystem_->move(source_uid, dest_uid, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "Move failed for source_uid: " + source_uid + " to " + dest_uid + " with error: " + result.error);
    } else {
        LOG_INFO("GRPCService", "Move successful for source_uid: " + source_uid + " to " + dest_uid);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::Copy(grpc::ServerContext* context,
                                  const fileengine_rpc::CopyRequest* request,
                                  fileengine_rpc::CopyResponse* response) {
    LOG_DEBUG("GRPCService", "Copy called for source_uid: " + request->source_uid() + " to " + request->destination_parent_uid());
    std::string source_uid = request->source_uid();
    std::string dest_uid = request->destination_parent_uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions on source and destination
    if (!validate_user_permissions(source_uid, auth_context, 0400)) { // READ permission
        response->set_success(false);
        response->set_error("User does not have permission to read source file");
        LOG_ERROR("GRPCService", "Copy failed: User " + user + " does not have permission to read source file " + source_uid);
        return grpc::Status::OK;
    }

    if (!validate_user_permissions(dest_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to write to destination directory");
        LOG_ERROR("GRPCService", "Copy failed: User " + user + " does not have permission to write to destination directory " + dest_uid);
        return grpc::Status::OK;
    }

    auto result = filesystem_->copy(source_uid, dest_uid, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "Copy failed for source_uid: " + source_uid + " to " + dest_uid + " with error: " + result.error);
    } else {
        LOG_INFO("GRPCService", "Copy successful for source_uid: " + source_uid + " to " + dest_uid);
    }

    return grpc::Status::OK;
}

// Version operations
grpc::Status GRPCFileService::ListVersions(grpc::ServerContext* context,
                                          const fileengine_rpc::ListVersionsRequest* request,
                                          fileengine_rpc::ListVersionsResponse* response) {
    LOG_DEBUG("GRPCService", "ListVersions called for uid: " + request->uid());
    std::string file_uid = request->uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs read access to list file versions
    if (!validate_user_permissions(file_uid, auth_context, 0400)) { // READ permission
        response->set_success(false);
        response->set_error("User does not have permission to list file versions");
        LOG_ERROR("GRPCService", "ListVersions failed: User " + user + " does not have permission to list file versions for " + file_uid);
        return grpc::Status::OK;
    }

    auto result = filesystem_->list_versions(file_uid, user, tenant);

    response->set_success(result.success);
    if (result.success) {
        for (const auto& version : result.value) {
            response->add_versions(version);
        }
        LOG_INFO("GRPCService", "ListVersions successful for uid: " + file_uid);
    } else {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "ListVersions failed for uid: " + file_uid + " with error: " + result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::GetVersion(grpc::ServerContext* context,
                                        const fileengine_rpc::GetVersionRequest* request,
                                        fileengine_rpc::GetVersionResponse* response) {
    LOG_DEBUG("GRPCService", "GetVersion called for uid: " + request->uid() + " with version " + request->version_timestamp());
    std::string file_uid = request->uid();
    std::string version_timestamp = request->version_timestamp();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs read access to access file version
    if (!validate_user_permissions(file_uid, auth_context, 0400)) { // READ permission
        response->set_success(false);
        response->set_error("User does not have permission to access file version");
        LOG_ERROR("GRPCService", "GetVersion failed: User " + user + " does not have permission to access file version for " + file_uid);
        return grpc::Status::OK;
    }

    auto result = filesystem_->get_version(file_uid, version_timestamp, user, tenant);

    response->set_success(result.success);
    if (result.success) {
        response->set_data(std::string(result.value.begin(), result.value.end()));
        LOG_INFO("GRPCService", "GetVersion successful for uid: " + file_uid);
    } else {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "GetVersion failed for uid: " + file_uid + " with error: " + result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::RestoreToVersion(grpc::ServerContext* context,
                                              const fileengine_rpc::RestoreToVersionRequest* request,
                                              fileengine_rpc::RestoreToVersionResponse* response) {
    LOG_DEBUG("GRPCService", "RestoreToVersion called for uid: " + request->uid() + " with version " + request->version_timestamp());
    std::string file_uid = request->uid();
    std::string version_timestamp = request->version_timestamp();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs write access to restore to version
    // In some systems, this might be a special permission, but typically requires write access
    if (!validate_user_permissions(file_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to restore to version");
        LOG_ERROR("GRPCService", "RestoreToVersion failed: User " + user + " does not have permission to restore to version for " + file_uid);
        return grpc::Status::OK;
    }

    auto result = filesystem_->restore_to_version(file_uid, version_timestamp, user, tenant);

    response->set_success(result.success);
    if (result.success) {
        response->set_restored_version(version_timestamp);
        response->set_error("");
        LOG_INFO("GRPCService", "RestoreToVersion successful for uid: " + file_uid);
    } else {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "RestoreToVersion failed for uid: " + file_uid + " with error: " + result.error);
    }

    return grpc::Status::OK;
}

// Metadata operations
grpc::Status GRPCFileService::SetMetadata(grpc::ServerContext* context,
                                         const fileengine_rpc::SetMetadataRequest* request,
                                         fileengine_rpc::SetMetadataResponse* response) {
    LOG_DEBUG("GRPCService", "SetMetadata called for uid: " + request->uid() + " with key " + request->key());
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
        LOG_ERROR("GRPCService", "SetMetadata failed: User " + user + " does not have permission to set metadata for " + file_uid);
        return grpc::Status::OK;
    }

    auto result = filesystem_->set_metadata(file_uid, key, value, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "SetMetadata failed for uid: " + file_uid + " with error: " + result.error);
    } else {
        LOG_INFO("GRPCService", "SetMetadata successful for uid: " + file_uid);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::GetMetadata(grpc::ServerContext* context,
                                         const fileengine_rpc::GetMetadataRequest* request,
                                         fileengine_rpc::GetMetadataResponse* response) {
    LOG_DEBUG("GRPCService", "GetMetadata called for uid: " + request->uid() + " with key " + request->key());
    std::string file_uid = request->uid();
    std::string key = request->key();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs read access to get metadata
    if (!validate_user_permissions(file_uid, auth_context, 0400)) { // READ permission
        response->set_success(false);
        response->set_error("User does not have permission to get metadata");
        LOG_ERROR("GRPCService", "GetMetadata failed: User " + user + " does not have permission to get metadata for " + file_uid);
        return grpc::Status::OK;
    }

    auto result = filesystem_->get_metadata(file_uid, key, user, tenant);

    response->set_success(result.success);
    if (result.success) {
        response->set_value(result.value);
        LOG_INFO("GRPCService", "GetMetadata successful for uid: " + file_uid);
    } else {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "GetMetadata failed for uid: " + file_uid + " with error: " + result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::GetAllMetadata(grpc::ServerContext* context,
                                            const fileengine_rpc::GetAllMetadataRequest* request,
                                            fileengine_rpc::GetAllMetadataResponse* response) {
    LOG_DEBUG("GRPCService", "GetAllMetadata called for uid: " + request->uid());
    std::string file_uid = request->uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs read access to get metadata
    if (!validate_user_permissions(file_uid, auth_context, 0400)) { // READ permission
        response->set_success(false);
        response->set_error("User does not have permission to get metadata");
        LOG_ERROR("GRPCService", "GetAllMetadata failed: User " + user + " does not have permission to get metadata for " + file_uid);
        return grpc::Status::OK;
    }

    auto result = filesystem_->get_all_metadata(file_uid, user, tenant);

    response->set_success(result.success);
    if (result.success) {
        for (const auto& [k, v] : result.value) {
            (*response->mutable_metadata())[k] = v;
        }
        LOG_INFO("GRPCService", "GetAllMetadata successful for uid: " + file_uid);
    } else {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "GetAllMetadata failed for uid: " + file_uid + " with error: " + result.error);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::DeleteMetadata(grpc::ServerContext* context,
                                            const fileengine_rpc::DeleteMetadataRequest* request,
                                            fileengine_rpc::DeleteMetadataResponse* response) {
    LOG_DEBUG("GRPCService", "DeleteMetadata called for uid: " + request->uid() + " with key " + request->key());
    std::string file_uid = request->uid();
    std::string key = request->key();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs write access to delete metadata
    if (!validate_user_permissions(file_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to delete metadata");
        LOG_ERROR("GRPCService", "DeleteMetadata failed: User " + user + " does not have permission to delete metadata for " + file_uid);
        return grpc::Status::OK;
    }

    auto result = filesystem_->delete_metadata(file_uid, key, user, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "DeleteMetadata failed for uid: " + file_uid + " with error: " + result.error);
    } else {
        LOG_INFO("GRPCService", "DeleteMetadata successful for uid: " + file_uid);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::GetMetadataForVersion(grpc::ServerContext* context,
                                                   const fileengine_rpc::GetMetadataForVersionRequest* request,
                                                   fileengine_rpc::GetMetadataForVersionResponse* response) {
    LOG_DEBUG("GRPCService", "GetMetadataForVersion called for uid: " + request->uid() + " with version " + request->version_timestamp());
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
        LOG_ERROR("GRPCService", "GetMetadataForVersion failed: User " + user + " does not have permission to get metadata for version for " + file_uid);
        return grpc::Status::OK;
    }

    // In a real implementation, this would get metadata for a specific version
    // For now, we'll return an error to indicate it's not supported
    response->set_success(false);
    response->set_error("Get metadata for version functionality not implemented in this version");
    LOG_ERROR("GRPCService", "GetMetadataForVersion failed: Not implemented");

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::GetAllMetadataForVersion(grpc::ServerContext* context,
                                                      const fileengine_rpc::GetAllMetadataForVersionRequest* request,
                                                      fileengine_rpc::GetAllMetadataForVersionResponse* response) {
    LOG_DEBUG("GRPCService", "GetAllMetadataForVersion called for uid: " + request->uid() + " with version " + request->version_timestamp());
    std::string file_uid = request->uid();
    std::string version_timestamp = request->version_timestamp();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs read access to get metadata
    if (!validate_user_permissions(file_uid, auth_context, 0400)) { // READ permission
        response->set_success(false);
        response->set_error("User does not have permission to get metadata for version");
        LOG_ERROR("GRPCService", "GetAllMetadataForVersion failed: User " + user + " does not have permission to get metadata for version for " + file_uid);
        return grpc::Status::OK;
    }

    // In a real implementation, this would get all metadata for a specific version
    // For now, we'll return an error to indicate it's not supported
    response->set_success(false);
    response->set_error("Get all metadata for version functionality not implemented in this version");
    LOG_ERROR("GRPCService", "GetAllMetadataForVersion failed: Not implemented");

    return grpc::Status::OK;
}

// ACL operations
grpc::Status GRPCFileService::GrantPermission(grpc::ServerContext* context,
                                             const fileengine_rpc::GrantPermissionRequest* request,
                                             fileengine_rpc::GrantPermissionResponse* response) {
    LOG_DEBUG("GRPCService", "GrantPermission called for resource_uid: " + request->resource_uid() + " for principal " + request->principal());
    std::string resource_uid = request->resource_uid();
    std::string principal = request->principal();
    fileengine_rpc::Permission permission = request->permission();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Only admins or users with grant permission can grant permissions
    // Check if it's a root user first
    if (user != "root" && !validate_user_permissions(resource_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to grant permissions");
        LOG_ERROR("GRPCService", "GrantPermission failed: User " + user + " does not have permission to grant permissions on " + resource_uid);
        return grpc::Status::OK;
    }

    // Convert gRPC Permission to our internal representation
    int converted_permissions;
    switch(permission) {
        case fileengine_rpc::Permission::READ:
            converted_permissions = static_cast<int>(fileengine::Permission::READ);
            break;
        case fileengine_rpc::Permission::WRITE:
            converted_permissions = static_cast<int>(fileengine::Permission::WRITE);
            break;
        case fileengine_rpc::Permission::DELETE:
            converted_permissions = static_cast<int>(fileengine::Permission::DELETE);
            break;
        case fileengine_rpc::Permission::LIST_DELETED:
            converted_permissions = static_cast<int>(fileengine::Permission::LIST_DELETED);
            break;
        case fileengine_rpc::Permission::UNDELETE:
            converted_permissions = static_cast<int>(fileengine::Permission::UNDELETE);
            break;
        case fileengine_rpc::Permission::VIEW_VERSIONS:
            converted_permissions = static_cast<int>(fileengine::Permission::VIEW_VERSIONS);
            break;
        case fileengine_rpc::Permission::RETRIEVE_BACK_VERSION:
            converted_permissions = static_cast<int>(fileengine::Permission::RETRIEVE_BACK_VERSION);
            break;
        case fileengine_rpc::Permission::RESTORE_TO_VERSION:
            converted_permissions = static_cast<int>(fileengine::Permission::RESTORE_TO_VERSION);
            break;
        case fileengine_rpc::Permission::EXECUTE:
            converted_permissions = static_cast<int>(fileengine::Permission::EXECUTE);
            break;
        default:
            converted_permissions = static_cast<int>(fileengine::Permission::READ);  // Default to read permission
            break;
    }

    auto result = acl_manager_->grant_permission(resource_uid, principal,
                                                 PrincipalType::USER,  // Simplified for this example
                                                 converted_permissions, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "GrantPermission failed for resource_uid: " + resource_uid + " with error: " + result.error);
    } else {
        LOG_INFO("GRPCService", "GrantPermission successful for resource_uid: " + resource_uid);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::RevokePermission(grpc::ServerContext* context,
                                              const fileengine_rpc::RevokePermissionRequest* request,
                                              fileengine_rpc::RevokePermissionResponse* response) {
    LOG_DEBUG("GRPCService", "RevokePermission called for resource_uid: " + request->resource_uid() + " for principal " + request->principal());
    std::string resource_uid = request->resource_uid();
    std::string principal = request->principal();
    fileengine_rpc::Permission permission = request->permission();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Only admins or users with appropriate permissions can revoke permissions
    if (user != "root" && !validate_user_permissions(resource_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to revoke permissions");
        LOG_ERROR("GRPCService", "RevokePermission failed: User " + user + " does not have permission to revoke permissions on " + resource_uid);
        return grpc::Status::OK;
    }

    int converted_permissions;
    switch(permission) {
        case fileengine_rpc::Permission::READ:
            converted_permissions = static_cast<int>(fileengine::Permission::READ);
            break;
        case fileengine_rpc::Permission::WRITE:
            converted_permissions = static_cast<int>(fileengine::Permission::WRITE);
            break;
        case fileengine_rpc::Permission::DELETE:
            converted_permissions = static_cast<int>(fileengine::Permission::DELETE);
            break;
        case fileengine_rpc::Permission::LIST_DELETED:
            converted_permissions = static_cast<int>(fileengine::Permission::LIST_DELETED);
            break;
        case fileengine_rpc::Permission::UNDELETE:
            converted_permissions = static_cast<int>(fileengine::Permission::UNDELETE);
            break;
        case fileengine_rpc::Permission::VIEW_VERSIONS:
            converted_permissions = static_cast<int>(fileengine::Permission::VIEW_VERSIONS);
            break;
        case fileengine_rpc::Permission::RETRIEVE_BACK_VERSION:
            converted_permissions = static_cast<int>(fileengine::Permission::RETRIEVE_BACK_VERSION);
            break;
        case fileengine_rpc::Permission::RESTORE_TO_VERSION:
            converted_permissions = static_cast<int>(fileengine::Permission::RESTORE_TO_VERSION);
            break;
        case fileengine_rpc::Permission::EXECUTE:
            converted_permissions = static_cast<int>(fileengine::Permission::EXECUTE);
            break;
        default:
            converted_permissions = static_cast<int>(fileengine::Permission::READ);  // Default to read permission
            break;
    }

    auto result = acl_manager_->revoke_permission(resource_uid, principal,
                                                  PrincipalType::USER,  // Simplified for this example
                                                  converted_permissions, tenant);

    response->set_success(result.success);
    if (!result.success) {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "RevokePermission failed for resource_uid: " + resource_uid + " with error: " + result.error);
    } else {
        LOG_INFO("GRPCService", "RevokePermission successful for resource_uid: " + resource_uid);
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::CheckPermission(grpc::ServerContext* context,
                                             const fileengine_rpc::CheckPermissionRequest* request,
                                             fileengine_rpc::CheckPermissionResponse* response) {
    LOG_DEBUG("GRPCService", "CheckPermission called for resource_uid: " + request->resource_uid() + " for user " + request->auth().user());
    std::string resource_uid = request->resource_uid();
    fileengine_rpc::Permission required_permission = request->required_permission();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Convert roles from gRPC context to internal representation
    std::vector<std::string> roles = get_roles_from_auth_context(auth_context);

    int required_permissions_int;
    switch(required_permission) {
        case fileengine_rpc::Permission::READ:
            required_permissions_int = static_cast<int>(fileengine::Permission::READ);
            break;
        case fileengine_rpc::Permission::WRITE:
            required_permissions_int = static_cast<int>(fileengine::Permission::WRITE);
            break;
        case fileengine_rpc::Permission::DELETE:
            required_permissions_int = static_cast<int>(fileengine::Permission::DELETE);
            break;
        case fileengine_rpc::Permission::LIST_DELETED:
            required_permissions_int = static_cast<int>(fileengine::Permission::LIST_DELETED);
            break;
        case fileengine_rpc::Permission::UNDELETE:
            required_permissions_int = static_cast<int>(fileengine::Permission::UNDELETE);
            break;
        case fileengine_rpc::Permission::VIEW_VERSIONS:
            required_permissions_int = static_cast<int>(fileengine::Permission::VIEW_VERSIONS);
            break;
        case fileengine_rpc::Permission::RETRIEVE_BACK_VERSION:
            required_permissions_int = static_cast<int>(fileengine::Permission::RETRIEVE_BACK_VERSION);
            break;
        case fileengine_rpc::Permission::RESTORE_TO_VERSION:
            required_permissions_int = static_cast<int>(fileengine::Permission::RESTORE_TO_VERSION);
            break;
        case fileengine_rpc::Permission::EXECUTE:
            required_permissions_int = static_cast<int>(fileengine::Permission::EXECUTE);
            break;
        default:
            required_permissions_int = static_cast<int>(fileengine::Permission::READ);  // Default to read permission
            break;
    }

    auto result = acl_manager_->check_permission(resource_uid, user, roles,
                                                 required_permissions_int, tenant);

    response->set_success(result.success);
    if (result.success) {
        response->set_has_permission(result.value);
        LOG_INFO("GRPCService", "CheckPermission successful for resource_uid: " + resource_uid);
    } else {
        response->set_error(result.error);
        LOG_ERROR("GRPCService", "CheckPermission failed for resource_uid: " + resource_uid + " with error: " + result.error);
    }

    return grpc::Status::OK;
}

// Streaming operations for large files
grpc::Status GRPCFileService::StreamFileUpload(grpc::ServerContext* context,
                                              grpc::ServerReader<fileengine_rpc::PutFileRequest>* reader,
                                              fileengine_rpc::PutFileResponse* response) {
    LOG_DEBUG("GRPCService", "StreamFileUpload called");
    // Check if server is in read-only mode
    if (is_server_in_readonly_mode()) {
        response->set_success(false);
        response->set_error("Server is in read-only mode due to database disconnection");
        LOG_ERROR("GRPCService", "StreamFileUpload failed: Server is in read-only mode");
        return grpc::Status::OK;
    }

    fileengine_rpc::PutFileRequest request;
    std::vector<uint8_t> full_data;
    std::string file_uid;
    fileengine_rpc::AuthenticationContext auth_context;
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
            LOG_ERROR("GRPCService", "StreamFileUpload failed: User " + user + " does not have permission to write to file " + file_uid);
            return grpc::Status::OK;
        }

        auto result = filesystem_->put(file_uid, full_data, user, tenant);

        response->set_success(result.success);
        if (!result.success) {
            response->set_error(result.error);
            LOG_ERROR("GRPCService", "StreamFileUpload failed for uid: " + file_uid + " with error: " + result.error);
        } else {
            LOG_INFO("GRPCService", "StreamFileUpload successful for uid: " + file_uid);
        }
    } else {
        response->set_success(false);
        response->set_error("No file data received");
        LOG_ERROR("GRPCService", "StreamFileUpload failed: No file data received");
    }

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::StreamFileDownload(grpc::ServerContext* context,
                                                const fileengine_rpc::GetFileRequest* request,
                                                grpc::ServerWriter<fileengine_rpc::GetFileResponse>* writer) {
    LOG_DEBUG("GRPCService", "StreamFileDownload called for uid: " + request->uid());
    std::string file_uid = request->uid();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs read access to the file
    if (!validate_user_permissions(file_uid, auth_context, 0400)) { // READ permission
        fileengine_rpc::GetFileResponse response;
        response.set_success(false);
        response.set_error("User does not have permission to read file");
        writer->Write(response);
        LOG_ERROR("GRPCService", "StreamFileDownload failed: User " + user + " does not have permission to read file " + file_uid);
        return grpc::Status::OK;
    }

    auto result = filesystem_->get(file_uid, user, tenant);

    if (result.success) {
        // Simulate streaming by sending chunks
        const size_t chunk_size = 1024 * 64; // 64KB chunks
        const auto& data = result.value;
        size_t offset = 0;

        while (offset < data.size()) {
            fileengine_rpc::GetFileResponse response;
            size_t current_chunk_size = std::min(chunk_size, data.size() - offset);
            std::string chunk_data(reinterpret_cast<const char*>(&data[offset]), current_chunk_size);
            
            response.set_data(chunk_data);
            response.set_success(true);
            response.set_error("");

            if (!writer->Write(response)) {
                LOG_ERROR("GRPCService", "StreamFileDownload failed for uid: " + file_uid + " with error: Client disconnected");
                break; // Client disconnected
            }

            offset += current_chunk_size;
        }
        LOG_INFO("GRPCService", "StreamFileDownload successful for uid: " + file_uid);
    } else {
        fileengine_rpc::GetFileResponse response;
        response.set_success(false);
        response.set_error(result.error);
        writer->Write(response);
        LOG_ERROR("GRPCService", "StreamFileDownload failed for uid: " + file_uid + " with error: " + result.error);
    }

    return grpc::Status::OK;
}

// Administrative operations
grpc::Status GRPCFileService::GetStorageUsage(grpc::ServerContext* context,
                                            const fileengine_rpc::StorageUsageRequest* request,
                                            fileengine_rpc::StorageUsageResponse* response) {
    LOG_DEBUG("GRPCService", "GetStorageUsage called for tenant: " + request->tenant());
    auto auth_context = request->auth();
    std::string tenant = request->tenant();

    // In a real implementation, this would check storage usage
    // For this example, we'll return some simulated values
    response->set_success(true);
    response->set_error("");
    response->set_total_space(1024 * 1024 * 1024);  // 1GB
    response->set_used_space(512 * 1024 * 1024);    // 512MB
    response->set_available_space(512 * 1024 * 1024); // 512MB
    response->set_usage_percentage(0.5); // 50%
    LOG_INFO("GRPCService", "GetStorageUsage successful for tenant: " + tenant);

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::PurgeOldVersions(grpc::ServerContext* context,
                                              const fileengine_rpc::PurgeOldVersionsRequest* request,
                                              fileengine_rpc::PurgeOldVersionsResponse* response) {
    LOG_DEBUG("GRPCService", "PurgeOldVersions called for uid: " + request->uid());
    std::string file_uid = request->uid();
    int keep_count = request->keep_count();
    auto auth_context = request->auth();

    std::string tenant = get_tenant_from_auth_context(auth_context);
    std::string user = get_user_from_auth_context(auth_context);

    // Check permissions - user needs write access to the file
    if (user != "root" && !validate_user_permissions(file_uid, auth_context, 0200)) { // WRITE permission
        response->set_success(false);
        response->set_error("User does not have permission to purge old versions");
        LOG_ERROR("GRPCService", "PurgeOldVersions failed: User " + user + " does not have permission to purge old versions for " + file_uid);
        return grpc::Status::OK;
    }

    // In a real implementation, this would remove old versions
    // For now, we'll return an error to indicate it's not supported
    response->set_success(false);
    response->set_error("Purge old versions functionality not implemented in this version");
    LOG_ERROR("GRPCService", "PurgeOldVersions failed: Not implemented");

    return grpc::Status::OK;
}

grpc::Status GRPCFileService::TriggerSync(grpc::ServerContext* context,
                                         const fileengine_rpc::TriggerSyncRequest* request,
                                         fileengine_rpc::TriggerSyncResponse* response) {
    LOG_DEBUG("GRPCService", "TriggerSync called for tenant: " + request->tenant());
    std::string tenant = request->tenant();
    auto auth_context = request->auth();

    // In a real implementation, we would trigger a sync operation here
    // For this example, we'll just return success to indicate the call was accepted
    response->set_success(true);
    response->set_error("");
    LOG_INFO("GRPCService", "TriggerSync successful for tenant: " + tenant);

    return grpc::Status::OK;
}

} // namespace fileengine