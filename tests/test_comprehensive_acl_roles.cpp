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
    Result<std::string> create_file_with_acls(const std::string& uid,
                                               const std::string&, const std::string&,
                                               const std::string&,
                                               FileType, const std::string&,
                                               int,
                                               const std::vector<AclGrant>&,
                                               const std::string& = "") override {
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
                         int type, int permissions,
                         const std::string& tenant = "",
                         const std::string& /*performed_by*/ = "",
                         int /*effect*/ = 0) override {
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
                            int type, int permissions,
                            const std::string& tenant = "",
                            const std::string& /*performed_by*/ = "",
                            int /*effect*/ = 0) override {
        auto& resource_acls = acls_[resource_uid];
        for (auto& entry : resource_acls) {
            if (entry.principal == principal && entry.type == type) {
                entry.permissions &= ~permissions;
            }
        }
        resource_acls.erase(
            std::remove_if(resource_acls.begin(), resource_acls.end(),
                          [&](const AclEntry& entry) {
                              return entry.principal == principal && entry.type == type
                                     && entry.permissions == 0;
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
                                                int type,
                                                const std::string& tenant = "") override {
        auto it = acls_.find(resource_uid);
        if (it != acls_.end()) {
            std::vector<AclEntry> user_acls;
            for (const auto& entry : it->second) {
                if (entry.principal == principal && entry.type == type) {
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

void test_user_permissions() {
    std::cout << "Testing user-level permissions...\n";
    
    auto db = std::make_shared<MockDatabase>();
    AclManager acl_manager(db);
    
    std::string resource_uid = "test-resource-uuid";
    std::string user = "test_user";
    
    // Test 1: Grant READ permission to user
    auto result = acl_manager.grant_permission(resource_uid, user, PrincipalType::USER, 
                                              static_cast<int>(Permission::READ));
    assert(result.success);
    std::cout << "  ✓ Granted READ permission to user\n";
    
    // Test 2: Verify user has READ permission
    auto perm_result = acl_manager.get_effective_permissions(resource_uid, user, {});
    assert(perm_result.success);
    assert((perm_result.value & static_cast<int>(Permission::READ)) == static_cast<int>(Permission::READ));
    std::cout << "  ✓ User has READ permission\n";
    
    // Test 3: Verify user does NOT have WRITE permission
    assert((perm_result.value & static_cast<int>(Permission::WRITE)) != static_cast<int>(Permission::WRITE));
    std::cout << "  ✓ User does NOT have WRITE permission\n";
    
    // Test 4: Grant WRITE permission to user
    result = acl_manager.grant_permission(resource_uid, user, PrincipalType::USER, 
                                         static_cast<int>(Permission::WRITE));
    assert(result.success);
    std::cout << "  ✓ Granted WRITE permission to user\n";
    
    // Test 5: Verify user now has both READ and WRITE permissions
    perm_result = acl_manager.get_effective_permissions(resource_uid, user, {});
    assert(perm_result.success);
    assert((perm_result.value & static_cast<int>(Permission::READ)) == static_cast<int>(Permission::READ));
    assert((perm_result.value & static_cast<int>(Permission::WRITE)) == static_cast<int>(Permission::WRITE));
    std::cout << "  ✓ User has both READ and WRITE permissions\n";
    
    // Test 6: Bit-mask revoke — only the READ bit is cleared, WRITE remains.
    result = acl_manager.revoke_permission(resource_uid, user, PrincipalType::USER,
                                          static_cast<int>(Permission::READ));
    assert(result.success);
    std::cout << "  ✓ Revoked READ from user\n";

    // Test 7: Verify only READ was cleared; WRITE still applies.
    perm_result = acl_manager.get_effective_permissions(resource_uid, user, {});
    assert(perm_result.success);
    assert((perm_result.value & static_cast<int>(Permission::READ)) != static_cast<int>(Permission::READ));
    assert((perm_result.value & static_cast<int>(Permission::WRITE)) == static_cast<int>(Permission::WRITE));
    std::cout << "  ✓ READ cleared, WRITE retained (bit-mask revoke)\n";
    
    std::cout << "User-level permissions tests passed!\n\n";
}

void test_role_permissions() {
    std::cout << "Testing role-level permissions...\n";
    
    auto db = std::make_shared<MockDatabase>();
    AclManager acl_manager(db);
    
    std::string resource_uid = "test-resource-uuid";
    std::string user = "test_user";
    std::string role = "test_role";
    
    // Test 1: Grant READ permission to role
    auto result = acl_manager.grant_permission(resource_uid, role, PrincipalType::ROLE, 
                                              static_cast<int>(Permission::READ));
    assert(result.success);
    std::cout << "  ✓ Granted READ permission to role\n";
    
    // Test 2: Verify user with role has READ permission
    std::vector<std::string> roles = {role};
    auto perm_result = acl_manager.get_effective_permissions(resource_uid, user, roles);
    assert(perm_result.success);
    assert((perm_result.value & static_cast<int>(Permission::READ)) == static_cast<int>(Permission::READ));
    std::cout << "  ✓ User with role has READ permission\n";
    
    // Test 3: Grant WRITE permission to role
    result = acl_manager.grant_permission(resource_uid, role, PrincipalType::ROLE, 
                                         static_cast<int>(Permission::WRITE));
    assert(result.success);
    std::cout << "  ✓ Granted WRITE permission to role\n";
    
    // Test 4: Verify user with role now has both READ and WRITE permissions
    perm_result = acl_manager.get_effective_permissions(resource_uid, user, roles);
    assert(perm_result.success);
    assert((perm_result.value & static_cast<int>(Permission::READ)) == static_cast<int>(Permission::READ));
    assert((perm_result.value & static_cast<int>(Permission::WRITE)) == static_cast<int>(Permission::WRITE));
    std::cout << "  ✓ User with role has both READ and WRITE permissions\n";
    
    // Test 5: Verify user without role does NOT have permissions
    auto no_role_result = acl_manager.get_effective_permissions(resource_uid, user, {});
    assert(no_role_result.success);
    assert((no_role_result.value & static_cast<int>(Permission::READ)) != static_cast<int>(Permission::READ));
    assert((no_role_result.value & static_cast<int>(Permission::WRITE)) != static_cast<int>(Permission::WRITE));
    std::cout << "  ✓ User without role does NOT have permissions\n";
    
    std::cout << "Role-level permissions tests passed!\n\n";
}

void test_group_permissions() {
    std::cout << "Testing group-level permissions...\n";
    
    auto db = std::make_shared<MockDatabase>();
    AclManager acl_manager(db);
    
    std::string resource_uid = "test-resource-uuid";
    std::string user = "test_user";
    std::string group = "test_group";
    
    // Test 1: Grant READ permission to group
    auto result = acl_manager.grant_permission(resource_uid, group, PrincipalType::GROUP, 
                                              static_cast<int>(Permission::READ));
    assert(result.success);
    std::cout << "  ✓ Granted READ permission to group\n";
    
    // Test 2: GROUP ACLs apply to ALL users who don't have user-specific or role-specific ACLs.
    // Note: the GROUP check does NOT match against the roles vector - it matches on
    // PrincipalType::GROUP regardless of principal name. So any user without a USER or
    // ROLE match will get GROUP permissions.
    auto perm_result = acl_manager.get_effective_permissions(resource_uid, user, {});
    assert(perm_result.success);
    assert((perm_result.value & static_cast<int>(Permission::READ)) == static_cast<int>(Permission::READ));
    std::cout << "  ✓ User gets GROUP permission (applies to all users without user/role ACLs)\n";
    
    std::cout << "Group-level permissions tests passed!\n\n";
}

void test_permission_priority() {
    std::cout << "Testing permission priority (user > role > group > other)...\n";
    
    auto db = std::make_shared<MockDatabase>();
    AclManager acl_manager(db);
    
    std::string resource_uid = "test-resource-uuid";
    std::string user = "test_user";
    std::string role = "test_role";
    std::string group = "test_group";
    
    // Test 1: Grant READ to user, WRITE to role, EXECUTE to group
    auto result = acl_manager.grant_permission(resource_uid, user, PrincipalType::USER, 
                                              static_cast<int>(Permission::READ));
    assert(result.success);
    
    result = acl_manager.grant_permission(resource_uid, role, PrincipalType::ROLE, 
                                         static_cast<int>(Permission::WRITE));
    assert(result.success);
    
    result = acl_manager.grant_permission(resource_uid, group, PrincipalType::GROUP, 
                                         static_cast<int>(Permission::EXECUTE));
    assert(result.success);
    
    std::cout << "  ✓ Set up permissions at different levels\n";

    // Test 2: Union model — USER, ROLE, and GROUP grants accumulate.
    std::vector<std::string> roles = {role, group};
    auto perm_result = acl_manager.get_effective_permissions(resource_uid, user, roles);
    assert(perm_result.success);

    assert((perm_result.value & static_cast<int>(Permission::READ)) == static_cast<int>(Permission::READ));
    std::cout << "  ✓ User has READ from user-level ACL\n";

    assert((perm_result.value & static_cast<int>(Permission::WRITE)) == static_cast<int>(Permission::WRITE));
    assert((perm_result.value & static_cast<int>(Permission::EXECUTE)) == static_cast<int>(Permission::EXECUTE));
    std::cout << "  ✓ Role and group permissions accumulate with user-level ACL (union)\n";

    // Test 3: A different user (no USER ACL) with the role gets role permissions only
    auto other_perm = acl_manager.get_effective_permissions(resource_uid, "other_user", {role});
    assert(other_perm.success);
    assert((other_perm.value & static_cast<int>(Permission::WRITE)) == static_cast<int>(Permission::WRITE));
    assert((other_perm.value & static_cast<int>(Permission::READ)) != static_cast<int>(Permission::READ));
    std::cout << "  ✓ User without user-level ACL gets role permissions\n";
    
    std::cout << "Permission priority tests passed!\n\n";
}

void test_root_directory_rule() {
    std::cout << "Testing root directory ACL behavior...\n";

    auto db = std::make_shared<MockDatabase>();
    AclManager acl_manager(db);

    std::string root_uid = "";  // Empty string represents the root directory
    std::string user = "any_user";

    // NOTE: The root-always-readable rule is implemented in the FileSystem layer
    // (filesystem.cpp), NOT in AclManager. At the AclManager level, an empty UID
    // with no ACLs yields zero permissions, which is correct behavior.
    auto perm_result = acl_manager.get_effective_permissions(root_uid, user, {});
    assert(perm_result.success);

    // With no ACLs set on root, AclManager returns 0 permissions
    assert(perm_result.value == 0);
    std::cout << "  ✓ Root directory has no ACL permissions by default (handled at FileSystem layer)\n";

    // If we explicitly grant READ on root, it works normally
    acl_manager.grant_permission(root_uid, "other", PrincipalType::OTHER, static_cast<int>(Permission::READ));
    perm_result = acl_manager.get_effective_permissions(root_uid, user, {});
    assert(perm_result.success);
    assert((perm_result.value & static_cast<int>(Permission::READ)) == static_cast<int>(Permission::READ));
    std::cout << "  ✓ Root directory is readable after explicit OTHER grant\n";

    assert((perm_result.value & static_cast<int>(Permission::WRITE)) != static_cast<int>(Permission::WRITE));
    std::cout << "  ✓ Root directory does NOT have WRITE permission\n";

    std::cout << "Root directory ACL tests passed!\n\n";
}

void test_permission_combinations() {
    std::cout << "Testing permission combinations...\n";
    
    auto db = std::make_shared<MockDatabase>();
    AclManager acl_manager(db);
    
    std::string resource_uid = "test-resource-uuid";
    std::string user = "test_user";
    std::string role = "test_role";
    
    // Test 1: Grant multiple permissions to a role
    int all_perms = static_cast<int>(fileengine::Permission::READ) |
                    static_cast<int>(fileengine::Permission::WRITE) |
                    static_cast<int>(fileengine::Permission::DELETE) |
                    static_cast<int>(fileengine::Permission::EXECUTE);

    auto result = acl_manager.grant_permission(resource_uid, role, PrincipalType::ROLE, all_perms);
    assert(result.success);
    std::cout << "  ✓ Granted all permissions to role\n";

    // Test 2: Verify user with role has all permissions
    std::vector<std::string> roles = {role};
    auto perm_result = acl_manager.get_effective_permissions(resource_uid, user, roles);
    assert(perm_result.success);

    // Check that user has all the permissions granted to the role
    assert((perm_result.value & static_cast<int>(fileengine::Permission::READ)) == static_cast<int>(fileengine::Permission::READ));
    assert((perm_result.value & static_cast<int>(fileengine::Permission::WRITE)) == static_cast<int>(fileengine::Permission::WRITE));
    assert((perm_result.value & static_cast<int>(fileengine::Permission::DELETE)) == static_cast<int>(fileengine::Permission::DELETE));
    assert((perm_result.value & static_cast<int>(fileengine::Permission::EXECUTE)) == static_cast<int>(fileengine::Permission::EXECUTE));
    std::cout << "  ✓ User with role has all permissions\n";

    // Test 3: Grant EXECUTE permission directly to user (overlapping with role).
    // Under the union model, USER and ROLE grants accumulate — the USER grant
    // does NOT suppress role permissions.
    result = acl_manager.grant_permission(resource_uid, user, PrincipalType::USER,
                                         static_cast<int>(fileengine::Permission::EXECUTE));
    assert(result.success);
    std::cout << "  ✓ Granted EXECUTE permission to user (overlapping with role)\n";

    // Test 4: User now has USER grant (EXECUTE) on top of ROLE grant (READ|WRITE|DELETE|EXECUTE).
    perm_result = acl_manager.get_effective_permissions(resource_uid, user, roles);
    assert(perm_result.success);
    assert((perm_result.value & static_cast<int>(fileengine::Permission::EXECUTE)) == static_cast<int>(fileengine::Permission::EXECUTE));
    assert((perm_result.value & static_cast<int>(fileengine::Permission::READ)) == static_cast<int>(fileengine::Permission::READ));
    assert((perm_result.value & static_cast<int>(fileengine::Permission::WRITE)) == static_cast<int>(fileengine::Permission::WRITE));
    assert((perm_result.value & static_cast<int>(fileengine::Permission::DELETE)) == static_cast<int>(fileengine::Permission::DELETE));
    std::cout << "  ✓ User and role grants accumulate (union)\n";
    
    std::cout << "Permission combination tests passed!\n\n";
}

int main() {
    try {
        std::cout << "Starting comprehensive ACL and role tests...\n\n";
        
        test_user_permissions();
        test_role_permissions();
        test_group_permissions();
        test_permission_priority();
        test_root_directory_rule();
        test_permission_combinations();
        
        std::cout << "🎉 All comprehensive ACL and role tests passed successfully!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n❌ Test failed with unknown exception" << std::endl;
        return 1;
    }
}