#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <utility>
#include <algorithm>
#include <optional>
#include <functional>
#include "fileengine/acl_manager.h"
#include "fileengine/role_manager.h"
#include "fileengine/types.h"
#include "fileengine/IDatabase.h"

using namespace fileengine;

// =============================================================================
// Test infrastructure
// =============================================================================

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, msg) do { \
    if (!(condition)) { \
        std::cerr << "  FAIL: " << msg << " [" << __FILE__ << ":" << __LINE__ << "]\n"; \
        throw std::runtime_error(std::string("Assertion failed: ") + msg); \
    } \
} while(0)

void run_test(const std::string& name, std::function<void()> test_fn) {
    tests_run++;
    try {
        test_fn();
        tests_passed++;
        std::cout << "  PASS: " << name << "\n";
    } catch (const std::exception& e) {
        tests_failed++;
        std::cerr << "  FAIL: " << name << " - " << e.what() << "\n";
    }
}

// =============================================================================
// Mock database with tenant-aware ACL storage
// =============================================================================

class MockDatabase : public IDatabase {
public:
    bool connect() override { return true; }
    void disconnect() override {}
    bool is_connected() const override { return true; }
    Result<void> create_schema() override { return Result<void>::ok(); }
    Result<void> drop_schema() override { return Result<void>::ok(); }

    Result<std::string> insert_file(const std::string& uid, const std::string&,
                                   const std::string&, const std::string&,
                                   FileType, const std::string&,
                                   int, const std::string&) override {
        return Result<std::string>::ok(uid);
    }
    Result<void> update_file_modified(const std::string&, const std::string&) override { return Result<void>::ok(); }
    Result<void> update_file_current_version(const std::string&, const std::string&, const std::string&) override { return Result<void>::ok(); }
    Result<bool> delete_file(const std::string&, const std::string&) override { return Result<bool>::ok(true); }
    Result<bool> undelete_file(const std::string&, const std::string&) override { return Result<bool>::ok(true); }
    Result<std::optional<FileInfo>> get_file_by_uid(const std::string&, const std::string&) override { return Result<std::optional<FileInfo>>::ok(std::nullopt); }
    Result<std::optional<FileInfo>> get_file_by_path(const std::string&, const std::string&) override { return Result<std::optional<FileInfo>>::ok(std::nullopt); }
    Result<void> update_file_name(const std::string&, const std::string&, const std::string&) override { return Result<void>::ok(); }
    Result<std::vector<FileInfo>> list_files_in_directory(const std::string&, const std::string&) override { return Result<std::vector<FileInfo>>::ok({}); }
    Result<std::vector<FileInfo>> list_files_in_directory_with_deleted(const std::string&, const std::string&) override { return Result<std::vector<FileInfo>>::ok({}); }
    Result<std::vector<FileInfo>> list_all_files(const std::string&) override { return Result<std::vector<FileInfo>>::ok({}); }
    Result<std::optional<FileInfo>> get_file_by_name_and_parent(const std::string&, const std::string&, const std::string&) override { return Result<std::optional<FileInfo>>::ok(std::nullopt); }
    Result<std::optional<FileInfo>> get_file_by_name_and_parent_include_deleted(const std::string&, const std::string&, const std::string&) override { return Result<std::optional<FileInfo>>::ok(std::nullopt); }
    Result<int64_t> get_file_size(const std::string&, const std::string&) override { return Result<int64_t>::ok(0); }
    Result<int64_t> get_directory_size(const std::string&, const std::string&) override { return Result<int64_t>::ok(0); }
    Result<std::optional<FileInfo>> get_file_by_uid_include_deleted(const std::string&, const std::string&) override { return Result<std::optional<FileInfo>>::ok(std::nullopt); }
    Result<void> update_file_parent(const std::string&, const std::string&, const std::string&) override { return Result<void>::ok(); }
    Result<std::string> path_to_uid(const std::string&, const std::string&) override { return Result<std::string>::ok(""); }
    Result<std::vector<std::string>> uid_to_path(const std::string&, const std::string&) override { return Result<std::vector<std::string>>::ok({}); }
    Result<int64_t> insert_version(const std::string&, const std::string&, int64_t, const std::string&, const std::string&) override { return Result<int64_t>::ok(0); }
    Result<std::optional<std::string>> get_version_storage_path(const std::string&, const std::string&, const std::string&) override { return Result<std::optional<std::string>>::ok(std::nullopt); }
    Result<std::vector<std::string>> list_versions(const std::string&, const std::string&) override { return Result<std::vector<std::string>>::ok({}); }
    Result<bool> restore_to_version(const std::string&, const std::string&, const std::string&, const std::string&) override { return Result<bool>::ok(true); }
    Result<void> set_metadata(const std::string&, const std::string&, const std::string&, const std::string&, const std::string&) override { return Result<void>::ok(); }
    Result<std::optional<std::string>> get_metadata(const std::string&, const std::string&, const std::string&, const std::string&) override { return Result<std::optional<std::string>>::ok(std::nullopt); }
    Result<std::map<std::string, std::string>> get_all_metadata(const std::string&, const std::string&, const std::string&) override { return Result<std::map<std::string, std::string>>::ok({}); }
    Result<void> delete_metadata(const std::string&, const std::string&, const std::string&, const std::string&) override { return Result<void>::ok(); }
    Result<void> execute(const std::string&, const std::string&) override { return Result<void>::ok(); }
    Result<std::vector<std::vector<std::string>>> query(const std::string&, const std::string&) override { return Result<std::vector<std::vector<std::string>>>::ok({}); }
    Result<void> update_file_access_stats(const std::string&, const std::string&, const std::string&) override { return Result<void>::ok(); }
    Result<std::vector<std::string>> get_least_accessed_files(int, const std::string&) override { return Result<std::vector<std::string>>::ok({}); }
    Result<std::vector<std::string>> get_infrequently_accessed_files(int, const std::string&) override { return Result<std::vector<std::string>>::ok({}); }
    Result<int64_t> get_storage_usage(const std::string&) override { return Result<int64_t>::ok(0); }
    Result<int64_t> get_storage_capacity(const std::string&) override { return Result<int64_t>::ok(0); }
    Result<void> create_tenant_schema(const std::string&) override { return Result<void>::ok(); }
    Result<bool> tenant_schema_exists(const std::string&) override { return Result<bool>::ok(true); }
    Result<void> cleanup_tenant_data(const std::string&) override { return Result<void>::ok(); }
    Result<std::vector<std::string>> list_tenants() override { return Result<std::vector<std::string>>::ok({}); }

    // =========================================================================
    // ACL operations - tenant-aware in-memory storage
    // =========================================================================

    Result<void> add_acl(const std::string& resource_uid, const std::string& principal,
                         int type, int permissions, const std::string& tenant = "") override {
        AclEntry entry;
        entry.resource_uid = resource_uid;
        entry.principal = principal;
        entry.type = type;
        entry.permissions = permissions;

        std::string key = tenant + "::" + resource_uid;
        acls_[key].push_back(entry);
        return Result<void>::ok();
    }

    Result<void> remove_acl(const std::string& resource_uid, const std::string& principal,
                            int type, int permissions, const std::string& tenant = "") override {
        std::string key = tenant + "::" + resource_uid;
        auto& resource_acls = acls_[key];
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
        std::string key = tenant + "::" + resource_uid;
        auto it = acls_.find(key);
        if (it != acls_.end()) {
            return Result<std::vector<AclEntry>>::ok(it->second);
        }
        return Result<std::vector<AclEntry>>::ok(std::vector<AclEntry>{});
    }

    Result<std::vector<AclEntry>> get_user_acls(const std::string& resource_uid,
                                                const std::string& principal,
                                                int type,
                                                const std::string& tenant = "") override {
        std::string key = tenant + "::" + resource_uid;
        auto it = acls_.find(key);
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

    // Role management - tenant-aware in-memory persistence (matches the real
    // Database now that Phase 4 wires the tables up).
    Result<void> create_role(const std::string& role, const std::string& tenant) override {
        if (role.empty()) return Result<void>::err("Role name cannot be empty");
        roles_[tenant].insert(role);
        return Result<void>::ok();
    }
    Result<void> delete_role(const std::string& role, const std::string& tenant) override {
        if (role.empty()) return Result<void>::err("Role name cannot be empty");
        roles_[tenant].erase(role);
        auto& ur = user_roles_[tenant];
        for (auto it = ur.begin(); it != ur.end(); ) {
            it = (it->second == role) ? ur.erase(it) : std::next(it);
        }
        return Result<void>::ok();
    }
    Result<void> assign_user_to_role(const std::string& user, const std::string& role, const std::string& tenant) override {
        if (user.empty() || role.empty()) return Result<void>::err("User and role names cannot be empty");
        user_roles_[tenant].insert({user, role});
        return Result<void>::ok();
    }
    Result<void> remove_user_from_role(const std::string& user, const std::string& role, const std::string& tenant) override {
        if (user.empty() || role.empty()) return Result<void>::err("User and role names cannot be empty");
        auto& ur = user_roles_[tenant];
        for (auto it = ur.begin(); it != ur.end(); ) {
            it = (it->first == user && it->second == role) ? ur.erase(it) : std::next(it);
        }
        return Result<void>::ok();
    }
    Result<std::vector<std::string>> get_roles_for_user(const std::string& user, const std::string& tenant) override {
        std::vector<std::string> out;
        auto it = user_roles_.find(tenant);
        if (it != user_roles_.end()) {
            for (const auto& p : it->second) {
                if (p.first == user) out.push_back(p.second);
            }
        }
        return Result<std::vector<std::string>>::ok(out);
    }
    Result<std::vector<std::string>> get_users_for_role(const std::string& role, const std::string& tenant) override {
        std::vector<std::string> out;
        auto it = user_roles_.find(tenant);
        if (it != user_roles_.end()) {
            for (const auto& p : it->second) {
                if (p.second == role) out.push_back(p.first);
            }
        }
        return Result<std::vector<std::string>>::ok(out);
    }
    Result<std::vector<std::string>> get_all_roles(const std::string& tenant) override {
        std::vector<std::string> out;
        auto it = roles_.find(tenant);
        if (it != roles_.end()) {
            out.assign(it->second.begin(), it->second.end());
        }
        return Result<std::vector<std::string>>::ok(out);
    }

    // Accessor for test verification
    size_t get_acl_count(const std::string& resource_uid, const std::string& tenant = "") {
        std::string key = tenant + "::" + resource_uid;
        auto it = acls_.find(key);
        return (it != acls_.end()) ? it->second.size() : 0;
    }

private:
    std::map<std::string, std::vector<AclEntry>> acls_;
    std::map<std::string, std::set<std::string>> roles_;
    std::map<std::string, std::set<std::pair<std::string, std::string>>> user_roles_;
};

// Helper constants
static const int PERM_READ = static_cast<int>(Permission::READ);
static const int PERM_WRITE = static_cast<int>(Permission::WRITE);
static const int PERM_DELETE = static_cast<int>(Permission::DELETE);
static const int PERM_LIST_DELETED = static_cast<int>(Permission::LIST_DELETED);
static const int PERM_UNDELETE = static_cast<int>(Permission::UNDELETE);
static const int PERM_VIEW_VERSIONS = static_cast<int>(Permission::VIEW_VERSIONS);
static const int PERM_RETRIEVE_BACK_VERSION = static_cast<int>(Permission::RETRIEVE_BACK_VERSION);
static const int PERM_RESTORE_TO_VERSION = static_cast<int>(Permission::RESTORE_TO_VERSION);
static const int PERM_EXECUTE = static_cast<int>(Permission::EXECUTE);
static const int PERM_ALL = PERM_READ | PERM_WRITE | PERM_DELETE | PERM_LIST_DELETED |
                            PERM_UNDELETE | PERM_VIEW_VERSIONS | PERM_RETRIEVE_BACK_VERSION |
                            PERM_RESTORE_TO_VERSION | PERM_EXECUTE;

// Helper: check if a specific permission bit is set
static bool has_perm(int effective, int perm) {
    return (effective & perm) == perm;
}

// =============================================================================
// 1. PERMISSION COMBINATION MODEL (UNION)
//    The system uses additive POSIX-ACL / NFSv4-style semantics: every matching
//    grant across USER, ROLE, GROUP, and OTHER contributes its bits to the
//    effective permission set. No principal type suppresses another.
//    See design_documents/acl_rbac_review_and_plan.md (Phase 2).
// =============================================================================

void test_priority_user_only() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-001";

    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ);

    auto result = acl.get_effective_permissions(res, "alice", {});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(has_perm(result.value, PERM_READ), "alice should have READ");
    TEST_ASSERT(!has_perm(result.value, PERM_WRITE), "alice should NOT have WRITE");
}

void test_priority_role_only() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-002";

    acl.grant_permission(res, "editor", PrincipalType::ROLE, PERM_READ | PERM_WRITE);

    // User with matching role gets role permissions
    auto result = acl.get_effective_permissions(res, "bob", {"editor"});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(has_perm(result.value, PERM_READ), "bob with editor role should have READ");
    TEST_ASSERT(has_perm(result.value, PERM_WRITE), "bob with editor role should have WRITE");

    // User without matching role gets nothing (no OTHER rule set)
    auto result2 = acl.get_effective_permissions(res, "bob", {});
    TEST_ASSERT(result2.success, "get_effective_permissions should succeed");
    TEST_ASSERT(!has_perm(result2.value, PERM_READ), "bob without role should NOT have READ");
}

void test_priority_group_only() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-003";

    acl.grant_permission(res, "developers", PrincipalType::GROUP, PERM_READ | PERM_WRITE);

    // GROUP rules apply to ALL users who don't have user-specific or role-specific ACLs
    // Note: the GROUP check doesn't match against the roles vector - it applies universally
    auto result = acl.get_effective_permissions(res, "charlie", {});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(has_perm(result.value, PERM_READ), "charlie should have READ from group");
    TEST_ASSERT(has_perm(result.value, PERM_WRITE), "charlie should have WRITE from group");
}

void test_priority_other_only() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-004";

    acl.grant_permission(res, "other", PrincipalType::OTHER, PERM_READ);

    // OTHER applies when no user, role, or group matched
    auto result = acl.get_effective_permissions(res, "anyone", {});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(has_perm(result.value, PERM_READ), "anyone should have READ from other");
    TEST_ASSERT(!has_perm(result.value, PERM_WRITE), "anyone should NOT have WRITE");
}

void test_priority_user_role_union() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-005";

    acl.grant_permission(res, "admin", PrincipalType::ROLE, PERM_ALL);
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ);

    // Union model: alice's USER grant of READ adds to admin's ROLE grant of ALL.
    auto result = acl.get_effective_permissions(res, "alice", {"admin"});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(has_perm(result.value, PERM_READ), "alice should have READ");
    TEST_ASSERT(has_perm(result.value, PERM_WRITE), "alice should have WRITE from admin role");
    TEST_ASSERT(has_perm(result.value, PERM_DELETE), "alice should have DELETE from admin role");
}

void test_priority_user_group_union() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-006";

    acl.grant_permission(res, "devs", PrincipalType::GROUP, PERM_READ | PERM_WRITE | PERM_DELETE);
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ);

    auto result = acl.get_effective_permissions(res, "alice", {});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(has_perm(result.value, PERM_READ), "alice should have READ");
    TEST_ASSERT(has_perm(result.value, PERM_WRITE), "alice should have WRITE from group");
    TEST_ASSERT(has_perm(result.value, PERM_DELETE), "alice should have DELETE from group");
}

void test_priority_user_other_union() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-007";

    acl.grant_permission(res, "other", PrincipalType::OTHER, PERM_READ | PERM_WRITE);
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ);

    auto result = acl.get_effective_permissions(res, "alice", {});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(has_perm(result.value, PERM_READ), "alice should have READ");
    TEST_ASSERT(has_perm(result.value, PERM_WRITE), "alice should have WRITE from OTHER");
}

void test_priority_role_group_union() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-008";

    acl.grant_permission(res, "devs", PrincipalType::GROUP, PERM_READ | PERM_WRITE | PERM_DELETE);
    acl.grant_permission(res, "viewer", PrincipalType::ROLE, PERM_READ);

    auto result = acl.get_effective_permissions(res, "bob", {"viewer"});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(has_perm(result.value, PERM_READ), "bob should have READ");
    TEST_ASSERT(has_perm(result.value, PERM_WRITE), "bob should have WRITE from group");
    TEST_ASSERT(has_perm(result.value, PERM_DELETE), "bob should have DELETE from group");
}

void test_priority_role_other_union() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-009";

    acl.grant_permission(res, "other", PrincipalType::OTHER, PERM_READ | PERM_WRITE);
    acl.grant_permission(res, "viewer", PrincipalType::ROLE, PERM_READ);

    auto result = acl.get_effective_permissions(res, "bob", {"viewer"});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(has_perm(result.value, PERM_READ), "bob should have READ");
    TEST_ASSERT(has_perm(result.value, PERM_WRITE), "bob should have WRITE from OTHER");
}

void test_priority_group_and_other_both_apply() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-010";

    acl.grant_permission(res, "other", PrincipalType::OTHER, PERM_WRITE);
    acl.grant_permission(res, "team", PrincipalType::GROUP, PERM_READ);

    // Union model: GROUP and OTHER grants accumulate.
    auto result = acl.get_effective_permissions(res, "charlie", {});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(has_perm(result.value, PERM_READ), "charlie should have READ from group");
    TEST_ASSERT(has_perm(result.value, PERM_WRITE), "charlie should have WRITE from other");
}

// =============================================================================
// 2. ROLE-BASED ACCESS CONTROL (RBAC)
// =============================================================================

void test_rbac_single_role() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-rbac-001";

    acl.grant_permission(res, "editor", PrincipalType::ROLE, PERM_READ | PERM_WRITE);

    auto result = acl.check_permission(res, "user1", {"editor"}, PERM_READ);
    TEST_ASSERT(result.success && result.value, "editor should have READ");

    result = acl.check_permission(res, "user1", {"editor"}, PERM_WRITE);
    TEST_ASSERT(result.success && result.value, "editor should have WRITE");

    result = acl.check_permission(res, "user1", {"editor"}, PERM_DELETE);
    TEST_ASSERT(result.success && !result.value, "editor should NOT have DELETE");
}

void test_rbac_multiple_roles_cumulative() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-rbac-002";

    // Two separate role grants
    acl.grant_permission(res, "reader", PrincipalType::ROLE, PERM_READ);
    acl.grant_permission(res, "writer", PrincipalType::ROLE, PERM_WRITE);

    // User with both roles should have cumulative permissions
    auto result = acl.get_effective_permissions(res, "user1", {"reader", "writer"});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(has_perm(result.value, PERM_READ), "user with reader+writer should have READ");
    TEST_ASSERT(has_perm(result.value, PERM_WRITE), "user with reader+writer should have WRITE");
    TEST_ASSERT(!has_perm(result.value, PERM_DELETE), "user with reader+writer should NOT have DELETE");
}

void test_rbac_role_hierarchy_users_contributors_admins() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-rbac-003";

    // Typical role hierarchy
    acl.grant_permission(res, "users", PrincipalType::ROLE, PERM_READ);
    acl.grant_permission(res, "contributors", PrincipalType::ROLE, PERM_READ | PERM_WRITE);
    acl.grant_permission(res, "administrators", PrincipalType::ROLE, PERM_ALL);

    // Regular user: READ only
    auto result = acl.get_effective_permissions(res, "viewer", {"users"});
    TEST_ASSERT(has_perm(result.value, PERM_READ), "users role should have READ");
    TEST_ASSERT(!has_perm(result.value, PERM_WRITE), "users role should NOT have WRITE");
    TEST_ASSERT(!has_perm(result.value, PERM_DELETE), "users role should NOT have DELETE");

    // Contributor: READ + WRITE
    result = acl.get_effective_permissions(res, "contrib", {"contributors"});
    TEST_ASSERT(has_perm(result.value, PERM_READ), "contributors should have READ");
    TEST_ASSERT(has_perm(result.value, PERM_WRITE), "contributors should have WRITE");
    TEST_ASSERT(!has_perm(result.value, PERM_DELETE), "contributors should NOT have DELETE");

    // Admin: everything
    result = acl.get_effective_permissions(res, "admin", {"administrators"});
    TEST_ASSERT(has_perm(result.value, PERM_READ), "administrators should have READ");
    TEST_ASSERT(has_perm(result.value, PERM_WRITE), "administrators should have WRITE");
    TEST_ASSERT(has_perm(result.value, PERM_DELETE), "administrators should have DELETE");
    TEST_ASSERT(has_perm(result.value, PERM_EXECUTE), "administrators should have EXECUTE");
    TEST_ASSERT(has_perm(result.value, PERM_LIST_DELETED), "administrators should have LIST_DELETED");
    TEST_ASSERT(has_perm(result.value, PERM_UNDELETE), "administrators should have UNDELETE");
    TEST_ASSERT(has_perm(result.value, PERM_VIEW_VERSIONS), "administrators should have VIEW_VERSIONS");
    TEST_ASSERT(has_perm(result.value, PERM_RETRIEVE_BACK_VERSION), "administrators should have RETRIEVE_BACK_VERSION");
    TEST_ASSERT(has_perm(result.value, PERM_RESTORE_TO_VERSION), "administrators should have RESTORE_TO_VERSION");
}

void test_rbac_unmatched_role() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-rbac-004";

    acl.grant_permission(res, "editor", PrincipalType::ROLE, PERM_READ | PERM_WRITE);

    // User has a role not defined on this resource
    auto result = acl.get_effective_permissions(res, "user1", {"viewer"});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(result.value == 0, "unmatched role should yield no permissions");
}

void test_rbac_empty_roles_vector() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-rbac-005";

    acl.grant_permission(res, "editor", PrincipalType::ROLE, PERM_READ | PERM_WRITE);

    auto result = acl.get_effective_permissions(res, "user1", {});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(result.value == 0, "empty roles should yield no permissions (no other/group fallback)");
}

void test_role_manager_crud_round_trip() {
    auto db = std::make_shared<MockDatabase>();
    RoleManager rm(db);

    // create -> get_all_roles
    TEST_ASSERT(rm.create_role("admin").success, "create_role(admin) should succeed");
    TEST_ASSERT(rm.create_role("viewer").success, "create_role(viewer) should succeed");
    auto all = rm.get_all_roles();
    TEST_ASSERT(all.success && all.value.size() == 2, "get_all_roles should return 2 roles");

    // assign -> get_roles_for_user / get_users_for_role
    TEST_ASSERT(rm.assign_user_to_role("alice", "admin").success, "assign should succeed");
    TEST_ASSERT(rm.assign_user_to_role("alice", "viewer").success, "assign viewer should succeed");
    TEST_ASSERT(rm.assign_user_to_role("bob", "viewer").success, "assign bob/viewer should succeed");

    auto alice_roles = rm.get_roles_for_user("alice");
    TEST_ASSERT(alice_roles.success && alice_roles.value.size() == 2, "alice should have 2 roles");

    auto viewer_users = rm.get_users_for_role("viewer");
    TEST_ASSERT(viewer_users.success && viewer_users.value.size() == 2, "viewer should have 2 users");

    // remove single assignment
    TEST_ASSERT(rm.remove_user_from_role("alice", "viewer").success, "remove should succeed");
    alice_roles = rm.get_roles_for_user("alice");
    TEST_ASSERT(alice_roles.value.size() == 1 && alice_roles.value[0] == "admin",
                "alice should now only have admin");

    // delete_role cascades to user_roles
    TEST_ASSERT(rm.delete_role("viewer").success, "delete_role should succeed");
    auto bob_roles = rm.get_roles_for_user("bob");
    TEST_ASSERT(bob_roles.success && bob_roles.value.empty(),
                "deleting a role should clear its assignments");
}

void test_db_roles_union_with_request_roles_in_check_permission() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    RoleManager rm(db);
    std::string res = "shared-resource";

    // Resource is readable by anyone in the "editor" role.
    acl.grant_permission(res, "editor", PrincipalType::ROLE, PERM_READ | PERM_WRITE);

    // alice is assigned the editor role server-side, NOT passed in the request.
    rm.create_role("editor");
    rm.assign_user_to_role("alice", "editor");

    // Request roles are empty, but DB-stored role should be picked up.
    auto perms = acl.get_effective_permissions(res, "alice", {});
    TEST_ASSERT(perms.success, "get_effective_permissions should succeed");
    TEST_ASSERT(has_perm(perms.value, PERM_READ),
                "alice should get READ via DB-stored editor role even when request roles empty");
    TEST_ASSERT(has_perm(perms.value, PERM_WRITE),
                "alice should get WRITE via DB-stored editor role even when request roles empty");
}

void test_rbac_same_user_different_roles_different_resources() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);

    acl.grant_permission("project-a", "developer", PrincipalType::ROLE, PERM_READ | PERM_WRITE);
    acl.grant_permission("project-b", "viewer", PrincipalType::ROLE, PERM_READ);

    // alice is developer on project-a and viewer on project-b
    auto result_a = acl.get_effective_permissions("project-a", "alice", {"developer", "viewer"});
    TEST_ASSERT(has_perm(result_a.value, PERM_READ), "alice should have READ on project-a");
    TEST_ASSERT(has_perm(result_a.value, PERM_WRITE), "alice should have WRITE on project-a");

    auto result_b = acl.get_effective_permissions("project-b", "alice", {"developer", "viewer"});
    TEST_ASSERT(has_perm(result_b.value, PERM_READ), "alice should have READ on project-b");
    TEST_ASSERT(!has_perm(result_b.value, PERM_WRITE), "alice should NOT have WRITE on project-b");
}

// =============================================================================
// 3. CLAIM-BASED ACCESS CONTROL
//    Claims are defined in AuthenticationContext proto but NOT implemented
//    in the permission checking logic. These tests document this gap.
// =============================================================================

void test_claims_not_implemented() {
    // The AuthenticationContext proto message has:
    //   map<string, string> claims = 4;
    // But AclManager::check_permission() and calculate_effective_permissions()
    // do NOT accept or inspect claims.
    //
    // This test documents that claim-based (ABAC) access control is NOT
    // currently implemented. Permission decisions are based solely on:
    //   1. User identity (user parameter)
    //   2. Role memberships (roles parameter)
    //   3. Group ACLs
    //   4. OTHER ACLs
    //
    // To implement claims, the check_permission() signature would need to
    // accept a claims map and the calculate_effective_permissions() algorithm
    // would need to evaluate claim predicates.

    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-claims-001";

    // Grant permissions - only user/role/group/other supported
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ);

    // check_permission does not accept claims - only user + roles
    auto result = acl.check_permission(res, "alice", {}, PERM_READ);
    TEST_ASSERT(result.success && result.value, "basic check works without claims");

    // Documenting: there is no way to grant/deny based on claims like
    // "department=engineering" or "clearance_level=secret"
    TEST_ASSERT(true, "Claims-based access control is not yet implemented");
}

// =============================================================================
// 4. ALL PERMISSION TYPES - individual and combined
// =============================================================================

void test_all_individual_permissions() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);

    struct PermTest {
        const char* name;
        int perm;
    };

    PermTest perms[] = {
        {"READ", PERM_READ},
        {"WRITE", PERM_WRITE},
        {"DELETE", PERM_DELETE},
        {"LIST_DELETED", PERM_LIST_DELETED},
        {"UNDELETE", PERM_UNDELETE},
        {"VIEW_VERSIONS", PERM_VIEW_VERSIONS},
        {"RETRIEVE_BACK_VERSION", PERM_RETRIEVE_BACK_VERSION},
        {"RESTORE_TO_VERSION", PERM_RESTORE_TO_VERSION},
        {"EXECUTE", PERM_EXECUTE},
    };

    for (const auto& p : perms) {
        std::string res = std::string("res-perm-") + p.name;
        acl.grant_permission(res, "user", PrincipalType::USER, p.perm);

        auto result = acl.check_permission(res, "user", {}, p.perm);
        TEST_ASSERT(result.success && result.value,
                    std::string("user should have ") + p.name + " permission");

        // Verify user does NOT have other permissions
        for (const auto& other : perms) {
            if (other.perm != p.perm) {
                auto neg = acl.check_permission(res, "user", {}, other.perm);
                TEST_ASSERT(neg.success && !neg.value,
                            std::string("user should NOT have ") + other.name +
                            " when only " + p.name + " granted");
            }
        }
    }
}

void test_permission_bitmask_combination() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-bitmask-001";

    // Grant READ|WRITE|DELETE as a single bitmask
    acl.grant_permission(res, "editor", PrincipalType::ROLE, PERM_READ | PERM_WRITE | PERM_DELETE);

    auto result = acl.get_effective_permissions(res, "user", {"editor"});
    TEST_ASSERT(has_perm(result.value, PERM_READ), "should have READ");
    TEST_ASSERT(has_perm(result.value, PERM_WRITE), "should have WRITE");
    TEST_ASSERT(has_perm(result.value, PERM_DELETE), "should have DELETE");
    TEST_ASSERT(!has_perm(result.value, PERM_EXECUTE), "should NOT have EXECUTE");
    TEST_ASSERT(!has_perm(result.value, PERM_LIST_DELETED), "should NOT have LIST_DELETED");
}

void test_version_specific_permissions_via_roles() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-version-perms";

    // Version-viewer role: can view and retrieve but not restore
    int version_view = PERM_VIEW_VERSIONS | PERM_RETRIEVE_BACK_VERSION;
    acl.grant_permission(res, "version_viewer", PrincipalType::ROLE, version_view);

    // Version-admin role: can also restore
    int version_admin = version_view | PERM_RESTORE_TO_VERSION;
    acl.grant_permission(res, "version_admin", PrincipalType::ROLE, version_admin);

    // Viewer can view but not restore
    auto result = acl.get_effective_permissions(res, "user1", {"version_viewer"});
    TEST_ASSERT(has_perm(result.value, PERM_VIEW_VERSIONS), "viewer should see versions");
    TEST_ASSERT(has_perm(result.value, PERM_RETRIEVE_BACK_VERSION), "viewer should retrieve versions");
    TEST_ASSERT(!has_perm(result.value, PERM_RESTORE_TO_VERSION), "viewer should NOT restore versions");

    // Admin can restore
    result = acl.get_effective_permissions(res, "user2", {"version_admin"});
    TEST_ASSERT(has_perm(result.value, PERM_VIEW_VERSIONS), "admin should see versions");
    TEST_ASSERT(has_perm(result.value, PERM_RETRIEVE_BACK_VERSION), "admin should retrieve versions");
    TEST_ASSERT(has_perm(result.value, PERM_RESTORE_TO_VERSION), "admin should restore versions");
}

void test_deleted_file_permissions_via_roles() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-deleted-perms";

    // Auditor: can list deleted items but not undelete
    acl.grant_permission(res, "auditor", PrincipalType::ROLE, PERM_READ | PERM_LIST_DELETED);

    // Recovery-admin: can list deleted and undelete
    acl.grant_permission(res, "recovery_admin", PrincipalType::ROLE,
                         PERM_READ | PERM_LIST_DELETED | PERM_UNDELETE);

    auto result = acl.get_effective_permissions(res, "user1", {"auditor"});
    TEST_ASSERT(has_perm(result.value, PERM_LIST_DELETED), "auditor should list deleted");
    TEST_ASSERT(!has_perm(result.value, PERM_UNDELETE), "auditor should NOT undelete");

    result = acl.get_effective_permissions(res, "user2", {"recovery_admin"});
    TEST_ASSERT(has_perm(result.value, PERM_LIST_DELETED), "recovery_admin should list deleted");
    TEST_ASSERT(has_perm(result.value, PERM_UNDELETE), "recovery_admin should undelete");
}

// =============================================================================
// 5. check_permission() - verifying required permissions bitmask
// =============================================================================

void test_check_permission_requires_all_bits() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-check-001";

    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ);

    // Check for READ alone: should pass
    auto result = acl.check_permission(res, "alice", {}, PERM_READ);
    TEST_ASSERT(result.success && result.value, "alice should have READ");

    // Check for READ|WRITE: should fail (missing WRITE)
    result = acl.check_permission(res, "alice", {}, PERM_READ | PERM_WRITE);
    TEST_ASSERT(result.success && !result.value, "alice should NOT have READ|WRITE");
}

void test_check_permission_with_superset() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-check-002";

    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ | PERM_WRITE | PERM_DELETE);

    // Check for subset of granted permissions
    auto result = acl.check_permission(res, "alice", {}, PERM_READ);
    TEST_ASSERT(result.success && result.value, "alice with R|W|D should pass READ check");

    result = acl.check_permission(res, "alice", {}, PERM_WRITE);
    TEST_ASSERT(result.success && result.value, "alice with R|W|D should pass WRITE check");

    result = acl.check_permission(res, "alice", {}, PERM_READ | PERM_WRITE);
    TEST_ASSERT(result.success && result.value, "alice with R|W|D should pass READ|WRITE check");
}

// =============================================================================
// 6. GRANT AND REVOKE BEHAVIOR
// =============================================================================

void test_grant_creates_separate_entries() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-grant-001";

    // Each grant creates a separate ACL entry
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ);
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_WRITE);

    auto acls = acl.get_acls_for_resource(res);
    TEST_ASSERT(acls.success, "get_acls should succeed");
    TEST_ASSERT(acls.value.size() == 2, "should have 2 separate ACL entries");

    // Effective permissions should be the OR of both
    auto result = acl.get_effective_permissions(res, "alice", {});
    TEST_ASSERT(has_perm(result.value, PERM_READ), "alice should have READ");
    TEST_ASSERT(has_perm(result.value, PERM_WRITE), "alice should have WRITE");
}

void test_revoke_clears_only_requested_bits() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-revoke-001";

    // Grant READ|WRITE in a single call (matches how the DB merges on conflict).
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ | PERM_WRITE);

    // Revoke only the READ bit — WRITE must remain.
    acl.revoke_permission(res, "alice", PrincipalType::USER, PERM_READ);

    auto result = acl.get_effective_permissions(res, "alice", {});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(!has_perm(result.value, PERM_READ), "alice should NOT have READ after partial revoke");
    TEST_ASSERT(has_perm(result.value, PERM_WRITE), "alice should still have WRITE after partial revoke");
}

void test_revoke_all_bits_deletes_row() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-revoke-001b";

    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ | PERM_WRITE);

    // Pass -1 (all bits) to fully revoke the principal's row.
    acl.revoke_permission(res, "alice", PrincipalType::USER, -1);

    auto acls = acl.get_acls_for_resource(res);
    TEST_ASSERT(acls.success, "get_acls_for_resource should succeed");
    TEST_ASSERT(acls.value.empty(), "row should be deleted when all bits revoked");
}

void test_revoke_only_affects_matching_type() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-revoke-002";

    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ);
    acl.grant_permission(res, "alice", PrincipalType::ROLE, PERM_WRITE);

    // Revoke USER type only
    acl.revoke_permission(res, "alice", PrincipalType::USER, PERM_READ);

    // ROLE entry should remain
    auto acls = acl.get_acls_for_resource(res);
    TEST_ASSERT(acls.value.size() == 1, "should have 1 remaining ACL entry (role)");
    TEST_ASSERT(acls.value[0].type == PrincipalType::ROLE, "remaining entry should be ROLE type");
}

void test_revoke_nonexistent_permission() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-revoke-003";

    // Revoking when nothing was granted should succeed (no-op)
    auto result = acl.revoke_permission(res, "alice", PrincipalType::USER, PERM_READ);
    TEST_ASSERT(result.success, "revoking nonexistent permission should succeed");
}

// =============================================================================
// 7. ACL INHERITANCE
// =============================================================================

void test_inherit_acls_copies_parent_to_child() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string parent = "dir-parent";
    std::string child = "dir-child";

    acl.grant_permission(parent, "alice", PrincipalType::USER, PERM_READ | PERM_WRITE);
    acl.grant_permission(parent, "editor", PrincipalType::ROLE, PERM_READ);
    acl.grant_permission(parent, "other", PrincipalType::OTHER, PERM_READ);

    auto result = acl.inherit_acls(parent, child);
    TEST_ASSERT(result.success, "inherit_acls should succeed");

    // Child should have same ACLs as parent
    auto child_acls = acl.get_acls_for_resource(child);
    TEST_ASSERT(child_acls.value.size() == 3, "child should have 3 ACL entries from parent");

    // Verify alice has READ|WRITE on child
    auto child_perms = acl.get_effective_permissions(child, "alice", {});
    TEST_ASSERT(has_perm(child_perms.value, PERM_READ), "alice should have READ on child");
    TEST_ASSERT(has_perm(child_perms.value, PERM_WRITE), "alice should have WRITE on child");
}

void test_inherit_acls_is_one_time_copy() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string parent = "dir-parent-2";
    std::string child = "dir-child-2";

    acl.grant_permission(parent, "alice", PrincipalType::USER, PERM_READ);
    acl.inherit_acls(parent, child);

    // Add more permissions to parent AFTER inheritance
    acl.grant_permission(parent, "alice", PrincipalType::USER, PERM_WRITE);

    // Child should NOT get the new permission
    auto child_perms = acl.get_effective_permissions(child, "alice", {});
    TEST_ASSERT(has_perm(child_perms.value, PERM_READ), "child should have READ (inherited)");
    TEST_ASSERT(!has_perm(child_perms.value, PERM_WRITE), "child should NOT have WRITE (added after inherit)");
}

void test_inherit_from_empty_parent() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string parent = "empty-parent";
    std::string child = "child-of-empty";

    auto result = acl.inherit_acls(parent, child);
    TEST_ASSERT(result.success, "inheriting from empty parent should succeed");

    auto child_acls = acl.get_acls_for_resource(child);
    TEST_ASSERT(child_acls.value.empty(), "child should have no ACLs from empty parent");
}

// =============================================================================
// 8. DEFAULT ACLs
// =============================================================================

void test_default_acls_creator_gets_rwe() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "new-resource";

    auto result = acl.apply_default_acls(res, "creator_user");
    TEST_ASSERT(result.success, "apply_default_acls should succeed");

    auto perms = acl.get_effective_permissions(res, "creator_user", {});
    TEST_ASSERT(has_perm(perms.value, PERM_READ), "creator should have READ");
    TEST_ASSERT(has_perm(perms.value, PERM_WRITE), "creator should have WRITE");
    TEST_ASSERT(has_perm(perms.value, PERM_EXECUTE), "creator should have EXECUTE");
    TEST_ASSERT(!has_perm(perms.value, PERM_DELETE), "creator should NOT have DELETE by default");
}

void test_default_acls_other_gets_read_when_world_readable() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "new-resource-2";

    // World-readable default is opt-in.
    acl.set_default_world_readable(true);
    acl.apply_default_acls(res, "creator_user");

    auto perms = acl.get_effective_permissions(res, "someone_else", {});
    TEST_ASSERT(has_perm(perms.value, PERM_READ), "others should have READ when world-readable is enabled");
    TEST_ASSERT(!has_perm(perms.value, PERM_WRITE), "others should NOT have WRITE");
}

void test_default_acls_private_by_default() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "new-resource-private";

    // Default: world-readable is OFF.
    acl.apply_default_acls(res, "creator_user");

    auto perms = acl.get_effective_permissions(res, "someone_else", {});
    TEST_ASSERT(!has_perm(perms.value, PERM_READ), "non-creator should have NO permissions by default");
    TEST_ASSERT(perms.value == 0, "non-creator should have zero permissions (private-by-default)");
}

void test_default_acls_creator_has_full_user_bits() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "new-resource-3";

    acl.apply_default_acls(res, "creator_user");

    auto perms = acl.get_effective_permissions(res, "creator_user", {});
    TEST_ASSERT(has_perm(perms.value, PERM_WRITE), "creator should have WRITE from USER ACL");
    TEST_ASSERT(has_perm(perms.value, PERM_EXECUTE), "creator should have EXECUTE from USER ACL");
}

// Regression: the gRPC enforcement layer historically passed POSIX-octal
// literals (0400, 0200) for READ/WRITE, but Permission::READ is 0x400 and
// Permission::WRITE is 0x200. Octal 0400 == 0x100 == DELETE bit, and octal
// 0200 == 0x080 == LIST_DELETED bit, so every gRPC check silently asked for
// the wrong permission. This test pins the constant values and proves the
// creator's default ACL satisfies the correct enum check but NOT the legacy
// octal check, so any re-introduction of octal literals fails fast.
void test_grpc_bitmask_regression_octal_vs_hex() {
    // Pin the numeric values: if the enum changes, this needs revisiting.
    TEST_ASSERT(static_cast<int>(Permission::READ)  == 0x400, "Permission::READ must be 0x400");
    TEST_ASSERT(static_cast<int>(Permission::WRITE) == 0x200, "Permission::WRITE must be 0x200");

    // 0400 octal = 256 = 0x100 = DELETE; 0200 octal = 128 = 0x080 = LIST_DELETED.
    const int legacy_octal_read  = 0400;
    const int legacy_octal_write = 0200;
    TEST_ASSERT(legacy_octal_read  == static_cast<int>(Permission::DELETE),
                "octal 0400 collides with DELETE bit");
    TEST_ASSERT(legacy_octal_write == static_cast<int>(Permission::LIST_DELETED),
                "octal 0200 collides with LIST_DELETED bit");

    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "regression-resource";
    acl.apply_default_acls(res, "alice");

    // Correct enum constants: creator can READ and WRITE their own resource.
    auto correct_read  = acl.check_permission(res, "alice", {}, static_cast<int>(Permission::READ));
    auto correct_write = acl.check_permission(res, "alice", {}, static_cast<int>(Permission::WRITE));
    TEST_ASSERT(correct_read.success  && correct_read.value,
                "creator must satisfy READ check using Permission::READ");
    TEST_ASSERT(correct_write.success && correct_write.value,
                "creator must satisfy WRITE check using Permission::WRITE");

    // Legacy octal constants: creator's default ACL does NOT grant DELETE or
    // LIST_DELETED, so the octal-bit check would have failed. This documents
    // the bug class — if these flip to true, someone re-introduced octal.
    auto legacy_read  = acl.check_permission(res, "alice", {}, legacy_octal_read);
    auto legacy_write = acl.check_permission(res, "alice", {}, legacy_octal_write);
    TEST_ASSERT(legacy_read.success  && !legacy_read.value,
                "legacy octal 0400 must NOT satisfy creator's default ACL");
    TEST_ASSERT(legacy_write.success && !legacy_write.value,
                "legacy octal 0200 must NOT satisfy creator's default ACL");
}

// =============================================================================
// 9. TENANT ISOLATION
// =============================================================================

void test_tenant_acls_are_isolated() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "shared-resource-uid";

    // Grant READ on tenant-a
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ, "tenant-a");

    // Grant WRITE on tenant-b
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_WRITE, "tenant-b");

    // Check tenant-a: should have READ only
    auto perms_a = acl.get_effective_permissions(res, "alice", {}, "tenant-a");
    TEST_ASSERT(perms_a.success, "tenant-a check should succeed");
    TEST_ASSERT(has_perm(perms_a.value, PERM_READ), "alice should have READ on tenant-a");
    TEST_ASSERT(!has_perm(perms_a.value, PERM_WRITE), "alice should NOT have WRITE on tenant-a");

    // Check tenant-b: should have WRITE only
    auto perms_b = acl.get_effective_permissions(res, "alice", {}, "tenant-b");
    TEST_ASSERT(perms_b.success, "tenant-b check should succeed");
    TEST_ASSERT(!has_perm(perms_b.value, PERM_READ), "alice should NOT have READ on tenant-b");
    TEST_ASSERT(has_perm(perms_b.value, PERM_WRITE), "alice should have WRITE on tenant-b");
}

void test_tenant_role_isolation() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "multi-tenant-res";

    acl.grant_permission(res, "editor", PrincipalType::ROLE, PERM_READ | PERM_WRITE, "org-1");
    acl.grant_permission(res, "editor", PrincipalType::ROLE, PERM_READ, "org-2");

    auto perms_1 = acl.get_effective_permissions(res, "user1", {"editor"}, "org-1");
    TEST_ASSERT(has_perm(perms_1.value, PERM_WRITE), "editor in org-1 should have WRITE");

    auto perms_2 = acl.get_effective_permissions(res, "user1", {"editor"}, "org-2");
    TEST_ASSERT(!has_perm(perms_2.value, PERM_WRITE), "editor in org-2 should NOT have WRITE");
}

void test_tenant_revoke_isolation() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "tenant-revoke-res";

    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ, "t1");
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ, "t2");

    // Revoke on t1 should not affect t2
    acl.revoke_permission(res, "alice", PrincipalType::USER, PERM_READ, "t1");

    auto perms_t1 = acl.get_effective_permissions(res, "alice", {}, "t1");
    TEST_ASSERT(!has_perm(perms_t1.value, PERM_READ), "alice should NOT have READ on t1 after revoke");

    auto perms_t2 = acl.get_effective_permissions(res, "alice", {}, "t2");
    TEST_ASSERT(has_perm(perms_t2.value, PERM_READ), "alice should still have READ on t2");
}

// =============================================================================
// 10. EDGE CASES
// =============================================================================

void test_no_acls_on_resource() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "no-acls-resource";

    auto result = acl.get_effective_permissions(res, "anyone", {"any_role"});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(result.value == 0, "no ACLs should yield zero permissions");

    auto check = acl.check_permission(res, "anyone", {}, PERM_READ);
    TEST_ASSERT(check.success && !check.value, "check_permission should deny when no ACLs");
}

void test_different_resources_independent_acls() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);

    acl.grant_permission("resource-a", "alice", PrincipalType::USER, PERM_READ);
    acl.grant_permission("resource-b", "alice", PrincipalType::USER, PERM_WRITE);

    auto perms_a = acl.get_effective_permissions("resource-a", "alice", {});
    TEST_ASSERT(has_perm(perms_a.value, PERM_READ), "alice should have READ on resource-a");
    TEST_ASSERT(!has_perm(perms_a.value, PERM_WRITE), "alice should NOT have WRITE on resource-a");

    auto perms_b = acl.get_effective_permissions("resource-b", "alice", {});
    TEST_ASSERT(!has_perm(perms_b.value, PERM_READ), "alice should NOT have READ on resource-b");
    TEST_ASSERT(has_perm(perms_b.value, PERM_WRITE), "alice should have WRITE on resource-b");
}

void test_different_users_same_resource() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "shared-resource";

    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ | PERM_WRITE);
    acl.grant_permission(res, "bob", PrincipalType::USER, PERM_READ);

    auto alice_perms = acl.get_effective_permissions(res, "alice", {});
    TEST_ASSERT(has_perm(alice_perms.value, PERM_WRITE), "alice should have WRITE");

    auto bob_perms = acl.get_effective_permissions(res, "bob", {});
    TEST_ASSERT(!has_perm(bob_perms.value, PERM_WRITE), "bob should NOT have WRITE");
}

void test_multiple_group_acls_cumulative() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-multi-group";

    // Multiple GROUP ACLs accumulate (no principal matching for groups)
    acl.grant_permission(res, "group-a", PrincipalType::GROUP, PERM_READ);
    acl.grant_permission(res, "group-b", PrincipalType::GROUP, PERM_WRITE);

    // User with no user/role ACLs falls through to group
    auto perms = acl.get_effective_permissions(res, "user1", {});
    TEST_ASSERT(has_perm(perms.value, PERM_READ), "should have READ from group-a");
    TEST_ASSERT(has_perm(perms.value, PERM_WRITE), "should have WRITE from group-b");
}

void test_multiple_other_acls_cumulative() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-multi-other";

    acl.grant_permission(res, "other", PrincipalType::OTHER, PERM_READ);
    acl.grant_permission(res, "everyone", PrincipalType::OTHER, PERM_EXECUTE);

    auto perms = acl.get_effective_permissions(res, "anyone", {});
    TEST_ASSERT(has_perm(perms.value, PERM_READ), "should have READ from other");
    TEST_ASSERT(has_perm(perms.value, PERM_EXECUTE), "should have EXECUTE from other");
}

void test_grant_zero_permissions() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-zero-perms";

    auto result = acl.grant_permission(res, "alice", PrincipalType::USER, 0);
    TEST_ASSERT(result.success, "granting zero permissions should succeed");

    // A zero-bits USER grant contributes nothing under the union model — it
    // does NOT suppress role grants. Today there's no deny semantic, so the
    // only way to revoke is to remove the role assignment or call
    // revoke_permission(). See plan §6.1 for future deny rules.
    acl.grant_permission(res, "editor", PrincipalType::ROLE, PERM_READ | PERM_WRITE);

    auto perms = acl.get_effective_permissions(res, "alice", {"editor"});
    TEST_ASSERT(has_perm(perms.value, PERM_READ), "zero-perm user ACL does not suppress role READ");
    TEST_ASSERT(has_perm(perms.value, PERM_WRITE), "zero-perm user ACL does not suppress role WRITE");
}

// =============================================================================
// 11. ROLE MANAGER STUB VALIDATION
// =============================================================================

void test_role_manager_empty_validation() {
    auto db = std::make_shared<MockDatabase>();
    RoleManager rm(db);

    auto result = rm.create_role("");
    TEST_ASSERT(!result.success, "creating role with empty name should fail");

    result = rm.delete_role("");
    TEST_ASSERT(!result.success, "deleting role with empty name should fail");

    result = rm.assign_user_to_role("", "admin");
    TEST_ASSERT(!result.success, "assigning empty user should fail");

    result = rm.assign_user_to_role("alice", "");
    TEST_ASSERT(!result.success, "assigning empty role should fail");

    result = rm.remove_user_from_role("", "admin");
    TEST_ASSERT(!result.success, "removing empty user should fail");

    result = rm.remove_user_from_role("alice", "");
    TEST_ASSERT(!result.success, "removing empty role should fail");
}

void test_role_manager_valid_operations_persist() {
    auto db = std::make_shared<MockDatabase>();
    RoleManager rm(db);

    auto result = rm.create_role("admin");
    TEST_ASSERT(result.success, "create_role should succeed");

    result = rm.assign_user_to_role("alice", "admin");
    TEST_ASSERT(result.success, "assign_user_to_role should succeed");

    // Phase 4: assignments persist.
    auto roles = rm.get_roles_for_user("alice");
    TEST_ASSERT(roles.success && roles.value.size() == 1 && roles.value[0] == "admin",
                "get_roles_for_user should return [admin]");

    auto users = rm.get_users_for_role("admin");
    TEST_ASSERT(users.success && users.value.size() == 1 && users.value[0] == "alice",
                "get_users_for_role should return [alice]");

    result = rm.remove_user_from_role("alice", "admin");
    TEST_ASSERT(result.success, "remove_user_from_role should succeed");
    roles = rm.get_roles_for_user("alice");
    TEST_ASSERT(roles.success && roles.value.empty(), "alice should have no roles after removal");

    result = rm.delete_role("admin");
    TEST_ASSERT(result.success, "delete_role should succeed");
    auto all = rm.get_all_roles();
    TEST_ASSERT(all.success && all.value.empty(), "no roles should remain after delete");
}

// =============================================================================
// 12. COMPLEX REAL-WORLD SCENARIOS
// =============================================================================

void test_scenario_file_sharing_workflow() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    // Opt into world-readable defaults so the project dir is shared by default.
    acl.set_default_world_readable(true);
    std::string project_dir = "project-dir-uuid";
    std::string readme = "readme-file-uuid";

    // Setup: owner creates project dir with default ACLs
    acl.apply_default_acls(project_dir, "owner");
    acl.apply_default_acls(readme, "owner");

    // Grant team role READ|WRITE on project dir
    acl.grant_permission(project_dir, "team-member", PrincipalType::ROLE, PERM_READ | PERM_WRITE);

    // Grant external reviewer READ only
    acl.grant_permission(readme, "reviewer", PrincipalType::ROLE, PERM_READ);

    // Owner has full access (READ|WRITE|EXECUTE from defaults)
    auto owner_perms = acl.get_effective_permissions(project_dir, "owner", {"team-member"});
    TEST_ASSERT(has_perm(owner_perms.value, PERM_READ), "owner should have READ");
    TEST_ASSERT(has_perm(owner_perms.value, PERM_WRITE), "owner should have WRITE");
    // Note: owner has USER ACL so role is ignored - still has WRITE from USER ACL

    // Team member can read and write project dir (via role)
    auto team_perms = acl.get_effective_permissions(project_dir, "alice", {"team-member"});
    TEST_ASSERT(has_perm(team_perms.value, PERM_READ), "team member should READ project dir");
    TEST_ASSERT(has_perm(team_perms.value, PERM_WRITE), "team member should WRITE project dir");

    // Reviewer can only read the readme (via role)
    auto reviewer_perms = acl.get_effective_permissions(readme, "external_user", {"reviewer"});
    TEST_ASSERT(has_perm(reviewer_perms.value, PERM_READ), "reviewer should READ readme");
    TEST_ASSERT(!has_perm(reviewer_perms.value, PERM_WRITE), "reviewer should NOT WRITE readme");

    // Reviewer has no access to project dir (role not granted there)
    // Falls through to OTHER which was set by apply_default_acls (READ)
    auto reviewer_dir_perms = acl.get_effective_permissions(project_dir, "external_user", {"reviewer"});
    // No "reviewer" role ACL on project_dir, and no user ACL → falls to OTHER (READ from defaults)
    TEST_ASSERT(has_perm(reviewer_dir_perms.value, PERM_READ), "reviewer should have OTHER READ on project dir");
    TEST_ASSERT(!has_perm(reviewer_dir_perms.value, PERM_WRITE), "reviewer should NOT WRITE project dir");
}

void test_scenario_multi_role_user() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "sensitive-doc";

    // Finance team can READ
    acl.grant_permission(res, "finance", PrincipalType::ROLE, PERM_READ);
    // Managers can WRITE
    acl.grant_permission(res, "manager", PrincipalType::ROLE, PERM_WRITE);
    // Auditors can see deleted and versions
    acl.grant_permission(res, "auditor", PrincipalType::ROLE, PERM_LIST_DELETED | PERM_VIEW_VERSIONS);

    // User with all three roles gets cumulative permissions
    auto perms = acl.get_effective_permissions(res, "finance_manager_auditor",
                                               {"finance", "manager", "auditor"});
    TEST_ASSERT(has_perm(perms.value, PERM_READ), "multi-role user should have READ");
    TEST_ASSERT(has_perm(perms.value, PERM_WRITE), "multi-role user should have WRITE");
    TEST_ASSERT(has_perm(perms.value, PERM_LIST_DELETED), "multi-role user should have LIST_DELETED");
    TEST_ASSERT(has_perm(perms.value, PERM_VIEW_VERSIONS), "multi-role user should have VIEW_VERSIONS");
    TEST_ASSERT(!has_perm(perms.value, PERM_DELETE), "multi-role user should NOT have DELETE");
}

void test_scenario_role_grants_accumulate_with_user() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "restricted-resource";

    // Admin role has full permissions
    acl.grant_permission(res, "admin", PrincipalType::ROLE, PERM_ALL);

    // Adding a USER grant cannot subtract — union model.
    // The only way to restrict alice's effective permissions is to remove her
    // admin role assignment (or, in the future, use deny rules; see plan §6.1).
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ);

    auto perms = acl.get_effective_permissions(res, "alice", {"admin"});
    TEST_ASSERT(has_perm(perms.value, PERM_READ), "alice should have READ");
    TEST_ASSERT(has_perm(perms.value, PERM_WRITE), "alice should have WRITE via admin role");
    TEST_ASSERT(has_perm(perms.value, PERM_DELETE), "alice should have DELETE via admin role");
}

void test_scenario_access_after_role_removal() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "project-resource";

    acl.grant_permission(res, "developer", PrincipalType::ROLE, PERM_READ | PERM_WRITE);
    acl.grant_permission(res, "other", PrincipalType::OTHER, PERM_READ);

    // User with developer role can read+write
    auto perms = acl.get_effective_permissions(res, "bob", {"developer"});
    TEST_ASSERT(has_perm(perms.value, PERM_WRITE), "bob with developer role should WRITE");

    // When bob loses developer role (external auth change), permissions drop
    // Simulated by passing empty roles - falls through to OTHER
    perms = acl.get_effective_permissions(res, "bob", {});
    TEST_ASSERT(has_perm(perms.value, PERM_READ), "bob without role should still READ (from OTHER)");
    TEST_ASSERT(!has_perm(perms.value, PERM_WRITE), "bob without role should NOT WRITE");
}

// =============================================================================
// Main test runner
// =============================================================================

int main() {
    std::cout << "=== Comprehensive ACL and RBAC Test Suite ===\n\n";

    std::cout << "--- 1. Permission Priority System (USER > ROLE > GROUP > OTHER) ---\n";
    run_test("user-only permissions", test_priority_user_only);
    run_test("role-only permissions", test_priority_role_only);
    run_test("group-only permissions", test_priority_group_only);
    run_test("other-only permissions", test_priority_other_only);
    run_test("user + role grants union", test_priority_user_role_union);
    run_test("user + group grants union", test_priority_user_group_union);
    run_test("user + other grants union", test_priority_user_other_union);
    run_test("role + group grants union", test_priority_role_group_union);
    run_test("role + other grants union", test_priority_role_other_union);
    run_test("group and other both apply", test_priority_group_and_other_both_apply);

    std::cout << "\n--- 2. Role-Based Access Control (RBAC) ---\n";
    run_test("single role grant and check", test_rbac_single_role);
    run_test("multiple roles cumulative permissions", test_rbac_multiple_roles_cumulative);
    run_test("role hierarchy: users/contributors/admins", test_rbac_role_hierarchy_users_contributors_admins);
    run_test("unmatched role yields no permissions", test_rbac_unmatched_role);
    run_test("empty roles vector yields no permissions", test_rbac_empty_roles_vector);
    run_test("role manager CRUD round-trip", test_role_manager_crud_round_trip);
    run_test("DB-stored roles union with request roles in check_permission", test_db_roles_union_with_request_roles_in_check_permission);
    run_test("same user different roles different resources", test_rbac_same_user_different_roles_different_resources);

    std::cout << "\n--- 3. Claim-Based Access Control (ABAC) ---\n";
    run_test("claims not implemented (documented gap)", test_claims_not_implemented);

    std::cout << "\n--- 4. All Permission Types ---\n";
    run_test("all individual permissions", test_all_individual_permissions);
    run_test("permission bitmask combination", test_permission_bitmask_combination);
    run_test("version-specific permissions via roles", test_version_specific_permissions_via_roles);
    run_test("deleted file permissions via roles", test_deleted_file_permissions_via_roles);

    std::cout << "\n--- 5. check_permission() Semantics ---\n";
    run_test("check_permission requires all bits", test_check_permission_requires_all_bits);
    run_test("check_permission with superset", test_check_permission_with_superset);

    std::cout << "\n--- 6. Grant and Revoke Behavior ---\n";
    run_test("grant creates separate entries", test_grant_creates_separate_entries);
    run_test("revoke clears only the requested bits", test_revoke_clears_only_requested_bits);
    run_test("revoke -1 deletes the row entirely", test_revoke_all_bits_deletes_row);
    run_test("revoke only affects matching type", test_revoke_only_affects_matching_type);
    run_test("revoke nonexistent permission is no-op", test_revoke_nonexistent_permission);

    std::cout << "\n--- 7. ACL Inheritance ---\n";
    run_test("inherit copies parent ACLs to child", test_inherit_acls_copies_parent_to_child);
    run_test("inheritance is one-time copy", test_inherit_acls_is_one_time_copy);
    run_test("inherit from empty parent", test_inherit_from_empty_parent);

    std::cout << "\n--- 8. Default ACLs ---\n";
    run_test("default ACLs: creator gets R|W|X", test_default_acls_creator_gets_rwe);
    run_test("default ACLs: other gets READ when world-readable enabled", test_default_acls_other_gets_read_when_world_readable);
    run_test("default ACLs: private-by-default (world-readable OFF)", test_default_acls_private_by_default);
    run_test("default ACLs: creator has full user bits", test_default_acls_creator_has_full_user_bits);
    run_test("regression: gRPC bitmask octal vs hex", test_grpc_bitmask_regression_octal_vs_hex);

    std::cout << "\n--- 9. Tenant Isolation ---\n";
    run_test("tenant ACLs are isolated", test_tenant_acls_are_isolated);
    run_test("tenant role isolation", test_tenant_role_isolation);
    run_test("tenant revoke isolation", test_tenant_revoke_isolation);

    std::cout << "\n--- 10. Edge Cases ---\n";
    run_test("no ACLs on resource yields zero permissions", test_no_acls_on_resource);
    run_test("different resources have independent ACLs", test_different_resources_independent_acls);
    run_test("different users on same resource", test_different_users_same_resource);
    run_test("multiple group ACLs are cumulative", test_multiple_group_acls_cumulative);
    run_test("multiple other ACLs are cumulative", test_multiple_other_acls_cumulative);
    run_test("zero-permission user grant does not suppress role", test_grant_zero_permissions);

    std::cout << "\n--- 11. RoleManager Stub Validation ---\n";
    run_test("role manager rejects empty names", test_role_manager_empty_validation);
    run_test("role manager operations persist", test_role_manager_valid_operations_persist);

    std::cout << "\n--- 12. Real-World Scenarios ---\n";
    run_test("file sharing workflow", test_scenario_file_sharing_workflow);
    run_test("multi-role user with cumulative permissions", test_scenario_multi_role_user);
    run_test("role grants accumulate with user grants (union)", test_scenario_role_grants_accumulate_with_user);
    run_test("access changes when role is removed", test_scenario_access_after_role_removal);

    std::cout << "\n=== Results: " << tests_passed << "/" << tests_run << " passed";
    if (tests_failed > 0) {
        std::cout << ", " << tests_failed << " FAILED";
    }
    std::cout << " ===\n";

    return tests_failed > 0 ? 1 : 0;
}
