// Copyright (C) 2026 James Hickman
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

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