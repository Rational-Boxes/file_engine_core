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
//
// Subtree ACL application, against a real database.
//
// The substance of this feature is one SQL statement, so a mock proves nothing:
// the questions are whether the recursive CTE reaches every descendant, whether
// the conflict clause merges rather than replaces, whether an emptied row is
// deleted, and whether exactly ONE accountability record is written however many
// nodes were touched. All four are properties of the statement.

#include "fileengine/accountability.h"
#include "fileengine/acl_manager.h"
#include "fileengine/database.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace fileengine;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            std::cout << "  FAIL: " << (msg) << "  [" << __FILE__ << ":"        \
                      << __LINE__ << "]\n";                                     \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

namespace {

std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

AccountabilityContext ctx_for(const std::string& actor) {
    AccountabilityContext c;
    c.actor        = actor;
    c.source_iface = "test";
    c.source_addr  = "127.0.0.1";
    return c;
}

std::vector<std::string>& created_tenants() {
    static std::vector<std::string> t;
    return t;
}

std::string new_tenant(Database& db, const char* label) {
    static const std::string run = std::to_string(static_cast<long>(::getpid()));
    static int counter = 0;
    const std::string tenant = std::string("aclsub_") + label + "_" + run + "_" +
                               std::to_string(counter++);
    created_tenants().push_back(tenant);
    CHECK(db.create_tenant_schema(tenant, AccountabilityContext::system()).success,
          "tenant provisions");
    return tenant;
}

// root/
//   a/      (folder)
//     a1    (file)
//     a2    (file)
//   b/      (folder)
//     b1    (file)
// Returns the uids in creation order: root, a, a1, a2, b, b1.
std::vector<std::string> build_tree(Database& db, const std::string& tenant) {
    const std::string owner = "owner@example.com";
    auto mk = [&](const std::string& uid, const std::string& name,
                  const std::string& parent, FileType type) {
        auto r = db.insert_file(uid, name, "/" + name, parent, type, owner, 0, tenant);
        CHECK(r.success, "insert " + name + (r.success ? "" : ": " + r.error));
    };
    const std::string p = tenant + "-";
    mk(p + "root", "root", "",          FileType::DIRECTORY);
    mk(p + "a",    "a",    p + "root",  FileType::DIRECTORY);
    mk(p + "a1",   "a1",   p + "a",     FileType::REGULAR_FILE);
    mk(p + "a2",   "a2",   p + "a",     FileType::REGULAR_FILE);
    mk(p + "b",    "b",    p + "root",  FileType::DIRECTORY);
    mk(p + "b1",   "b1",   p + "b",     FileType::REGULAR_FILE);
    return {p + "root", p + "a", p + "a1", p + "a2", p + "b", p + "b1"};
}

int perms_of(Database& db, const std::string& uid, const std::string& principal,
             const std::string& tenant) {
    auto acls = db.get_acls_for_resource(uid, tenant);
    if (!acls.success) return -1;
    for (const auto& e : acls.value) {
        if (e.principal == principal) return e.permissions;
    }
    return 0;   // no row
}

const int kRead  = static_cast<int>(Permission::READ);
const int kWrite = static_cast<int>(Permission::WRITE);

// ── the tests ───────────────────────────────────────────────────────────────

void test_reaches_every_descendant(Database& db) {
    std::cout << "- a grant reaches every descendant, not just the root\n";
    const std::string tenant = new_tenant(db, "reach");
    auto uids = build_tree(db, tenant);

    auto r = db.add_acl_subtree(uids[0], "alice", 0, kRead, tenant, ctx_for("admin"), 0);
    CHECK(r.success, "subtree grant succeeds" + std::string(r.success ? "" : ": " + r.error));
    CHECK(r.value == 6, "six nodes written, got " + std::to_string(r.value));

    for (const auto& u : uids) {
        CHECK((perms_of(db, u, "alice", tenant) & kRead) != 0,
              "READ present on " + u);
    }
}

void test_merges_rather_than_replaces(Database& db) {
    std::cout << "- an existing rule keeps its bits (OR, not overwrite)\n";
    const std::string tenant = new_tenant(db, "merge");
    auto uids = build_tree(db, tenant);

    // One node already carries WRITE from an earlier, narrower decision.
    CHECK(db.add_acl(uids[2], "alice", 0, kWrite, tenant, ctx_for("admin"), 0).success,
          "seed a single-node WRITE");

    CHECK(db.add_acl_subtree(uids[0], "alice", 0, kRead, tenant, ctx_for("admin"), 0).success,
          "subtree READ");

    const int merged = perms_of(db, uids[2], "alice", tenant);
    CHECK((merged & kWrite) != 0, "the pre-existing WRITE survives the subtree grant");
    CHECK((merged & kRead) != 0,  "and READ was added");
}

void test_revoke_clears_and_prunes(Database& db) {
    std::cout << "- a revoke clears the bits and deletes rows left empty\n";
    const std::string tenant = new_tenant(db, "revoke");
    auto uids = build_tree(db, tenant);

    CHECK(db.add_acl_subtree(uids[0], "alice", 0, kRead | kWrite, tenant, ctx_for("admin"), 0).success,
          "grant READ|WRITE across the tree");
    auto rev = db.remove_acl_subtree(uids[0], "alice", 0, kWrite, tenant, ctx_for("admin"), 0);
    CHECK(rev.success, "subtree revoke succeeds");

    const int left = perms_of(db, uids[3], "alice", tenant);
    CHECK((left & kWrite) == 0, "WRITE is gone");
    CHECK((left & kRead) != 0,  "READ remains");

    // Now take the rest. The row must DISAPPEAR rather than linger at zero: this
    // system is read-by-default, so an ALLOW row with no bits and no row at all
    // must be indistinguishable, or a revoke leaves evidence that reads as a
    // grant.
    CHECK(db.remove_acl_subtree(uids[0], "alice", 0, kRead, tenant, ctx_for("admin"), 0).success,
          "revoke the remainder");
    auto acls = db.get_acls_for_resource(uids[3], tenant);
    CHECK(acls.success, "read back the ACLs");
    bool found_alice = false;
    for (const auto& e : acls.value) if (e.principal == "alice") found_alice = true;
    CHECK(!found_alice, "the emptied row was deleted, not left at zero");
}

void test_one_record_regardless_of_size(Database& db) {
    std::cout << "- ONE accountability record, however many nodes are touched\n";
    const std::string tenant = new_tenant(db, "record");
    auto uids = build_tree(db, tenant);

    bool has_more = false;
    const auto before = db.list_accountability_records(tenant, 0, 1000, has_more);
    CHECK(before.success, "read the chain before");
    const size_t n_before = before.success ? before.value.size() : 0;

    CHECK(db.add_acl_subtree(uids[0], "alice", 0, kRead, tenant, ctx_for("admin"), 0).success,
          "subtree grant");

    const auto after = db.list_accountability_records(tenant, 0, 1000, has_more);
    CHECK(after.success, "read the chain after");
    const size_t added = after.success ? after.value.size() - n_before : 0;
    CHECK(added == 1, "exactly one record for six nodes, got " + std::to_string(added));

    if (after.success && added == 1) {
        const auto& rec = after.value.back().record;
        CHECK(rec.action == std::string(accountability_action::kAclGrantSubtree),
              "recorded as acl.grant.subtree, got " + rec.action);
        CHECK(rec.target_uid == uids[0], "the record names the ROOT");
    }
}

void test_refuses_without_an_actor(Database& db) {
    std::cout << "- an unattributed recursive change is refused\n";
    const std::string tenant = new_tenant(db, "actor");
    auto uids = build_tree(db, tenant);
    AccountabilityContext empty;   // no actor
    auto r = db.add_acl_subtree(uids[0], "alice", 0, kRead, tenant, empty, 0);
    CHECK(!r.success, "refused");
    CHECK(perms_of(db, uids[1], "alice", tenant) == 0, "and nothing was written");
}

void test_deleted_nodes_are_skipped(Database& db) {
    std::cout << "- a soft-deleted node does not receive the grant\n";
    const std::string tenant = new_tenant(db, "deleted");
    auto uids = build_tree(db, tenant);
    CHECK(db.delete_file(uids[4], tenant).success, "soft-delete folder b");

    auto r = db.add_acl_subtree(uids[0], "alice", 0, kRead, tenant, ctx_for("admin"), 0);
    CHECK(r.success, "subtree grant");
    CHECK(perms_of(db, uids[4], "alice", tenant) == 0, "the deleted folder is skipped");
    CHECK(perms_of(db, uids[5], "alice", tenant) == 0,
          "and so is everything under it — an ACL on trashed content would "
          "resurrect with it");
    CHECK((perms_of(db, uids[2], "alice", tenant) & kRead) != 0,
          "while the live branch is unaffected");
}

}  // namespace

int main() {
    std::cout << "=== acl_subtree_live_tests ===\n";

    const std::string host = env_or("FILEENGINE_PG_HOST", env_or("FE_TEST_PG_HOST", "localhost"));
    const int port = std::stoi(env_or("FILEENGINE_PG_PORT", env_or("FE_TEST_PG_PORT", "5434")));
    const std::string name = env_or("FILEENGINE_PG_DATABASE", env_or("FE_TEST_PG_DB", "fileengine"));
    const std::string user = env_or("FILEENGINE_PG_USER", env_or("FE_TEST_PG_USER", "postgres"));
    const std::string pass = env_or("FILEENGINE_PG_PASSWORD", env_or("FE_TEST_PG_PASSWORD", "postgres"));

    Database db(host, port, name, user, pass, /*pool_size=*/8);
    if (!db.connect()) {
        std::cout << "SKIP: no Postgres at " << host << ":" << port
                  << " — set FILEENGINE_PG_* to point at one.\n";
        return 77;
    }
    if (!db.create_schema().success) {
        std::cout << "SKIP: could not create/verify the global schema.\n";
        return 77;
    }

    test_reaches_every_descendant(db);
    test_merges_rather_than_replaces(db);
    test_revoke_clears_and_prunes(db);
    test_one_record_regardless_of_size(db);
    test_refuses_without_an_actor(db);
    test_deleted_nodes_are_skipped(db);

    for (const auto& t : created_tenants()) {
        db.cleanup_tenant_data(t, ctx_for("test-teardown"));
    }

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
