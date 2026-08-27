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

// The accountability guarantees that only a real database can demonstrate
// (design_documents/PROPOSAL_accountability_record.md §8, acceptance 1-7).
//
// Mocks cannot prove any of this. The whole claim is about TRANSACTIONS —
// "the operation and its record commit together or not at all" — plus a
// commit-ordered sequence under genuine concurrency and a clock that misbehaves.
// A mock that records both in memory would pass while proving nothing.
//
// Skips cleanly (exit 77, which ctest maps to SKIPPED) when Postgres is not
// reachable, so it is safe to run anywhere.

#include "fileengine/accountability.h"
#include "fileengine/acl_manager.h"
#include "fileengine/database.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <thread>
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

AccountabilityContext ctx_for(const std::string& actor,
                              std::vector<std::string> roles = {}) {
    AccountabilityContext c;
    c.actor        = actor;
    c.actor_roles  = std::move(roles);
    c.source_iface = "test";
    c.source_addr  = "127.0.0.1";
    return c;
}

// Each test gets its own tenant so a failure cannot cascade through a shared
// chain, and so the per-tenant independence claims (§4.3.5) are exercised by
// construction rather than asserted separately.
//
// The name includes the pid: these tests assert on exact record COUNTS, and
// create_tenant_schema is idempotent, so a name reused across runs would inherit
// the previous run's chain and every count assertion would be wrong on the
// second run only — the worst shape of flake to debug.
std::string unique_tenant(const char* label) {
    static const std::string run_id = std::to_string(static_cast<long>(::getpid()));
    static std::atomic<int> counter{0};
    return std::string("acct_") + label + "_" + run_id + "_" +
           std::to_string(counter.fetch_add(1));
}

// Tenants this run created, so they can be dropped at the end. Left behind, they
// would accumulate a schema per run in a shared dev database.
std::vector<std::string>& created_tenants() {
    static std::vector<std::string> tenants;
    return tenants;
}

std::string new_tenant(Database& db, const char* label) {
    const std::string tenant = unique_tenant(label);
    created_tenants().push_back(tenant);
    CHECK(db.create_tenant_schema(tenant, AccountabilityContext::system()).success,
          "tenant provisions");
    return tenant;
}

struct Row {
    long long   seq = 0;
    long long   ts_micros = 0;
    std::string actor;
    std::string category;
    std::string action;
    std::string target_uid;
    std::string principal;
    std::string detail;
};

// Read the raw table rather than going through list_accountability_records, so a
// bug in the read path cannot mask a bug in the write path.
std::vector<Row> read_rows(Database& db, const std::string& tenant) {
    const std::string schema = "tenant_" + tenant;
    auto result = db.query(
        "SELECT seq, "
        "  (EXTRACT(EPOCH FROM date_trunc('second', ts))::bigint * 1000000 "
        "   + (EXTRACT(MICROSECONDS FROM ts)::bigint % 1000000)), "
        "  actor, category, action, COALESCE(target_uid, ''), "
        "  COALESCE(principal, ''), detail::text "
        "FROM \"" + schema + "\".accountability_record ORDER BY seq ASC;", tenant);
    std::vector<Row> rows;
    if (!result.success) return rows;
    for (const auto& r : result.value) {
        if (r.size() < 8) continue;
        rows.push_back({std::stoll(r[0]), std::stoll(r[1]), r[2], r[3], r[4], r[5], r[6], r[7]});
    }
    return rows;
}

long long acl_row_count(Database& db, const std::string& tenant,
                        const std::string& resource_uid) {
    const std::string schema = "tenant_" + tenant;
    auto result = db.query("SELECT COUNT(*) FROM \"" + schema + "\".acls "
                           "WHERE resource_uid = '" + resource_uid + "';", tenant);
    if (!result.success || result.value.empty() || result.value[0].empty()) return -1;
    return std::stoll(result.value[0][0]);
}

}  // namespace

// ── acceptance 1: every in-scope operation writes exactly one attributed row ──

static void test_each_operation_writes_one_attributed_record(Database& db) {
    std::cout << "test_each_operation_writes_one_attributed_record\n";
    const std::string tenant = new_tenant(db, "ops");

    CHECK(db.add_acl("res-1", "bob", 0, 0x400, tenant, ctx_for("alice")).success,
          "grant succeeds");
    CHECK(db.remove_acl("res-1", "bob", 0, 0x400, tenant, ctx_for("alice")).success,
          "revoke succeeds");
    CHECK(db.create_role("editors", tenant, ctx_for("alice")).success, "role created");
    CHECK(db.assign_user_to_role("bob", "editors", tenant, ctx_for("alice")).success,
          "membership assigned");
    CHECK(db.remove_user_from_role("bob", "editors", tenant, ctx_for("alice")).success,
          "membership removed");
    CHECK(db.delete_role("editors", tenant, ctx_for("alice")).success, "role deleted");

    auto rows = read_rows(db, tenant);
    CHECK(rows.size() == 6, "six operations produced six records");
    if (rows.size() != 6) return;

    const char* expected[] = {"acl.grant", "acl.revoke", "role.create",
                              "role.assign", "role.remove", "role.delete"};
    for (size_t i = 0; i < 6; ++i) {
        CHECK(rows[i].action == expected[i],
              std::string("record ") + std::to_string(i) + " is " + expected[i]);
        // §5.1: never empty. An unattributed record looks like coverage.
        CHECK(rows[i].actor == "alice", "the record names the actor, never blank");
    }
    CHECK(rows[0].category == "authorization", "grant is an authorization event");
    CHECK(rows[2].category == "identity", "role management is an identity event");
    CHECK(rows[0].principal == "bob", "the grant records whose access changed");

    // The revoke that emptied the row records THAT it destroyed it — the §2.1
    // failure mode is a revoke whose evidence disappears with the row.
    CHECK(rows[1].detail.find("row_removed") != std::string::npos &&
          rows[1].detail.find("true") != std::string::npos,
          "the revoke records that the ACL row itself was destroyed");
    CHECK(acl_row_count(db, tenant, "res-1") == 0,
          "and the row really is gone, so the record is the only evidence");
}

// ── acceptance 2: atomicity, both directions ────────────────────────────────

static void test_a_failed_record_rolls_back_the_operation(Database& db) {
    std::cout << "test_a_failed_record_rolls_back_the_operation\n";
    const std::string tenant = new_tenant(db, "atomic");

    // Make the record impossible to write, then attempt a grant. Dropping the
    // chain head is the cleanest way to break only the accountability write
    // while leaving the ACL write perfectly capable of succeeding — which is
    // exactly the situation the guarantee has to survive.
    const std::string schema = "tenant_" + tenant;
    CHECK(db.execute("DROP TABLE \"" + schema + "\".accountability_chain_head;", tenant).success,
          "chain head removed to force a record failure");

    auto result = db.add_acl("res-atomic", "bob", 0, 0x400, tenant, ctx_for("alice"));
    CHECK(!result.success, "the grant FAILS when its record cannot be written");
    CHECK(result.error.find("refused") != std::string::npos,
          "and says it was refused rather than reporting a bare DB error");

    // The real assertion: the ACL change is not visible. Fail-closed means the
    // permission change never happened, not that it happened un-recorded.
    CHECK(acl_row_count(db, tenant, "res-atomic") == 0,
          "no ACL row survives a refused grant");
}

static void test_a_failed_operation_writes_no_record(Database& db) {
    std::cout << "test_a_failed_operation_writes_no_record\n";
    const std::string tenant = new_tenant(db, "atomic2");

    // Break the operation's own table instead. The record must not survive it —
    // a chain entry claiming a grant that never landed is as bad as a missing one.
    const std::string schema = "tenant_" + tenant;
    CHECK(db.execute("ALTER TABLE \"" + schema + "\".acls RENAME TO acls_hidden;", tenant).success,
          "acls table hidden to force an operation failure");

    auto result = db.add_acl("res-x", "bob", 0, 0x400, tenant, ctx_for("alice"));
    CHECK(!result.success, "the grant fails");
    CHECK(read_rows(db, tenant).empty(), "and no record claims it happened");

    CHECK(db.execute("ALTER TABLE \"" + schema + "\".acls_hidden RENAME TO acls;", tenant).success,
          "restore");
}

static void test_an_actorless_operation_is_refused(Database& db) {
    std::cout << "test_an_actorless_operation_is_refused\n";
    const std::string tenant = new_tenant(db, "actor");

    AccountabilityContext anonymous;   // no actor
    auto result = db.add_acl("res-anon", "bob", 0, 0x400, tenant, anonymous);
    CHECK(!result.success, "a grant that cannot name its actor is refused");
    CHECK(acl_row_count(db, tenant, "res-anon") == 0,
          "and does not silently apply un-attributed");
    CHECK(read_rows(db, tenant).empty(), "no anonymous record is written either");
}

// ── acceptance 3: creation-time ACLs stay off the chain (§4.1, §5.3.2) ──────

static void test_creation_default_acls_do_not_chain(Database& db) {
    std::cout << "test_creation_default_acls_do_not_chain\n";
    const std::string tenant = new_tenant(db, "defaults");

    auto shared_db = std::shared_ptr<IDatabase>(&db, [](IDatabase*) {});
    AclManager acl(shared_db);
    CHECK(acl.apply_default_acls("res-new", "alice", tenant).success,
          "creation-time defaults apply");

    // The ACL rows exist...
    CHECK(acl_row_count(db, tenant, "res-new") > 0, "the default ACL row was written");
    // ...but nothing was chained. Creation-time ACLs are attributed by the
    // resource's own row; chaining them would put the per-tenant serializing
    // lock on the content-creation path, which is what acceptance 17 forbids.
    CHECK(read_rows(db, tenant).empty(),
          "creating a resource does not take the accountability chain lock");

    // A deliberate grant on the same resource DOES chain — so the distinction is
    // about the act, not about the resource or the table.
    CHECK(db.add_acl("res-new", "carol", 0, 0x200, tenant, ctx_for("alice")).success,
          "a deliberate grant succeeds");
    CHECK(read_rows(db, tenant).size() == 1, "and is chained");
}

// ── acceptance 5: the record outlives the cull, and records it ──────────────

static void test_cull_is_recorded_and_history_survives_it(Database& db) {
    std::cout << "test_cull_is_recorded_and_history_survives_it\n";
    const std::string tenant = new_tenant(db, "cull");

    // An earlier authorization change on the same object.
    CHECK(db.add_acl("doc-1", "bob", 0, 0x400, tenant, ctx_for("alice")).success,
          "earlier grant");

    std::vector<std::string> versions = {"v3", "v2", "v1"};   // newest first
    CHECK(db.insert_version("doc-1", "v1", 10, "/p/v1", "alice", tenant).success, "v1");
    CHECK(db.insert_version("doc-1", "v2", 10, "/p/v2", "alice", tenant).success, "v2");
    CHECK(db.insert_version("doc-1", "v3", 10, "/p/v3", "alice", tenant).success, "v3");

    auto purged = db.purge_versions("doc-1", {"v2", "v1"}, "v3", 1, tenant, ctx_for("alice"));
    CHECK(purged.success && purged.value == 2, "two versions culled");

    auto rows = read_rows(db, tenant);
    CHECK(rows.size() == 2, "the cull added a record; the earlier one is still there");
    if (rows.size() != 2) return;
    // §5.2: culling erases history by design, so a record that vanished with it
    // would erase the evidence OF the cull.
    CHECK(rows[0].action == "acl.grant", "the pre-cull record survives the cull");
    CHECK(rows[1].action == "cull.versions" && rows[1].category == "destruction",
          "and the cull itself is recorded as a destruction");
    CHECK(rows[1].detail.find("\"keep_count\": 1") != std::string::npos ||
          rows[1].detail.find("\"keep_count\":1") != std::string::npos,
          "naming the keep-count");
    CHECK(rows[1].detail.find("v3") != std::string::npos, "and the resulting cut");
    CHECK(rows[1].target_uid == "doc-1", "against the right object");

    // One record for the batch, not one per destroyed version (§5.3.2).
    CHECK(rows[1].detail.find("\"versions_removed\": 2") != std::string::npos ||
          rows[1].detail.find("\"versions_removed\":2") != std::string::npos,
          "a bulk destruction is ONE record describing the batch");
}

static void test_a_cull_that_removes_nothing_records_nothing(Database& db) {
    std::cout << "test_a_cull_that_removes_nothing_records_nothing\n";
    const std::string tenant = new_tenant(db, "nocull");
    auto purged = db.purge_versions("doc-none", {}, "v1", 1, tenant, ctx_for("alice"));
    CHECK(purged.success && purged.value == 0, "nothing to cull");
    CHECK(read_rows(db, tenant).empty(),
          "an immutable chain must not carry a claim that something was destroyed "
          "when nothing was");
}

// ── acceptance 7: the chain and ordering hold under concurrency ─────────────

static void test_chain_is_gap_free_and_ordered_under_concurrency(Database& db) {
    std::cout << "test_chain_is_gap_free_and_ordered_under_concurrency\n";
    const std::string tenant = new_tenant(db, "concurrent");

    // This is the test a BIGSERIAL seq would fail (§5.3.1): concurrent writers
    // take values at INSERT and make them visible at COMMIT, so 10 can commit
    // before 9 and a cursor reader loses 9 permanently. Rolled-back transactions
    // also burn values, making gaps routine and gap detection useless.
    constexpr int kWriters = 8;
    constexpr int kPerWriter = 12;
    std::atomic<int> failures{0};
    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([&, w] {
            for (int i = 0; i < kPerWriter; ++i) {
                const std::string resource = "res-" + std::to_string(w) + "-" + std::to_string(i);
                if (!db.add_acl(resource, "bob", 0, 0x400, tenant,
                                ctx_for("writer" + std::to_string(w))).success) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    for (auto& t : writers) t.join();
    CHECK(failures.load() == 0, "every concurrent grant succeeded");

    auto rows = read_rows(db, tenant);
    CHECK(rows.size() == static_cast<size_t>(kWriters * kPerWriter),
          "every write produced exactly one record");

    // Gap-free: a rollback releases the lock WITHOUT advancing last_seq, so
    // numbers are never burned. That is what makes a gap an unambiguous
    // integrity alarm rather than routine noise a consumer has to tolerate.
    bool contiguous = true, strictly_increasing_ts = true;
    std::set<long long> seen_ts;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].seq != static_cast<long long>(i) + 1) contiguous = false;
        if (i > 0 && rows[i].ts_micros <= rows[i - 1].ts_micros) strictly_increasing_ts = false;
        seen_ts.insert(rows[i].ts_micros);
    }
    CHECK(contiguous, "seq is contiguous from 1 with no gaps and no duplicates");
    CHECK(strictly_increasing_ts, "ts is strictly increasing and matches seq order");
    CHECK(seen_ts.size() == rows.size(),
          "no two records share a ts, so a ts cursor needs no tiebreak");
}

static void test_chain_links_verify_end_to_end(Database& db) {
    std::cout << "test_chain_links_verify_end_to_end\n";
    const std::string tenant = new_tenant(db, "chain");
    for (int i = 0; i < 5; ++i) {
        CHECK(db.add_acl("res-" + std::to_string(i), "bob", 0, 0x400, tenant,
                         ctx_for("alice", {"editors"})).success, "grant");
    }

    bool has_more = false;
    auto records = db.list_accountability_records(tenant, 0, 100, has_more);
    CHECK(records.success, "the pull endpoint reads them back");
    CHECK(records.value.size() == 5, "all five");
    CHECK(!has_more, "and says there are no more");

    // Recompute the whole chain from the returned values, exactly as the
    // consumer does. This is the check that catches a tampered row AND a
    // transport that reorders, duplicates or drops — one mechanism, both jobs.
    std::vector<std::uint8_t> prev;
    bool chain_ok = true, links_ok = true;
    for (size_t i = 0; i < records.value.size(); ++i) {
        const auto& r = records.value[i];
        if (r.prev_hash != prev) links_ok = false;
        const auto expected = chain_hash(prev, canonical_record(r.seq, r.ts_micros, r.record));
        if (expected != r.hash) chain_ok = false;
        prev = r.hash;
    }
    CHECK(links_ok, "prev_hash[n] == hash[n-1] all the way down");
    CHECK(chain_ok, "and every row's hash recomputes from the row");

    // The first row's prev_hash is genuinely empty, not a hash of nothing.
    CHECK(records.value[0].prev_hash.empty(), "the chain starts unlinked");

    // Tamper with a committed row and prove the recomputation catches it.
    const std::string schema = "tenant_" + tenant;
    CHECK(db.execute("UPDATE \"" + schema + "\".accountability_record "
                     "SET actor = 'mallory' WHERE seq = 3;", tenant).success, "tamper");
    auto after = db.list_accountability_records(tenant, 0, 100, has_more);
    CHECK(after.success, "still readable");
    bool detected = false;
    prev.clear();
    for (const auto& r : after.value) {
        if (chain_hash(prev, canonical_record(r.seq, r.ts_micros, r.record)) != r.hash) {
            detected = true;
        }
        prev = r.hash;
    }
    CHECK(detected, "editing a committed row is detected on the next read");
}

// ── acceptance 7: clock hostility (§5.3.3) ─────────────────────────────────

static void test_ts_survives_a_backwards_clock_step(Database& db) {
    std::cout << "test_ts_survives_a_backwards_clock_step\n";
    const std::string tenant = new_tenant(db, "clock");

    CHECK(db.add_acl("res-a", "bob", 0, 0x400, tenant, ctx_for("alice")).success, "first");

    // A test cannot step the machine's clock, but it can create the state a
    // backwards step produces: a head whose last_ts is in the FUTURE relative to
    // clock_timestamp(). That is precisely what the append then has to survive —
    // an NTP correction, a VM migration, a leap-second smear.
    const std::string schema = "tenant_" + tenant;
    CHECK(db.execute("UPDATE \"" + schema + "\".accountability_chain_head "
                     "SET last_ts = clock_timestamp() + interval '1 hour';", tenant).success,
          "head timestamp pushed an hour into the future");

    for (int i = 0; i < 3; ++i) {
        CHECK(db.add_acl("res-after-" + std::to_string(i), "bob", 0, 0x400, tenant,
                         ctx_for("alice")).success, "writes continue across the step");
    }

    auto rows = read_rows(db, tenant);
    CHECK(rows.size() == 4, "every write landed");
    bool strictly_increasing = true, contiguous = true;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].seq != static_cast<long long>(i) + 1) contiguous = false;
        if (i > 0 && rows[i].ts_micros <= rows[i - 1].ts_micros) strictly_increasing = false;
    }
    CHECK(strictly_increasing,
          "ts still increases strictly — the monotonic guard, not luck about NTP");
    CHECK(contiguous, "and no record was skipped or duplicated");

    // The guard advances by the minimum, so time does not run away.
    CHECK(rows[3].ts_micros - rows[1].ts_micros <= 10,
          "the guard steps by microseconds, not by re-reading a wrong clock");
}

// ── acceptance 6: queryable from the core alone ─────────────────────────────

static void test_the_core_can_answer_accountability_questions_alone(Database& db) {
    std::cout << "test_the_core_can_answer_accountability_questions_alone\n";
    const std::string tenant = new_tenant(db, "query");

    CHECK(db.add_acl("doc-q", "bob", 0, 0x400, tenant, ctx_for("alice")).success, "grant");
    CHECK(db.add_acl("doc-q", "carol", 0, 0x400, tenant, ctx_for("dave")).success, "grant");
    CHECK(db.remove_acl("doc-q", "bob", 0, 0x400, tenant, ctx_for("dave")).success, "revoke");

    const std::string schema = "tenant_" + tenant;
    // "every authorization change affecting principal P"
    auto by_principal = db.query(
        "SELECT action FROM \"" + schema + "\".accountability_record "
        "WHERE principal = 'bob' ORDER BY seq;", tenant);
    CHECK(by_principal.success && by_principal.value.size() == 2,
          "'every change affecting bob' is answerable with no audit_service and no Redis");

    // "everything actor A did"
    auto by_actor = db.query(
        "SELECT action FROM \"" + schema + "\".accountability_record "
        "WHERE actor = 'dave' ORDER BY seq;", tenant);
    CHECK(by_actor.success && by_actor.value.size() == 2, "'everything dave did' likewise");
}

// ── the pull cursor (§4.3) ─────────────────────────────────────────────────

static void test_cursor_reads_forward_exactly_once(Database& db) {
    std::cout << "test_cursor_reads_forward_exactly_once\n";
    const std::string tenant = new_tenant(db, "cursor");
    for (int i = 0; i < 7; ++i) {
        CHECK(db.add_acl("res-" + std::to_string(i), "bob", 0, 0x400, tenant,
                         ctx_for("alice")).success, "grant");
    }

    // Drain in small pages the way a consumer does, advancing recorded_until to
    // the ts of the last record it appended.
    long long recorded_until = 0;
    std::vector<long long> drained;
    bool has_more = true;
    int guard = 0;
    while (has_more && guard++ < 20) {
        auto page = db.list_accountability_records(tenant, recorded_until, 3, has_more);
        CHECK(page.success, "page reads");
        if (!page.success) break;
        for (const auto& r : page.value) {
            drained.push_back(r.seq);
            recorded_until = r.ts_micros;
        }
    }
    CHECK(drained.size() == 7, "every record was delivered");
    bool exactly_once = true;
    for (size_t i = 0; i < drained.size(); ++i) {
        if (drained[i] != static_cast<long long>(i) + 1) exactly_once = false;
    }
    CHECK(exactly_once, "exactly once each, in order, with no skips or repeats");

    // At the watermark there is nothing new — and asking again is not an error.
    auto empty = db.list_accountability_records(tenant, recorded_until, 10, has_more);
    CHECK(empty.success && empty.value.empty() && !has_more,
          "a caught-up consumer sees nothing new");

    // A reset cursor replays the whole history — impossible when the record's
    // only home was a stream that trims (§4.3).
    auto replay = db.list_accountability_records(tenant, 0, 100, has_more);
    CHECK(replay.success && replay.value.size() == 7,
          "a consumer with a reset cursor replays core history from zero");

    auto head = db.accountability_head_seq(tenant);
    CHECK(head.success && head.value == 7,
          "the head seq matches, so a hint can be checked for staleness");
}

static void test_detail_survives_the_read_path_byte_for_byte(Database& db) {
    std::cout << "test_detail_survives_the_read_path_byte_for_byte\n";
    const std::string tenant = new_tenant(db, "detail");
    CHECK(db.add_acl("res-d", "bob", 0, 0x400, tenant, ctx_for("alice")).success, "grant");

    bool has_more = false;
    auto records = db.list_accountability_records(tenant, 0, 10, has_more);
    CHECK(records.success && records.value.size() == 1, "read back");
    if (records.value.size() != 1) return;

    // Postgres normalizes JSONB into its own key order and spacing. If the read
    // path handed that back, the consumer's re-hash would fail on every single
    // record — so this is the test that the re-canonicalization is exact.
    const auto& r = records.value[0];
    const auto expected = chain_hash(r.prev_hash, canonical_record(r.seq, r.ts_micros, r.record));
    CHECK(expected == r.hash,
          "the detail returned by the read path reproduces the stored hash");
    CHECK(r.record.detail.canonical_json().find(' ') == std::string::npos,
          "and comes back in canonical form, not Postgres's JSONB spacing");
}

// ── tenant lifecycle on the global chain (§7.3) ────────────────────────────

static void test_tenant_lifecycle_is_recorded_globally(Database& db) {
    std::cout << "test_tenant_lifecycle_is_recorded_globally\n";
    const std::string tenant = unique_tenant("lifecycle");
    created_tenants().push_back(tenant);

    bool has_more = false;
    auto before = db.list_accountability_records(kGlobalChainKey, 0, 5000, has_more);
    CHECK(before.success, "the global chain is readable");
    const size_t baseline = before.value.size();

    CHECK(db.create_tenant_schema(tenant, ctx_for("root", {"system_admin"})).success,
          "tenant created");

    auto after_create = db.list_accountability_records(kGlobalChainKey, 0, 5000, has_more);
    CHECK(after_create.success && after_create.value.size() == baseline + 1,
          "creating a tenant adds exactly one global record");
    const auto& created = after_create.value.back();
    CHECK(created.record.action == "tenant.create", "recorded as tenant.create");
    CHECK(created.record.global_tenant == tenant, "naming the tenant");
    CHECK(created.record.ctx.actor == "root", "and the operator");

    // Called again for an existing tenant — this happens on nearly every context
    // lookup — and must NOT append a duplicate creation record.
    CHECK(db.create_tenant_schema(tenant, ctx_for("root")).success, "idempotent re-provision");
    auto after_repeat = db.list_accountability_records(kGlobalChainKey, 0, 5000, has_more);
    CHECK(after_repeat.success && after_repeat.value.size() == baseline + 1,
          "re-provisioning an existing tenant records nothing");

    // Put something in the tenant's own chain, then destroy the tenant.
    CHECK(db.add_acl("res-l", "bob", 0, 0x400, tenant, ctx_for("alice")).success, "grant");
    CHECK(read_rows(db, tenant).size() == 1, "the tenant has its own history");

    CHECK(db.cleanup_tenant_data(tenant, ctx_for("root", {"system_admin"})).success,
          "tenant destroyed");

    auto after_delete = db.list_accountability_records(kGlobalChainKey, 0, 5000, has_more);
    CHECK(after_delete.success && after_delete.value.size() == baseline + 2,
          "the deletion is recorded globally");
    const auto& deleted = after_delete.value.back();
    CHECK(deleted.record.action == "tenant.delete", "recorded as tenant.delete");
    CHECK(deleted.record.category == AccountabilityCategory::Destruction,
          "as a destruction");
    CHECK(deleted.record.global_tenant == tenant, "naming the tenant");
    CHECK(deleted.record.ctx.actor == "root", "and who removed it");

    // The tenant's own history went with it — that is the §7.3 decision, not a
    // bug. What survives is the fact, in a place the deleter cannot reach.
    CHECK(read_rows(db, tenant).empty(),
          "the tenant's own accountability history is destroyed with the tenant");

    // The global chain still verifies end to end across the deletion.
    std::vector<std::uint8_t> prev;
    bool chain_ok = true;
    for (const auto& r : after_delete.value) {
        if (r.prev_hash != prev) chain_ok = false;
        if (chain_hash(prev, canonical_record(r.seq, r.ts_micros, r.record)) != r.hash) {
            chain_ok = false;
        }
        prev = r.hash;
    }
    CHECK(chain_ok, "and the global chain verifies end to end across the deletion");
}

static void test_a_recreated_tenant_starts_a_fresh_chain(Database& db) {
    std::cout << "test_a_recreated_tenant_starts_a_fresh_chain\n";
    const std::string tenant = unique_tenant("recreate");
    created_tenants().push_back(tenant);
    CHECK(db.create_tenant_schema(tenant, ctx_for("root")).success, "created");
    CHECK(db.add_acl("res-r", "bob", 0, 0x400, tenant, ctx_for("alice")).success, "grant");
    CHECK(db.cleanup_tenant_data(tenant, ctx_for("root")).success, "destroyed");

    CHECK(db.create_tenant_schema(tenant, ctx_for("root")).success, "recreated with the same name");
    CHECK(db.add_acl("res-r2", "bob", 0, 0x400, tenant, ctx_for("alice")).success, "grant");

    auto rows = read_rows(db, tenant);
    CHECK(rows.size() == 1, "the new tenant sees only its own history");
    // A chain that resumed at the old seq would splice two unrelated histories
    // together and verify as though they were one.
    CHECK(!rows.empty() && rows[0].seq == 1, "and starts a fresh chain at seq 1");
}

// ── per-tenant independence (§4.3.5) ───────────────────────────────────────

static void test_tenant_chains_are_independent(Database& db) {
    std::cout << "test_tenant_chains_are_independent\n";
    const std::string a = unique_tenant("iso_a");
    const std::string b = unique_tenant("iso_b");
    CHECK(db.create_tenant_schema(a, AccountabilityContext::system()).success, "tenant a");
    CHECK(db.create_tenant_schema(b, AccountabilityContext::system()).success, "tenant b");

    for (int i = 0; i < 4; ++i) {
        CHECK(db.add_acl("res-a" + std::to_string(i), "bob", 0, 0x400, a, ctx_for("alice")).success, "a");
    }
    CHECK(db.add_acl("res-b0", "bob", 0, 0x400, b, ctx_for("alice")).success, "b");

    auto rows_a = read_rows(db, a);
    auto rows_b = read_rows(db, b);
    CHECK(rows_a.size() == 4 && rows_b.size() == 1, "each tenant sees only its own");
    // Independent numbering: a busy tenant does not push another tenant's seq
    // along, which a single global chain could not offer.
    CHECK(!rows_b.empty() && rows_b[0].seq == 1,
          "tenant b's chain starts at 1 regardless of tenant a's activity");

    bool has_more = false;
    auto pull_b = db.list_accountability_records(b, 0, 100, has_more);
    CHECK(pull_b.success && pull_b.value.size() == 1,
          "a tenant's history can be read and verified on its own, "
          "with no access to any other tenant's records");
}

int main() {
    std::cout << "=== accountability_live_tests ===\n";

    const std::string host = env_or("FILEENGINE_PG_HOST", env_or("FE_TEST_PG_HOST", "localhost"));
    const int port = std::stoi(env_or("FILEENGINE_PG_PORT", env_or("FE_TEST_PG_PORT", "5434")));
    const std::string name = env_or("FILEENGINE_PG_DATABASE", env_or("FE_TEST_PG_DB", "fileengine"));
    const std::string user = env_or("FILEENGINE_PG_USER", env_or("FE_TEST_PG_USER", "postgres"));
    const std::string pass = env_or("FILEENGINE_PG_PASSWORD", env_or("FE_TEST_PG_PASSWORD", "postgres"));

    Database db(host, port, name, user, pass, /*pool_size=*/12);
    if (!db.connect()) {
        std::cout << "SKIP: no Postgres at " << host << ":" << port
                  << " — set FILEENGINE_PG_* to point at one.\n";
        return 77;   // ctest SKIP_RETURN_CODE
    }
    if (!db.create_schema().success) {
        std::cout << "SKIP: could not create/verify the global schema.\n";
        return 77;
    }

    test_each_operation_writes_one_attributed_record(db);
    test_a_failed_record_rolls_back_the_operation(db);
    test_a_failed_operation_writes_no_record(db);
    test_an_actorless_operation_is_refused(db);
    test_creation_default_acls_do_not_chain(db);
    test_cull_is_recorded_and_history_survives_it(db);
    test_a_cull_that_removes_nothing_records_nothing(db);
    test_chain_is_gap_free_and_ordered_under_concurrency(db);
    test_chain_links_verify_end_to_end(db);
    test_ts_survives_a_backwards_clock_step(db);
    test_the_core_can_answer_accountability_questions_alone(db);
    test_cursor_reads_forward_exactly_once(db);
    test_detail_survives_the_read_path_byte_for_byte(db);
    test_tenant_lifecycle_is_recorded_globally(db);
    test_a_recreated_tenant_starts_a_fresh_chain(db);
    test_tenant_chains_are_independent(db);

    // Drop everything this run created. Best-effort and deliberately not
    // asserted on: a cleanup failure must not turn a passing run red, and the
    // pid-suffixed names mean leftovers never affect a later run's assertions.
    for (const auto& tenant : created_tenants()) {
        db.cleanup_tenant_data(tenant, ctx_for("test-teardown"));
    }

    std::cout << "\n=== " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed ===\n";
    return g_failures == 0 ? 0 : 1;
}
