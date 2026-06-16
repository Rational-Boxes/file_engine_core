#include "fileengine/acl_manager.h"
#include "fileengine/IDatabase.h"
#include <algorithm>
#include <sstream>

// ACLs are stored separately from file metadata to maintain security boundaries
// Regular users can access file metadata but not bypass access control

namespace fileengine {

AclManager::AclManager(std::shared_ptr<IDatabase> db) : db_(db) {
}

Result<void> AclManager::grant_permission(const std::string& resource_uid,
                                          const std::string& principal,
                                          PrincipalType type,
                                          int permissions,
                                          const std::string& tenant) {
    // Use the database's dedicated ACL methods
    // This ensures ACLs are stored separately from file metadata
    auto result = db_->add_acl(resource_uid, principal, static_cast<int>(type), permissions, tenant);

    return result;
}

Result<void> AclManager::revoke_permission(const std::string& resource_uid,
                                           const std::string& principal,
                                           PrincipalType type,
                                           int permissions,
                                           const std::string& tenant) {
    // Bit-mask revoke: only the requested bits are cleared. If the principal's
    // remaining bitmask is zero the row is deleted by the DB layer.
    return db_->remove_acl(resource_uid, principal, static_cast<int>(type), permissions, tenant);
}

Result<bool> AclManager::check_permission(const std::string& resource_uid,
                                          const std::string& user,
                                          const std::vector<std::string>& roles,
                                          int required_permissions,
                                          const std::string& tenant) {
    auto acls_result = get_acls_for_resource(resource_uid, tenant);
    if (!acls_result.success) {
        return Result<bool>::err(acls_result.error);
    }

    auto effective_roles = resolve_effective_roles(user, roles, tenant);
    int effective_perms = calculate_effective_permissions(acls_result.value, user, effective_roles);

    bool has_permission = (effective_perms & required_permissions) == required_permissions;
    return Result<bool>::ok(has_permission);
}

Result<std::vector<ACLRule>> AclManager::get_acls_for_resource(const std::string& resource_uid,
                                                               const std::string& tenant) {
    std::vector<ACLRule> acls;

    // Query dedicated ACL tables
    auto acl_result = db_->get_acls_for_resource(resource_uid, tenant);
    if (!acl_result.success) {
        return Result<std::vector<ACLRule>>::err(acl_result.error);
    }

    // Transform the database format to internal ACLRule format
    for (const auto& db_acl : acl_result.value) {
        ACLRule rule;
        rule.resource_uid = resource_uid;
        rule.principal = db_acl.principal;
        rule.type = static_cast<PrincipalType>(db_acl.type);
        rule.permissions = db_acl.permissions;

        acls.push_back(rule);
    }

    return Result<std::vector<ACLRule>>::ok(acls);
}

Result<int> AclManager::get_effective_permissions(const std::string& resource_uid,
                                                 const std::string& user,
                                                 const std::vector<std::string>& roles,
                                                 const std::string& tenant) {
    auto acls_result = get_acls_for_resource(resource_uid, tenant);
    if (!acls_result.success) {
        return Result<int>::err(acls_result.error);
    }

    auto effective_roles = resolve_effective_roles(user, roles, tenant);
    int effective_perms = calculate_effective_permissions(acls_result.value, user, effective_roles);

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
    // Creator always gets full access on the resource they created.
    auto grant_result = grant_permission(resource_uid, creator, PrincipalType::USER,
                                        static_cast<int>(Permission::READ) | static_cast<int>(Permission::WRITE) | static_cast<int>(Permission::EXECUTE), tenant);
    if (!grant_result.success) {
        return grant_result;
    }

    // World-readable OTHER->READ is opt-in. Default is private-by-default.
    if (default_world_readable_) {
        return grant_permission(resource_uid, "other", PrincipalType::OTHER,
                                static_cast<int>(Permission::READ), tenant);
    }

    return Result<void>::ok();
}

Result<void> AclManager::inherit_acls(const std::string& parent_uid, 
                                     const std::string& child_uid, 
                                     const std::string& tenant) {
    // Get ACLs from parent
    auto parent_acls_result = get_acls_for_resource(parent_uid, tenant);
    if (!parent_acls_result.success) {
        return Result<void>::err(parent_acls_result.error);
    }
    
    // Copy ACLs to child
    for (const auto& rule : parent_acls_result.value) {
        auto result = grant_permission(child_uid, rule.principal, rule.type, 
                                     rule.permissions, tenant);
        if (!result.success) {
            // Log error but continue copying other ACLs
            continue;
        }
    }
    
    return Result<void>::ok();
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

        user_acls.push_back(rule);
    }

    return Result<std::vector<ACLRule>>::ok(user_acls);
}

int AclManager::calculate_effective_permissions(const std::vector<ACLRule>& rules,
                                               const std::string& user,
                                               const std::vector<std::string>& roles) {
    // Union model: every matching grant contributes its bits, regardless of
    // principal type. A per-user grant of READ does NOT suppress role-provided
    // WRITE on the same resource. This matches POSIX-ACL / NFSv4-style additive
    // grants and avoids the surprising "user-rule masks role-rule" footgun
    // documented in design_documents/acl_rbac_review_and_plan.md.
    int effective_perms = 0;

    for (const auto& rule : rules) {
        switch (rule.type) {
            case PrincipalType::USER:
                if (rule.principal == user) {
                    effective_perms |= rule.permissions;
                }
                break;
            case PrincipalType::ROLE:
                for (const auto& role : roles) {
                    if (rule.principal == role) {
                        effective_perms |= rule.permissions;
                        break;
                    }
                }
                break;
            case PrincipalType::GROUP:
                // GROUP rules apply to anyone — group membership is not yet
                // modeled (see plan §2.3). Treat as a global grant to mirror
                // the prior behavior until groups are persisted.
                effective_perms |= rule.permissions;
                break;
            case PrincipalType::OTHER:
                effective_perms |= rule.permissions;
                break;
        }
    }

    return effective_perms;
}

} // namespace fileengine