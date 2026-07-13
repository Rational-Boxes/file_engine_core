#pragma once

#include "types.h"
#include <string>
#include <vector>
#include <memory>
#include <map>

namespace fileengine {

enum class Permission {
    CULL_VERSIONS = 0x2000, // Permanently purge old versions — the one genuine
                            // destroy-data operation. Never granted by default;
                            // must be granted explicitly. See plan §2.5.
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

// The union of every permission bit — a system_admin's effective permission
// set, mirroring the check_permission bypass (which passes for any required
// permission). Control bits (ACL_INHERIT) are included so that
// (kAllPermissions & required) == required holds for every possible `required`.
inline constexpr int kAllPermissions =
      static_cast<int>(Permission::CULL_VERSIONS)
    | static_cast<int>(Permission::ACL_INHERIT)
    | static_cast<int>(Permission::MANAGE_ACL)
    | static_cast<int>(Permission::READ)
    | static_cast<int>(Permission::WRITE)
    | static_cast<int>(Permission::DELETE)
    | static_cast<int>(Permission::LIST_DELETED)
    | static_cast<int>(Permission::UNDELETE)
    | static_cast<int>(Permission::VIEW_VERSIONS)
    | static_cast<int>(Permission::RETRIEVE_BACK_VERSION)
    | static_cast<int>(Permission::RESTORE_TO_VERSION)
    | static_cast<int>(Permission::EXECUTE);

enum class PrincipalType {
    USER,
    ROLE,      // Role-based permissions. Roles ARE the group mechanism —
               // LDAP groups are resolved to role names upstream, so a "group"
               // grant is just a ROLE grant. There is no separate group type.
    GROUP,     // RESERVED / UNUSED. Kept only so the enum's integer values stay
               // wire/DB-stable (USER=0, ROLE=1, GROUP=2, OTHER=3, CLAIM=4);
               // removing it would renumber OTHER/CLAIM and corrupt stored rows.
               // Redundant with ROLE (roles are groups); no code path creates it,
               // and the evaluator treats it as no-match (fail-closed).
    OTHER,
    CLAIM      // ABAC: matches a principal whose auth claims contain a
               // specific key=value pair. The stored `principal` is the
               // bare "key=value" string (the "claim:" wire prefix is
               // stripped at the gRPC boundary). See plan §6.4.
};

// ALLOW (default) contributes bits; DENY subtracts them at evaluation time.
// Resolution is hierarchical (USER > CLAIM > ROLE > OTHER > read-default):
// DENY is absolute *within its tier*, but a more specific tier that settles a
// bit wins over a less specific DENY (see calculate_effective_permissions). The
// integer values are wire-stable — they map directly to the DB `effect` column
// and the proto AclEffect enum.
enum class AclEffect {
    ALLOW = 0,
    DENY  = 1
};

struct ACLRule {
    std::string principal;      // user id, role name, "key=value" claim, or "everyone"
    PrincipalType type;         // USER, ROLE, CLAIM, or OTHER (GROUP is reserved/unused)
    int permissions;            // Bitmask of permissions
    std::string resource_uid;   // Resource this ACL applies to
    AclEffect effect = AclEffect::ALLOW;
};

class IDatabase;

// Reserved role names. A user whose effective roles (request_roles ∪ DB-stored
// roles) contain either of these bypasses all ACL checks for the resource. There
// is no enable flag — the trust model is that upstream authentication only
// attaches these roles to legitimately privileged requests.
//
//   kSystemAdminRole — a GLOBAL platform operator. Reserved for deployment
//     operators; the bridges grant it only from an explicit operators group,
//     never from a per-tenant "administrators" group.
//   kTenantAdminRole — admin of the CALLER'S OWN tenant only. Its bypass is
//     inherently tenant-scoped: this AclManager resolves against a single
//     tenant's schema (db_), so a tenant_admin can never reach another tenant's
//     resources. The bridges map a tenant's "administrators" group to this role.
//
// Separating the two contains the blast radius of a per-tenant admin (finding
// H2): a mis-placed or attacker-created "administrators" group grants control of
// one tenant, not the whole deployment.
inline constexpr const char* kSystemAdminRole = "system_admin";
inline constexpr const char* kTenantAdminRole = "tenant_admin";

// Human-facing name for the "everyone" principal. A rule whose PrincipalType
// is OTHER matches every principal regardless of the stored principal string,
// so a rule on this name targets all users. Grant DENY READ to
// {kEveryonePrincipal, PrincipalType::OTHER} to hide a resource (and, via parent
// traversal, its whole subtree) from everyone — overriding the read-by-default
// baseline. Note a more specific tier (USER/CLAIM/ROLE ALLOW) still resolves
// before this everyone-tier DENY. See plan §2.4.
inline constexpr const char* kEveryonePrincipal = "everyone";

class AclManager {
public:
    AclManager(std::shared_ptr<IDatabase> db);

    // Toggle whether apply_default_acls grants OTHER->READ on new resources.
    // Defaults to false (private-by-default).
    void set_default_world_readable(bool enabled) { default_world_readable_ = enabled; }
    bool default_world_readable() const { return default_world_readable_; }

    // Read-by-default. When enabled (the default), every principal holds a
    // baseline READ on every resource, so an entity with no specific ACL is
    // readable by any user. The baseline is cleared by any matching DENY READ
    // (e.g. DENY to the everyone/OTHER principal), and a resource is only
    // reachable if its parent container chain is also readable — see
    // check_permission's path traversal. Set false for a strict
    // private-by-default deployment.
    void set_default_read(bool enabled) { default_read_ = enabled; }
    bool default_read() const { return default_read_; }

    // Returns true iff the user (via request roles or DB-stored roles) holds
    // kSystemAdminRole (the global platform operator).
    bool is_system_admin(const std::string& user,
                         const std::vector<std::string>& request_roles,
                         const std::string& tenant = "");

    // Returns true iff the user holds kTenantAdminRole (admin of THIS tenant).
    bool is_tenant_admin(const std::string& user,
                         const std::vector<std::string>& request_roles,
                         const std::string& tenant = "");

    // Returns true iff the user is an admin of this tenant by EITHER role
    // (system_admin OR tenant_admin). Use this for in-tenant admin gates (e.g.
    // root-directory creation) where both should qualify.
    bool is_admin(const std::string& user,
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
    
    // Grant permission to a principal (user/role/claim/everyone) on a resource.
    // performed_by is the
    // last arg so legacy positional callers that pass `tenant` as the 5th
    // argument still bind to the right slot. effect defaults to ALLOW; pass
    // DENY to add bits to the deny set instead (DENY wins within its tier).
    Result<void> grant_permission(const std::string& resource_uid,
                                  const std::string& principal,
                                  PrincipalType type,
                                  int permissions,
                                  const std::string& tenant = "",
                                  const std::string& performed_by = "",
                                  AclEffect effect = AclEffect::ALLOW);

    // Revoke permission from a principal (user/role/claim/everyone) on a
    // resource. The effect arg
    // selects which row (ALLOW or DENY) the bits come out of.
    Result<void> revoke_permission(const std::string& resource_uid,
                                   const std::string& principal,
                                   PrincipalType type,
                                   int permissions,
                                   const std::string& tenant = "",
                                   const std::string& performed_by = "",
                                   AclEffect effect = AclEffect::ALLOW);
    
    // Check if a user has specific permissions on a resource. `claims` are the
    // principal's auth claims (key->value); they let CLAIM-type (ABAC) rules
    // match. Defaulted + last so existing positional callers are unaffected.
    Result<bool> check_permission(const std::string& resource_uid,
                                  const std::string& user,
                                  const std::vector<std::string>& roles,
                                  int required_permissions,
                                  const std::string& tenant = "",
                                  const std::map<std::string, std::string>& claims = {});
    
    // Get all ACLs for a resource
    Result<std::vector<ACLRule>> get_acls_for_resource(const std::string& resource_uid, 
                                                       const std::string& tenant = "");
    
    // Get effective permissions for a user on a resource. `claims` (last,
    // defaulted) feed CLAIM-type (ABAC) rule matching.
    Result<int> get_effective_permissions(const std::string& resource_uid,
                                         const std::string& user,
                                         const std::vector<std::string>& roles,
                                         const std::string& tenant = "",
                                         const std::map<std::string, std::string>& claims = {});
    
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

    // Reachability by deletion: true iff any STRICT ancestor (parent chain, not
    // the node itself) is soft-deleted. A soft-deleted folder hides its whole
    // subtree WITHOUT touching each descendant's own `deleted` flag, so delete/
    // undelete of a parent never rewrites a child's independent state. Used by
    // the permission path and by exists()/listdir() so descendants of a deleted
    // folder never leak into any surface (listing, direct UID, search, dashboard).
    // A node with no record or an empty parent is root-level, hence not hidden.
    bool has_deleted_ancestor(const std::string& resource_uid,
                              const std::string& tenant = "");

private:
    std::shared_ptr<IDatabase> db_;
    bool default_world_readable_ = false;
    bool default_read_ = true;
    
    // Internal helper to get user permissions from the database
    Result<std::vector<ACLRule>> get_user_acls(const std::string& resource_uid, 
                                               const std::string& user, 
                                               const std::string& tenant);
    
    // Internal helper to calculate effective permissions. `claims` are matched
    // against CLAIM-type rules (ABAC).
    int calculate_effective_permissions(const std::vector<ACLRule>& rules,
                                      const std::string& user,
                                      const std::vector<std::string>& roles,
                                      const std::map<std::string, std::string>& claims);

    // Path traversal: returns true iff every ancestor container of
    // `resource_uid` grants READ to the principal. A DENY READ on any ancestor
    // (e.g. a DENY to everyone on a folder) makes the whole subtree
    // unreachable, naturally respecting parent-container permissions. A
    // resource with no file record or an empty parent is treated as
    // root-level, hence reachable. `roles` are already-resolved effective
    // roles. Fails closed if an ancestor's ACLs cannot be resolved.
    bool ancestors_readable(const std::string& resource_uid,
                            const std::string& user,
                            const std::vector<std::string>& roles,
                            const std::map<std::string, std::string>& claims,
                            const std::string& tenant);

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