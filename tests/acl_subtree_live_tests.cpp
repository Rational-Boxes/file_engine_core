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
#include <chrono>
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

void test_recent_files_newest_first(Database& db) {
    std::cout << "- recent files come back newest first, one row per file\n";
    const std::string tenant = new_tenant(db, "recent");
    auto uids = build_tree(db, tenant);

    // Three versions on a1, one on a2 — a1 must appear ONCE, at its newest.
    CHECK(db.insert_version(uids[2], "20260101_000000.000", 0, "/dev/null", "alice", tenant).success, "a1 v1");
    CHECK(db.insert_version(uids[3], "20260102_000000.000", 0, "/dev/null", "bob", tenant).success, "a2 v1");
    CHECK(db.insert_version(uids[2], "20260103_000000.000", 0, "/dev/null", "carol", tenant).success, "a1 v2");

    auto r = db.list_recent_files(tenant, "", 0, 50);
    CHECK(r.success, "query succeeds" + std::string(r.success ? "" : ": " + r.error));
    if (!r.success) return;

    CHECK(r.value.size() == 2, "two files, not three versions — got " +
                               std::to_string(r.value.size()));
    if (r.value.size() >= 2) {
        CHECK(r.value[0].uid == uids[2], "a1 is first (its newest version wins)");
        CHECK(r.value[0].version == "20260103_000000.000", "and it is the NEWEST version");
        CHECK(r.value[0].modified_by == "carol", "attributed to the latest reviser");
        CHECK(r.value[1].uid == uids[3], "a2 second");
    }
}

void test_recent_files_excludes_deleted_and_folders(Database& db) {
    std::cout << "- deleted files and folders are not 'recent activity'\n";
    const std::string tenant = new_tenant(db, "recentdel");
    auto uids = build_tree(db, tenant);
    CHECK(db.insert_version(uids[2], "20260101_000000.000", 0, "/dev/null", "alice", tenant).success, "a1 v1");
    CHECK(db.insert_version(uids[5], "20260102_000000.000", 0, "/dev/null", "bob", tenant).success, "b1 v1");
    CHECK(db.delete_file(uids[5], tenant).success, "soft-delete b1");

    auto r = db.list_recent_files(tenant, "", 0, 50);
    CHECK(r.success, "query succeeds");
    if (!r.success) return;
    for (const auto& f : r.value) {
        CHECK(f.uid != uids[5], "the deleted file is absent");
        CHECK(f.uid != uids[1] && f.uid != uids[4] && f.uid != uids[0],
              "no folders — a folder has no version of its own");
    }
    CHECK(r.value.size() == 1, "only the one live versioned file, got " +
                               std::to_string(r.value.size()));
}

void test_recent_files_scoped_to_a_subtree(Database& db) {
    std::cout << "- under_uid restricts to a subtree\n";
    const std::string tenant = new_tenant(db, "recentsub");
    auto uids = build_tree(db, tenant);
    CHECK(db.insert_version(uids[2], "20260101_000000.000", 0, "/dev/null", "alice", tenant).success, "a1 v1");
    CHECK(db.insert_version(uids[5], "20260102_000000.000", 0, "/dev/null", "bob", tenant).success, "b1 v1");

    auto r = db.list_recent_files(tenant, uids[1] /* folder a */, 0, 50);
    CHECK(r.success, "query succeeds");
    if (!r.success) return;
    CHECK(r.value.size() == 1, "only a's subtree, got " + std::to_string(r.value.size()));
    if (!r.value.empty()) CHECK(r.value[0].uid == uids[2], "and it is a1");
}

void test_recent_files_since_bound(Database& db) {
    std::cout << "- since_epoch drops anything older\n";
    const std::string tenant = new_tenant(db, "recentsince");
    auto uids = build_tree(db, tenant);
    CHECK(db.insert_version(uids[2], "20260101_000000.000", 0, "/dev/null", "alice", tenant).success, "old");
    CHECK(db.insert_version(uids[3], "20260601_000000.000", 0, "/dev/null", "bob", tenant).success, "new");

    // 2026-03-01T00:00:00Z
    auto r = db.list_recent_files(tenant, "", 1772323200LL, 50);
    CHECK(r.success, "query succeeds");
    if (!r.success) return;
    CHECK(r.value.size() == 1, "only the newer one, got " + std::to_string(r.value.size()));
    if (!r.value.empty()) CHECK(r.value[0].uid == uids[3], "and it is a2");
}

void test_recent_files_scan_limit_is_honoured(Database& db) {
    std::cout << "- the scan bound caps the work\n";
    const std::string tenant = new_tenant(db, "recentcap");
    auto uids = build_tree(db, tenant);
    CHECK(db.insert_version(uids[2], "20260101_000000.000", 0, "/dev/null", "alice", tenant).success, "a1");
    CHECK(db.insert_version(uids[3], "20260102_000000.000", 0, "/dev/null", "bob", tenant).success, "a2");
    CHECK(db.insert_version(uids[5], "20260103_000000.000", 0, "/dev/null", "carol", tenant).success, "b1");

    auto r = db.list_recent_files(tenant, "", 0, 2);
    CHECK(r.success, "query succeeds");
    CHECK(r.success && r.value.size() == 2, "exactly the scan limit, got " +
          std::to_string(r.success ? r.value.size() : 0));
}

// ── folder mtime ────────────────────────────────────────────────────────────
//
// A folder's mtime is the newest version anywhere beneath it. It used to be
// recomputed by descending the whole subtree on EVERY read — 94% of a four-core
// host during a bulk import. It is now memoised on the folder row, pushed up on
// write, and forgotten where the newest can move backwards.
//
// The risk a memo introduces is that it lies. These tests compare it against a
// fresh walk rather than against an expected constant, because the property that
// matters is agreement, not any particular value.

std::int64_t folder_mtime(Database& db, const std::string& uid, const std::string& tenant) {
    auto f = db.get_file_by_uid(uid, tenant);
    if (!f.success || !f.value.has_value()) return -1;
    return std::chrono::duration_cast<std::chrono::seconds>(
               f.value->modified_at.time_since_epoch()).count();
}

void test_folder_mtime_follows_the_newest_version(Database& db) {
    std::cout << "- a new version moves every ancestor's mtime\n";
    const std::string tenant = new_tenant(db, "mtime");
    auto uids = build_tree(db, tenant);

    CHECK(db.insert_version(uids[2], "20260101_000000.000", 0, "/dev/null", "alice", tenant).success, "a1 v1");
    const std::int64_t after_first_a = folder_mtime(db, uids[1], tenant);
    const std::int64_t after_first_root = folder_mtime(db, uids[0], tenant);
    CHECK(after_first_a > 0, "folder a has an mtime");
    CHECK(after_first_root == after_first_a, "and it reached the root too");

    CHECK(db.insert_version(uids[3], "20260601_000000.000", 0, "/dev/null", "bob", tenant).success, "a2 v1");
    CHECK(folder_mtime(db, uids[1], tenant) > after_first_a,
          "a newer version moved the folder forward");
    CHECK(folder_mtime(db, uids[0], tenant) > after_first_root,
          "and moved the root forward");
}

void test_an_older_version_does_not_move_it_backwards(Database& db) {
    std::cout << "- an out-of-order arrival cannot move it backwards\n";
    const std::string tenant = new_tenant(db, "mtimeback");
    auto uids = build_tree(db, tenant);
    CHECK(db.insert_version(uids[2], "20260601_000000.000", 0, "/dev/null", "alice", tenant).success, "new");
    const std::int64_t newest = folder_mtime(db, uids[1], tenant);
    CHECK(db.insert_version(uids[3], "20260101_000000.000", 0, "/dev/null", "bob", tenant).success, "older");
    CHECK(folder_mtime(db, uids[1], tenant) == newest,
          "the folder still reports the NEWEST version, not the last written");
}

void test_the_memo_agrees_with_a_fresh_walk(Database& db) {
    std::cout << "- the memo agrees with recomputing from scratch\n";
    const std::string tenant = new_tenant(db, "mtimeagree");
    auto uids = build_tree(db, tenant);
    CHECK(db.insert_version(uids[2], "20260101_000000.000", 0, "/dev/null", "alice", tenant).success, "a1");
    CHECK(db.insert_version(uids[5], "20260301_000000.000", 0, "/dev/null", "bob",   tenant).success, "b1");

    const std::int64_t memoised = folder_mtime(db, uids[0], tenant);

    // Forget it the way a delete would, then read again: the value must be
    // rebuilt identically. A memo that survives this is one that cannot drift
    // silently.
    CHECK(db.forget_subtree_mtime(uids[0], tenant).success, "forget the memo");
    const std::int64_t recomputed = folder_mtime(db, uids[0], tenant);
    CHECK(memoised == recomputed,
          "memoised " + std::to_string(memoised) + " vs recomputed " +
          std::to_string(recomputed));
}

void test_deleting_the_newest_moves_the_folder_back(Database& db) {
    std::cout << "- deleting the newest file moves the folder's mtime back\n";
    const std::string tenant = new_tenant(db, "mtimedel");
    auto uids = build_tree(db, tenant);
    CHECK(db.insert_version(uids[2], "20260101_000000.000", 0, "/dev/null", "alice", tenant).success, "a1 old");
    CHECK(db.insert_version(uids[3], "20260601_000000.000", 0, "/dev/null", "bob",   tenant).success, "a2 new");
    const std::int64_t with_newest = folder_mtime(db, uids[1], tenant);

    CHECK(db.delete_file(uids[3], tenant).success, "delete the newest");
    const std::int64_t after = folder_mtime(db, uids[1], tenant);
    CHECK(after < with_newest,
          "the folder no longer reports content it does not contain (" +
          std::to_string(after) + " vs " + std::to_string(with_newest) + ")");
}

void test_moving_a_file_updates_both_folders(Database& db) {
    std::cout << "- a move updates the folder it left and the one it joined\n";
    const std::string tenant = new_tenant(db, "mtimemove");
    auto uids = build_tree(db, tenant);
    CHECK(db.insert_version(uids[2], "20260101_000000.000", 0, "/dev/null", "alice", tenant).success, "a1");
    CHECK(db.insert_version(uids[3], "20260601_000000.000", 0, "/dev/null", "bob",   tenant).success, "a2 (newest)");
    CHECK(db.insert_version(uids[5], "20260201_000000.000", 0, "/dev/null", "carol", tenant).success, "b1");

    const std::int64_t a_before = folder_mtime(db, uids[1], tenant);
    const std::int64_t b_before = folder_mtime(db, uids[4], tenant);

    // Move the newest file out of a and into b.
    CHECK(db.update_file_parent(uids[3], uids[4], tenant).success, "move a2 -> b");

    const std::int64_t a_after = folder_mtime(db, uids[1], tenant);
    const std::int64_t b_after = folder_mtime(db, uids[4], tenant);
    CHECK(a_after < a_before, "the source folder went back — it lost its newest");
    CHECK(b_after > b_before, "the destination folder moved forward");
}

}  // namespace

int main() {
    std::cout << "=== acl_subtree + recent_files live tests ===\n";

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

    test_recent_files_newest_first(db);
    test_recent_files_excludes_deleted_and_folders(db);
    test_recent_files_scoped_to_a_subtree(db);
    test_recent_files_since_bound(db);
    test_recent_files_scan_limit_is_honoured(db);

    test_folder_mtime_follows_the_newest_version(db);
    test_an_older_version_does_not_move_it_backwards(db);
    test_the_memo_agrees_with_a_fresh_walk(db);
    test_deleting_the_newest_moves_the_folder_back(db);
    test_moving_a_file_updates_both_folders(db);

    for (const auto& t : created_tenants()) {
        db.cleanup_tenant_data(t, ctx_for("test-teardown"));
    }

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
