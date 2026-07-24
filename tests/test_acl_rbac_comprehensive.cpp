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
    Result<std::string> create_file_with_acls(const std::string& uid,
                                               const std::string&,
                                               const std::string&,
                                               const std::string&,
                                               FileType,
                                               const std::string&,
                                               int,
                                               const std::vector<AclGrant>& grants,
                                               const std::string& tenant = "") override {
        // Mock simulates atomicity by sequencing add_acl calls; tests that
        // care about transactional semantics should run against real Postgres.
        for (const auto& g : grants) {
            add_acl(uid, g.principal, g.type, g.permissions, tenant, g.performed_by, g.effect);
        }
        last_atomic_create_grants_ = grants;
        ++atomic_create_count_;
        return Result<std::string>::ok(uid);
    }
    Result<void> update_file_modified(const std::string&, const std::string&) override { return Result<void>::ok(); }
    Result<void> update_file_current_version(const std::string&, const std::string&, const std::string&) override { return Result<void>::ok(); }
    Result<bool> delete_file(const std::string&, const std::string&) override { return Result<bool>::ok(true); }
    Result<bool> undelete_file(const std::string&, const std::string&) override { return Result<bool>::ok(true); }
    Result<std::optional<FileInfo>> get_file_by_uid(const std::string& uid, const std::string& tenant) override {
        std::string key = tenant + "::" + uid;
        auto it = files_.find(key);
        if (it == files_.end()) return Result<std::optional<FileInfo>>::ok(std::nullopt);
        return Result<std::optional<FileInfo>>::ok(it->second);
    }
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
    Result<int64_t> insert_version(const std::string&, const std::string&, int64_t, const std::string&, const std::string&, const std::string&) override { return Result<int64_t>::ok(0); }
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
                         int type, int permissions,
                         const std::string& tenant = "",
                         const std::string& performed_by = "",
                         int effect = 0) override {
        std::string key = tenant + "::" + resource_uid;
        auto& resource_acls = acls_[key];
        // Upsert by (principal, type, effect). ALLOW and DENY coexist as
        // separate rows. Successive grants OR the bits — matches the real
        // DB's `SET permissions = acls.permissions | $4`.
        bool updated = false;
        for (auto& entry : resource_acls) {
            if (entry.principal == principal && entry.type == type && entry.effect == effect) {
                entry.permissions |= permissions;
                updated = true;
                break;
            }
        }
        if (!updated) {
            AclEntry entry;
            entry.resource_uid = resource_uid;
            entry.principal = principal;
            entry.type = type;
            entry.permissions = permissions;
            entry.effect = effect;
            resource_acls.push_back(entry);
        }
        std::string action = (effect == 1) ? "grant_deny" : "grant";
        audit_log_.push_back({resource_uid, principal, type, action, permissions, performed_by, tenant});
        return Result<void>::ok();
    }

    Result<void> remove_acl(const std::string& resource_uid, const std::string& principal,
                            int type, int permissions,
                            const std::string& tenant = "",
                            const std::string& performed_by = "",
                            int effect = 0) override {
        std::string key = tenant + "::" + resource_uid;
        auto& resource_acls = acls_[key];
        int permissions_after = 0;
        bool found = false;
        for (auto& entry : resource_acls) {
            if (entry.principal == principal && entry.type == type && entry.effect == effect) {
                entry.permissions &= ~permissions;
                permissions_after = entry.permissions;
                found = true;
            }
        }
        resource_acls.erase(
            std::remove_if(resource_acls.begin(), resource_acls.end(),
                          [&](const AclEntry& entry) {
                              return entry.principal == principal && entry.type == type
                                     && entry.effect == effect && entry.permissions == 0;
                          }),
            resource_acls.end());
        if (found) {
            std::string action = (effect == 1) ? "revoke_deny" : "revoke";
            audit_log_.push_back({resource_uid, principal, type, action, permissions_after, performed_by, tenant});
        }
        return Result<void>::ok();
    }

    Result<std::vector<AclEntry>> get_acls_for_resource(const std::string& resource_uid,
                                                        const std::string& tenant = "") override {
        ++get_acls_call_count_;
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

    // Catalog distinct CLAIM-type (type 4) principals; case-insensitive prefix.
    Result<std::vector<std::string>> list_claims(const std::string& prefix,
                                                 int limit,
                                                 const std::string& tenant = "") override {
        auto lower = [](std::string s) {
            for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32;
            return s;
        };
        std::string lp = lower(prefix);
        std::vector<std::string> claims;
        for (const auto& kv : acls_) {
            for (const auto& entry : kv.second) {
                if (entry.type != 4) continue;  // PrincipalType::CLAIM
                if (!lp.empty() && lower(entry.principal).compare(0, lp.size(), lp) != 0) continue;
                if (std::find(claims.begin(), claims.end(), entry.principal) == claims.end())
                    claims.push_back(entry.principal);
            }
        }
        std::sort(claims.begin(), claims.end());
        if (limit > 0 && (int)claims.size() > limit) claims.resize(limit);
        return Result<std::vector<std::string>>::ok(claims);
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

    // Register a file/dir with its parent so get_file_by_uid can drive the
    // AclManager's parent-chain traversal in tests. tenant defaults to "".
    void set_file(const std::string& uid, const std::string& parent_uid,
                  const std::string& tenant = "") {
        FileInfo fi;
        fi.uid = uid;
        fi.parent_uid = parent_uid;
        fi.type = FileType::DIRECTORY;
        files_[tenant + "::" + uid] = fi;
    }

    // Accessor for test verification
    size_t get_acl_count(const std::string& resource_uid, const std::string& tenant = "") {
        std::string key = tenant + "::" + resource_uid;
        auto it = acls_.find(key);
        return (it != acls_.end()) ? it->second.size() : 0;
    }

    struct AuditEvent {
        std::string resource_uid;
        std::string principal;
        int type;
        std::string action;       // "grant" | "revoke" | "grant_deny" | "revoke_deny"
        int permissions;          // for grants: granted bitmask; for revokes: bitmask after revoke
        std::string performed_by;
        std::string tenant;
    };
    const std::vector<AuditEvent>& audit_log() const { return audit_log_; }

    // Phase 6 §6.2: visibility into the atomic-create path.
    int atomic_create_count() const { return atomic_create_count_; }
    const std::vector<AclGrant>& last_atomic_create_grants() const { return last_atomic_create_grants_; }

    // Phase 6 §6.3: how many times the DB layer's get_acls_for_resource was
    // called. Used to verify the request-scoped cache really does coalesce.
    int get_acls_call_count() const { return get_acls_call_count_; }
    void reset_get_acls_call_count() { get_acls_call_count_ = 0; }

private:
    std::map<std::string, std::vector<AclEntry>> acls_;
    std::map<std::string, FileInfo> files_;   // key: tenant + "::" + uid
    std::map<std::string, std::set<std::string>> roles_;
    std::map<std::string, std::set<std::pair<std::string, std::string>>> user_roles_;
    std::vector<AuditEvent> audit_log_;
    int atomic_create_count_ = 0;
    std::vector<AclGrant> last_atomic_create_grants_;
    int get_acls_call_count_ = 0;
};

// Helper constants
static const int PERM_ACL_INHERIT = static_cast<int>(Permission::ACL_INHERIT);
static const int PERM_MANAGE_ACL = static_cast<int>(Permission::MANAGE_ACL);
static const int PERM_READ = static_cast<int>(Permission::READ);
static const int PERM_WRITE = static_cast<int>(Permission::WRITE);
static const int PERM_DELETE = static_cast<int>(Permission::DELETE);
static const int PERM_LIST_DELETED = static_cast<int>(Permission::LIST_DELETED);
static const int PERM_UNDELETE = static_cast<int>(Permission::UNDELETE);
static const int PERM_VIEW_VERSIONS = static_cast<int>(Permission::VIEW_VERSIONS);
static const int PERM_RETRIEVE_BACK_VERSION = static_cast<int>(Permission::RETRIEVE_BACK_VERSION);
static const int PERM_RESTORE_TO_VERSION = static_cast<int>(Permission::RESTORE_TO_VERSION);
static const int PERM_EXECUTE = static_cast<int>(Permission::EXECUTE);
static const int PERM_CULL_VERSIONS = static_cast<int>(Permission::CULL_VERSIONS);
static const int PERM_ALL = PERM_MANAGE_ACL | PERM_READ | PERM_WRITE | PERM_DELETE | PERM_LIST_DELETED |
                            PERM_UNDELETE | PERM_VIEW_VERSIONS | PERM_RETRIEVE_BACK_VERSION |
                            PERM_RESTORE_TO_VERSION | PERM_EXECUTE | PERM_CULL_VERSIONS;

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

    // User without matching role gets no role-granted bits. READ is present via
    // the read-by-default baseline; the role's WRITE must not leak to non-members.
    auto result2 = acl.get_effective_permissions(res, "bob", {});
    TEST_ASSERT(result2.success, "get_effective_permissions should succeed");
    TEST_ASSERT(has_perm(result2.value, PERM_READ), "bob has baseline READ (read-by-default)");
    TEST_ASSERT(!has_perm(result2.value, PERM_WRITE), "bob without role should NOT have role-granted WRITE");
}

void test_priority_group_only() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-003";

    // Roles ARE the group mechanism (LDAP groups resolve to role names), so a
    // "group" grant is a ROLE grant applied to users who hold that role.
    acl.grant_permission(res, "developers", PrincipalType::ROLE, PERM_READ | PERM_WRITE);

    auto result = acl.get_effective_permissions(res, "charlie", {"developers"});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(has_perm(result.value, PERM_READ), "charlie (group/role member) should have READ");
    TEST_ASSERT(has_perm(result.value, PERM_WRITE), "charlie (group/role member) should have WRITE");
    // A non-member must NOT receive the group/role grant.
    auto nonmember = acl.get_effective_permissions(res, "charlie", {});
    TEST_ASSERT(!has_perm(nonmember.value, PERM_WRITE), "non-member must NOT get the group/role WRITE");
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

    // "group" grant == ROLE grant; alice holds the devs role.
    acl.grant_permission(res, "devs", PrincipalType::ROLE, PERM_READ | PERM_WRITE | PERM_DELETE);
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ);

    auto result = acl.get_effective_permissions(res, "alice", {"devs"});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(has_perm(result.value, PERM_READ), "alice should have READ");
    TEST_ASSERT(has_perm(result.value, PERM_WRITE), "alice should have WRITE from her group/role");
    TEST_ASSERT(has_perm(result.value, PERM_DELETE), "alice should have DELETE from her group/role");
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

    // Two group/role grants union for a user who holds both roles.
    acl.grant_permission(res, "devs", PrincipalType::ROLE, PERM_READ | PERM_WRITE | PERM_DELETE);
    acl.grant_permission(res, "viewer", PrincipalType::ROLE, PERM_READ);

    auto result = acl.get_effective_permissions(res, "bob", {"viewer", "devs"});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(has_perm(result.value, PERM_READ), "bob should have READ");
    TEST_ASSERT(has_perm(result.value, PERM_WRITE), "bob should have WRITE from the devs group/role");
    TEST_ASSERT(has_perm(result.value, PERM_DELETE), "bob should have DELETE from the devs group/role");
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

    // User has a role not defined on this resource. The editor role's WRITE
    // must not apply; only the read-by-default baseline READ remains.
    auto result = acl.get_effective_permissions(res, "user1", {"viewer"});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(!has_perm(result.value, PERM_WRITE), "unmatched role should yield no role permissions");
    TEST_ASSERT(result.value == PERM_READ, "unmatched role leaves only the baseline READ");
}

void test_rbac_empty_roles_vector() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-rbac-005";

    acl.grant_permission(res, "editor", PrincipalType::ROLE, PERM_READ | PERM_WRITE);

    auto result = acl.get_effective_permissions(res, "user1", {});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(!has_perm(result.value, PERM_WRITE), "empty roles should yield no role WRITE");
    TEST_ASSERT(result.value == PERM_READ, "empty roles leave only the baseline READ");
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
// 3. CLAIM-BASED (ABAC) ACCESS CONTROL
//    A CLAIM-type ACL rule stores its principal as "key=value" and matches a
//    requester whose auth claims contain that exact pair. Claims flow through
//    check_permission() and get_effective_permissions() as a trailing map arg.
// =============================================================================

void test_claims_grant_matches_on_claim() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-claims-001";

    // Grant READ|WRITE to anyone whose claims contain department=engineering.
    acl.grant_permission(res, "department=engineering", PrincipalType::CLAIM,
                         PERM_READ | PERM_WRITE);

    // A requester presenting the matching claim is granted.
    std::map<std::string, std::string> eng = {{"department", "engineering"}};
    auto r1 = acl.check_permission(res, "alice", {}, PERM_READ, "", eng);
    TEST_ASSERT(r1.success && r1.value, "matching claim grants READ");
    auto r2 = acl.check_permission(res, "alice", {}, PERM_WRITE, "", eng);
    TEST_ASSERT(r2.success && r2.value, "matching claim grants WRITE");

    // A requester with a different claim value gets no claim-granted bits.
    // (WRITE is probed because READ is granted to everyone by the baseline.)
    std::map<std::string, std::string> sales = {{"department", "sales"}};
    auto r3 = acl.check_permission(res, "bob", {}, PERM_WRITE, "", sales);
    TEST_ASSERT(r3.success && !r3.value, "non-matching claim value denied WRITE");

    // A requester with no claims at all gets no claim-granted bits.
    auto r4 = acl.check_permission(res, "carol", {}, PERM_WRITE, "", {});
    TEST_ASSERT(r4.success && !r4.value, "absent claim denied WRITE");
}

void test_claims_effective_permissions_union_with_roles() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-claims-002";

    // DELETE via claim, WRITE via role: effective set is the union. (DELETE is
    // used as the claim's observable bit because READ is granted to everyone by
    // the read-by-default baseline and so cannot witness the claim's effect.)
    acl.grant_permission(res, "clearance=secret", PrincipalType::CLAIM, PERM_DELETE);
    acl.grant_permission(res, "editor", PrincipalType::ROLE, PERM_WRITE);

    std::map<std::string, std::string> secret = {{"clearance", "secret"}};
    auto eff = acl.get_effective_permissions(res, "dave", {"editor"}, "", secret);
    TEST_ASSERT(eff.success, "effective lookup succeeds");
    TEST_ASSERT(has_perm(eff.value, PERM_DELETE), "claim contributes DELETE");
    TEST_ASSERT(has_perm(eff.value, PERM_WRITE), "role contributes WRITE");

    // Same principal without the claim loses only the claim-granted DELETE.
    auto eff2 = acl.get_effective_permissions(res, "dave", {"editor"}, "", {});
    TEST_ASSERT(!has_perm(eff2.value, PERM_DELETE), "no claim -> no DELETE");
    TEST_ASSERT(has_perm(eff2.value, PERM_WRITE), "role WRITE still present");
}

void test_claims_deny_overrides_allow() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-claims-003";

    // ALLOW READ to everyone, but DENY READ to a quarantined claim.
    acl.grant_permission(res, "other", PrincipalType::OTHER, PERM_READ);
    acl.grant_permission(res, "status=quarantined", PrincipalType::CLAIM, PERM_READ,
                         "", "", AclEffect::DENY);

    std::map<std::string, std::string> bad = {{"status", "quarantined"}};
    auto r1 = acl.check_permission(res, "eve", {}, PERM_READ, "", bad);
    TEST_ASSERT(r1.success && !r1.value, "matching DENY claim overrides ALLOW");

    // A requester without the quarantine claim keeps the OTHER ALLOW.
    auto r2 = acl.check_permission(res, "frank", {}, PERM_READ, "", {});
    TEST_ASSERT(r2.success && r2.value, "non-quarantined keeps READ");
}

void test_claims_malformed_principal_never_matches() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-claims-004";

    // A CLAIM principal with no '=' is malformed and must never match. WRITE is
    // probed because READ is granted to everyone by the read-by-default baseline.
    acl.grant_permission(res, "no-equals-sign", PrincipalType::CLAIM, PERM_WRITE);
    std::map<std::string, std::string> any = {{"no-equals-sign", "x"}};
    auto r = acl.check_permission(res, "grace", {}, PERM_WRITE, "", any);
    TEST_ASSERT(r.success && !r.value, "malformed claim rule never matches");
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

        // Verify user does NOT have other permissions. READ is excluded: it is
        // granted to every user by the read-by-default baseline, so its
        // presence is not evidence of the specific grant under test.
        for (const auto& other : perms) {
            if (other.perm != p.perm && other.perm != PERM_READ) {
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

void test_grant_accumulates_per_principal_effect() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-grant-001";

    // Successive grants for the same (principal, type, effect) OR the new
    // bits into the existing row, so callers can grant bits incrementally
    // without overwriting earlier grants. The DB enforces this with
    //   ON CONFLICT DO UPDATE SET permissions = acls.permissions | $4
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ);
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_WRITE);

    auto acls = acl.get_acls_for_resource(res);
    TEST_ASSERT(acls.success, "get_acls should succeed");
    TEST_ASSERT(acls.value.size() == 1, "ALLOW row is upserted, not duplicated");

    auto result = acl.get_effective_permissions(res, "alice", {});
    TEST_ASSERT(has_perm(result.value, PERM_READ), "alice retains READ from the first grant");
    TEST_ASSERT(has_perm(result.value, PERM_WRITE), "alice gains WRITE from the second grant");
}

void test_revoke_clears_only_requested_bits() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "res-revoke-001";

    // Grant WRITE|DELETE in a single call (matches how the DB merges on conflict).
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_WRITE | PERM_DELETE);

    // Revoke only the WRITE bit — DELETE must remain. (Non-baseline bits are
    // used so the revoke is observable; revoking an ALLOW READ would be masked
    // by the read-by-default baseline.)
    acl.revoke_permission(res, "alice", PrincipalType::USER, PERM_WRITE);

    auto result = acl.get_effective_permissions(res, "alice", {});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(!has_perm(result.value, PERM_WRITE), "alice should NOT have WRITE after partial revoke");
    TEST_ASSERT(has_perm(result.value, PERM_DELETE), "alice should still have DELETE after partial revoke");
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

void test_inherit_acls_copies_inheritable_rules_only() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string parent = "dir-parent";
    std::string child = "dir-child";

    // Two rules marked inheritable, one not.
    const int inherit = PERM_ACL_INHERIT;
    acl.grant_permission(parent, "alice", PrincipalType::USER, PERM_READ | PERM_WRITE | inherit);
    acl.grant_permission(parent, "editor", PrincipalType::ROLE, PERM_READ | inherit);
    acl.grant_permission(parent, "other", PrincipalType::OTHER, PERM_READ); // no inherit bit

    auto result = acl.inherit_acls(parent, child);
    TEST_ASSERT(result.success, "inherit_acls should succeed");

    auto child_acls = acl.get_acls_for_resource(child);
    TEST_ASSERT(child_acls.value.size() == 2,
                "child should only inherit the two rules marked ACL_INHERIT");

    // alice still has READ|WRITE on the child (the inherit bit is copied too
    // so inheritance cascades to grandchildren).
    auto child_perms = acl.get_effective_permissions(child, "alice", {});
    TEST_ASSERT(has_perm(child_perms.value, PERM_READ), "alice should have READ on child");
    TEST_ASSERT(has_perm(child_perms.value, PERM_WRITE), "alice should have WRITE on child");
}

void test_inherit_acls_skips_non_inheritable_rules() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string parent = "dir-non-inherit-parent";
    std::string child = "dir-non-inherit-child";

    // None of the parent rules carry ACL_INHERIT.
    acl.grant_permission(parent, "alice", PrincipalType::USER, PERM_READ);
    acl.grant_permission(parent, "bob", PrincipalType::USER, PERM_WRITE);

    auto result = acl.inherit_acls(parent, child);
    TEST_ASSERT(result.success, "inherit_acls should succeed even with nothing to copy");

    auto child_acls = acl.get_acls_for_resource(child);
    TEST_ASSERT(child_acls.value.empty(),
                "no rules should be inherited when none carry ACL_INHERIT");
}

void test_inherit_acls_is_one_time_copy() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string parent = "dir-parent-2";
    std::string child = "dir-child-2";

    acl.grant_permission(parent, "alice", PrincipalType::USER, PERM_READ | PERM_ACL_INHERIT);
    acl.inherit_acls(parent, child);

    // Add more permissions to parent AFTER inheritance.
    acl.grant_permission(parent, "alice", PrincipalType::USER, PERM_WRITE | PERM_ACL_INHERIT);

    // Child should NOT get the new permission.
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
    // Creator gets FULL control over what they create: read/write/execute, delete,
    // the deleted-item lifecycle (list-deleted/undelete) and the version lifecycle
    // (view/retrieve/restore), plus MANAGE_ACL. CULL_VERSIONS stays opt-in.
    TEST_ASSERT(has_perm(perms.value, PERM_READ), "creator should have READ");
    TEST_ASSERT(has_perm(perms.value, PERM_WRITE), "creator should have WRITE");
    TEST_ASSERT(has_perm(perms.value, PERM_EXECUTE), "creator should have EXECUTE");
    TEST_ASSERT(has_perm(perms.value, PERM_DELETE), "creator should have DELETE");
    TEST_ASSERT(has_perm(perms.value, PERM_LIST_DELETED), "creator should have LIST_DELETED");
    TEST_ASSERT(has_perm(perms.value, PERM_UNDELETE), "creator should have UNDELETE");
    TEST_ASSERT(has_perm(perms.value, PERM_VIEW_VERSIONS), "creator should have VIEW_VERSIONS");
    TEST_ASSERT(has_perm(perms.value, PERM_RETRIEVE_BACK_VERSION), "creator should have RETRIEVE_BACK_VERSION");
    TEST_ASSERT(has_perm(perms.value, PERM_RESTORE_TO_VERSION), "creator should have RESTORE_TO_VERSION");
    TEST_ASSERT(has_perm(perms.value, PERM_MANAGE_ACL), "creator should have MANAGE_ACL");
    TEST_ASSERT(!has_perm(perms.value, PERM_CULL_VERSIONS), "creator should NOT have CULL_VERSIONS by default (destructive, opt-in)");
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

void test_default_acls_read_by_default() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "new-resource-default-read";

    // Read-by-default: a new resource is readable by any user even though only
    // the creator was granted an explicit ACL.
    acl.apply_default_acls(res, "creator_user");

    auto perms = acl.get_effective_permissions(res, "someone_else", {});
    TEST_ASSERT(has_perm(perms.value, PERM_READ), "non-creator should have baseline READ (read-by-default)");
    TEST_ASSERT(!has_perm(perms.value, PERM_WRITE), "non-creator should NOT have WRITE by default");
}

void test_default_read_can_be_disabled() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "strict-resource";

    // Strict deployments can opt back into private-by-default.
    acl.set_default_read(false);
    acl.apply_default_acls(res, "creator_user");

    auto perms = acl.get_effective_permissions(res, "someone_else", {});
    TEST_ASSERT(perms.value == 0, "with default-read disabled a non-creator has zero permissions");
}

void test_default_acls_creator_has_full_user_bits() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "new-resource-3";

    acl.apply_default_acls(res, "creator_user");

    auto perms = acl.get_effective_permissions(res, "creator_user", {});
    TEST_ASSERT(has_perm(perms.value, PERM_WRITE), "creator should have WRITE from USER ACL");
    TEST_ASSERT(has_perm(perms.value, PERM_EXECUTE), "creator should have EXECUTE from USER ACL");
    TEST_ASSERT(has_perm(perms.value, PERM_MANAGE_ACL),
                "creator should have MANAGE_ACL so they can grant/revoke on their resource");
}

void test_manage_acl_required_separately_from_write() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "managed-resource";

    // bob has WRITE but no MANAGE_ACL.
    acl.grant_permission(res, "bob", PrincipalType::USER, PERM_READ | PERM_WRITE);

    // check_permission(MANAGE_ACL) should return false even though bob has WRITE.
    auto can_manage = acl.check_permission(res, "bob", {}, PERM_MANAGE_ACL);
    TEST_ASSERT(can_manage.success, "check_permission should succeed");
    TEST_ASSERT(!can_manage.value,
                "WRITE alone must not satisfy a MANAGE_ACL check (no escalation gap)");

    // The creator has MANAGE_ACL via apply_default_acls.
    acl.apply_default_acls(res, "owner");
    auto owner_can_manage = acl.check_permission(res, "owner", {}, PERM_MANAGE_ACL);
    TEST_ASSERT(owner_can_manage.success && owner_can_manage.value,
                "creator should satisfy a MANAGE_ACL check");
}

void test_system_admin_role_bypasses_acls() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "locked-resource";

    // Resource has no ACLs at all — a normal user has only the baseline READ,
    // never WRITE. WRITE is probed so the admin bypass below is observable.
    auto baseline = acl.check_permission(res, "alice", {}, PERM_WRITE);
    TEST_ASSERT(baseline.success && !baseline.value,
                "alice without ACL should not have WRITE on locked resource");

    // Holding system_admin in the request roles immediately grants access —
    // there is no enable flag. Upstream is trusted to only attach this role
    // to legitimately privileged requests.
    auto via_request = acl.check_permission(res, "alice", {kSystemAdminRole}, PERM_WRITE);
    TEST_ASSERT(via_request.success && via_request.value,
                "system_admin role in request roles should bypass ACLs");
    TEST_ASSERT(acl.is_system_admin("alice", {kSystemAdminRole}),
                "is_system_admin should reflect the role check");

    // A user assigned system_admin in the DB also bypasses (request and DB
    // roles are unioned via resolve_effective_roles).
    acl.is_system_admin("bob", {kSystemAdminRole}); // no-op, just consistency
    RoleManager rm(db);
    rm.create_role(kSystemAdminRole);
    rm.assign_user_to_role("bob", kSystemAdminRole);
    auto via_db = acl.check_permission(res, "bob", {}, PERM_WRITE);
    TEST_ASSERT(via_db.success && via_db.value,
                "system_admin role via DB-stored assignment should also bypass");
}

void test_audit_log_records_actor_for_grant_and_revoke() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "audited-resource";

    // Direct AclManager API: actor is the trailing performed_by argument.
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ | PERM_WRITE,
                         /*tenant=*/"", /*performed_by=*/"admin");
    acl.revoke_permission(res, "alice", PrincipalType::USER, PERM_WRITE,
                         /*tenant=*/"", /*performed_by=*/"auditor");

    const auto& log = db->audit_log();
    TEST_ASSERT(log.size() == 2, "audit log should have one row per grant + one per revoke");

    TEST_ASSERT(log[0].action == "grant", "first event should be grant");
    TEST_ASSERT(log[0].performed_by == "admin", "grant should record admin as actor");
    TEST_ASSERT(log[0].resource_uid == res, "grant should reference the resource");

    TEST_ASSERT(log[1].action == "revoke", "second event should be revoke");
    TEST_ASSERT(log[1].performed_by == "auditor", "revoke should record auditor as actor");
}

void test_audit_log_records_creator_for_default_acls() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "fresh-resource";

    acl.apply_default_acls(res, "creator_user");

    const auto& log = db->audit_log();
    TEST_ASSERT(!log.empty(), "apply_default_acls should emit at least one audit row");
    TEST_ASSERT(log[0].action == "grant", "default ACL is recorded as a grant");
    TEST_ASSERT(log[0].performed_by == "creator_user",
                "default ACL should record the creator as the actor");
    TEST_ASSERT(log[0].principal == "creator_user",
                "default ACL principal is the creator's USER row");
}

// =============================================================================
// 13. DENY RULES (Phase 6 §6.1)
// =============================================================================

void test_deny_rule_subtracts_from_allow() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "deny-test-001";

    // alice has READ|WRITE via ALLOW, but a DENY removes WRITE.
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ | PERM_WRITE,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::ALLOW);
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_WRITE,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::DENY);

    auto perms = acl.get_effective_permissions(res, "alice", {});
    TEST_ASSERT(perms.success, "get_effective_permissions should succeed");
    TEST_ASSERT(has_perm(perms.value, PERM_READ),
                "alice should still have READ — only WRITE was denied");
    TEST_ASSERT(!has_perm(perms.value, PERM_WRITE),
                "alice should NOT have WRITE — deny wins over allow");
}

void test_deny_role_grant_for_specific_user() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "shared-doc";

    // "everyone in role editor" gets READ|WRITE — alice is the exception.
    acl.grant_permission(res, "editor", PrincipalType::ROLE, PERM_READ | PERM_WRITE);
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_WRITE,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::DENY);

    // bob (an editor) gets full role permissions.
    auto bob = acl.get_effective_permissions(res, "bob", {"editor"});
    TEST_ASSERT(bob.success, "bob check should succeed");
    TEST_ASSERT(has_perm(bob.value, PERM_READ), "bob should have READ via role");
    TEST_ASSERT(has_perm(bob.value, PERM_WRITE), "bob should have WRITE via role");

    // alice (also an editor) gets READ but not WRITE.
    auto alice = acl.get_effective_permissions(res, "alice", {"editor"});
    TEST_ASSERT(alice.success, "alice check should succeed");
    TEST_ASSERT(has_perm(alice.value, PERM_READ), "alice should have READ via role");
    TEST_ASSERT(!has_perm(alice.value, PERM_WRITE),
                "alice should NOT have WRITE — user-level deny wins over role allow");
}

void test_deny_on_bit_never_granted_is_noop() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "no-leak-resource";

    // DENY READ on a resource where alice has no explicit ALLOW. The DENY
    // clears the read-by-default baseline (deny always wins), leaving zero.
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::DENY);

    auto perms = acl.get_effective_permissions(res, "alice", {});
    TEST_ASSERT(perms.success && perms.value == 0,
                "deny READ clears the baseline, leaving zero permissions");
}

void test_deny_and_allow_coexist_as_separate_rows() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "two-row-resource";

    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ | PERM_WRITE,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::ALLOW);
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_WRITE,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::DENY);

    auto acls = acl.get_acls_for_resource(res);
    TEST_ASSERT(acls.success && acls.value.size() == 2,
                "ALLOW and DENY for same principal coexist as separate rows");
    bool saw_allow = false, saw_deny = false;
    for (const auto& r : acls.value) {
        if (r.effect == AclEffect::ALLOW) saw_allow = true;
        if (r.effect == AclEffect::DENY) saw_deny = true;
    }
    TEST_ASSERT(saw_allow && saw_deny, "both effect rows should be present");
}

void test_deny_revoke_targets_the_deny_row() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "revoke-target-test";

    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ | PERM_WRITE,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::ALLOW);
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_WRITE,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::DENY);

    // Revoking from the DENY row removes the block — alice gets WRITE back.
    acl.revoke_permission(res, "alice", PrincipalType::USER, -1,
                          /*tenant=*/"", /*performed_by=*/"", AclEffect::DENY);

    auto perms = acl.get_effective_permissions(res, "alice", {});
    TEST_ASSERT(has_perm(perms.value, PERM_READ), "READ remains from allow");
    TEST_ASSERT(has_perm(perms.value, PERM_WRITE),
                "WRITE is restored after the deny row is revoked");
}

// =============================================================================
// 14. ATOMIC RESOURCE CREATION (Phase 6 §6.2)
// =============================================================================

void test_create_file_with_acls_applies_all_grants() {
    auto db = std::make_shared<MockDatabase>();

    std::vector<IDatabase::AclGrant> grants;
    {
        IDatabase::AclGrant g;
        g.principal = "creator"; g.type = static_cast<int>(PrincipalType::USER);
        g.permissions = PERM_READ | PERM_WRITE | PERM_MANAGE_ACL;
        g.performed_by = "creator"; g.effect = 0;
        grants.push_back(g);
    }
    {
        IDatabase::AclGrant g;
        g.principal = "editor"; g.type = static_cast<int>(PrincipalType::ROLE);
        g.permissions = PERM_READ | PERM_ACL_INHERIT;
        g.performed_by = "creator"; g.effect = 0;
        grants.push_back(g);
    }

    auto result = db->create_file_with_acls("file-uid", "name", "/name", "",
                                            FileType::REGULAR_FILE, "creator", 0644,
                                            grants, "");
    TEST_ASSERT(result.success, "create_file_with_acls should succeed");
    TEST_ASSERT(result.value == "file-uid", "should return the file uid");
    TEST_ASSERT(db->atomic_create_count() == 1, "atomic create path should be invoked once");

    // All grants must be persisted (mock simulates the transaction by applying
    // each add_acl in order).
    auto acls = db->get_acls_for_resource("file-uid", "");
    TEST_ASSERT(acls.success && acls.value.size() == 2,
                "both grants should be visible after atomic creation");
}

// =============================================================================
// 15. REQUEST-SCOPED PERMISSION CACHE (Phase 6 §6.3)
// =============================================================================

void test_cache_coalesces_repeat_reads_within_scope() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "cached-resource";
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ | PERM_WRITE);

    db->reset_get_acls_call_count();
    {
        AclManager::CacheScope scope(acl);
        // Three repeat checks for the same resource.
        acl.check_permission(res, "alice", {}, PERM_READ);
        acl.check_permission(res, "alice", {}, PERM_WRITE);
        acl.get_effective_permissions(res, "alice", {});
    }
    TEST_ASSERT(db->get_acls_call_count() == 1,
                "within a CacheScope the DB should be read exactly once for the same resource");
}

void test_cache_disabled_outside_scope() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "uncached-resource";
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ);

    db->reset_get_acls_call_count();
    acl.check_permission(res, "alice", {}, PERM_READ);
    acl.check_permission(res, "alice", {}, PERM_READ);
    TEST_ASSERT(db->get_acls_call_count() == 2,
                "without a CacheScope every check should re-hit the DB");
}

void test_cache_invalidated_on_grant_within_scope() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "mutated-resource";
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ);

    db->reset_get_acls_call_count();
    {
        AclManager::CacheScope scope(acl);
        acl.check_permission(res, "alice", {}, PERM_READ); // DB read 1, cached
        // A mid-scope grant invalidates the cached entry so a subsequent
        // check sees the new permission.
        acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ | PERM_WRITE);
        auto perms = acl.get_effective_permissions(res, "alice", {});
        TEST_ASSERT(has_perm(perms.value, PERM_WRITE),
                    "post-grant check should see WRITE, not the stale cached value");
    }
    TEST_ASSERT(db->get_acls_call_count() >= 2,
                "DB should be hit again after the invalidation");
}

void test_nested_cache_scopes_share_outer_cache() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "nested-scope-resource";
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ);

    db->reset_get_acls_call_count();
    {
        AclManager::CacheScope outer(acl);
        acl.check_permission(res, "alice", {}, PERM_READ); // DB read, cached
        {
            // Nested scope must NOT reset the outer cache.
            AclManager::CacheScope inner(acl);
            acl.check_permission(res, "alice", {}, PERM_READ); // cached hit
        }
        acl.check_permission(res, "alice", {}, PERM_READ); // still cached
    }
    TEST_ASSERT(db->get_acls_call_count() == 1,
                "nested CacheScope should not clobber the outer cache");
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
    // Grant exactly READ|WRITE (NOT DELETE/LIST_DELETED) so the octal-collision
    // checks below are meaningful — the creator's default ACL now includes DELETE
    // and LIST_DELETED (full owner control), which would mask this regression.
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ | PERM_WRITE);

    // Correct enum constants: creator can READ and WRITE their own resource.
    auto correct_read  = acl.check_permission(res, "alice", {}, static_cast<int>(Permission::READ));
    auto correct_write = acl.check_permission(res, "alice", {}, static_cast<int>(Permission::WRITE));
    TEST_ASSERT(correct_read.success  && correct_read.value,
                "creator must satisfy READ check using Permission::READ");
    TEST_ASSERT(correct_write.success && correct_write.value,
                "creator must satisfy WRITE check using Permission::WRITE");

    // Legacy octal constants: alice was granted only READ|WRITE, so a check for
    // DELETE (0400) or LIST_DELETED (0200) must fail. This documents the bug
    // class — if these flip to true, someone re-introduced octal.
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
    TEST_ASSERT(has_perm(perms_b.value, PERM_WRITE), "alice should have WRITE on tenant-b");
    // tenant-a granted READ, tenant-b granted WRITE; isolation is witnessed by
    // WRITE (READ is baseline on both tenants).
    TEST_ASSERT(perms_b.value == (PERM_READ | PERM_WRITE), "tenant-b has only its WRITE grant plus baseline READ");
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

    // Use WRITE (a non-baseline bit) so the revoke is observable; revoking an
    // ALLOW READ would be masked by the read-by-default baseline.
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_WRITE, "t1");
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_WRITE, "t2");

    // Revoke on t1 should not affect t2
    acl.revoke_permission(res, "alice", PrincipalType::USER, PERM_WRITE, "t1");

    auto perms_t1 = acl.get_effective_permissions(res, "alice", {}, "t1");
    TEST_ASSERT(!has_perm(perms_t1.value, PERM_WRITE), "alice should NOT have WRITE on t1 after revoke");

    auto perms_t2 = acl.get_effective_permissions(res, "alice", {}, "t2");
    TEST_ASSERT(has_perm(perms_t2.value, PERM_WRITE), "alice should still have WRITE on t2");
}

// =============================================================================
// 10. EDGE CASES
// =============================================================================

void test_no_acls_on_resource_is_readable() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "no-acls-resource";

    // Read-by-default: a resource with no ACLs is readable by anyone, but grants
    // no write/elevated bits.
    auto result = acl.get_effective_permissions(res, "anyone", {"any_role"});
    TEST_ASSERT(result.success, "get_effective_permissions should succeed");
    TEST_ASSERT(result.value == PERM_READ, "no ACLs yields exactly the baseline READ");

    auto check_read = acl.check_permission(res, "anyone", {}, PERM_READ);
    TEST_ASSERT(check_read.success && check_read.value, "check_permission allows READ with no ACLs (read-by-default)");

    auto check_write = acl.check_permission(res, "anyone", {}, PERM_WRITE);
    TEST_ASSERT(check_write.success && !check_write.value, "check_permission denies WRITE with no ACLs");
}

void test_different_resources_independent_acls() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);

    acl.grant_permission("resource-a", "alice", PrincipalType::USER, PERM_READ);
    acl.grant_permission("resource-b", "alice", PrincipalType::USER, PERM_WRITE);

    auto perms_a = acl.get_effective_permissions("resource-a", "alice", {});
    TEST_ASSERT(has_perm(perms_a.value, PERM_READ), "alice should have READ on resource-a");
    TEST_ASSERT(!has_perm(perms_a.value, PERM_WRITE), "alice should NOT have WRITE on resource-a");

    // resource-a granted READ, resource-b granted WRITE; independence is
    // witnessed by WRITE not crossing over (READ is baseline on both).
    auto perms_b = acl.get_effective_permissions("resource-b", "alice", {});
    TEST_ASSERT(has_perm(perms_b.value, PERM_WRITE), "alice should have WRITE on resource-b");
    auto perms_a2 = acl.get_effective_permissions("resource-a", "alice", {});
    TEST_ASSERT(!has_perm(perms_a2.value, PERM_WRITE), "resource-b's WRITE must not leak to resource-a");
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

    // Multiple group/role ACLs accumulate for a user who holds those roles
    // (roles are the group mechanism).
    acl.grant_permission(res, "group-a", PrincipalType::ROLE, PERM_READ);
    acl.grant_permission(res, "group-b", PrincipalType::ROLE, PERM_WRITE);

    auto perms = acl.get_effective_permissions(res, "user1", {"group-a", "group-b"});
    TEST_ASSERT(has_perm(perms.value, PERM_READ), "should have READ from group-a role");
    TEST_ASSERT(has_perm(perms.value, PERM_WRITE), "should have WRITE from group-b role");
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
// 13. Read-by-default, everyone group, and parent-container traversal
// =============================================================================

void test_everyone_deny_blocks_read() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "doc-hidden";

    // DENY READ to everyone (OTHER) hides the resource entirely, overriding the
    // read-by-default baseline.
    acl.grant_permission(res, kEveryonePrincipal, PrincipalType::OTHER, PERM_READ,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::DENY);

    auto eff = acl.get_effective_permissions(res, "anyone", {});
    TEST_ASSERT(!has_perm(eff.value, PERM_READ), "everyone DENY clears baseline READ");

    auto check = acl.check_permission(res, "anyone", {}, PERM_READ);
    TEST_ASSERT(check.success && !check.value, "everyone DENY blocks READ for any user");
}

void test_user_allow_beats_everyone_deny() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "home-alice";

    // Home-folder pattern: deny everyone, but the owner keeps access. The user
    // tier resolves before the everyone tier, so alice's ALLOW wins over the
    // everyone DENY — while a non-owner still falls through to that DENY.
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_READ);
    acl.grant_permission(res, kEveryonePrincipal, PrincipalType::OTHER, PERM_READ,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::DENY);

    auto eff = acl.get_effective_permissions(res, "alice", {});
    TEST_ASSERT(has_perm(eff.value, PERM_READ), "user ALLOW beats everyone DENY (home folder)");

    auto other = acl.get_effective_permissions(res, "bob", {});
    TEST_ASSERT(!has_perm(other.value, PERM_READ), "everyone DENY still blocks a non-owner");
}

void test_role_allow_beats_everyone_deny() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "dept-engineering";

    // Role-gated section: deny everyone read, allow the role read+write. A member
    // resolves at the role/claim tier (before the everyone DENY); a non-member
    // falls through to the everyone DENY.
    acl.grant_permission(res, "engineering", PrincipalType::ROLE, PERM_READ | PERM_WRITE);
    acl.grant_permission(res, kEveryonePrincipal, PrincipalType::OTHER, PERM_READ,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::DENY);

    auto member = acl.get_effective_permissions(res, "alice", {"engineering"});
    TEST_ASSERT(has_perm(member.value, PERM_READ) && has_perm(member.value, PERM_WRITE),
                "role ALLOW beats everyone DENY for a member");
    auto outsider = acl.get_effective_permissions(res, "bob", {"sales"});
    TEST_ASSERT(!has_perm(outsider.value, PERM_READ), "everyone DENY blocks a non-member");
}

void test_deny_wins_within_a_tier() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "doc-mixed";

    // Same tier (role): a DENY beats an ALLOW for the same bit.
    acl.grant_permission(res, "r-allow", PrincipalType::ROLE, PERM_READ | PERM_WRITE);
    acl.grant_permission(res, "r-deny", PrincipalType::ROLE, PERM_WRITE,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::DENY);

    auto eff = acl.get_effective_permissions(res, "u", {"r-allow", "r-deny"});
    TEST_ASSERT(has_perm(eff.value, PERM_READ), "role READ allowed");
    TEST_ASSERT(!has_perm(eff.value, PERM_WRITE), "role DENY beats role ALLOW in-tier (write)");
}

void test_user_tier_beats_role_tier() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);

    // User tier resolves before the role tier: a user DENY beats a role ALLOW,
    // and a user ALLOW beats a role DENY, for the same bit.
    std::string res = "doc-tiers";
    acl.grant_permission(res, "team", PrincipalType::ROLE, PERM_READ | PERM_WRITE);
    acl.grant_permission(res, "alice", PrincipalType::USER, PERM_WRITE,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::DENY);
    auto eff = acl.get_effective_permissions(res, "alice", {"team"});
    TEST_ASSERT(has_perm(eff.value, PERM_READ), "role READ applies (user tier silent on read)");
    TEST_ASSERT(!has_perm(eff.value, PERM_WRITE), "user DENY beats role ALLOW (write)");

    std::string res2 = "doc-tiers2";
    acl.grant_permission(res2, "team", PrincipalType::ROLE, PERM_WRITE,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::DENY);
    acl.grant_permission(res2, "alice", PrincipalType::USER, PERM_WRITE);
    auto eff2 = acl.get_effective_permissions(res2, "alice", {"team"});
    TEST_ASSERT(has_perm(eff2.value, PERM_WRITE), "user ALLOW beats role DENY (write)");
}

void test_write_denied_by_default() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    // No ACLs: everyone reads (read-by-default) but write is not granted.
    auto eff = acl.get_effective_permissions("doc-fresh", "anyone", {});
    TEST_ASSERT(has_perm(eff.value, PERM_READ), "read allowed by default");
    TEST_ASSERT(!has_perm(eff.value, PERM_WRITE), "write denied by default (fall-through)");
}

void test_parent_readable_child_readable() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    // /folder/file with no specific ACLs anywhere.
    db->set_file("folder", "");        // folder at root
    db->set_file("file", "folder");    // file under folder

    // A user with no roles and no ACLs can read the file because the file is
    // readable by default AND the containing folder is readable.
    auto check = acl.check_permission("file", "nobody", {}, PERM_READ);
    TEST_ASSERT(check.success && check.value,
                "file is readable when it and its parent folder are readable by default");
}

void test_parent_deny_blocks_child_subtree() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    db->set_file("folder", "");
    db->set_file("file", "folder");

    // DENY READ to everyone on the FOLDER. The file itself has no ACL (baseline
    // readable), but it must become unreachable because its parent is hidden.
    acl.grant_permission("folder", kEveryonePrincipal, PrincipalType::OTHER, PERM_READ,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::DENY);

    // Effective permissions reflect traversal: an unreadable parent collapses
    // the child's effective set to none.
    auto eff = acl.get_effective_permissions("file", "nobody", {});
    TEST_ASSERT(eff.value == 0, "unreachable child reports zero effective permissions");

    // And the enforcing check agrees.
    auto child = acl.check_permission("file", "nobody", {}, PERM_READ);
    TEST_ASSERT(child.success && !child.value, "parent DENY hides the child file (subtree)");

    // And the folder itself is unreadable.
    auto folder = acl.check_permission("folder", "nobody", {}, PERM_READ);
    TEST_ASSERT(folder.success && !folder.value, "folder with everyone DENY is unreadable");
}

void test_effective_permissions_reflect_readable_parent() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    db->set_file("folder", "");
    db->set_file("file", "folder");

    // Parent readable (by default), child grants alice WRITE. Effective perms
    // for alice include both baseline READ and the granted WRITE — traversal
    // does not strip perms when the path is reachable.
    acl.grant_permission("file", "alice", PrincipalType::USER, PERM_WRITE);

    auto eff = acl.get_effective_permissions("file", "alice", {});
    TEST_ASSERT(has_perm(eff.value, PERM_READ), "reachable child keeps baseline READ");
    TEST_ASSERT(has_perm(eff.value, PERM_WRITE), "reachable child keeps its granted WRITE");
}

void test_effective_permissions_reflect_system_admin() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    db->set_file("folder", "");
    db->set_file("file", "folder");

    // Subtree hidden from everyone — would collapse a normal user's effective
    // perms to zero. A system_admin bypasses ACLs entirely, so its effective
    // set is every permission regardless of ACLs or traversal.
    acl.grant_permission("folder", kEveryonePrincipal, PrincipalType::OTHER, PERM_READ,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::DENY);

    auto eff = acl.get_effective_permissions("file", "root", {kSystemAdminRole});
    TEST_ASSERT(eff.success, "effective lookup succeeds for admin");
    TEST_ASSERT(eff.value == kAllPermissions,
                "system_admin effective perms = all bits, bypassing ACLs and traversal");
    TEST_ASSERT(has_perm(eff.value, PERM_CULL_VERSIONS),
                "admin effective set includes even CULL_VERSIONS");

    // A normal user on the same path collapses to zero (contrast).
    auto eff_user = acl.get_effective_permissions("file", "nobody", {});
    TEST_ASSERT(eff_user.value == 0, "non-admin on hidden subtree has zero effective perms");
}

void test_parent_deny_blocks_grandchild() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    db->set_file("top", "");
    db->set_file("mid", "top");
    db->set_file("leaf", "mid");

    // DENY READ to everyone at the TOP level blocks the whole subtree.
    acl.grant_permission("top", kEveryonePrincipal, PrincipalType::OTHER, PERM_READ,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::DENY);

    auto leaf = acl.check_permission("leaf", "nobody", {}, PERM_READ);
    TEST_ASSERT(leaf.success && !leaf.value, "ancestor DENY several levels up blocks the grandchild");
}

void test_child_allow_cannot_override_ancestor_deny() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    db->set_file("folder", "");
    db->set_file("file", "folder");

    // Parent hidden from everyone, but the child explicitly grants alice READ.
    // Path semantics: alice still cannot reach the file through a folder she
    // cannot read.
    acl.grant_permission("folder", kEveryonePrincipal, PrincipalType::OTHER, PERM_READ,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::DENY);
    acl.grant_permission("file", "alice", PrincipalType::USER, PERM_READ);

    auto check = acl.check_permission("file", "alice", {}, PERM_READ);
    TEST_ASSERT(check.success && !check.value,
                "a child ALLOW cannot override an unreadable ancestor");
}

void test_system_admin_bypasses_parent_deny() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    db->set_file("folder", "");
    db->set_file("file", "folder");

    acl.grant_permission("folder", kEveryonePrincipal, PrincipalType::OTHER, PERM_READ,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::DENY);

    // system_admin bypasses ACLs entirely, including path traversal.
    auto check = acl.check_permission("file", "root", {kSystemAdminRole}, PERM_READ);
    TEST_ASSERT(check.success && check.value, "system_admin bypasses parent DENY");
}

void test_user_with_parent_read_can_read_unowned_file() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    db->set_file("folder", "");
    db->set_file("file", "folder");

    // The exact case from the request: a file with no specific ACL, a user with
    // no role memberships, who has access to the containing folder -> can read
    // the file. (Folder readable by default; nothing denies.)
    auto check = acl.check_permission("file", "stranger", {}, PERM_READ);
    TEST_ASSERT(check.success && check.value,
                "user with access to the containing folder can read an unowned file");
}

// =============================================================================
// 14. CULL_VERSIONS — dedicated destroy-data permission
// =============================================================================

void test_cull_versions_not_implied_by_write() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "doc-with-history";

    // A user with broad write-ish access still cannot cull versions.
    acl.grant_permission(res, "editor", PrincipalType::USER,
                         PERM_READ | PERM_WRITE | PERM_DELETE);

    auto check = acl.check_permission(res, "editor", {}, PERM_CULL_VERSIONS);
    TEST_ASSERT(check.success && !check.value,
                "WRITE/DELETE must not imply CULL_VERSIONS (destroy-data is separate)");
}

void test_cull_versions_requires_explicit_grant() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "doc-cullable";

    acl.grant_permission(res, "archivist", PrincipalType::USER, PERM_CULL_VERSIONS);

    auto check = acl.check_permission(res, "archivist", {}, PERM_CULL_VERSIONS);
    TEST_ASSERT(check.success && check.value,
                "explicit CULL_VERSIONS grant permits version culling");
}

void test_cull_versions_not_in_default_creator_grants() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    std::string res = "fresh-doc";

    // Even the creator does NOT receive CULL_VERSIONS by default — it must be
    // granted explicitly because it permanently destroys data.
    acl.apply_default_acls(res, "creator_user");

    auto eff = acl.get_effective_permissions(res, "creator_user", {});
    TEST_ASSERT(has_perm(eff.value, PERM_WRITE), "creator still has WRITE by default");
    TEST_ASSERT(!has_perm(eff.value, PERM_CULL_VERSIONS),
                "creator must NOT have CULL_VERSIONS by default");
}

void test_cull_versions_blocked_by_everyone_deny_and_traversal() {
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    db->set_file("folder", "");
    db->set_file("file", "folder");

    // archivist holds CULL_VERSIONS on the file, but the parent folder is hidden
    // from everyone — path traversal still blocks the destroy-data op.
    acl.grant_permission("file", "archivist", PrincipalType::USER, PERM_CULL_VERSIONS);
    acl.grant_permission("folder", kEveryonePrincipal, PrincipalType::OTHER, PERM_READ,
                         /*tenant=*/"", /*performed_by=*/"", AclEffect::DENY);

    auto check = acl.check_permission("file", "archivist", {}, PERM_CULL_VERSIONS);
    TEST_ASSERT(check.success && !check.value,
                "CULL_VERSIONS is still gated by parent-container readability");
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
    run_test("claim grant matches on claim (ABAC)", test_claims_grant_matches_on_claim);
    run_test("claim + role effective union", test_claims_effective_permissions_union_with_roles);
    run_test("claim DENY overrides ALLOW", test_claims_deny_overrides_allow);
    run_test("malformed claim rule never matches", test_claims_malformed_principal_never_matches);

    std::cout << "\n--- 4. All Permission Types ---\n";
    run_test("all individual permissions", test_all_individual_permissions);
    run_test("permission bitmask combination", test_permission_bitmask_combination);
    run_test("version-specific permissions via roles", test_version_specific_permissions_via_roles);
    run_test("deleted file permissions via roles", test_deleted_file_permissions_via_roles);

    std::cout << "\n--- 5. check_permission() Semantics ---\n";
    run_test("check_permission requires all bits", test_check_permission_requires_all_bits);
    run_test("check_permission with superset", test_check_permission_with_superset);

    std::cout << "\n--- 6. Grant and Revoke Behavior ---\n";
    run_test("grant accumulates bits per (principal, type, effect)", test_grant_accumulates_per_principal_effect);
    run_test("revoke clears only the requested bits", test_revoke_clears_only_requested_bits);
    run_test("revoke -1 deletes the row entirely", test_revoke_all_bits_deletes_row);
    run_test("revoke only affects matching type", test_revoke_only_affects_matching_type);
    run_test("revoke nonexistent permission is no-op", test_revoke_nonexistent_permission);

    std::cout << "\n--- 7. ACL Inheritance ---\n";
    run_test("inherit copies only ACL_INHERIT-marked rules", test_inherit_acls_copies_inheritable_rules_only);
    run_test("inherit skips rules without ACL_INHERIT", test_inherit_acls_skips_non_inheritable_rules);
    run_test("inheritance is one-time copy", test_inherit_acls_is_one_time_copy);
    run_test("inherit from empty parent", test_inherit_from_empty_parent);

    std::cout << "\n--- 8. Default ACLs ---\n";
    run_test("default ACLs: creator gets FULL control", test_default_acls_creator_gets_rwe);
    run_test("default ACLs: other gets READ when world-readable enabled", test_default_acls_other_gets_read_when_world_readable);
    run_test("default ACLs: read-by-default (any user can read)", test_default_acls_read_by_default);
    run_test("default-read can be disabled (private-by-default opt-in)", test_default_read_can_be_disabled);
    run_test("default ACLs: creator has full user bits", test_default_acls_creator_has_full_user_bits);
    run_test("MANAGE_ACL is required separately from WRITE", test_manage_acl_required_separately_from_write);
    run_test("system_admin role bypasses ACLs (request and DB paths)", test_system_admin_role_bypasses_acls);
    run_test("audit log records actor for grant and revoke", test_audit_log_records_actor_for_grant_and_revoke);
    run_test("audit log records creator for default ACLs", test_audit_log_records_creator_for_default_acls);
    run_test("deny rule subtracts from allow", test_deny_rule_subtracts_from_allow);
    run_test("deny role grant for a specific user", test_deny_role_grant_for_specific_user);
    run_test("deny on bit never granted is a no-op", test_deny_on_bit_never_granted_is_noop);
    run_test("deny and allow coexist as separate rows", test_deny_and_allow_coexist_as_separate_rows);
    run_test("revoke targets the deny row when effect=DENY", test_deny_revoke_targets_the_deny_row);
    run_test("create_file_with_acls applies all grants", test_create_file_with_acls_applies_all_grants);
    run_test("cache coalesces repeat reads within scope", test_cache_coalesces_repeat_reads_within_scope);
    run_test("cache disabled outside scope", test_cache_disabled_outside_scope);
    run_test("cache invalidated on grant within scope", test_cache_invalidated_on_grant_within_scope);
    run_test("nested cache scopes share outer cache", test_nested_cache_scopes_share_outer_cache);
    run_test("regression: gRPC bitmask octal vs hex", test_grpc_bitmask_regression_octal_vs_hex);

    std::cout << "\n--- 9. Tenant Isolation ---\n";
    run_test("tenant ACLs are isolated", test_tenant_acls_are_isolated);
    run_test("tenant role isolation", test_tenant_role_isolation);
    run_test("tenant revoke isolation", test_tenant_revoke_isolation);

    std::cout << "\n--- 10. Edge Cases ---\n";
    run_test("no ACLs on resource is readable by default", test_no_acls_on_resource_is_readable);
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

    std::cout << "\n--- 13. Read-by-default, everyone group, parent traversal ---\n";
    run_test("everyone DENY blocks read for any user", test_everyone_deny_blocks_read);
    run_test("user ALLOW beats everyone DENY (home folder)", test_user_allow_beats_everyone_deny);
    run_test("role ALLOW beats everyone DENY (gated section)", test_role_allow_beats_everyone_deny);
    run_test("DENY wins within a tier", test_deny_wins_within_a_tier);
    run_test("user tier beats role tier", test_user_tier_beats_role_tier);
    run_test("write denied by default (read-by-default)", test_write_denied_by_default);
    run_test("readable parent -> child readable by default", test_parent_readable_child_readable);
    run_test("user with parent access reads unowned file", test_user_with_parent_read_can_read_unowned_file);
    run_test("parent DENY blocks child subtree", test_parent_deny_blocks_child_subtree);
    run_test("effective perms reflect readable parent", test_effective_permissions_reflect_readable_parent);
    run_test("effective perms reflect system_admin bypass", test_effective_permissions_reflect_system_admin);
    run_test("ancestor DENY blocks grandchild", test_parent_deny_blocks_grandchild);
    run_test("child ALLOW cannot override ancestor DENY", test_child_allow_cannot_override_ancestor_deny);
    run_test("system_admin bypasses parent DENY", test_system_admin_bypasses_parent_deny);

    std::cout << "\n--- 14. CULL_VERSIONS (destroy-data permission) ---\n";
    run_test("CULL_VERSIONS not implied by WRITE/DELETE", test_cull_versions_not_implied_by_write);
    run_test("CULL_VERSIONS requires explicit grant", test_cull_versions_requires_explicit_grant);
    run_test("CULL_VERSIONS not in default creator grants", test_cull_versions_not_in_default_creator_grants);
    run_test("CULL_VERSIONS still gated by parent traversal", test_cull_versions_blocked_by_everyone_deny_and_traversal);

    std::cout << "\n=== Results: " << tests_passed << "/" << tests_run << " passed";
    if (tests_failed > 0) {
        std::cout << ", " << tests_failed << " FAILED";
    }
    std::cout << " ===\n";

    return tests_failed > 0 ? 1 : 0;
}
