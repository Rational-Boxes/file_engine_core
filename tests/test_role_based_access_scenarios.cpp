#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>
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
    
    // Minimal implementations for required methods
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
    
    // ACL operations
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
    
    // Role management operations (corrected implementation - no persistent storage)
    Result<void> create_role(const std::string& role, const std::string& tenant = "") override {
        if (role.empty()) {
            return Result<void>::err("Role name cannot be empty");
        }
        return Result<void>::ok();
    }
    
    Result<void> delete_role(const std::string& role, const std::string& tenant = "") override {
        if (role.empty()) {
            return Result<void>::err("Role name cannot be empty");
        }
        return Result<void>::ok();
    }
    
    Result<void> assign_user_to_role(const std::string& user, const std::string& role, 
                                     const std::string& tenant = "") override {
        if (user.empty() || role.empty()) {
            return Result<void>::err("User and role names cannot be empty");
        }
        return Result<void>::ok();
    }
    
    Result<void> remove_user_from_role(const std::string& user, const std::string& role, 
                                       const std::string& tenant = "") override {
        if (user.empty() || role.empty()) {
            return Result<void>::err("User and role names cannot be empty");
        }
        return Result<void>::ok();
    }
    
    Result<std::vector<std::string>> get_roles_for_user(const std::string& user, 
                                                        const std::string& tenant = "") override {
        // In the corrected implementation, roles are passed with each request
        // The database doesn't store user-role mappings
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

void test_role_based_access_scenarios() {
    std::cout << "Testing role-based access scenarios...\n";
    
    // Create a mock database
    auto db = std::make_shared<MockDatabase>();
    
    // Create an ACL manager
    AclManager acl_manager(db);
    
    // Define test resource UIDs
    std::string root_uid = "root-directory-uuid";
    std::string folder_uid = "test-folder-uuid";
    
    // Define roles
    std::string users_role = "users";      // Read-only access
    std::string contributors_role = "contributors";  // Read and write access
    std::string admins_role = "administrators";      // Full access
    
    // Test 1: Grant READ permission to 'users' role on root directory
    std::cout << "Test 1: Granting READ permission to 'users' role on root directory...\n";
    auto result = acl_manager.grant_permission(root_uid, users_role, PrincipalType::ROLE, 
                                              static_cast<int>(Permission::READ));
    assert(result.success);
    std::cout << "  Granted READ permission to 'users' role: OK\n";
    
    // Test 2: Grant READ and WRITE permissions to 'contributors' role on root directory
    std::cout << "Test 2: Granting READ and WRITE permissions to 'contributors' role on root directory...\n";
    result = acl_manager.grant_permission(root_uid, contributors_role, PrincipalType::ROLE, 
                                         static_cast<int>(Permission::READ) | static_cast<int>(Permission::WRITE));
    assert(result.success);
    std::cout << "  Granted READ and WRITE permissions to 'contributors' role: OK\n";
    
    // Test 3: Grant ALL permissions to 'administrators' role on root directory
    std::cout << "Test 3: Granting ALL permissions to 'administrators' role on root directory...\n";
    int all_permissions = static_cast<int>(Permission::READ) | static_cast<int>(Permission::WRITE) |
                          static_cast<int>(Permission::DELETE) | static_cast<int>(Permission::EXECUTE);
    result = acl_manager.grant_permission(root_uid, admins_role, PrincipalType::ROLE, all_permissions);
    assert(result.success);
    std::cout << "  Granted ALL permissions to 'administrators' role: OK\n";

    // Test 4: Grant READ permission to 'users' role on a specific folder
    std::cout << "Test 4: Granting READ permission to 'users' role on test folder...\n";
    result = acl_manager.grant_permission(folder_uid, users_role, PrincipalType::ROLE,
                                         static_cast<int>(Permission::READ));
    assert(result.success);
    std::cout << "  Granted READ permission to 'users' role on folder: OK\n";

    // Test 5: Grant READ and WRITE permissions to 'contributors' role on the same folder
    std::cout << "Test 5: Granting READ and WRITE permissions to 'contributors' role on test folder...\n";
    result = acl_manager.grant_permission(folder_uid, contributors_role, PrincipalType::ROLE,
                                         static_cast<int>(Permission::READ) | static_cast<int>(Permission::WRITE));
    assert(result.success);
    std::cout << "  Granted READ and WRITE permissions to 'contributors' role on folder: OK\n";

    // Test 6: Grant ALL permissions to 'administrators' role on the same folder
    std::cout << "Test 6: Granting ALL permissions to 'administrators' role on test folder...\n";
    result = acl_manager.grant_permission(folder_uid, admins_role, PrincipalType::ROLE, all_permissions);
    assert(result.success);
    std::cout << "  Granted ALL permissions to 'administrators' role on folder: OK\n";

    // Test 7: Verify access for user with 'users' role (should have READ only)
    std::cout << "Test 7: Verifying access for user with 'users' role...\n";
    std::vector<std::string> user_roles = {users_role};
    auto perm_result = acl_manager.get_effective_permissions(root_uid, "test_user", user_roles);
    assert(perm_result.success);

    // Check that user has READ but not WRITE
    bool has_read = (perm_result.value & static_cast<int>(Permission::READ)) == static_cast<int>(Permission::READ);
    bool has_write = (perm_result.value & static_cast<int>(Permission::WRITE)) == static_cast<int>(Permission::WRITE);

    assert(has_read);
    assert(!has_write);  // Users should NOT have write permission
    std::cout << "  User with 'users' role has READ but not WRITE on root: OK\n";

    // Also test on folder
    perm_result = acl_manager.get_effective_permissions(folder_uid, "test_user", user_roles);
    assert(perm_result.success);
    has_read = (perm_result.value & static_cast<int>(Permission::READ)) == static_cast<int>(Permission::READ);
    has_write = (perm_result.value & static_cast<int>(Permission::WRITE)) == static_cast<int>(Permission::WRITE);

    assert(has_read);
    assert(!has_write);  // Users should NOT have write permission
    std::cout << "  User with 'users' role has READ but not WRITE on folder: OK\n";

    // Test 8: Verify access for user with 'contributors' role (should have READ and WRITE)
    std::cout << "Test 8: Verifying access for user with 'contributors' role...\n";
    std::vector<std::string> contrib_roles = {contributors_role};
    perm_result = acl_manager.get_effective_permissions(root_uid, "contributor_user", contrib_roles);
    assert(perm_result.success);

    // Check that user has READ and WRITE
    has_read = (perm_result.value & static_cast<int>(Permission::READ)) == static_cast<int>(Permission::READ);
    has_write = (perm_result.value & static_cast<int>(Permission::WRITE)) == static_cast<int>(Permission::WRITE);
    bool has_delete = (perm_result.value & static_cast<int>(Permission::DELETE)) == static_cast<int>(Permission::DELETE);

    assert(has_read);
    assert(has_write);
    assert(!has_delete);  // Contributors should NOT have delete permission (unless explicitly granted)
    std::cout << "  User with 'contributors' role has READ and WRITE on root: OK\n";

    // Also test on folder
    perm_result = acl_manager.get_effective_permissions(folder_uid, "contributor_user", contrib_roles);
    assert(perm_result.success);
    has_read = (perm_result.value & static_cast<int>(Permission::READ)) == static_cast<int>(Permission::READ);
    has_write = (perm_result.value & static_cast<int>(Permission::WRITE)) == static_cast<int>(Permission::WRITE);

    assert(has_read);
    assert(has_write);
    std::cout << "  User with 'contributors' role has READ and WRITE on folder: OK\n";

    // Test 9: Verify access for user with 'administrators' role (should have full access)
    std::cout << "Test 9: Verifying access for user with 'administrators' role...\n";
    std::vector<std::string> admin_roles = {admins_role};
    perm_result = acl_manager.get_effective_permissions(root_uid, "admin_user", admin_roles);
    assert(perm_result.success);

    // Check that admin has all permissions
    has_read = (perm_result.value & static_cast<int>(Permission::READ)) == static_cast<int>(Permission::READ);
    has_write = (perm_result.value & static_cast<int>(Permission::WRITE)) == static_cast<int>(Permission::WRITE);
    has_delete = (perm_result.value & static_cast<int>(Permission::DELETE)) == static_cast<int>(Permission::DELETE);
    bool has_execute = (perm_result.value & static_cast<int>(Permission::EXECUTE)) == static_cast<int>(Permission::EXECUTE);

    assert(has_read);
    assert(has_write);
    assert(has_delete);
    assert(has_execute);
    std::cout << "  User with 'administrators' role has full access on root: OK\n";

    // Also test on folder
    perm_result = acl_manager.get_effective_permissions(folder_uid, "admin_user", admin_roles);
    assert(perm_result.success);
    has_read = (perm_result.value & static_cast<int>(Permission::READ)) == static_cast<int>(Permission::READ);
    has_write = (perm_result.value & static_cast<int>(Permission::WRITE)) == static_cast<int>(Permission::WRITE);
    has_delete = (perm_result.value & static_cast<int>(Permission::DELETE)) == static_cast<int>(Permission::DELETE);
    has_execute = (perm_result.value & static_cast<int>(Permission::EXECUTE)) == static_cast<int>(Permission::EXECUTE);

    assert(has_read);
    assert(has_write);
    assert(has_delete);
    assert(has_execute);
    std::cout << "  User with 'administrators' role has full access on folder: OK\n";

    // Test 10: Verify ACLs are properly set
    std::cout << "Test 10: Verifying ACLs are properly set...\n";
    auto acls_result = acl_manager.get_acls_for_resource(root_uid);
    assert(acls_result.success);
    assert(acls_result.value.size() >= 3); // Should have at least 3 ACLs: users, contributors, administrators
    std::cout << "  Found " << acls_result.value.size() << " ACLs for root: OK\n";

    // Verify each ACL entry for root
    bool users_acl_found = false, contributors_acl_found = false, admins_acl_found = false;
    for (const auto& acl : acls_result.value) {
        if (acl.principal == users_role && acl.type == PrincipalType::ROLE) {
            users_acl_found = true;
            assert(acl.permissions == static_cast<int>(Permission::READ));
        } else if (acl.principal == contributors_role && acl.type == PrincipalType::ROLE) {
            contributors_acl_found = true;
            assert((acl.permissions & static_cast<int>(Permission::READ)) == static_cast<int>(Permission::READ));
            assert((acl.permissions & static_cast<int>(Permission::WRITE)) == static_cast<int>(Permission::WRITE));
        } else if (acl.principal == admins_role && acl.type == PrincipalType::ROLE) {
            admins_acl_found = true;
            // Admins should have all permissions
            assert((acl.permissions & all_permissions) == all_permissions);
        }
    }
    assert(users_acl_found && contributors_acl_found && admins_acl_found);
    std::cout << "  All ACL entries for root verified: OK\n";

    // Verify ACLs for folder
    auto folder_acls_result = acl_manager.get_acls_for_resource(folder_uid);
    assert(folder_acls_result.success);
    assert(folder_acls_result.value.size() >= 3); // Should have at least 3 ACLs: users, contributors, administrators
    std::cout << "  Found " << folder_acls_result.value.size() << " ACLs for folder: OK\n";

    // Verify each ACL entry for folder
    bool folder_users_acl_found = false;
    bool folder_contributors_acl_found = false;
    bool folder_admins_acl_found = false;
    for (const auto& acl : folder_acls_result.value) {
        if (acl.principal == users_role && acl.type == PrincipalType::ROLE) {
            folder_users_acl_found = true;
            assert(acl.permissions == static_cast<int>(Permission::READ));
        } else if (acl.principal == contributors_role && acl.type == PrincipalType::ROLE) {
            folder_contributors_acl_found = true;
            assert((acl.permissions & static_cast<int>(Permission::READ)) == static_cast<int>(Permission::READ));
            assert((acl.permissions & static_cast<int>(Permission::WRITE)) == static_cast<int>(Permission::WRITE));
        } else if (acl.principal == admins_role && acl.type == PrincipalType::ROLE) {
            folder_admins_acl_found = true;
            // Admins should have all permissions
            int all_folder_permissions = static_cast<int>(Permission::READ) | static_cast<int>(Permission::WRITE) |
                                         static_cast<int>(Permission::DELETE) | static_cast<int>(Permission::EXECUTE);
            assert((acl.permissions & all_folder_permissions) == all_folder_permissions);
        }
    }
    assert(folder_users_acl_found && folder_contributors_acl_found && folder_admins_acl_found);
    std::cout << "  All ACL entries for folder verified: OK\n";
    
    std::cout << "\nAll role-based access scenarios passed!\n";
}

int main() {
    try {
        test_role_based_access_scenarios();
        std::cout << "\n✅ All role-based access tests passed successfully!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n❌ Test failed with unknown exception" << std::endl;
        return 1;
    }
}