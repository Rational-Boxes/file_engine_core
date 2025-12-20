#pragma once

#include "types.h"
#include <string>
#include <vector>
#include <memory>
#include <map>

namespace fileengine {

enum class Permission {
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
    GROUP,
    OTHER
};

struct ACLRule {
    std::string principal;      // User or group name
    PrincipalType type;         // User, group, or other
    int permissions;            // Bitmask of permissions
    std::string resource_uid;   // Resource this ACL applies to
};

class IDatabase;

class AclManager {
public:
    AclManager(std::shared_ptr<IDatabase> db);
    
    // Grant permission to a user/group on a resource
    Result<void> grant_permission(const std::string& resource_uid, 
                                  const std::string& principal, 
                                  PrincipalType type, 
                                  int permissions, 
                                  const std::string& tenant = "");
    
    // Revoke permission from a user/group on a resource
    Result<void> revoke_permission(const std::string& resource_uid, 
                                   const std::string& principal, 
                                   PrincipalType type, 
                                   int permissions, 
                                   const std::string& tenant = "");
    
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
    
    // Copy ACLs from parent to child (inheritance)
    Result<void> inherit_acls(const std::string& parent_uid, 
                             const std::string& child_uid, 
                             const std::string& tenant = "");
    
private:
    std::shared_ptr<IDatabase> db_;
    
    // Internal helper to get user permissions from the database
    Result<std::vector<ACLRule>> get_user_acls(const std::string& resource_uid, 
                                               const std::string& user, 
                                               const std::string& tenant);
    
    // Internal helper to calculate effective permissions
    int calculate_effective_permissions(const std::vector<ACLRule>& rules, 
                                      const std::string& user, 
                                      const std::vector<std::string>& roles);
};

} // namespace fileengine