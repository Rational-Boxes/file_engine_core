#include "fileengine/acl_manager.h"
#include "fileengine/IDatabase.h"
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <sstream>

// ACLs are stored separately from file metadata to maintain security boundaries
// Regular users can access file metadata but not bypass access control

namespace fileengine {

namespace {
// Request-scoped lookup cache. Thread-local so a single AclManager can serve
// concurrent gRPC handlers — each thread sees only its own cache. Wrapped in
// an optional so the cache only exists inside an explicit CacheScope and
// outside scopes get_acls_for_resource hits the DB as before.
thread_local std::optional<std::map<std::string, std::vector<ACLRule>>> tls_acl_cache;
}

AclManager::AclManager(std::shared_ptr<IDatabase> db) : db_(db) {
}

// Reentrance counter so nested CacheScope objects don't clobber each other.
// The outermost scope owns the cache; nested ones are no-ops. This lets a
// gRPC handler open a scope and the FileSystem methods it calls open scopes
// of their own without losing the handler-level cached reads.
namespace {
thread_local int tls_cache_scope_depth = 0;
}

void AclManager::enter_cache_scope() {
    if (tls_cache_scope_depth == 0) {
        tls_acl_cache.emplace();
    }
    ++tls_cache_scope_depth;
}

void AclManager::exit_cache_scope() {
    --tls_cache_scope_depth;
    if (tls_cache_scope_depth == 0) {
        tls_acl_cache.reset();
    }
}

bool AclManager::try_get_cached_acls(const std::string& cache_key,
                                     std::vector<ACLRule>& out) const {
    if (!tls_acl_cache.has_value()) return false;
    auto it = tls_acl_cache->find(cache_key);
    if (it == tls_acl_cache->end()) return false;
    out = it->second;
    return true;
}

void AclManager::put_cached_acls(const std::string& cache_key,
                                 const std::vector<ACLRule>& acls) const {
    if (tls_acl_cache.has_value()) {
        (*tls_acl_cache)[cache_key] = acls;
    }
}

void AclManager::invalidate_cache_entry(const std::string& resource_uid,
                                        const std::string& tenant) const {
    if (!tls_acl_cache.has_value()) return;
    tls_acl_cache->erase(tenant + "::" + resource_uid);
}

Result<void> AclManager::grant_permission(const std::string& resource_uid,
                                          const std::string& principal,
                                          PrincipalType type,
                                          int permissions,
                                          const std::string& tenant,
                                          const std::string& performed_by,
                                          AclEffect effect) {
    auto result = db_->add_acl(resource_uid, principal, static_cast<int>(type), permissions,
                               tenant, performed_by, static_cast<int>(effect));
    // Invalidate any cached read of this resource's ACLs so subsequent checks
    // in the same scope see the new grant.
    invalidate_cache_entry(resource_uid, tenant);
    return result;
}

Result<void> AclManager::revoke_permission(const std::string& resource_uid,
                                           const std::string& principal,
                                           PrincipalType type,
                                           int permissions,
                                           const std::string& tenant,
                                           const std::string& performed_by,
                                           AclEffect effect) {
    // Bit-mask revoke: only the requested bits are cleared. If the principal's
    // remaining bitmask is zero the row is deleted by the DB layer.
    auto result = db_->remove_acl(resource_uid, principal, static_cast<int>(type), permissions,
                                  tenant, performed_by, static_cast<int>(effect));
    invalidate_cache_entry(resource_uid, tenant);
    return result;
}

Result<bool> AclManager::check_permission(const std::string& resource_uid,
                                          const std::string& user,
                                          const std::vector<std::string>& roles,
                                          int required_permissions,
                                          const std::string& tenant,
                                          const std::map<std::string, std::string>& claims) {
    auto effective_roles = resolve_effective_roles(user, roles, tenant);

    // Admin bypass: kSystemAdminRole (global operator) OR kTenantAdminRole
    // (admin of THIS tenant). tenant_admin's scope is structural — this
    // AclManager resolves against a single tenant's schema (db_), so it can
    // never reach another tenant. No server-side enable flag — upstream is
    // trusted to attach these only to legitimately admin requests. (Finding H2.)
    if (std::find(effective_roles.begin(), effective_roles.end(), kSystemAdminRole) != effective_roles.end()
        || std::find(effective_roles.begin(), effective_roles.end(), kTenantAdminRole) != effective_roles.end()) {
        return Result<bool>::ok(true);
    }

    auto acls_result = get_acls_for_resource(resource_uid, tenant);
    if (!acls_result.success) {
        return Result<bool>::err(acls_result.error);
    }

    int effective_perms = calculate_effective_permissions(acls_result.value, user, effective_roles, claims);
    if ((effective_perms & required_permissions) != required_permissions) {
        return Result<bool>::ok(false);
    }

    // Path traversal: a resource is only reachable if every ancestor container
    // grants READ to the principal. This makes a DENY READ on a folder (e.g. to
    // everyone) hide the entire subtree, naturally respecting the parent
    // container's permissions.
    if (!ancestors_readable(resource_uid, user, effective_roles, claims, tenant)) {
        return Result<bool>::ok(false);
    }

    return Result<bool>::ok(true);
}

bool AclManager::is_system_admin(const std::string& user,
                                 const std::vector<std::string>& request_roles,
                                 const std::string& tenant) {
    auto effective_roles = resolve_effective_roles(user, request_roles, tenant);
    return std::find(effective_roles.begin(), effective_roles.end(), kSystemAdminRole)
           != effective_roles.end();
}

bool AclManager::is_tenant_admin(const std::string& user,
                                 const std::vector<std::string>& request_roles,
                                 const std::string& tenant) {
    auto effective_roles = resolve_effective_roles(user, request_roles, tenant);
    return std::find(effective_roles.begin(), effective_roles.end(), kTenantAdminRole)
           != effective_roles.end();
}

bool AclManager::is_admin(const std::string& user,
                          const std::vector<std::string>& request_roles,
                          const std::string& tenant) {
    auto effective_roles = resolve_effective_roles(user, request_roles, tenant);
    return std::find(effective_roles.begin(), effective_roles.end(), kSystemAdminRole) != effective_roles.end()
        || std::find(effective_roles.begin(), effective_roles.end(), kTenantAdminRole) != effective_roles.end();
}

Result<std::vector<ACLRule>> AclManager::get_acls_for_resource(const std::string& resource_uid,
                                                               const std::string& tenant) {
    // Request-scoped cache hit (only active inside a CacheScope).
    std::string cache_key = tenant + "::" + resource_uid;
    std::vector<ACLRule> cached;
    if (try_get_cached_acls(cache_key, cached)) {
        return Result<std::vector<ACLRule>>::ok(cached);
    }

    auto acl_result = db_->get_acls_for_resource(resource_uid, tenant);
    if (!acl_result.success) {
        return Result<std::vector<ACLRule>>::err(acl_result.error);
    }

    std::vector<ACLRule> acls;
    for (const auto& db_acl : acl_result.value) {
        ACLRule rule;
        rule.resource_uid = resource_uid;
        rule.principal = db_acl.principal;
        rule.type = static_cast<PrincipalType>(db_acl.type);
        rule.permissions = db_acl.permissions;
        rule.effect = static_cast<AclEffect>(db_acl.effect);

        acls.push_back(rule);
    }

    put_cached_acls(cache_key, acls);
    return Result<std::vector<ACLRule>>::ok(acls);
}

Result<int> AclManager::get_effective_permissions(const std::string& resource_uid,
                                                 const std::string& user,
                                                 const std::vector<std::string>& roles,
                                                 const std::string& tenant,
                                                 const std::map<std::string, std::string>& claims) {
    auto effective_roles = resolve_effective_roles(user, roles, tenant);

    // Admin bypass (system_admin OR tenant_admin) grants every bit — mirroring
    // check_permission. Resolved before any ACL/parent lookup so an admin's
    // answer never depends on (or is collapsed by) the resource's ACLs or an
    // unreadable ancestor. tenant_admin is tenant-scoped by db_. (Finding H2.)
    if (std::find(effective_roles.begin(), effective_roles.end(), kSystemAdminRole) != effective_roles.end()
        || std::find(effective_roles.begin(), effective_roles.end(), kTenantAdminRole) != effective_roles.end()) {
        return Result<int>::ok(kAllPermissions);
    }

    auto acls_result = get_acls_for_resource(resource_uid, tenant);
    if (!acls_result.success) {
        return Result<int>::err(acls_result.error);
    }

    int effective_perms = calculate_effective_permissions(acls_result.value, user, effective_roles, claims);

    // Reflect parent-container traversal: if any ancestor container is
    // unreadable the resource is unreachable, so the effective permission set
    // collapses to none — the same access an enforcing check_permission would
    // grant. (Skip the walk when there's nothing to lose.)
    if (effective_perms != 0 &&
        !ancestors_readable(resource_uid, user, effective_roles, claims, tenant)) {
        effective_perms = 0;
    }

    return Result<int>::ok(effective_perms);
}

std::vector<std::string> AclManager::resolve_effective_roles(const std::string& user,
                                                             const std::vector<std::string>& request_roles,
                                                             const std::string& tenant) {
    // Start with the request-supplied roles (federated IdP path).
    std::vector<std::string> roles = request_roles;

    // Union in DB-stored roles for the user (local management path). A DB
    // failure here is non-fatal — we degrade to request roles rather than
    // denying every permission check, but we don't silently swallow either:
    // the db_ method will have logged the underlying error already.
    auto db_roles = db_->get_roles_for_user(user, tenant);
    if (db_roles.success) {
        for (const auto& r : db_roles.value) {
            if (std::find(roles.begin(), roles.end(), r) == roles.end()) {
                roles.push_back(r);
            }
        }
    }

    return roles;
}

Result<void> AclManager::apply_default_acls(const std::string& resource_uid,
                                           const std::string& creator,
                                           const std::string& tenant) {
    // Creator gets FULL control on the resource they created — so they can manage
    // their own content end to end: delete + list-deleted/undelete, and the full
    // version lifecycle (view/retrieve/restore), plus MANAGE_ACL to share it.
    // ACL_INHERIT cascades the rule to children. (CULL_VERSIONS is destructive and
    // stays opt-in, granted explicitly.)
    auto grant_result = grant_permission(resource_uid, creator, PrincipalType::USER,
                                        static_cast<int>(Permission::READ)
                                        | static_cast<int>(Permission::WRITE)
                                        | static_cast<int>(Permission::EXECUTE)
                                        | static_cast<int>(Permission::DELETE)
                                        | static_cast<int>(Permission::LIST_DELETED)
                                        | static_cast<int>(Permission::UNDELETE)
                                        | static_cast<int>(Permission::VIEW_VERSIONS)
                                        | static_cast<int>(Permission::RETRIEVE_BACK_VERSION)
                                        | static_cast<int>(Permission::RESTORE_TO_VERSION)
                                        | static_cast<int>(Permission::MANAGE_ACL)
                                        | static_cast<int>(Permission::ACL_INHERIT),
                                        tenant, creator);
    if (!grant_result.success) {
        return grant_result;
    }

    // World-readable OTHER->READ is opt-in. Default is private-by-default.
    if (default_world_readable_) {
        return grant_permission(resource_uid, "other", PrincipalType::OTHER,
                                static_cast<int>(Permission::READ),
                                tenant, creator);
    }

    return Result<void>::ok();
}

Result<void> AclManager::inherit_acls(const std::string& parent_uid,
                                     const std::string& child_uid,
                                     const std::string& tenant,
                                     const std::string& performed_by) {
    auto parent_acls_result = get_acls_for_resource(parent_uid, tenant);
    if (!parent_acls_result.success) {
        return Result<void>::err(parent_acls_result.error);
    }

    const int inherit_bit = static_cast<int>(Permission::ACL_INHERIT);

    // Copy only rules marked with ACL_INHERIT. The bit is preserved on the
    // child so inheritance cascades to grandchildren; callers can break the
    // chain at any level by revoking the bit. Fail-loud: any per-rule failure
    // aborts so the caller sees a partial-state error.
    for (const auto& rule : parent_acls_result.value) {
        if ((rule.permissions & inherit_bit) == 0) {
            continue;
        }
        auto result = grant_permission(child_uid, rule.principal, rule.type,
                                       rule.permissions, tenant, performed_by);
        if (!result.success) {
            return Result<void>::err("inherit_acls: failed to copy rule for "
                                     + rule.principal + ": " + result.error);
        }
    }

    return Result<void>::ok();
}

bool AclManager::parent_has_inheritable_acls(const std::string& parent_uid,
                                             const std::string& tenant) {
    auto parent_acls = get_acls_for_resource(parent_uid, tenant);
    if (!parent_acls.success) return false;
    const int inherit_bit = static_cast<int>(Permission::ACL_INHERIT);
    for (const auto& rule : parent_acls.value) {
        if (rule.permissions & inherit_bit) return true;
    }
    return false;
}

Result<std::vector<ACLRule>> AclManager::get_user_acls(const std::string& resource_uid,
                                                       const std::string& user,
                                                       const std::string& tenant) {
    // Query dedicated ACL tables directly for user-specific ACLs.
    // The AclManager API treats `user` as a USER principal, so scope the DB
    // lookup to USER-typed rows to avoid conflating user and role names.
    auto user_acl_result = db_->get_user_acls(resource_uid, user,
                                              static_cast<int>(PrincipalType::USER), tenant);
    if (!user_acl_result.success) {
        return Result<std::vector<ACLRule>>::err(user_acl_result.error);
    }

    std::vector<ACLRule> user_acls;

    // Transform the database format to internal ACLRule format
    for (const auto& db_acl : user_acl_result.value) {
        ACLRule rule;
        rule.resource_uid = resource_uid;
        rule.principal = db_acl.principal;
        rule.type = static_cast<PrincipalType>(db_acl.type);
        rule.permissions = db_acl.permissions;
        rule.effect = static_cast<AclEffect>(db_acl.effect);

        user_acls.push_back(rule);
    }

    return Result<std::vector<ACLRule>>::ok(user_acls);
}

int AclManager::calculate_effective_permissions(const std::vector<ACLRule>& rules,
                                               const std::string& user,
                                               const std::vector<std::string>& roles,
                                               const std::map<std::string, std::string>& claims) {
    // Hierarchical, most-specific-wins evaluation. For each permission bit the
    // most specific tier that has a matching rule decides it; a less specific
    // tier can never override a bit a more specific tier already settled. Within
    // a single tier a DENY beats an ALLOW. Tiers, most specific first:
    //   [0] USER          — a rule for this exact user
    //   [1] CLAIM         — a matching ABAC claim (key=value)
    //   [2] ROLE          — a matching role (roles ARE the group mechanism:
    //                       LDAP groups are resolved to role names upstream)
    //   [3] OTHER         — explicit "everyone" rules (e.g. world-readable)
    //   fall-through      — READ allowed (read-by-default), everything else denied
    //
    // Rationale (finding M1): resolution is a deliberate identity hierarchy —
    // USER, then CLAIM, then ROLE, then the everyone tier, then the read-only
    // default. A more-specific ALLOW overrides a less-specific DENY (e.g. a
    // private home folder: everyone DENY + owner ALLOW), and a specific claim
    // resolves before a broader role. DENY is absolute only *within* a tier — it
    // does not leap across tiers. Read-by-default is the terminal fall-through;
    // disable via set_default_read(false) for strict private-by-default. See
    // plan §6.1.

    // Classify a rule into its specificity tier (lower = more specific), or -1
    // for no match.
    auto tier_of = [&](const ACLRule& rule) -> int {
        switch (rule.type) {
            case PrincipalType::USER:
                return rule.principal == user ? 0 : -1;
            case PrincipalType::CLAIM: {
                // ABAC: principal is "key=value"; matches iff that exact claim is
                // presented. Malformed (no '=') or absent/mismatched never matches.
                auto eq = rule.principal.find('=');
                if (eq == std::string::npos) return -1;
                const std::string key = rule.principal.substr(0, eq);
                const std::string value = rule.principal.substr(eq + 1);
                auto it = claims.find(key);
                return (it != claims.end() && it->second == value) ? 1 : -1;
            }
            case PrincipalType::ROLE:
                for (const auto& role : roles) {
                    if (rule.principal == role) return 2;
                }
                return -1;
            case PrincipalType::GROUP:
                // Reserved/legacy slot — NOT a usable principal. Roles ARE the
                // group mechanism (LDAP groups resolve to ROLE principals), so
                // GROUP is redundant and no code path ever creates one. The enum
                // value is retained only for wire/DB numbering stability (see
                // acl_manager.h). Match nobody (fail-closed) so a stray row —
                // e.g. a manual DB insert — grants/denies no one rather than
                // everyone. (Findings M2 + "roles are groups" cleanup.)
                return -1;
            case PrincipalType::OTHER:
                return 3;
        }
        return -1;
    };

    constexpr int kTiers = 4;  // USER, CLAIM, ROLE, OTHER
    int allow[kTiers] = {0, 0, 0, 0};
    int deny[kTiers] = {0, 0, 0, 0};
    for (const auto& rule : rules) {
        int t = tier_of(rule);
        if (t < 0) continue;
        if (rule.effect == AclEffect::DENY) deny[t] |= rule.permissions;
        else allow[t] |= rule.permissions;
    }

    int result = 0;
    int undecided = ~0;  // permission bits not yet settled by a more specific tier
    for (int t = 0; t < kTiers; ++t) {
        result |= (allow[t] & ~deny[t]) & undecided;  // grant this tier's allows (deny wins in-tier)
        undecided &= ~(allow[t] | deny[t]);            // any bit this tier touched is now settled
    }
    if (default_read_) {
        result |= static_cast<int>(Permission::READ) & undecided;  // terminal read-by-default
    }
    return result;
}

bool AclManager::has_deleted_ancestor(const std::string& resource_uid,
                                      const std::string& tenant) {
    // Walk the parent chain (STRICT ancestors only — never the node itself, whose
    // own `deleted` flag is enforced by the caller's get_file_by_uid). If any
    // ancestor is soft-deleted the resource is hidden. `include_deleted` is used
    // so a deleted ancestor is *seen* (the default lookup filters deleted rows).
    std::set<std::string> visited;
    std::string current = resource_uid;
    while (true) {
        auto file = db_->get_file_by_uid_include_deleted(current, tenant);
        if (!file.success || !file.value.has_value()) {
            return false; // no record — root / bare resource — not hidden
        }
        const std::string parent = file.value->parent_uid;
        if (parent.empty()) {
            return false; // reached the filesystem root
        }
        if (!visited.insert(parent).second) {
            return false; // defensive cycle guard
        }
        auto pinfo = db_->get_file_by_uid_include_deleted(parent, tenant);
        if (pinfo.success && pinfo.value.has_value() && pinfo.value->deleted) {
            return true; // a strict ancestor is soft-deleted -> subtree hidden
        }
        current = parent;
    }
}

bool AclManager::ancestors_readable(const std::string& resource_uid,
                                    const std::string& user,
                                    const std::vector<std::string>& roles,
                                    const std::map<std::string, std::string>& claims,
                                    const std::string& tenant) {
    // Reachability by deletion first: a resource under a soft-deleted folder is
    // hidden regardless of ACLs, so it never leaks through any permission-gated
    // surface (stat/get/read/listdir/check_permission -> search, dashboard).
    if (has_deleted_ancestor(resource_uid, tenant)) {
        return false;
    }

    // Walk from the resource up to the filesystem root. Every ancestor
    // container must grant READ to the principal, else the resource is
    // unreachable — this is what makes a DENY READ on a folder (e.g. to
    // everyone) hide its entire subtree. `roles` are already-resolved
    // effective roles, passed straight through.
    const int read_bit = static_cast<int>(Permission::READ);
    std::set<std::string> visited;
    std::string current = resource_uid;

    while (true) {
        auto file = db_->get_file_by_uid(current, tenant);
        // No file record (bare resource / unit-test fixture) — nothing above it
        // to traverse; treat as root-level and therefore reachable.
        if (!file.success || !file.value.has_value()) {
            return true;
        }
        const std::string parent = file.value->parent_uid;
        if (parent.empty()) {
            return true; // reached the filesystem root
        }
        if (!visited.insert(parent).second) {
            return true; // defensive cycle guard — should not happen
        }
        auto parent_acls = get_acls_for_resource(parent, tenant);
        if (!parent_acls.success) {
            return false; // fail closed if an ancestor's ACLs can't be resolved
        }
        int eff = calculate_effective_permissions(parent_acls.value, user, roles, claims);
        if ((eff & read_bit) != read_bit) {
            return false; // an ancestor denies READ -> subtree unreachable
        }
        current = parent;
    }
}

} // namespace fileengine