#ifndef FILEENGINE_GRPC_SERVICE_H
#define FILEENGINE_GRPC_SERVICE_H

#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>
#include <vector>

// Include the generated gRPC files
#include "fileservice.grpc.pb.h"

#include "fileengine/filesystem.h"
#include "fileengine/tenant_manager.h"
#include "fileengine/acl_manager.h"

namespace fileengine_service {
    // Forward declare generated types to avoid conflicts with fileengine:: namespace
    class FileService;
    class MakeDirectoryRequest;
    class MakeDirectoryResponse;
    class RemoveDirectoryRequest;
    class RemoveDirectoryResponse;
    class ListDirectoryRequest;
    class ListDirectoryResponse;
    class ListDirectoryWithDeletedRequest;
    class ListDirectoryWithDeletedResponse;
    class TouchRequest;
    class TouchResponse;
    class RemoveFileRequest;
    class RemoveFileResponse;
    class UndeleteFileRequest;
    class UndeleteFileResponse;
    class PutFileRequest;
    class PutFileResponse;
    class GetFileRequest;
    class GetFileResponse;
    class StatRequest;
    class StatResponse;
    class ExistsRequest;
    class ExistsResponse;
    class RenameRequest;
    class RenameResponse;
    class MoveRequest;
    class MoveResponse;
    class CopyRequest;
    class CopyResponse;
    class ListVersionsRequest;
    class ListVersionsResponse;
    class GetVersionRequest;
    class GetVersionResponse;
    class SetMetadataRequest;
    class SetMetadataResponse;
    class GetMetadataRequest;
    class GetMetadataResponse;
    class GetAllMetadataRequest;
    class GetAllMetadataResponse;
    class DeleteMetadataRequest;
    class DeleteMetadataResponse;
    class GetMetadataForVersionRequest;
    class GetMetadataForVersionResponse;
    class GetAllMetadataForVersionRequest;
    class GetAllMetadataForVersionResponse;
    class GrantPermissionRequest;
    class GrantPermissionResponse;
    class RevokePermissionRequest;
    class RevokePermissionResponse;
    class CheckPermissionRequest;
    class CheckPermissionResponse;
    class StorageUsageRequest;
    class StorageUsageResponse;
    class PurgeOldVersionsRequest;
    class PurgeOldVersionsResponse;
    class TriggerSyncRequest;
    class TriggerSyncResponse;
    class AuthenticationContext;
}

namespace fileengine {

// The gRPC service implementation that implements the generated fileengine_service::FileService::Service interface
class GRPCFileService final : public fileengine_service::FileService::Service {
public:
    explicit GRPCFileService(std::shared_ptr<FileSystem> filesystem,
                             std::shared_ptr<TenantManager> tenant_manager,
                             std::shared_ptr<AclManager> acl_manager,
                             bool root_user_enabled = false);

    // Directory operations
    grpc::Status MakeDirectory(grpc::ServerContext* context,
                              const fileengine_service::MakeDirectoryRequest* request,
                              fileengine_service::MakeDirectoryResponse* response) override;

    grpc::Status RemoveDirectory(grpc::ServerContext* context,
                                const fileengine_service::RemoveDirectoryRequest* request,
                                fileengine_service::RemoveDirectoryResponse* response) override;

    grpc::Status ListDirectory(grpc::ServerContext* context,
                              const fileengine_service::ListDirectoryRequest* request,
                              fileengine_service::ListDirectoryResponse* response) override;

    grpc::Status ListDirectoryWithDeleted(grpc::ServerContext* context,
                                         const fileengine_service::ListDirectoryWithDeletedRequest* request,
                                         fileengine_service::ListDirectoryWithDeletedResponse* response) override;

    // File operations
    grpc::Status Touch(grpc::ServerContext* context,
                      const fileengine_service::TouchRequest* request,
                      fileengine_service::TouchResponse* response) override;

    grpc::Status RemoveFile(grpc::ServerContext* context,
                           const fileengine_service::RemoveFileRequest* request,
                           fileengine_service::RemoveFileResponse* response) override;

    grpc::Status UndeleteFile(grpc::ServerContext* context,
                             const fileengine_service::UndeleteFileRequest* request,
                             fileengine_service::UndeleteFileResponse* response) override;

    grpc::Status PutFile(grpc::ServerContext* context,
                        const fileengine_service::PutFileRequest* request,
                        fileengine_service::PutFileResponse* response) override;

    grpc::Status GetFile(grpc::ServerContext* context,
                        const fileengine_service::GetFileRequest* request,
                        fileengine_service::GetFileResponse* response) override;

    // File information
    grpc::Status Stat(grpc::ServerContext* context,
                     const fileengine_service::StatRequest* request,
                     fileengine_service::StatResponse* response) override;

    grpc::Status Exists(grpc::ServerContext* context,
                       const fileengine_service::ExistsRequest* request,
                       fileengine_service::ExistsResponse* response) override;

    // File manipulation operations
    grpc::Status Rename(grpc::ServerContext* context,
                       const fileengine_service::RenameRequest* request,
                       fileengine_service::RenameResponse* response) override;

    grpc::Status Move(grpc::ServerContext* context,
                     const fileengine_service::MoveRequest* request,
                     fileengine_service::MoveResponse* response) override;

    grpc::Status Copy(grpc::ServerContext* context,
                     const fileengine_service::CopyRequest* request,
                     fileengine_service::CopyResponse* response) override;

    // Version operations
    grpc::Status ListVersions(grpc::ServerContext* context,
                             const fileengine_service::ListVersionsRequest* request,
                             fileengine_service::ListVersionsResponse* response) override;

    grpc::Status GetVersion(grpc::ServerContext* context,
                           const fileengine_service::GetVersionRequest* request,
                           fileengine_service::GetVersionResponse* response) override;

    // Metadata operations
    grpc::Status SetMetadata(grpc::ServerContext* context,
                            const fileengine_service::SetMetadataRequest* request,
                            fileengine_service::SetMetadataResponse* response) override;

    grpc::Status GetMetadata(grpc::ServerContext* context,
                            const fileengine_service::GetMetadataRequest* request,
                            fileengine_service::GetMetadataResponse* response) override;

    grpc::Status GetAllMetadata(grpc::ServerContext* context,
                               const fileengine_service::GetAllMetadataRequest* request,
                               fileengine_service::GetAllMetadataResponse* response) override;

    grpc::Status DeleteMetadata(grpc::ServerContext* context,
                               const fileengine_service::DeleteMetadataRequest* request,
                               fileengine_service::DeleteMetadataResponse* response) override;

    grpc::Status GetMetadataForVersion(grpc::ServerContext* context,
                                      const fileengine_service::GetMetadataForVersionRequest* request,
                                      fileengine_service::GetMetadataForVersionResponse* response) override;

    grpc::Status GetAllMetadataForVersion(grpc::ServerContext* context,
                                         const fileengine_service::GetAllMetadataForVersionRequest* request,
                                         fileengine_service::GetAllMetadataForVersionResponse* response) override;

    // ACL operations
    grpc::Status GrantPermission(grpc::ServerContext* context,
                                const fileengine_service::GrantPermissionRequest* request,
                                fileengine_service::GrantPermissionResponse* response) override;

    grpc::Status RevokePermission(grpc::ServerContext* context,
                                 const fileengine_service::RevokePermissionRequest* request,
                                 fileengine_service::RevokePermissionResponse* response) override;

    grpc::Status CheckPermission(grpc::ServerContext* context,
                                const fileengine_service::CheckPermissionRequest* request,
                                fileengine_service::CheckPermissionResponse* response) override;

    // Streaming operations for large files
    grpc::Status StreamFileUpload(grpc::ServerContext* context,
                                 grpc::ServerReader<fileengine_service::PutFileRequest>* reader,
                                 fileengine_service::PutFileResponse* response) override;

    grpc::Status StreamFileDownload(grpc::ServerContext* context,
                                   const fileengine_service::GetFileRequest* request,
                                   grpc::ServerWriter<fileengine_service::GetFileResponse>* writer) override;

    // Administrative operations
    grpc::Status GetStorageUsage(grpc::ServerContext* context,
                                const fileengine_service::StorageUsageRequest* request,
                                fileengine_service::StorageUsageResponse* response) override;

    grpc::Status PurgeOldVersions(grpc::ServerContext* context,
                                 const fileengine_service::PurgeOldVersionsRequest* request,
                                 fileengine_service::PurgeOldVersionsResponse* response) override;

    grpc::Status TriggerSync(grpc::ServerContext* context,
                            const fileengine_service::TriggerSyncRequest* request,
                            fileengine_service::TriggerSyncResponse* response) override;

private:
    std::shared_ptr<FileSystem> filesystem_;
    std::shared_ptr<TenantManager> tenant_manager_;
    std::shared_ptr<AclManager> acl_manager_;
    bool root_user_enabled_;

    // Helper function to extract tenant from auth context
    std::string get_tenant_from_auth_context(const fileengine_service::AuthenticationContext& auth_ctx);

    // Helper function to extract user from auth context
    std::string get_user_from_auth_context(const fileengine_service::AuthenticationContext& auth_ctx);

    // Helper function to get roles from auth context
    std::vector<std::string> get_roles_from_auth_context(const fileengine_service::AuthenticationContext& auth_ctx);

    // Helper function to validate permissions
    bool validate_user_permissions(const std::string& resource_uid,
                                  const fileengine_service::AuthenticationContext& auth_ctx,
                                  int required_permissions);
};

} // namespace fileengine

#endif // FILEENGINE_GRPC_SERVICE_H