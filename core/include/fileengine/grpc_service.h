#ifndef FILEENGINE_GRPC_SERVICE_H
#define FILEENGINE_GRPC_SERVICE_H

#include <grpcpp/grpcpp.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Include the generated gRPC files - these define the service interface
#include "fileservice.grpc.pb.h"

#include "fileengine/filesystem.h"
#include "fileengine/tenant_manager.h"
#include "fileengine/acl_manager.h"
#include "fileengine/role_manager.h"
#include "fileengine/connection_pool_manager.h"
#include "fileengine/storage_tracker.h"
#include "fileengine/audit_sink.h"

namespace fileengine {

// The gRPC service implementation that implements the generated FileService::Service interface
class GRPCFileService final : public fileengine_rpc::FileService::Service {
public:
    explicit GRPCFileService(std::shared_ptr<FileSystem> filesystem,
                             std::shared_ptr<TenantManager> tenant_manager,
                             std::shared_ptr<AclManager> acl_manager,
                             std::unique_ptr<StorageTracker> storage_tracker,
                             std::shared_ptr<IAuditSink> audit_sink = nullptr,
                             const std::string& audit_access_mode = "full",
                             bool audit_hidden_children = false);

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

    grpc::Status RestoreToVersion(grpc::ServerContext* context,
                                 const fileengine_rpc::RestoreToVersionRequest* request,
                                 fileengine_rpc::RestoreToVersionResponse* response) override;

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

    grpc::Status GetEffectivePermissions(grpc::ServerContext* context,
                                const fileengine_rpc::GetEffectivePermissionsRequest* request,
                                fileengine_rpc::GetEffectivePermissionsResponse* response) override;

    grpc::Status GetResourceAcls(grpc::ServerContext* context,
                                const fileengine_rpc::GetResourceAclsRequest* request,
                                fileengine_rpc::GetResourceAclsResponse* response) override;

    // Role management operations
    grpc::Status CreateRole(grpc::ServerContext* context,
                           const fileengine_rpc::CreateRoleRequest* request,
                           fileengine_rpc::CreateRoleResponse* response) override;

    grpc::Status DeleteRole(grpc::ServerContext* context,
                           const fileengine_rpc::DeleteRoleRequest* request,
                           fileengine_rpc::DeleteRoleResponse* response) override;

    grpc::Status AssignUserToRole(grpc::ServerContext* context,
                                 const fileengine_rpc::AssignUserToRoleRequest* request,
                                 fileengine_rpc::AssignUserToRoleResponse* response) override;

    grpc::Status RemoveUserFromRole(grpc::ServerContext* context,
                                   const fileengine_rpc::RemoveUserFromRoleRequest* request,
                                   fileengine_rpc::RemoveUserFromRoleResponse* response) override;

    grpc::Status GetRolesForUser(grpc::ServerContext* context,
                                const fileengine_rpc::GetRolesForUserRequest* request,
                                fileengine_rpc::GetRolesForUserResponse* response) override;

    grpc::Status GetUsersForRole(grpc::ServerContext* context,
                                const fileengine_rpc::GetUsersForRoleRequest* request,
                                fileengine_rpc::GetUsersForRoleResponse* response) override;

    grpc::Status GetAllRoles(grpc::ServerContext* context,
                            const fileengine_rpc::GetAllRolesRequest* request,
                            fileengine_rpc::GetAllRolesResponse* response) override;

    grpc::Status ListClaims(grpc::ServerContext* context,
                           const fileengine_rpc::ListClaimsRequest* request,
                           fileengine_rpc::ListClaimsResponse* response) override;

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
    std::unique_ptr<StorageTracker> storage_tracker_;
    std::shared_ptr<IAuditSink> audit_sink_;  // durable audit emitter (§5); may be null

    // Emit a permission-category audit entry (§3). Returns true if the entry was
    // durably captured (or auditing is disabled) — permission is a fail-closed
    // category (§6), so on a state-changing path a false return means the caller
    // must refuse the op rather than mutate un-audited. `permission_mask` is our
    // internal bitmask; `principal_type` is the PrincipalType int.
    bool emit_permission_audit(const std::string& tenant, const std::string& action,
                               AuditOutcome outcome, const std::string& actor,
                               const std::vector<std::string>& roles,
                               const std::string& resource_uid, const std::string& principal,
                               int principal_type, const char* effect, int permission_mask);

    // Emit a mutate-category audit entry (§3). Best-effort: mutate is NOT
    // fail-closed (§6 — the op proceeds and the WAL guarantees eventual capture),
    // so this returns void and its result is not gated on. `detail_json` is an
    // optional pre-serialized JSON object (e.g. destination for move), or empty.
    void emit_mutate_audit(const std::string& tenant, const std::string& action,
                           AuditOutcome outcome, const std::string& actor,
                           const std::vector<std::string>& roles,
                           const std::string& target_uid, AuditTargetType target_type,
                           const std::string& detail_json = "");

    // Emit an access-category audit entry (§3). Best-effort, and subject to the
    // AUDIT_ACCESS_MODE throughput valve (§6): a Denied outcome is ALWAYS recorded
    // in full (the security signal); a successful read is emitted per the mode
    // (full = every; sample:N = 1-in-N; count[:K] = an aggregate every K).
    void emit_access_audit(const std::string& tenant, const std::string& action,
                           AuditOutcome outcome, const std::string& actor,
                           const std::vector<std::string>& roles,
                           const std::string& target_uid, AuditTargetType target_type,
                           const std::string& detail_json = "");

    // Access-log throughput mode, parsed once from AUDIT_ACCESS_MODE.
    enum class AccessAuditMode { Full, Sample, Count };
    AccessAuditMode access_mode_ = AccessAuditMode::Full;
    std::uint64_t access_interval_ = 1;               // N for sample, K for count
    std::atomic<std::uint64_t> access_counter_{0};    // successful-read counter for sampling

    // When false (default), audit entries whose target is a hidden child / sidecar
    // (a rendition — a file's hidden child) are dropped from both the access and
    // mutate logs: the conversion service's thumbnail/preview churn is noise, not a
    // security signal. Set FILEENGINE_AUDIT_HIDDEN_CHILDREN=true to record them.
    bool audit_hidden_children_ = false;

    // The client IP forwarded by the bridge for THIS request (thread-local, since
    // gRPC dispatches each call on its own thread). Set by get_tenant_from_auth_context
    // — which every audited handler calls before emitting — and read by the emit_*
    // helpers so audit rows carry source_addr across all protocols (REST/WebDAV/…).
    static thread_local std::string t_audit_source_;

    // Helper function to extract tenant from auth context
    inline std::string get_tenant_from_auth_context(const fileengine_rpc::AuthenticationContext& auth_ctx) {
        t_audit_source_ = auth_ctx.source_addr();  // remember for this request's audit
        return auth_ctx.tenant().empty() ? "default" : auth_ctx.tenant();
    }

    // Helper function to extract user from auth context
    inline std::string get_user_from_auth_context(const fileengine_rpc::AuthenticationContext& auth_ctx) {
        return auth_ctx.user();
    }

    // Helper function to get roles from auth context
    inline std::vector<std::string> get_roles_from_auth_context(const fileengine_rpc::AuthenticationContext& auth_ctx) {
        std::vector<std::string> roles;
        for (const auto& role : auth_ctx.roles()) {
            roles.push_back(role);
        }
        return roles;
    }

    // Helper function to get the principal's claims (key->value) from the auth
    // context. These feed CLAIM-type (ABAC) ACL rule matching.
    inline std::map<std::string, std::string> get_claims_from_auth_context(const fileengine_rpc::AuthenticationContext& auth_ctx) {
        std::map<std::string, std::string> claims;
        for (const auto& kv : auth_ctx.claims()) {
            claims[kv.first] = kv.second;
        }
        return claims;
    }

    // Helper function to validate permissions. System-admin bypass is handled
    // inside AclManager::check_permission so this helper has a single path.
    inline bool validate_user_permissions(const std::string& resource_uid,
                                      const fileengine_rpc::AuthenticationContext& auth_ctx,
                                      int required_permissions) {
        std::string user = get_user_from_auth_context(auth_ctx);
        std::string tenant = get_tenant_from_auth_context(auth_ctx);

        // Special rule: the filesystem root (empty UID) is always readable.
        // This is enforced here (not in AclManager) so root listing works
        // before any ACL exists on it. See plan §4.2.
        if (resource_uid.empty() && (required_permissions & static_cast<int>(Permission::READ))) {
            return true;
        }

        if (!acl_manager_) {
            // Fail closed: without an ACL manager we cannot authorize, so deny.
            // (Security review L1.) The root-READ carve-out above still applies.
            return false;
        }

        std::vector<std::string> roles = get_roles_from_auth_context(auth_ctx);
        std::map<std::string, std::string> claims = get_claims_from_auth_context(auth_ctx);
        auto result = acl_manager_->check_permission(resource_uid, user, roles, required_permissions, tenant, claims);
        return result.success && result.value;
    }

    // Helper function to check if server is in read-only mode
    inline bool is_server_in_readonly_mode() const {
        // Check if the server is in disconnected read-only mode using ConnectionPoolManager
        return ConnectionPoolManager::get_instance().is_server_in_readonly_mode();
    }
};

} // namespace fileengine

#endif // FILEENGINE_GRPC_SERVICE_H