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

// Leak-proof access control: a soft-deleted folder must hide its ENTIRE subtree
// by reachability, WITHOUT rewriting any descendant's own `deleted` flag. These
// tests drive AclManager directly (the single path-traversal choke point that
// every permission-gated read funnels through) with a mock file tree, so they
// need no database.
//
// Tree under test (root has empty uid):
//     root ── F (dir) ── C  (file)
//              │        └─ G (dir) ── C2 (file)
//              └───────── (sibling) S (dir) ── SC (file)

#include <algorithm>
#include <cassert>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fileengine/IDatabase.h"
#include "fileengine/acl_manager.h"
#include "fileengine/types.h"

using namespace fileengine;

class MockDatabase : public IDatabase {
public:
    struct Node { std::string parent; bool is_dir; bool deleted; };
    std::map<std::string, Node> tree_;
    std::map<std::string, std::vector<AclEntry>> acls_;

    void add_node(const std::string& uid, const std::string& parent, bool is_dir) {
        tree_[uid] = Node{parent, is_dir, false};
    }
    void set_deleted(const std::string& uid, bool d) { tree_[uid].deleted = d; }
    void grant_read(const std::string& uid, const std::string& user) {
        AclEntry e;
        e.resource_uid = uid;
        e.principal = user;
        e.type = 0; // PrincipalType::USER
        e.permissions = static_cast<int>(Permission::READ);
        e.effect = 0; // ALLOW
        acls_[uid].push_back(e);
    }

    FileInfo make_info(const std::string& uid, const Node& n) {
        FileInfo info;
        info.uid = uid;
        info.name = uid;
        info.parent_uid = n.parent;
        info.type = n.is_dir ? FileType::DIRECTORY : FileType::REGULAR_FILE;
        info.deleted = n.deleted;
        return info;
    }

    // The default lookup filters deleted rows (what live reads see).
    Result<std::optional<FileInfo>> get_file_by_uid(const std::string& uid, const std::string& = "") override {
        auto it = tree_.find(uid);
        if (it == tree_.end() || it->second.deleted)
            return Result<std::optional<FileInfo>>::ok(std::nullopt);
        return Result<std::optional<FileInfo>>::ok(make_info(uid, it->second));
    }
    // The deleted-aware lookup the reachability walk relies on.
    Result<std::optional<FileInfo>> get_file_by_uid_include_deleted(const std::string& uid, const std::string& = "") override {
        auto it = tree_.find(uid);
        if (it == tree_.end())
            return Result<std::optional<FileInfo>>::ok(std::nullopt);
        return Result<std::optional<FileInfo>>::ok(make_info(uid, it->second));
    }
    Result<std::vector<AclEntry>> get_acls_for_resource(const std::string& resource_uid, const std::string& = "") override {
        auto it = acls_.find(resource_uid);
        if (it != acls_.end()) return Result<std::vector<AclEntry>>::ok(it->second);
        return Result<std::vector<AclEntry>>::ok(std::vector<AclEntry>{});
    }
    Result<std::vector<std::string>> get_roles_for_user(const std::string&, const std::string& = "") override {
        return Result<std::vector<std::string>>::ok(std::vector<std::string>{});
    }

    // ---- everything below is an unused no-op for these tests ----
    bool connect() override { return true; }
    void disconnect() override {}
    bool is_connected() const override { return true; }
    Result<void> create_schema() override { return Result<void>::ok(); }
    Result<void> drop_schema() override { return Result<void>::ok(); }
    Result<std::string> insert_file(const std::string& uid, const std::string&, const std::string&, const std::string&, FileType, const std::string&, int, const std::string& = "") override { return Result<std::string>::ok(uid); }
    Result<std::string> create_file_with_acls(const std::string& uid, const std::string&, const std::string&, const std::string&, FileType, const std::string&, int, const std::vector<AclGrant>&, const std::string& = "") override { return Result<std::string>::ok(uid); }
    Result<void> update_file_modified(const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<void> update_file_current_version(const std::string&, const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<bool> delete_file(const std::string&, const std::string& = "") override { return Result<bool>::ok(true); }
    Result<bool> undelete_file(const std::string&, const std::string& = "") override { return Result<bool>::ok(true); }
    Result<std::optional<FileInfo>> get_file_by_path(const std::string&, const std::string& = "") override { return Result<std::optional<FileInfo>>::ok(std::nullopt); }
    Result<void> update_file_name(const std::string&, const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<std::vector<FileInfo>> list_files_in_directory(const std::string&, const std::string& = "") override { return Result<std::vector<FileInfo>>::ok({}); }
    Result<std::vector<FileInfo>> list_files_in_directory_with_deleted(const std::string&, const std::string& = "") override { return Result<std::vector<FileInfo>>::ok({}); }
    Result<std::vector<FileInfo>> list_all_files(const std::string& = "") override { return Result<std::vector<FileInfo>>::ok({}); }
    Result<std::optional<FileInfo>> get_file_by_name_and_parent(const std::string&, const std::string&, const std::string& = "") override { return Result<std::optional<FileInfo>>::ok(std::nullopt); }
    Result<std::optional<FileInfo>> get_file_by_name_and_parent_include_deleted(const std::string&, const std::string&, const std::string& = "") override { return Result<std::optional<FileInfo>>::ok(std::nullopt); }
    Result<int64_t> get_file_size(const std::string&, const std::string& = "") override { return Result<int64_t>::ok(0); }
    Result<int64_t> get_directory_size(const std::string&, const std::string& = "") override { return Result<int64_t>::ok(0); }
    Result<void> update_file_parent(const std::string&, const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<std::string> path_to_uid(const std::string&, const std::string& = "") override { return Result<std::string>::ok(""); }
    Result<std::vector<std::string>> uid_to_path(const std::string&, const std::string& = "") override { return Result<std::vector<std::string>>::ok({}); }
    Result<int64_t> insert_version(const std::string&, const std::string&, int64_t, const std::string&, const std::string& = "", const std::string& = "") override { return Result<int64_t>::ok(0); }
    Result<std::optional<std::string>> get_version_storage_path(const std::string&, const std::string&, const std::string& = "") override { return Result<std::optional<std::string>>::ok(std::nullopt); }
    Result<std::vector<std::string>> list_versions(const std::string&, const std::string& = "") override { return Result<std::vector<std::string>>::ok({}); }
    Result<bool> restore_to_version(const std::string&, const std::string&, const std::string&, const std::string& = "") override { return Result<bool>::ok(true); }
    Result<void> set_metadata(const std::string&, const std::string&, const std::string&, const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<std::optional<std::string>> get_metadata(const std::string&, const std::string&, const std::string&, const std::string& = "") override { return Result<std::optional<std::string>>::ok(std::nullopt); }
    Result<std::map<std::string, std::string>> get_all_metadata(const std::string&, const std::string&, const std::string& = "") override { return Result<std::map<std::string, std::string>>::ok({}); }
    Result<void> delete_metadata(const std::string&, const std::string&, const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<void> execute(const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<std::vector<std::vector<std::string>>> query(const std::string&, const std::string& = "") override { return Result<std::vector<std::vector<std::string>>>::ok({}); }
    Result<void> update_file_access_stats(const std::string&, const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<std::vector<std::string>> get_least_accessed_files(int = 10, const std::string& = "") override { return Result<std::vector<std::string>>::ok({}); }
    Result<std::vector<std::string>> get_infrequently_accessed_files(int = 30, const std::string& = "") override { return Result<std::vector<std::string>>::ok({}); }
    Result<int64_t> get_storage_usage(const std::string& = "") override { return Result<int64_t>::ok(0); }
    Result<int64_t> get_storage_capacity(const std::string& = "") override { return Result<int64_t>::ok(0); }
    Result<void> create_tenant_schema(const std::string&) override { return Result<void>::ok(); }
    Result<bool> tenant_schema_exists(const std::string&) override { return Result<bool>::ok(true); }
    Result<void> cleanup_tenant_data(const std::string&) override { return Result<void>::ok(); }
    Result<std::vector<std::string>> list_tenants() override { return Result<std::vector<std::string>>::ok({}); }
    Result<void> add_acl(const std::string& r, const std::string& p, int t, int perm, const std::string& = "", const std::string& = "", int eff = 0) override { AclEntry e; e.resource_uid = r; e.principal = p; e.type = t; e.permissions = perm; e.effect = eff; acls_[r].push_back(e); return Result<void>::ok(); }
    Result<void> remove_acl(const std::string&, const std::string&, int, int, const std::string& = "", const std::string& = "", int = 0) override { return Result<void>::ok(); }
    Result<std::vector<AclEntry>> get_user_acls(const std::string& r, const std::string& p, int t, const std::string& = "") override { std::vector<AclEntry> out; auto it = acls_.find(r); if (it != acls_.end()) for (auto& e : it->second) if (e.principal == p && e.type == t) out.push_back(e); return Result<std::vector<AclEntry>>::ok(out); }
    Result<std::vector<std::string>> list_claims(const std::string&, int, const std::string& = "") override { return Result<std::vector<std::string>>::ok({}); }
    Result<void> create_role(const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<void> delete_role(const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<void> assign_user_to_role(const std::string&, const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<void> remove_user_from_role(const std::string&, const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<std::vector<std::string>> get_users_for_role(const std::string&, const std::string& = "") override { return Result<std::vector<std::string>>::ok({}); }
    Result<std::vector<std::string>> get_all_roles(const std::string& = "") override { return Result<std::vector<std::string>>::ok({}); }
};

static int g_checks = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            std::cerr << "  ✗ FAILED: " << (msg) << " (line " << __LINE__ << ")\n"; \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

int main() {
    auto db = std::make_shared<MockDatabase>();
    // root ── F ── C ; F ── G ── C2 ; root ── S ── SC
    db->add_node("F", "", true);
    db->add_node("C", "F", false);
    db->add_node("G", "F", true);
    db->add_node("C2", "G", false);
    db->add_node("S", "", true);
    db->add_node("SC", "S", false);
    for (const char* uid : {"F", "C", "G", "C2", "S", "SC"})
        db->grant_read(uid, "alice");

    AclManager acl(db);
    const int READ = static_cast<int>(Permission::READ);
    auto readable = [&](const std::string& uid) {
        return acl.check_permission(uid, "alice", {}, READ, "").value;
    };
    auto eff_read = [&](const std::string& uid) {
        return (acl.get_effective_permissions(uid, "alice", {}).value & READ) == READ;
    };

    std::cout << "Testing leak-proof deleted-reachability...\n";

    // --- Baseline: nothing deleted, everything reachable ---
    CHECK(readable("C") && readable("C2") && readable("F") && readable("SC"), "baseline readable");
    CHECK(!acl.has_deleted_ancestor("C"), "baseline: C has no deleted ancestor");
    CHECK(!acl.has_deleted_ancestor("F"), "baseline: F has no deleted ancestor");
    std::cout << "  ✓ baseline reachable\n";

    // --- Soft-delete folder F: its whole subtree must vanish ---
    db->set_deleted("F", true);
    CHECK(acl.has_deleted_ancestor("C"), "C hidden by deleted parent F");
    CHECK(acl.has_deleted_ancestor("G"), "G hidden by deleted parent F");
    CHECK(acl.has_deleted_ancestor("C2"), "C2 hidden by deleted GRANDparent F (deep)");
    CHECK(!acl.has_deleted_ancestor("F"), "F's OWN deleted flag is not an ancestor of itself");
    CHECK(!acl.has_deleted_ancestor("SC"), "sibling subtree unaffected");

    CHECK(!readable("C"), "LEAK-PROOF: descendant C not readable under deleted F");
    CHECK(!readable("C2"), "LEAK-PROOF: deep descendant C2 not readable under deleted F");
    CHECK(!eff_read("C"), "LEAK-PROOF: effective perms collapse to none for C");
    CHECK(!eff_read("C2"), "LEAK-PROOF: effective perms collapse to none for C2");
    CHECK(readable("SC"), "sibling SC still readable");
    // F itself stays permission-reachable; its own deleted state is enforced at
    // the operation layer (get_file_by_uid), not the ancestor gate.
    CHECK(readable("F"), "F itself still permission-reachable (own-flag handled elsewhere)");
    std::cout << "  ✓ deleting F hides its entire subtree (and only its subtree)\n";

    // --- Superuser bypass: system_admin is never locked out by reachability ---
    auto admin_readable = [&](const std::string& uid) {
        return acl.check_permission(uid, "root", {"system_admin"}, READ, "").value;
    };
    CHECK(admin_readable("C"), "system_admin bypasses reachability hiding for C");
    CHECK(admin_readable("C2"), "system_admin bypasses reachability hiding for deep C2");
    CHECK((acl.get_effective_permissions("C", "root", {"system_admin"}).value & READ) == READ,
          "system_admin effective perms are full even under a deleted folder");
    std::cout << "  ✓ system_admin bypasses the deleted-subtree lock-out\n";

    // --- Independence: F's deletion did NOT rewrite any child's own flag ---
    CHECK(db->get_file_by_uid_include_deleted("C").value.value().deleted == false, "C's own deleted flag untouched");
    CHECK(db->get_file_by_uid_include_deleted("C2").value.value().deleted == false, "C2's own deleted flag untouched");
    std::cout << "  ✓ no blanket child delete — children keep their own state\n";

    // --- Undelete F: the subtree becomes reachable again, unchanged ---
    db->set_deleted("F", false);
    CHECK(readable("C") && readable("C2"), "undelete F restores subtree reachability");
    CHECK(db->get_file_by_uid_include_deleted("C").value.value().deleted == false, "C still not individually deleted");
    std::cout << "  ✓ undeleting F restores exactly the subtree\n";

    // --- A child deleted independently stays deleted across parent delete+undelete ---
    db->set_deleted("C2", true);               // C2 individually soft-deleted
    db->set_deleted("F", true);                // then the whole folder deleted
    db->set_deleted("F", false);               // ...and undeleted
    CHECK(db->get_file_by_uid_include_deleted("C2").value.value().deleted == true,
          "independently-deleted C2 stays deleted after parent delete+undelete");
    CHECK(readable("C"), "sibling C (never individually deleted) is reachable again");
    std::cout << "  ✓ child delete-state is independent of parent delete/undelete\n";

    std::cout << "\n✅ All " << g_checks << " leak-proof checks passed.\n";
    return 0;
}
