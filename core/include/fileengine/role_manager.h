#pragma once

#include "types.h"
#include <string>
#include <vector>
#include <memory>

namespace fileengine {

class IDatabase;

class RoleManager {
public:
    RoleManager(std::shared_ptr<IDatabase> db);

    // Assign a user to a role
    Result<void> assign_user_to_role(const std::string& user, const std::string& role, 
                                     const std::string& tenant = "");

    // Remove a user from a role
    Result<void> remove_user_from_role(const std::string& user, const std::string& role, 
                                       const std::string& tenant = "");

    // Get all roles for a user
    Result<std::vector<std::string>> get_roles_for_user(const std::string& user, 
                                                         const std::string& tenant = "");

    // Create a new role
    Result<void> create_role(const std::string& role, const std::string& tenant = "");

    // Delete a role
    Result<void> delete_role(const std::string& role, const std::string& tenant = "");

    // Get all users assigned to a role
    Result<std::vector<std::string>> get_users_for_role(const std::string& role, 
                                                         const std::string& tenant = "");

    // Get all roles in the system
    Result<std::vector<std::string>> get_all_roles(const std::string& tenant = "");

private:
    std::shared_ptr<IDatabase> db_;
};

} // namespace fileengine