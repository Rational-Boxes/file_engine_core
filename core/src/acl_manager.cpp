#include "fileengine/acl_manager.h"
#include "fileengine/IDatabase.h"

// TODO: Architecture error: THe ACLs need to be managed in storage other than teh metadata tables since regular users need access to metadata but not bypass access control

namespace fileengine {

AclManager::AclManager(std::shared_ptr<IDatabase> db) : db_(db) {
}

Result<void> AclManager::grant_permission(const std::string& resource_uid, 
                                          const std::string& principal, 
                                          PrincipalType type, 
                                          int permissions, 
                                          const std::string& tenant) {
    // In a real implementation, this would store the permission in the database
    // For this implementation, we'll use the generic metadata system in the database
    // to store ACL information
    
    // Create a key for the ACL entry
    std::string key = "acl_" + principal + "_" + std::to_string(static_cast<int>(type));
    std::string value = std::to_string(permissions);
    
    // Use the database's metadata system to store the ACL
    // This is a simplified implementation - in a real system, you'd have dedicated ACL tables
    auto version_timestamp = "current"; // Would use actual version system in practice
    auto result = db_->set_metadata(resource_uid, version_timestamp, key, value, tenant);
    
    return result;
}

Result<void> AclManager::revoke_permission(const std::string& resource_uid, 
                                           const std::string& principal, 
                                           PrincipalType type, 
                                           int permissions, 
                                           const std::string& tenant) {
    // Create a key for the ACL entry
    std::string key = "acl_" + principal + "_" + std::to_string(static_cast<int>(type));
    
    // Remove the ACL entry
    auto version_timestamp = "current"; // Would use actual version system in practice
    auto result = db_->delete_metadata(resource_uid, version_timestamp, key, tenant);
    
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
    
    // In a real implementation, this would query a dedicated ACL table
    // For this implementation, we'll retrieve ACLs stored as metadata
    auto version_timestamp = "current"; // Would use actual version system in practice
    auto metadata_result = db_->get_all_metadata(resource_uid, version_timestamp, tenant);
    if (!metadata_result.success) {
        return Result<std::vector<ACLRule>>::err(metadata_result.error);
    }
    
    // Parse the metadata to extract ACL rules
    for (const auto& [key, value] : metadata_result.value) {
        if (key.substr(0, 4) == "acl_") {
            // Parse the key to extract principal and type
            size_t underscore_pos = key.find('_', 4); // Find the second underscore
            if (underscore_pos != std::string::npos) {
                std::string principal = key.substr(4, underscore_pos - 4);
                std::string type_str = key.substr(underscore_pos + 1);
                
                ACLRule rule;
                rule.resource_uid = resource_uid;
                rule.principal = principal;
                rule.type = static_cast<PrincipalType>(std::stoi(type_str));
                rule.permissions = std::stoi(value);
                
                acls.push_back(rule);
            }
        }
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
    // Get all ACLs for the resource
    auto all_acls_result = get_acls_for_resource(resource_uid, tenant);
    if (!all_acls_result.success) {
        return Result<std::vector<ACLRule>>::err(all_acls_result.error);
    }
    
    std::vector<ACLRule> user_acls;
    
    // Filter ACLs that apply to the user or their roles
    for (const auto& rule : all_acls_result.value) {
        if (rule.principal == user || rule.principal == "other") {
            user_acls.push_back(rule);
        }
        // In a real implementation, we'd also check for group memberships
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