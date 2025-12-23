#include "fileengine/acl_manager.h"
#include "fileengine/IDatabase.h"
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
    // Remove the ACL entry using dedicated ACL methods
    auto result = db_->remove_acl(resource_uid, principal, static_cast<int>(type), tenant);

    return result;
}

Result<bool> AclManager::check_permission(const std::string& resource_uid, 
                                          const std::string& user, 
                                          const std::vector<std::string>& roles,
                                          int required_permissions, 
                                          const std::string& tenant) {
    // Get all ACLs for the resource
    auto acls_result = get_acls_for_resource(resource_uid, tenant);
    if (!acls_result.success) {
        return Result<bool>::err(acls_result.error);
    }
    
    // Calculate effective permissions for the user
    int effective_perms = calculate_effective_permissions(acls_result.value, user, roles);
    
    // Check if the effective permissions include the required permissions
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
    // Get all ACLs for the resource
    auto acls_result = get_acls_for_resource(resource_uid, tenant);
    if (!acls_result.success) {
        return Result<int>::err(acls_result.error);
    }
    
    // Calculate effective permissions
    int effective_perms = calculate_effective_permissions(acls_result.value, user, roles);
    
    return Result<int>::ok(effective_perms);
}

Result<void> AclManager::apply_default_acls(const std::string& resource_uid, 
                                           const std::string& creator, 
                                           const std::string& tenant) {
    // Apply default permissions: creator gets full access, others get read-only
    auto grant_result = grant_permission(resource_uid, creator, PrincipalType::USER,
                                        static_cast<int>(Permission::READ) | static_cast<int>(Permission::WRITE) | static_cast<int>(Permission::EXECUTE), tenant);
    if (!grant_result.success) {
        return grant_result;
    }
    
    // Grant read permission to 'other' category
    auto other_result = grant_permission(resource_uid, "other", PrincipalType::OTHER,
                                        static_cast<int>(Permission::READ), tenant);
    
    return other_result;
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
    // Query dedicated ACL tables directly for user-specific ACLs
    auto user_acl_result = db_->get_user_acls(resource_uid, user, tenant);
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
    int effective_perms = 0;
    
    // Priority: user permissions > group permissions > other permissions
    bool user_found = false;
    bool other_found = false;
    
    for (const auto& rule : rules) {
        if (rule.principal == user && rule.type == PrincipalType::USER) {
            effective_perms |= rule.permissions;
            user_found = true;
        } else if (rule.type == PrincipalType::OTHER) {
            // Store other permissions but apply them only if user doesn't have specific permissions
            if (!user_found) {
                effective_perms |= rule.permissions;
            }
            other_found = true;
        }
        // In a real implementation, we'd also handle groups
    }
    
    // If no user-specific permissions found but other permissions exist, use them
    if (!user_found && other_found) {
        effective_perms = 0;  // Reset and recalculate
        for (const auto& rule : rules) {
            if (rule.type == PrincipalType::OTHER) {
                effective_perms |= rule.permissions;
            }
        }
    }
    
    return effective_perms;
}

} // namespace fileengine