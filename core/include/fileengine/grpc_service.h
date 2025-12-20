#ifndef FILEENGINE_GRPC_SERVICE_H
#define FILEENGINE_GRPC_SERVICE_H

#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>
#include <vector>

// Include the generated gRPC files - these define the service interface
#include "fileservice.grpc.pb.h"

#include "fileengine/filesystem.h"
#include "fileengine/tenant_manager.h"
#include "fileengine/acl_manager.h"

namespace fileengine {

// The gRPC service implementation that implements the generated FileService::Service interface
class GRPCFileService final : public fileengine_rpc::FileService::Service {
public:
    explicit GRPCFileService(std::shared_ptr<FileSystem> filesystem,
                             std::shared_ptr<TenantManager> tenant_manager,
                             std::shared_ptr<AclManager> acl_manager);

    // Directory operations
    grpc::Status MakeDirectory(grpc::ServerContext* context,
                              const fileengine_rpc::MakeDirectoryRequest* request,
                              fileengine_rpc::MakeDirectoryResponse* response) override;

    grpc::Status RemoveDirectory(grpc::ServerContext* context,
                                const fileengine_rpc::RemoveDirectoryRequest* request,
                                fileengine_rpc::RemoveDirectoryResponse* response) override;

    grpc::Status ListDirectory(grpc::ServerContext* context,
                              const fileengine_rpc::ListDirectoryRequest* request,
                              fileengine_rpc::ListDirectoryResponse* response) override;

    grpc::Status ListDirectoryWithDeleted(grpc::ServerContext* context,
                                         const fileengine_rpc::ListDirectoryWithDeletedRequest* request,
                                         fileengine_rpc::ListDirectoryWithDeletedResponse* response) override;

    // File operations
    grpc::Status Touch(grpc::ServerContext* context,
                      const fileengine_rpc::TouchRequest* request,
                      fileengine_rpc::TouchResponse* response) override;

    grpc::Status RemoveFile(grpc::ServerContext* context,
                           const fileengine_rpc::RemoveFileRequest* request,
                           fileengine_rpc::RemoveFileResponse* response) override;

    grpc::Status UndeleteFile(grpc::ServerContext* context,
                             const fileengine_rpc::UndeleteFileRequest* request,
                             fileengine_rpc::UndeleteFileResponse* response) override;

    grpc::Status PutFile(grpc::ServerContext* context,
                        const fileengine_rpc::PutFileRequest* request,
                        fileengine_rpc::PutFileResponse* response) override;

    grpc::Status GetFile(grpc::ServerContext* context,
                        const fileengine_rpc::GetFileRequest* request,
                        fileengine_rpc::GetFileResponse* response) override;

    // File information operations
    grpc::Status Stat(grpc::ServerContext* context,
                     const fileengine_rpc::StatRequest* request,
                     fileengine_rpc::StatResponse* response) override;

    grpc::Status Exists(grpc::ServerContext* context,
                       const fileengine_rpc::ExistsRequest* request,
                       fileengine_rpc::ExistsResponse* response) override;

    // File manipulation operations
    grpc::Status Rename(grpc::ServerContext* context,
                       const fileengine_rpc::RenameRequest* request,
                       fileengine_rpc::RenameResponse* response) override;

    grpc::Status Move(grpc::ServerContext* context,
                     const fileengine_rpc::MoveRequest* request,
                     fileengine_rpc::MoveResponse* response) override;

    grpc::Status Copy(grpc::ServerContext* context,
                     const fileengine_rpc::CopyRequest* request,
                     fileengine_rpc::CopyResponse* response) override;

    // Version operations
    grpc::Status ListVersions(grpc::ServerContext* context,
                             const fileengine_rpc::ListVersionsRequest* request,
                             fileengine_rpc::ListVersionsResponse* response) override;

    grpc::Status GetVersion(grpc::ServerContext* context,
                           const fileengine_rpc::GetVersionRequest* request,
                           fileengine_rpc::GetVersionResponse* response) override;

    // Metadata operations
    grpc::Status SetMetadata(grpc::ServerContext* context,
                            const fileengine_rpc::SetMetadataRequest* request,
                            fileengine_rpc::SetMetadataResponse* response) override;

    grpc::Status GetMetadata(grpc::ServerContext* context,
                            const fileengine_rpc::GetMetadataRequest* request,
                            fileengine_rpc::GetMetadataResponse* response) override;

    grpc::Status GetAllMetadata(grpc::ServerContext* context,
                               const fileengine_rpc::GetAllMetadataRequest* request,
                               fileengine_rpc::GetAllMetadataResponse* response) override;

    grpc::Status DeleteMetadata(grpc::ServerContext* context,
                               const fileengine_rpc::DeleteMetadataRequest* request,
                               fileengine_rpc::DeleteMetadataResponse* response) override;

    grpc::Status GetMetadataForVersion(grpc::ServerContext* context,
                                      const fileengine_rpc::GetMetadataForVersionRequest* request,
                                      fileengine_rpc::GetMetadataForVersionResponse* response) override;

    grpc::Status GetAllMetadataForVersion(grpc::ServerContext* context,
                                         const fileengine_rpc::GetAllMetadataForVersionRequest* request,
                                         fileengine_rpc::GetAllMetadataForVersionResponse* response) override;

    // ACL operations
    grpc::Status GrantPermission(grpc::ServerContext* context,
                                const fileengine_rpc::GrantPermissionRequest* request,
                                fileengine_rpc::GrantPermissionResponse* response) override;

    grpc::Status RevokePermission(grpc::ServerContext* context,
                                 const fileengine_rpc::RevokePermissionRequest* request,
                                 fileengine_rpc::RevokePermissionResponse* response) override;

    grpc::Status CheckPermission(grpc::ServerContext* context,
                                const fileengine_rpc::CheckPermissionRequest* request,
                                fileengine_rpc::CheckPermissionResponse* response) override;

    // Streaming operations for large files
    grpc::Status StreamFileUpload(grpc::ServerContext* context,
                                 grpc::ServerReader<fileengine_rpc::PutFileRequest>* reader,
                                 fileengine_rpc::PutFileResponse* response) override;

    grpc::Status StreamFileDownload(grpc::ServerContext* context,
                                   const fileengine_rpc::GetFileRequest* request,
                                   grpc::ServerWriter<fileengine_rpc::GetFileResponse>* writer) override;

    // Administrative operations
    grpc::Status GetStorageUsage(grpc::ServerContext* context,
                                const fileengine_rpc::StorageUsageRequest* request,
                                fileengine_rpc::StorageUsageResponse* response) override;

    grpc::Status PurgeOldVersions(grpc::ServerContext* context,
                                 const fileengine_rpc::PurgeOldVersionsRequest* request,
                                 fileengine_rpc::PurgeOldVersionsResponse* response) override;

    grpc::Status TriggerSync(grpc::ServerContext* context,
                            const fileengine_rpc::TriggerSyncRequest* request,
                            fileengine_rpc::TriggerSyncResponse* response) override;

private:
    std::shared_ptr<FileSystem> filesystem_;
    std::shared_ptr<TenantManager> tenant_manager_;
    std::shared_ptr<AclManager> acl_manager_;

    // Helper function to extract tenant from auth context
    std::string get_tenant_from_auth_context(const fileengine_rpc::AuthenticationContext& auth_ctx);

    // Helper function to extract user from auth context
    std::string get_user_from_auth_context(const fileengine_rpc::AuthenticationContext& auth_ctx);

    // Helper function to get roles from auth context
    std::vector<std::string> get_roles_from_auth_context(const fileengine_rpc::AuthenticationContext& auth_ctx);

    // Helper function to validate permissions
    bool validate_user_permissions(const std::string& resource_uid,
                                  const fileengine_rpc::AuthenticationContext& auth_ctx,
                                  int required_permissions);
};

} // namespace fileengine

#endif // FILEENGINE_GRPC_SERVICE_H