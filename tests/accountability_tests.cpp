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

// Unit tests for the accountability record's pure parts: the closed detail
// schema, validation, the canonical byte form, and the chain hash.
// (design_documents/PROPOSAL_accountability_record.md §4, §5.3, §5.4.7)
//
// These need no infrastructure at all, which matters: the canonical form and the
// hash are a CROSS-REPO CONTRACT. audit_service re-derives both in Python to
// verify every record it reads, so a change here that nobody notices does not
// produce a subtle bug — it makes every chain verification in the platform fail.
// Pinning the exact bytes here is what turns that into a test failure instead.
//
// The expected values below are pinned literals on purpose. Recomputing them
// with the same code under test would assert only that the code equals itself.

#include "fileengine/accountability.h"

#include <cstdio>
#include <iostream>
#include <string>
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

#define CHECK_EQ(actual, expected, msg)                                         \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!((actual) == (expected))) {                                        \
            std::cout << "  FAIL: " << (msg) << "\n    expected: " << (expected) \
                      << "\n    actual:   " << (actual) << "\n    ["            \
                      << __FILE__ << ":" << __LINE__ << "]\n";                  \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

static std::string hex(const std::vector<std::uint8_t>& bytes) {
    std::string out;
    char buf[3];
    for (std::uint8_t b : bytes) {
        std::snprintf(buf, sizeof(buf), "%02x", b);
        out += buf;
    }
    return out;
}

// A record that exercises every field, so canonical_record's field ORDER is
// pinned and not merely its content.
static AccountabilityRecord sample_grant() {
    AccountabilityRecord rec;
    rec.ctx.actor        = "alice";
    rec.ctx.actor_roles  = {"editors", "tenant_admin"};
    rec.ctx.source_iface = "grpc";
    rec.ctx.source_addr  = "10.0.0.7";
    rec.category         = AccountabilityCategory::Authorization;
    rec.action           = accountability_action::kAclGrant;
    rec.target_uid       = "f4c1a2";
    rec.target_type      = "acl";
    rec.principal        = "bob";
    rec.detail.set("principal_type", 0);
    rec.detail.set("effect", std::string("allow"));
    rec.detail.set("mask", 1024);
    rec.detail.set("mask_before", 0);
    rec.detail.set("mask_after", 1024);
    return rec;
}

// ── detail: the closed schema (§5.4.7) ──────────────────────────────────────

static void test_detail_canonical_json_is_sorted_and_compact() {
    std::cout << "test_detail_canonical_json_is_sorted_and_compact\n";
    AccountabilityDetail d;
    // Deliberately inserted out of order — canonical form must not depend on it.
    d.set("mask", 1024);
    d.set("effect", std::string("allow"));
    d.set("principal_type", 0);
    CHECK_EQ(d.canonical_json(),
             std::string("{\"effect\":\"allow\",\"mask\":1024,\"principal_type\":0}"),
             "keys sorted, no whitespace, numbers bare");
}

static void test_detail_types_render_as_json_tokens() {
    std::cout << "test_detail_types_render_as_json_tokens\n";
    AccountabilityDetail d;
    d.set("row_removed", true);
    d.set("keep_count", 5);
    d.set("cut_ts", std::string("2026-08-27T10:00:00Z"));
    CHECK_EQ(d.canonical_json(),
             std::string("{\"cut_ts\":\"2026-08-27T10:00:00Z\",\"keep_count\":5,"
                         "\"row_removed\":true}"),
             "bool bare, int bare, string quoted");
    CHECK(!d.empty(), "a populated detail is not empty");
    CHECK(AccountabilityDetail().empty(), "a fresh detail is empty");
}

static void test_detail_escapes_match_python_ensure_ascii() {
    std::cout << "test_detail_escapes_match_python_ensure_ascii\n";
    // The consumer re-hashes from the values it received using Python's
    // json.dumps(ensure_ascii=True). One differing escape breaks every
    // verification, so the escaping is pinned rather than assumed.
    AccountabilityDetail d;
    d.set("effect", std::string("a\"b\\c\nd\te"));
    CHECK_EQ(d.canonical_json(),
             std::string("{\"effect\":\"a\\\"b\\\\c\\nd\\te\"}"),
             "quote, backslash, newline and tab use the short escapes");

    AccountabilityDetail unicode;
    unicode.set("effect", std::string("caf\xc3\xa9"));          // U+00E9
    CHECK_EQ(unicode.canonical_json(), std::string("{\"effect\":\"caf\\u00e9\"}"),
             "non-ASCII is \\u-escaped, lowercase hex");

    AccountabilityDetail astral;
    astral.set("effect", std::string("\xf0\x9f\x94\x92"));      // U+1F512
    CHECK_EQ(astral.canonical_json(), std::string("{\"effect\":\"\\ud83d\\udd12\"}"),
             "above the BMP becomes a surrogate pair, as ensure_ascii does");

    AccountabilityDetail control;
    control.set("effect", std::string("\x01"));
    CHECK_EQ(control.canonical_json(), std::string("{\"effect\":\"\\u0001\"}"),
             "control characters without a short escape use \\u00xx");
}

static void test_detail_from_json_round_trips_exactly() {
    std::cout << "test_detail_from_json_round_trips_exactly\n";
    // Postgres normalizes JSONB into its own key order and spacing, so the read
    // path re-canonicalizes. If that round trip is not exact, every record the
    // consumer reads back fails its hash check.
    auto rec = sample_grant();
    const std::string canonical = rec.detail.canonical_json();

    // Simulate what Postgres hands back: reordered keys and injected whitespace.
    const std::string pg_shaped =
        "{\"mask_after\": 1024, \"effect\": \"allow\", \"mask\": 1024, "
        "\"principal_type\": 0, \"mask_before\": 0}";
    auto parsed = detail_from_json(pg_shaped);
    CHECK(parsed.success, "a JSONB text form parses");
    CHECK_EQ(parsed.value.canonical_json(), canonical,
             "re-canonicalizing recovers the exact bytes that were hashed");

    auto empty = detail_from_json("");
    CHECK(empty.success && empty.value.empty(), "an absent detail is an empty detail");

    // A nested value could not survive the round trip, so it is refused rather
    // than quietly flattened. Nothing the schemas allow is ever nested.
    auto nested = detail_from_json("{\"effect\": {\"a\": 1}}");
    CHECK(!nested.success, "a non-scalar detail value is refused, not flattened");

    auto not_object = detail_from_json("[1,2,3]");
    CHECK(!not_object.success, "a non-object detail is refused");
}

// ── validation: actor, scope, and the closed field set ──────────────────────

static void test_validate_requires_an_actor() {
    std::cout << "test_validate_requires_an_actor\n";
    // §5.1: an operation that cannot name its actor is a bug, and recording ""
    // is worse than recording nothing because it looks like coverage.
    auto rec = sample_grant();
    rec.ctx.actor.clear();
    auto result = validate_record(rec);
    CHECK(!result.success, "an actorless record is refused");
    CHECK(result.error.find("actor") != std::string::npos,
          "the refusal says why");

    rec.ctx.actor = "system";
    CHECK(validate_record(rec).success,
          "'system' is an explicit, acceptable actor — not a default");
}

static void test_validate_rejects_out_of_scope_actions() {
    std::cout << "test_validate_rejects_out_of_scope_actions\n";
    // §4.1's scope is a property of the code, not a convention: an action that is
    // not enumerated cannot be recorded at all.
    auto rec = sample_grant();
    rec.action = "file.written";
    auto result = validate_record(rec);
    CHECK(!result.success, "content writes are out of scope and cannot be recorded");
    CHECK(result.error.find("not in scope") != std::string::npos, "the refusal says why");
}

// Every action constant the code can emit must have a schema.
//
// This is the general form of a bug that shipped: `erase.initiated` was declared
// in accountability_action, emitted by the erasure path, and had no schema — so
// validate_record refused it, and because the record and the destruction commit
// together, the refusal aborted the whole erasure. The failure surfaced as
// "Erasure refused: accountability action is not in scope", at the far end of a
// feature, from a file nobody had touched.
//
// Asserting each new action individually would only ever catch the one somebody
// remembered to add. This asserts the property: declaring an action and
// forgetting its schema is a compile-and-ship-able mistake, and this is what
// makes it a test failure instead.
static void test_every_declared_action_has_a_schema() {
    std::cout << "test_every_declared_action_has_a_schema\n";
    namespace A = accountability_action;
    const char* kDeclared[] = {
        A::kAclGrant, A::kAclRevoke, A::kRoleCreate, A::kRoleDelete,
        A::kRoleAssign, A::kRoleRemove, A::kCullVersions,
        A::kErase, A::kEraseAck, A::kEraseComplete,
        A::kTenantCreate, A::kTenantDelete,
        A::kServiceTokenIssued, A::kServiceTokenRotated, A::kServiceTokenPruned,
        A::kServiceTokenRevoked, A::kServiceBootstrapEnrolled,
        A::kServiceBootstrapReopened, A::kServiceCapGranted, A::kServiceCapRevoked,
    };
    for (const char* action : kDeclared) {
        AccountabilityRecord rec = sample_grant();
        rec.action = action;
        rec.detail = AccountabilityDetail{};   // fields are per-action; scope is what is under test
        auto result = validate_record(rec);
        const bool in_scope = result.success ||
                              result.error.find("not in scope") == std::string::npos;
        CHECK(in_scope, std::string("action has a schema: ") + action);
    }
}

// The erasure records must never carry content. Erasing a file would otherwise
// mean erasing the record of its erasure (§5.4.7).
static void test_erasure_records_carry_no_content() {
    std::cout << "test_erasure_records_carry_no_content\n";
    AccountabilityRecord rec = sample_grant();
    rec.category = AccountabilityCategory::Destruction;
    rec.action   = accountability_action::kErase;
    rec.detail   = AccountabilityDetail{};
    rec.detail.set("erasure_id", std::string("e1"));
    rec.detail.set("name", std::string("Acme_Contract_J_Smith.pdf"));
    CHECK(!validate_record(rec).success,
          "a filename is not an enumerated field on an erasure record");
}

static void test_validate_rejects_category_action_mismatch() {
    std::cout << "test_validate_rejects_category_action_mismatch\n";
    auto rec = sample_grant();
    rec.category = AccountabilityCategory::Destruction;   // acl.grant is authorization
    CHECK(!validate_record(rec).success,
          "a category that contradicts its action is refused");
}

static void test_validate_rejects_unenumerated_detail_fields() {
    std::cout << "test_validate_rejects_unenumerated_detail_fields\n";
    // The §5.4.7 rule made mechanical. This is the test that says content cannot
    // leak into an immutable, never-culled structure by accident: one
    // well-meaning detail["new_value"] = value and the log holds payload forever.
    auto rec = sample_grant();
    rec.detail.set("new_value", std::string("the secret contract text"));
    auto result = validate_record(rec);
    CHECK(!result.success, "a field the action's schema does not enumerate is refused");
    CHECK(result.error.find("new_value") != std::string::npos,
          "the refusal names the offending field");

    // And the fields that ARE enumerated all pass, for every in-scope action.
    struct Case { AccountabilityCategory cat; const char* action; };
    const Case cases[] = {
        {AccountabilityCategory::Authorization, accountability_action::kAclGrant},
        {AccountabilityCategory::Authorization, accountability_action::kAclRevoke},
        {AccountabilityCategory::Identity,      accountability_action::kRoleCreate},
        {AccountabilityCategory::Identity,      accountability_action::kRoleDelete},
        {AccountabilityCategory::Identity,      accountability_action::kRoleAssign},
        {AccountabilityCategory::Identity,      accountability_action::kRoleRemove},
        {AccountabilityCategory::Destruction,   accountability_action::kCullVersions},
        {AccountabilityCategory::Lifecycle,     accountability_action::kTenantCreate},
        {AccountabilityCategory::Destruction,   accountability_action::kTenantDelete},
    };
    for (const auto& c : cases) {
        AccountabilityRecord r;
        r.ctx.actor = "alice";
        r.category  = c.cat;
        r.action    = c.action;
        CHECK(validate_record(r).success,
              std::string("in-scope action accepted with an empty detail: ") + c.action);
    }
}

// ── canonical form + chain (§5.3) ───────────────────────────────────────────

static void test_canonical_record_is_pinned() {
    std::cout << "test_canonical_record_is_pinned\n";
    const std::string canonical = canonical_record(1, 1756296000123456LL, sample_grant());
    CHECK_EQ(canonical,
             std::string("[1,1756296000123456,\"alice\",\"editors,tenant_admin\","
                         "\"grpc\",\"10.0.0.7\",\"authorization\",\"acl.grant\","
                         "\"f4c1a2\",\"acl\",\"bob\","
                         "\"{\\\"effect\\\":\\\"allow\\\",\\\"mask\\\":1024,"
                         "\\\"mask_after\\\":1024,\\\"mask_before\\\":0,"
                         "\\\"principal_type\\\":0}\",null]"),
             "the canonical form is a fixed-order compact JSON array");
}

static void test_canonical_record_distinguishes_absent_from_empty() {
    std::cout << "test_canonical_record_distinguishes_absent_from_empty\n";
    // "no principal" and "the principal named empty-string" must not collide, or
    // two genuinely different records could hash identically.
    AccountabilityRecord bare;
    bare.ctx.actor = "system";
    bare.category  = AccountabilityCategory::Identity;
    bare.action    = accountability_action::kRoleCreate;
    const std::string canonical = canonical_record(7, 42, bare);
    CHECK_EQ(canonical,
             std::string("[7,42,\"system\",null,null,null,\"identity\","
                         "\"role.create\",null,null,null,null,null]"),
             "absent optional fields are JSON null, not empty strings");
}

static void test_canonical_record_carries_tenant_only_for_global_rows() {
    std::cout << "test_canonical_record_carries_tenant_only_for_global_rows\n";
    // A tenant-scoped row leaves its tenant implicit in the schema it lives in:
    // the schema name is not reversible to the tenant id, so hashing it there
    // would make a tenant's chain unverifiable from its own data. The global
    // chain spans tenants, so there it is a real column and part of the hash.
    AccountabilityRecord global;
    global.ctx.actor     = "root";
    global.category      = AccountabilityCategory::Lifecycle;
    global.action        = accountability_action::kTenantCreate;
    global.target_type   = "tenant";
    global.global_tenant = "acme";
    global.detail.set("schema", std::string("tenant_acme"));
    const std::string canonical = canonical_record(3, 99, global);
    CHECK(canonical.find("\"acme\"]") != std::string::npos,
          "a global row's tenant is the last canonical field");

    AccountabilityRecord scoped = global;
    scoped.global_tenant.clear();
    CHECK(canonical_record(3, 99, scoped).find("null]") != std::string::npos,
          "a tenant-scoped row's tenant field is null");
    CHECK(canonical_record(3, 99, scoped) != canonical,
          "the two forms are distinguishable");
}

static void test_seq_and_ts_are_part_of_what_the_row_attests() {
    std::cout << "test_seq_and_ts_are_part_of_what_the_row_attests\n";
    // Unlike the audit chain (where seq is DB-assigned and excluded), seq here is
    // claimed under the same lock that orders the chain — so renumbering a row
    // must break verification, not silently succeed.
    const auto rec = sample_grant();
    const auto a = chain_hash({}, canonical_record(1, 1000, rec));
    const auto b = chain_hash({}, canonical_record(2, 1000, rec));
    const auto c = chain_hash({}, canonical_record(1, 1001, rec));
    CHECK(a != b, "changing seq changes the hash");
    CHECK(a != c, "changing ts changes the hash");
}

static void test_chain_hash_is_pinned_and_links() {
    std::cout << "test_chain_hash_is_pinned_and_links\n";
    const std::string canonical = canonical_record(1, 1756296000123456LL, sample_grant());

    // hash = SHA-256(prev_hash ‖ canonical); an empty prev contributes no bytes,
    // so the first row of a chain hashes its canonical form alone.
    const auto first = chain_hash({}, canonical);
    CHECK_EQ(first.size(), static_cast<size_t>(32), "SHA-256 is 32 bytes");
    CHECK_EQ(hex(first),
             std::string("fca7490691846d76f9b76ce67b29c266e7da09ca7f5adeb0c6ad2e06ff7eeb54"),
             "the first link is pinned (shared contract with audit_service)");
    // That value is not this code's own output taken on trust — it is the digest
    // of the Python-side derivation the consumer performs:
    //   detail    = json.dumps({...}, sort_keys=True, separators=(",",":"))
    //   canonical = json.dumps([seq, ts, actor, roles_csv, iface, addr, category,
    //                           action, target_uid, target_type, principal,
    //                           detail, None], separators=(",",":"))
    //   sha256(b"" + canonical.encode()).hexdigest()
    // If this assertion ever fails, one side of that contract moved and every
    // chain verification in the platform is about to start failing.

    // A second row over the same content hashes differently because it chains.
    const auto second = chain_hash(first, canonical);
    CHECK(second != first, "the same content at a different chain position differs");

    // And a forked prev produces a different link — which is what makes a
    // tampered or reordered chain detectable.
    std::vector<std::uint8_t> tampered = first;
    tampered[0] ^= 0xFF;
    CHECK(chain_hash(tampered, canonical) != second, "a broken prev breaks the link");
}

static void test_bytea_hex_round_trips() {
    std::cout << "test_bytea_hex_round_trips\n";
    const std::vector<std::uint8_t> bytes = {0x00, 0x01, 0x7f, 0x80, 0xff};
    const std::string text = bytea_hex(bytes);
    CHECK_EQ(text, std::string("\\x00017f80ff"), "Postgres bytea hex text format");
    CHECK(parse_bytea_hex(text) == bytes, "round trips");
    CHECK(parse_bytea_hex("00017f80ff") == bytes, "the \\x prefix is optional on input");
    CHECK(parse_bytea_hex("\\x").empty(), "an empty bytea parses to no bytes");
    CHECK(bytea_hex({}) == std::string("\\x"), "no bytes render as the empty bytea");
}

static void test_category_names_are_stable() {
    std::cout << "test_category_names_are_stable\n";
    // These strings are stored, hashed and read by another repo. Renaming one
    // silently invalidates every chain that contains it.
    CHECK_EQ(std::string(to_string(AccountabilityCategory::Authorization)),
             std::string("authorization"), "authorization");
    CHECK_EQ(std::string(to_string(AccountabilityCategory::Identity)),
             std::string("identity"), "identity");
    CHECK_EQ(std::string(to_string(AccountabilityCategory::Destruction)),
             std::string("destruction"), "destruction");
    CHECK_EQ(std::string(to_string(AccountabilityCategory::Lifecycle)),
             std::string("lifecycle"), "lifecycle");
}

static void test_global_chain_key_cannot_collide_with_a_tenant() {
    std::cout << "test_global_chain_key_cannot_collide_with_a_tenant\n";
    // The reserved key relies on '*' being stripped from schema names, so no
    // real tenant can ever resolve to it.
    const std::string key = kGlobalChainKey;
    CHECK(key.find('*') != std::string::npos,
          "the global key contains a character schema-name validation strips");
}

static void test_context_mode_defaults_to_recording() {
    std::cout << "test_context_mode_defaults_to_recording\n";
    // The safe default is that an ACL write IS an act worth chaining. Skipping
    // the record has to be asked for explicitly at the call site.
    AccountabilityContext ctx;
    CHECK(ctx.mode == AccountabilityMode::Record, "default mode is Record");
    CHECK(!ctx.valid(), "a default-constructed context has no actor and is invalid");

    auto sys = AccountabilityContext::system();
    CHECK(sys.valid() && sys.actor == "system", "the system identity is explicit");
    CHECK(sys.mode == AccountabilityMode::Record, "the system identity still records");
}

int main() {
    std::cout << "=== accountability_tests ===\n";
    test_detail_canonical_json_is_sorted_and_compact();
    test_detail_types_render_as_json_tokens();
    test_detail_escapes_match_python_ensure_ascii();
    test_detail_from_json_round_trips_exactly();
    test_validate_requires_an_actor();
    test_validate_rejects_out_of_scope_actions();
    test_every_declared_action_has_a_schema();
    test_erasure_records_carry_no_content();
    test_validate_rejects_category_action_mismatch();
    test_validate_rejects_unenumerated_detail_fields();
    test_canonical_record_is_pinned();
    test_canonical_record_distinguishes_absent_from_empty();
    test_canonical_record_carries_tenant_only_for_global_rows();
    test_seq_and_ts_are_part_of_what_the_row_attests();
    test_chain_hash_is_pinned_and_links();
    test_bytea_hex_round_trips();
    test_category_names_are_stable();
    test_global_chain_key_cannot_collide_with_a_tenant();

    std::cout << "\n=== " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed ===\n";
    return g_failures == 0 ? 0 : 1;
}
