#include "fileengine/role_manager.h"
#include "fileengine/IDatabase.h"

namespace fileengine {

RoleManager::RoleManager(std::shared_ptr<IDatabase> db) : db_(db) {
}

Result<void> RoleManager::create_role(const std::string& role, const std::string& tenant) {
    if (role.empty()) {
        return Result<void>::err("Role name cannot be empty");
    }
    return db_->create_role(role, tenant);
}

Result<void> RoleManager::delete_role(const std::string& role, const std::string& tenant) {
    if (role.empty()) {
        return Result<void>::err("Role name cannot be empty");
    }
    return db_->delete_role(role, tenant);
}

Result<void> RoleManager::assign_user_to_role(const std::string& user, const std::string& role,
                                              const std::string& tenant) {
    if (user.empty() || role.empty()) {
        return Result<void>::err("User and role names cannot be empty");
    }
    return db_->assign_user_to_role(user, role, tenant);
}

Result<void> RoleManager::remove_user_from_role(const std::string& user, const std::string& role,
                                                const std::string& tenant) {
    if (user.empty() || role.empty()) {
        return Result<void>::err("User and role names cannot be empty");
    }
    return db_->remove_user_from_role(user, role, tenant);
}

Result<std::vector<std::string>> RoleManager::get_roles_for_user(const std::string& user,
                                                                 const std::string& tenant) {
    if (user.empty()) {
        return Result<std::vector<std::string>>::err("User name cannot be empty");
    }
    return db_->get_roles_for_user(user, tenant);
}

Result<std::vector<std::string>> RoleManager::get_users_for_role(const std::string& role,
                                                                 const std::string& tenant) {
    if (role.empty()) {
        return Result<std::vector<std::string>>::err("Role name cannot be empty");
    }
    return db_->get_users_for_role(role, tenant);
}

Result<std::vector<std::string>> RoleManager::get_all_roles(const std::string& tenant) {
    return db_->get_all_roles(tenant);
}

} // namespace fileengine
