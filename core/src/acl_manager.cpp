#include "fileengine/acl_manager.h"
#include "fileengine/IDatabase.h"
#include <algorithm>
#include <map>
#include <optional>
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

    // System-admin bypass: holding kSystemAdminRole grants all permissions.
    // No server-side enable flag — upstream is trusted to only attach this
    // role to legitimately admin requests (see plan §4 / kSystemAdminRole doc).
    if (std::find(effective_roles.begin(), effective_roles.end(), kSystemAdminRole)
            != effective_roles.end()) {
        return Result<bool>::ok(true);
    }

    auto acls_result = get_acls_for_resource(resource_uid, tenant);
    if (!acls_result.success) {
        return Result<bool>::err(acls_result.error);
    }

    int effective_perms = calculate_effective_permissions(acls_result.value, user, effective_roles, claims);
    bool has_permission = (effective_perms & required_permissions) == required_permissions;
    return Result<bool>::ok(has_permission);
}

bool AclManager::is_system_admin(const std::string& user,
                                 const std::vector<std::string>& request_roles,
                                 const std::string& tenant) {
    auto effective_roles = resolve_effective_roles(user, request_roles, tenant);
    return std::find(effective_roles.begin(), effective_roles.end(), kSystemAdminRole)
           != effective_roles.end();
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
    auto acls_result = get_acls_for_resource(resource_uid, tenant);
    if (!acls_result.success) {
        return Result<int>::err(acls_result.error);
    }

    auto effective_roles = resolve_effective_roles(user, roles, tenant);
    int effective_perms = calculate_effective_permissions(acls_result.value, user, effective_roles, claims);

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
    // Creator always gets full access on the resource they created, including
    // MANAGE_ACL so they can grant/revoke permissions for other principals.
    // ACL_INHERIT is set so the creator's rule cascades to children if this
    // resource is later treated as a parent.
    auto grant_result = grant_permission(resource_uid, creator, PrincipalType::USER,
                                        static_cast<int>(Permission::READ)
                                        | static_cast<int>(Permission::WRITE)
                                        | static_cast<int>(Permission::EXECUTE)
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
    // lookup to USER-typed rows to avoid conflating role/group names.
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
    // Union model: every matching ALLOW grant contributes its bits, regardless
    // of principal type. DENY grants are unioned into a separate mask and
    // subtracted at the end so any matching deny wins (POSIX-NFSv4 style).
    // See plan §6.1.
    int allow_mask = 0;
    int deny_mask = 0;

    auto matches_principal = [&](const ACLRule& rule) -> bool {
        switch (rule.type) {
            case PrincipalType::USER:
                return rule.principal == user;
            case PrincipalType::ROLE:
                for (const auto& role : roles) {
                    if (rule.principal == role) return true;
                }
                return false;
            case PrincipalType::CLAIM: {
                // ABAC: the rule's principal is "key=value"; it matches iff the
                // principal presents that exact claim. A malformed principal
                // (no '=') or an absent/mismatched claim never matches. See
                // plan §6.4.
                auto eq = rule.principal.find('=');
                if (eq == std::string::npos) return false;
                const std::string key = rule.principal.substr(0, eq);
                const std::string value = rule.principal.substr(eq + 1);
                auto it = claims.find(key);
                return it != claims.end() && it->second == value;
            }
            case PrincipalType::GROUP:
                // GROUP membership is not yet modeled (plan §2.3); treat as global.
                return true;
            case PrincipalType::OTHER:
                return true;
        }
        return false;
    };

    for (const auto& rule : rules) {
        if (!matches_principal(rule)) continue;
        if (rule.effect == AclEffect::DENY) {
            deny_mask |= rule.permissions;
        } else {
            allow_mask |= rule.permissions;
        }
    }

    return allow_mask & ~deny_mask;
}

} // namespace fileengine