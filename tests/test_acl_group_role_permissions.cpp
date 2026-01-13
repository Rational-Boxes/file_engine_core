#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <optional>
#include "fileengine/acl_manager.h"
#include "fileengine/types.h"
#include "fileengine/IDatabase.h"

using namespace fileengine;

// Mock database implementation for testing
class MockDatabase : public IDatabase {
public:
    bool connect() override { return true; }
    void disconnect() override {}
    bool is_connected() const override { return true; }
    Result<void> create_schema() override { return Result<void>::ok(); }
    Result<void> drop_schema() override { return Result<void>::ok(); }
    
    // File metadata operations - minimal implementation for testing
    Result<std::string> insert_file(const std::string& uid, const std::string& name,
                                   const std::string& path, const std::string& parent_uid,
                                   FileType type, const std::string& owner,
                                   int permissions, const std::string& tenant = "") override {
        return Result<std::string>::ok(uid);
    }
    Result<void> update_file_modified(const std::string& uid, const std::string& tenant = "") override {
        return Result<void>::ok();
    }
    Result<void> update_file_current_version(const std::string& uid, const std::string& version_timestamp, const std::string& tenant = "") override {
        return Result<void>::ok();
    }
    Result<bool> delete_file(const std::string& uid, const std::string& tenant = "") override {
        return Result<bool>::ok(true);
    }
    Result<bool> undelete_file(const std::string& uid, const std::string& tenant = "") override {
        return Result<bool>::ok(true);
    }
    Result<std::optional<FileInfo>> get_file_by_uid(const std::string& uid, const std::string& tenant = "") override {
        return Result<std::optional<FileInfo>>::ok(std::nullopt);
    }
    Result<std::optional<FileInfo>> get_file_by_path(const std::string& path, const std::string& tenant = "") override {
        return Result<std::optional<FileInfo>>::ok(std::nullopt);
    }
    Result<void> update_file_name(const std::string& uid, const std::string& new_name, const std::string& tenant = "") override {
        return Result<void>::ok();
    }
    Result<std::vector<FileInfo>> list_files_in_directory(const std::string& parent_uid, const std::string& tenant = "") override {
        return Result<std::vector<FileInfo>>::ok(std::vector<FileInfo>{});
    }
    Result<std::vector<FileInfo>> list_files_in_directory_with_deleted(const std::string& parent_uid, const std::string& tenant = "") override {
        return Result<std::vector<FileInfo>>::ok(std::vector<FileInfo>{});
    }
    Result<std::vector<FileInfo>> list_all_files(const std::string& tenant = "") override {
        return Result<std::vector<FileInfo>>::ok(std::vector<FileInfo>{});
    }
    Result<std::optional<FileInfo>> get_file_by_name_and_parent(const std::string& name, const std::string& parent_uid, const std::string& tenant = "") override {
        return Result<std::optional<FileInfo>>::ok(std::nullopt);
    }
    Result<std::optional<FileInfo>> get_file_by_name_and_parent_include_deleted(const std::string& name, const std::string& parent_uid, const std::string& tenant = "") override {
        return Result<std::optional<FileInfo>>::ok(std::nullopt);
    }
    Result<int64_t> get_file_size(const std::string& file_uid, const std::string& tenant = "") override {
        return Result<int64_t>::ok(0);
    }
    Result<int64_t> get_directory_size(const std::string& dir_uid, const std::string& tenant = "") override {
        return Result<int64_t>::ok(0);
    }
    Result<std::optional<FileInfo>> get_file_by_uid_include_deleted(const std::string& uid, const std::string& tenant = "") override {
        return Result<std::optional<FileInfo>>::ok(std::nullopt);
    }
    Result<void> update_file_parent(const std::string& uid, const std::string& new_parent_uid, const std::string& tenant = "") override {
        return Result<void>::ok();
    }
    
    // Path-to-UUID mapping
    Result<std::string> path_to_uid(const std::string& path, const std::string& tenant = "") override {
        return Result<std::string>::ok("");
    }
    Result<std::vector<std::string>> uid_to_path(const std::string& uid, const std::string& tenant = "") override {
        return Result<std::vector<std::string>>::ok(std::vector<std::string>{});
    }
    
    // Version operations
    Result<int64_t> insert_version(const std::string& file_uid, const std::string& version_timestamp,
                                  int64_t size, const std::string& storage_path, const std::string& tenant = "") override {
        return Result<int64_t>::ok(0);
    }
    Result<std::optional<std::string>> get_version_storage_path(const std::string& file_uid, const std::string& version_timestamp, const std::string& tenant = "") override {
        return Result<std::optional<std::string>>::ok(std::nullopt);
    }
    Result<std::vector<std::string>> list_versions(const std::string& file_uid, const std::string& tenant = "") override {
        return Result<std::vector<std::string>>::ok(std::vector<std::string>{});
    }
    
    // Version restoration operations
    Result<bool> restore_to_version(const std::string& file_uid, const std::string& version_timestamp, const std::string& user, const std::string& tenant = "") override {
        return Result<bool>::ok(true);
    }
    
    // Metadata operations
    Result<void> set_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& value, const std::string& tenant = "") override {
        return Result<void>::ok();
    }
    Result<std::optional<std::string>> get_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& tenant = "") override {
        return Result<std::optional<std::string>>::ok(std::nullopt);
    }
    Result<std::map<std::string, std::string>> get_all_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& tenant = "") override {
        return Result<std::map<std::string, std::string>>::ok(std::map<std::string, std::string>{});
    }
    Result<void> delete_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& tenant = "") override {
        return Result<void>::ok();
    }
    
    // Direct SQL execution
    Result<void> execute(const std::string& sql, const std::string& tenant = "") override {
        return Result<void>::ok();
    }
    Result<std::vector<std::vector<std::string>>> query(const std::string& sql, const std::string& tenant = "") override {
        return Result<std::vector<std::vector<std::string>>>::ok(std::vector<std::vector<std::string>>{});
    }
    
    // Cache tracking operations
    Result<void> update_file_access_stats(const std::string& uid, const std::string& user, const std::string& tenant = "") override {
        return Result<void>::ok();
    }
    Result<std::vector<std::string>> get_least_accessed_files(int limit = 10, const std::string& tenant = "") override {
        return Result<std::vector<std::string>>::ok(std::vector<std::string>{});
    }
    Result<std::vector<std::string>> get_infrequently_accessed_files(int days_threshold = 30, const std::string& tenant = "") override {
        return Result<std::vector<std::string>>::ok(std::vector<std::string>{});
    }
    Result<int64_t> get_storage_usage(const std::string& tenant = "") override {
        return Result<int64_t>::ok(0);
    }
    Result<int64_t> get_storage_capacity(const std::string& tenant = "") override {
        return Result<int64_t>::ok(0);
    }
    
    // Tenant management operations
    Result<void> create_tenant_schema(const std::string& tenant) override {
        return Result<void>::ok();
    }
    Result<bool> tenant_schema_exists(const std::string& tenant) override {
        return Result<bool>::ok(true);
    }
    Result<void> cleanup_tenant_data(const std::string& tenant) override {
        return Result<void>::ok();
    }
    Result<std::vector<std::string>> list_tenants() override {
        return Result<std::vector<std::string>>::ok(std::vector<std::string>{});
    }
    
    // ACL operations - these are the important ones for our test
    Result<void> add_acl(const std::string& resource_uid, const std::string& principal,
                         int type, int permissions, const std::string& tenant = "") override {
        AclEntry entry;
        entry.resource_uid = resource_uid;
        entry.principal = principal;
        entry.type = type;
        entry.permissions = permissions;
        
        // Store in our mock database
        acls_[resource_uid].push_back(entry);
        return Result<void>::ok();
    }
    
    Result<void> remove_acl(const std::string& resource_uid, const std::string& principal,
                            int type, const std::string& tenant = "") override {
        auto& resource_acls = acls_[resource_uid];
        resource_acls.erase(
            std::remove_if(resource_acls.begin(), resource_acls.end(),
                          [&](const AclEntry& entry) {
                              return entry.principal == principal && entry.type == type;
                          }),
            resource_acls.end());
        return Result<void>::ok();
    }
    
    Result<std::vector<AclEntry>> get_acls_for_resource(const std::string& resource_uid,
                                                        const std::string& tenant = "") override {
        auto it = acls_.find(resource_uid);
        if (it != acls_.end()) {
            return Result<std::vector<AclEntry>>::ok(it->second);
        }
        return Result<std::vector<AclEntry>>::ok(std::vector<AclEntry>{});
    }
    
    Result<std::vector<AclEntry>> get_user_acls(const std::string& resource_uid,
                                                const std::string& principal,
                                                const std::string& tenant = "") override {
        auto it = acls_.find(resource_uid);
        if (it != acls_.end()) {
            std::vector<AclEntry> user_acls;
            for (const auto& entry : it->second) {
                if (entry.principal == principal) {
                    user_acls.push_back(entry);
                }
            }
            return Result<std::vector<AclEntry>>::ok(user_acls);
        }
        return Result<std::vector<AclEntry>>::ok(std::vector<AclEntry>{});
    }
    
    // Role management operations
    Result<void> create_role(const std::string& role, const std::string& tenant = "") override {
        return Result<void>::ok();
    }
    
    Result<void> delete_role(const std::string& role, const std::string& tenant = "") override {
        return Result<void>::ok();
    }
    
    Result<void> assign_user_to_role(const std::string& user, const std::string& role, 
                                     const std::string& tenant = "") override {
        return Result<void>::ok();
    }
    
    Result<void> remove_user_from_role(const std::string& user, const std::string& role, 
                                       const std::string& tenant = "") override {
        return Result<void>::ok();
    }
    
    Result<std::vector<std::string>> get_roles_for_user(const std::string& user, 
                                                        const std::string& tenant = "") override {
        // In a real implementation, this would return the roles for the user
        // For this test, we'll return an empty vector since roles are passed with requests
        return Result<std::vector<std::string>>::ok(std::vector<std::string>{});
    }
    
    Result<std::vector<std::string>> get_users_for_role(const std::string& role, 
                                                        const std::string& tenant = "") override {
        return Result<std::vector<std::string>>::ok(std::vector<std::string>{});
    }
    
    Result<std::vector<std::string>> get_all_roles(const std::string& tenant = "") override {
        return Result<std::vector<std::string>>::ok(std::vector<std::string>{});
    }

private:
    std::map<std::string, std::vector<AclEntry>> acls_;
};

void test_acl_group_role_permissions() {
    std::cout << "Testing ACL group/role based permissions...\n";
    
    // Create a mock database
    auto db = std::make_shared<MockDatabase>();
    
    // Create an ACL manager
    AclManager acl_manager(db);
    
    // Test resource UID
    std::string resource_uid = "test-resource-uuid";
    std::string user = "test-user";
    std::string group = "test-group";
    std::string role = "test-role";
    
    // Test 1: Grant READ permission to a user
    std::cout << "Test 1: Granting READ permission to user...\n";
    auto result = acl_manager.grant_permission(resource_uid, user, PrincipalType::USER, 
                                              static_cast<int>(Permission::READ));
    assert(result.success);
    
    // Test 2: Grant WRITE permission to a group
    std::cout << "Test 2: Granting WRITE permission to group...\n";
    result = acl_manager.grant_permission(resource_uid, group, PrincipalType::GROUP, 
                                         static_cast<int>(Permission::WRITE));
    assert(result.success);
    
    // Test 3: Grant DELETE permission to a role
    std::cout << "Test 3: Granting DELETE permission to role...\n";
    result = acl_manager.grant_permission(resource_uid, role, PrincipalType::ROLE, 
                                         static_cast<int>(Permission::DELETE));
    assert(result.success);
    
    // Test 4: Check permissions for user
    std::cout << "Test 4: Checking permissions for user...\n";
    std::vector<std::string> roles = {}; // Empty roles for this test
    auto perm_result = acl_manager.get_effective_permissions(resource_uid, user, roles);
    assert(perm_result.success);
    assert((perm_result.value & static_cast<int>(Permission::READ)) == static_cast<int>(Permission::READ));
    std::cout << "  User has READ permission: OK\n";
    
    // Test 5: Check permissions with roles (role-based access)
    std::cout << "Test 5: Checking permissions with role...\n";
    std::vector<std::string> user_roles = {role}; // User has the role with DELETE permission
    perm_result = acl_manager.get_effective_permissions(resource_uid, "another-user", user_roles);
    assert(perm_result.success);
    assert((perm_result.value & static_cast<int>(Permission::DELETE)) == static_cast<int>(Permission::DELETE));
    std::cout << "  User with role has DELETE permission: OK\n";
    
    // Test 6: Check ACLs for resource
    std::cout << "Test 6: Retrieving ACLs for resource...\n";
    auto acls_result = acl_manager.get_acls_for_resource(resource_uid);
    assert(acls_result.success);
    assert(acls_result.value.size() == 3); // Should have 3 ACLs: user, group, role
    std::cout << "  Found " << acls_result.value.size() << " ACLs: OK\n";
    
    // Verify each ACL entry
    bool user_acl_found = false, group_acl_found = false, role_acl_found = false;
    for (const auto& acl : acls_result.value) {
        if (acl.principal == user && acl.type == PrincipalType::USER) {
            user_acl_found = true;
            assert(acl.permissions == static_cast<int>(Permission::READ));
        } else if (acl.principal == group && acl.type == PrincipalType::GROUP) {
            group_acl_found = true;
            assert(acl.permissions == static_cast<int>(Permission::WRITE));
        } else if (acl.principal == role && acl.type == PrincipalType::ROLE) {
            role_acl_found = true;
            assert(acl.permissions == static_cast<int>(Permission::DELETE));
        }
    }
    assert(user_acl_found && group_acl_found && role_acl_found);
    std::cout << "  All ACL entries verified: OK\n";
    
    // Test 7: Permission priority (user > role > group > other)
    std::cout << "Test 7: Testing permission priority...\n";

    // Create a different user to test group permissions separately
    std::string group_user = "group-test-user";

    // Grant EXECUTE permission to the group
    result = acl_manager.grant_permission(resource_uid, group, PrincipalType::GROUP,
                                         static_cast<int>(Permission::EXECUTE));
    assert(result.success);

    // Check that a user with the group in their roles has the group's EXECUTE permission
    // In our implementation, groups are treated similarly to roles for permission calculation
    std::vector<std::string> user_with_group = {group}; // User belongs to the group
    perm_result = acl_manager.get_effective_permissions(resource_uid, "test-group-user", user_with_group);
    assert(perm_result.success);
    // User in the group should have EXECUTE from the group ACL
    assert((perm_result.value & static_cast<int>(Permission::EXECUTE)) == static_cast<int>(Permission::EXECUTE));
    std::cout << "  User in group has EXECUTE permission: OK\n";
    
    // Test 8: Check permission with role taking precedence over group
    std::vector<std::string> roles_with_higher_priority = {role, "another-role"};
    perm_result = acl_manager.get_effective_permissions(resource_uid, "some-user", roles_with_higher_priority);
    assert(perm_result.success);
    // Should have DELETE from role
    assert((perm_result.value & static_cast<int>(Permission::DELETE)) == static_cast<int>(Permission::DELETE));
    std::cout << "  Role-based permissions work correctly: OK\n";
    
    // Test 9: Revoke permission
    std::cout << "Test 9: Revoking permission...\n";
    result = acl_manager.revoke_permission(resource_uid, user, PrincipalType::USER, 
                                          static_cast<int>(Permission::READ));
    assert(result.success);
    
    // User should no longer have READ permission
    perm_result = acl_manager.get_effective_permissions(resource_uid, user, {});
    assert(perm_result.success);
    // Should not have READ permission anymore (but might still have EXECUTE from group membership)
    assert((perm_result.value & static_cast<int>(Permission::READ)) != static_cast<int>(Permission::READ));
    std::cout << "  Permission revoked successfully: OK\n";
    
    std::cout << "\nAll ACL group/role permission tests passed!\n";
}

int main() {
    try {
        test_acl_group_role_permissions();
        std::cout << "\n✅ All tests passed successfully!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n❌ Test failed with unknown exception" << std::endl;
        return 1;
    }
}