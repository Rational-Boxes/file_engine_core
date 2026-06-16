#pragma once

#include "types.h"
#include <string>
#include <vector>
#include <memory>
#include <map>

namespace fileengine {

enum class Permission {
    ACL_INHERIT = 0x1000, // Marks a rule for parent->child propagation
    MANAGE_ACL = 0x800, // Grant/revoke permissions on this resource
    READ = 0x400,     // Read permission
    WRITE = 0x200,    // Write permission
    DELETE = 0x100,   // Delete permission
    LIST_DELETED = 0x080, // List deleted items permission
    UNDELETE = 0x040, // Undelete permission
    VIEW_VERSIONS = 0x020, // View versions permission
    RETRIEVE_BACK_VERSION = 0x010, // Retrieve back version permission
    RESTORE_TO_VERSION = 0x008, // Restore to version permission
    EXECUTE = 0x001   // Execute permission (kept for compatibility)
};

// Define operators for Permission enum to enable bitwise operations
inline Permission operator|(Permission left, Permission right) {
    return static_cast<Permission>(static_cast<int>(left) | static_cast<int>(right));
}

inline Permission operator&(Permission left, Permission right) {
    return static_cast<Permission>(static_cast<int>(left) & static_cast<int>(right));
}

inline int operator&(Permission left, int right) {
    return static_cast<int>(left) & right;
}

inline int operator|(Permission left, int right) {
    return static_cast<int>(left) | right;
}

enum class PrincipalType {
    USER,
    ROLE,      // For role-based permissions
    GROUP,
    OTHER
};

// ALLOW (default) contributes bits; DENY subtracts them at evaluation time so
// a matching DENY always wins. The integer values are wire-stable — they map
// directly to the DB `effect` column and the proto AclEffect enum.
enum class AclEffect {
    ALLOW = 0,
    DENY  = 1
};

struct ACLRule {
    std::string principal;      // User or group name
    PrincipalType type;         // User, group, or other
    int permissions;            // Bitmask of permissions
    std::string resource_uid;   // Resource this ACL applies to
    AclEffect effect = AclEffect::ALLOW;
};

class IDatabase;

// Reserved role name. A user holding this role bypasses all ACL checks, but
// ONLY when set_system_admin_enabled(true) has been called (the server wires
// this from config.root_user_enabled — default off).
inline constexpr const char* kSystemAdminRole = "system_admin";

class AclManager {
public:
    AclManager(std::shared_ptr<IDatabase> db);

    // Toggle whether apply_default_acls grants OTHER->READ on new resources.
    // Defaults to false (private-by-default).
    void set_default_world_readable(bool enabled) { default_world_readable_ = enabled; }
    bool default_world_readable() const { return default_world_readable_; }

    // Toggle whether holding the system_admin role bypasses ACL checks.
    // Defaults to false — the bypass must be explicitly enabled in config.
    void set_system_admin_enabled(bool enabled) { system_admin_enabled_ = enabled; }
    bool system_admin_enabled() const { return system_admin_enabled_; }

    // Returns true iff system_admin_enabled is on AND the user (via request
    // roles or DB-stored roles) holds kSystemAdminRole.
    bool is_system_admin(const std::string& user,
                         const std::vector<std::string>& request_roles,
                         const std::string& tenant = "");

    // Request-scoped ACL lookup cache. Within the lifetime of a CacheScope,
    // repeat get_acls_for_resource lookups for the same (resource, tenant)
    // tuple on the same thread are served from memory instead of re-hitting
    // Postgres. Any grant/revoke through this AclManager invalidates the
    // affected entry, so the cache is consistent for the lifetime of one
    // gRPC handler. See plan §6.3.
    class CacheScope {
    public:
        explicit CacheScope(AclManager& m) : mgr_(&m) { mgr_->enter_cache_scope(); }
        ~CacheScope() { mgr_->exit_cache_scope(); }
        CacheScope(const CacheScope&) = delete;
        CacheScope& operator=(const CacheScope&) = delete;
        CacheScope(CacheScope&&) = delete;
        CacheScope& operator=(CacheScope&&) = delete;
    private:
        AclManager* mgr_;
    };
    
    // Grant permission to a user/group on a resource. performed_by is the
    // last arg so legacy positional callers that pass `tenant` as the 5th
    // argument still bind to the right slot. effect defaults to ALLOW; pass
    // DENY to add bits to the deny set instead (a matching DENY always wins).
    Result<void> grant_permission(const std::string& resource_uid,
                                  const std::string& principal,
                                  PrincipalType type,
                                  int permissions,
                                  const std::string& tenant = "",
                                  const std::string& performed_by = "",
                                  AclEffect effect = AclEffect::ALLOW);

    // Revoke permission from a user/group on a resource. The effect arg
    // selects which row (ALLOW or DENY) the bits come out of.
    Result<void> revoke_permission(const std::string& resource_uid,
                                   const std::string& principal,
                                   PrincipalType type,
                                   int permissions,
                                   const std::string& tenant = "",
                                   const std::string& performed_by = "",
                                   AclEffect effect = AclEffect::ALLOW);
    
    // Check if a user has specific permissions on a resource
    Result<bool> check_permission(const std::string& resource_uid, 
                                  const std::string& user, 
                                  const std::vector<std::string>& roles,
                                  int required_permissions, 
                                  const std::string& tenant = "");
    
    // Get all ACLs for a resource
    Result<std::vector<ACLRule>> get_acls_for_resource(const std::string& resource_uid, 
                                                       const std::string& tenant = "");
    
    // Get effective permissions for a user on a resource
    Result<int> get_effective_permissions(const std::string& resource_uid, 
                                         const std::string& user, 
                                         const std::vector<std::string>& roles,
                                         const std::string& tenant = "");
    
    // Apply default ACLs when creating a new resource
    Result<void> apply_default_acls(const std::string& resource_uid, 
                                   const std::string& creator, 
                                   const std::string& tenant = "");
    
    // Copy ACL_INHERIT-marked rules from parent to child. The bit is
    // preserved on the child so inheritance cascades. Fail-loud — any rule
    // copy failure aborts and returns the underlying error. performed_by
    // is recorded on the inherited rows + in the audit log.
    Result<void> inherit_acls(const std::string& parent_uid,
                             const std::string& child_uid,
                             const std::string& tenant = "",
                             const std::string& performed_by = "");

    // True iff the parent has at least one rule with ACL_INHERIT set.
    // Used by callers to decide between inherit_acls and apply_default_acls.
    bool parent_has_inheritable_acls(const std::string& parent_uid,
                                     const std::string& tenant = "");
    
private:
    std::shared_ptr<IDatabase> db_;
    bool default_world_readable_ = false;
    bool system_admin_enabled_ = false;
    
    // Internal helper to get user permissions from the database
    Result<std::vector<ACLRule>> get_user_acls(const std::string& resource_uid, 
                                               const std::string& user, 
                                               const std::string& tenant);
    
    // Internal helper to calculate effective permissions
    int calculate_effective_permissions(const std::vector<ACLRule>& rules,
                                      const std::string& user,
                                      const std::vector<std::string>& roles);

    // Union request-supplied roles with DB-stored roles for the user (deduped).
    // Request roles support federated IdP setups; DB roles support local
    // server-side role management.
    std::vector<std::string> resolve_effective_roles(const std::string& user,
                                                     const std::vector<std::string>& request_roles,
                                                     const std::string& tenant);

    // CacheScope hooks. Implementation uses a thread-local optional map so a
    // single AclManager instance can serve multiple concurrent requests, each
    // with its own private cache.
    void enter_cache_scope();
    void exit_cache_scope();
    bool try_get_cached_acls(const std::string& cache_key,
                             std::vector<ACLRule>& out) const;
    void put_cached_acls(const std::string& cache_key,
                         const std::vector<ACLRule>& acls) const;
    void invalidate_cache_entry(const std::string& resource_uid,
                                const std::string& tenant) const;
};

} // namespace fileengine