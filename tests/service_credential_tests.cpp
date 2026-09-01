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

// Service credentials and capability classification
// (design_documents/PROPOSAL_service_authentication.md §3.3, §6.1, §9).
//
// The most important test here is the last one. It enumerates the compiled
// service descriptor and fails if ANY RPC belongs to no capability — so a newly
// added method cannot reach production unclassified. Enforcing centrally is only
// worth anything if coverage is a property of the mechanism rather than of
// someone remembering, and this is what makes that true.

#include "fileengine/service_credential.h"
#include "fileengine/service_auth_interceptor.h"

#include "fileservice.pb.h"
#include <google/protobuf/descriptor.h>

#include <iostream>
#include <set>
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

// ── Token format (§3.3) ─────────────────────────────────────────────────────

static void test_token_round_trips() {
    std::cout << "test_token_round_trips\n";
    const std::string secret = generate_service_secret();
    CHECK(secret.size() >= 43, "a 256-bit secret is at least 43 base64 characters");

    const std::string token = format_service_token("http_bridge", secret);
    CHECK(token.rfind("fesvc_", 0) == 0,
          "the scannable prefix is present, so a leaked token is greppable");

    const ParsedToken parsed = parse_service_token(token);
    CHECK(parsed.valid, "a well-formed token parses");
    CHECK(parsed.service_id == "http_bridge", "the service id round trips");
    CHECK(parsed.secret == secret, "the secret round trips");
}

static void test_service_ids_containing_underscores_parse() {
    std::cout << "test_service_ids_containing_underscores_parse\n";
    // This is why the separator is '.' and not '_': every real service id
    // contains underscores, so an underscore separator could not be split
    // unambiguously.
    for (const char* id : {"http_bridge", "webdav_bridge", "folder_actions", "audit_service"}) {
        const ParsedToken p = parse_service_token(format_service_token(id, "abc"));
        CHECK(p.valid && p.service_id == id,
              std::string("underscored id parses: ") + id);
    }
}

static void test_malformed_tokens_are_refused() {
    std::cout << "test_malformed_tokens_are_refused\n";
    CHECK(!parse_service_token("").valid, "empty");
    CHECK(!parse_service_token("http_bridge.secret").valid, "no prefix");
    CHECK(!parse_service_token("fesvc_nodot").valid, "no separator");
    CHECK(!parse_service_token("fesvc_.secret").valid, "empty service id names nobody");
    CHECK(!parse_service_token("fesvc_svc.").valid, "empty secret");
    // A token for another deployment is structurally valid and must fail at
    // verification, not at parsing — §9.2 requires it be rejected identically
    // to an unknown one, which only holds if it gets that far.
    CHECK(parse_service_token("fesvc_http_bridge.someothersecret").valid,
          "a foreign but well-formed token parses, and is rejected later on the secret");
}

static void test_secrets_are_unique() {
    std::cout << "test_secrets_are_unique\n";
    std::set<std::string> seen;
    for (int i = 0; i < 200; ++i) seen.insert(generate_service_secret());
    CHECK(seen.size() == 200, "generated secrets do not repeat");
}

// ── Hashing (§3.3) ──────────────────────────────────────────────────────────

static void test_hashing_is_peppered_and_constant_time() {
    std::cout << "test_hashing_is_peppered_and_constant_time\n";
    const std::string secret = "a-full-entropy-secret";
    const auto a = hash_service_secret("pepper-one", secret);
    const auto b = hash_service_secret("pepper-one", secret);
    const auto c = hash_service_secret("pepper-two", secret);

    CHECK(a.size() == 32, "HMAC-SHA256 is 32 bytes");
    CHECK(constant_time_equals(a, b), "the same secret under the same pepper matches");
    CHECK(!constant_time_equals(a, c),
          "a different pepper yields a different hash — which is what makes the "
          "pepper worth keeping out of the database");
    CHECK(!constant_time_equals(a, hash_service_secret("pepper-one", "other")),
          "a different secret does not match");
}

static void test_an_absent_pepper_refuses_rather_than_degrades() {
    std::cout << "test_an_absent_pepper_refuses_rather_than_degrades\n";
    // Hashing under an empty pepper would store values that verify perfectly
    // and are worthless the moment the database leaks — a failure with no
    // visible symptom, which is the worst kind.
    CHECK(hash_service_secret("", "secret").empty(),
          "no pepper produces no hash, rather than an unpeppered one");
}

static void test_constant_time_compare_rejects_mismatched_lengths() {
    std::cout << "test_constant_time_compare_rejects_mismatched_lengths\n";
    CHECK(!constant_time_equals({1, 2, 3}, {1, 2}), "different lengths never match");
    CHECK(!constant_time_equals({}, {}), "two empty hashes are not a match");
}

// ── Capabilities (§6.1) ─────────────────────────────────────────────────────

static void test_capability_names_are_stable() {
    std::cout << "test_capability_names_are_stable\n";
    // Stored in the database and read by the CLI. Renaming one silently
    // reinterprets every stored grant.
    const char* expected[] = {"read", "write", "delete", "restore", "acl",
                              "roles", "admin", "destroy", "accountability",
                              // True delete, apart from `destroy`: granting it
                              // must not also restore the version-cull endpoint.
                              "erase",
                              // The erasure attestation surface, kept apart from
                              // `destroy` so a consumer can read what it owes and
                              // report back without being able to erase anything.
                              "erasure"};
    constexpr size_t kExpected = sizeof(expected) / sizeof(expected[0]);
    const auto& all = all_capabilities();
    CHECK(all.size() == kExpected, "ten capabilities, not five hundred cells");
    // Bounded by BOTH, not by all.size(): when the list grew this loop read past
    // the end of `expected` and compared against whatever followed it in memory,
    // so the failure it printed was garbage rather than the name that changed.
    for (size_t i = 0; i < all.size() && i < kExpected; ++i) {
        CHECK(std::string(to_string(all[i])) == expected[i],
              std::string("capability name is stable: ") + expected[i]);
    }
    Capability parsed;
    CHECK(parse_capability("destroy", parsed) && parsed == Capability::Destroy,
          "names round trip");
    CHECK(!parse_capability("everything", parsed),
          "an unknown name does not parse — a typo is rejected rather than stored "
          "as a capability nothing will ever match");
    CHECK(!parse_capability("*", parsed), "there is no wildcard capability");
    CHECK(!parse_capability("all", parsed), "and no blanket allow");
}

static void test_high_risk_capabilities_are_flagged() {
    std::cout << "test_high_risk_capabilities_are_flagged\n";
    CHECK(is_high_risk(Capability::Destroy), "destroy irreversibly removes committed data");
    CHECK(is_high_risk(Capability::Accountability),
          "accountability reads the security log, which across tenants reconstructs "
          "who did what to whom platform-wide");
    CHECK(!is_high_risk(Capability::Read), "read is not high risk");
    CHECK(!is_high_risk(Capability::Write), "nor write");
}

static void test_methods_map_to_their_capability() {
    std::cout << "test_methods_map_to_their_capability\n";
    struct Case { const char* method; Capability expected; };
    const Case cases[] = {
        {"/fileengine_rpc.FileService/GetFile",                  Capability::Read},
        {"/fileengine_rpc.FileService/PutFile",                  Capability::Write},
        {"/fileengine_rpc.FileService/RemoveFile",               Capability::Delete},
        {"/fileengine_rpc.FileService/RestoreToVersion",         Capability::Restore},
        {"/fileengine_rpc.FileService/GrantPermission",          Capability::Acl},
        {"/fileengine_rpc.FileService/CreateRole",               Capability::Roles},
        {"/fileengine_rpc.FileService/TriggerSync",              Capability::Admin},
        {"/fileengine_rpc.FileService/PurgeOldVersions",         Capability::Destroy},
        {"/fileengine_rpc.FileService/ListAccountabilityRecords", Capability::Accountability},
    };
    for (const auto& c : cases) {
        Capability got;
        CHECK(capability_for_method(c.method, got) && got == c.expected,
              std::string("classified: ") + c.method);
    }
}

static void test_an_unclassified_method_is_denied_to_everyone() {
    std::cout << "test_an_unclassified_method_is_denied_to_everyone\n";
    Capability got;
    CHECK(!capability_for_method("/fileengine_rpc.FileService/SomeNewRpc", got),
          "a method belonging to no capability resolves to none — default deny");

    // And that includes cli, which holds every capability but is NOT exempted
    // from the mechanism. "cli may call any method" would re-open the hole at
    // the one identity most likely to hold it.
    ServiceIdentity cli;
    cli.service_id   = "cli:alice";
    cli.capabilities = all_capabilities();
    CHECK(cli.is_cli(), "cli identities are recognised by prefix");
    for (Capability c : all_capabilities()) {
        CHECK(cli.has(c), std::string("cli holds ") + to_string(c));
    }
    // The gate is capability_for_method returning false — there is no code path
    // that consults the identity for an unclassified method at all.
}

static void test_identity_capability_checks() {
    std::cout << "test_identity_capability_checks\n";
    ServiceIdentity mcp;
    mcp.service_id   = "mcp";
    mcp.capabilities = {Capability::Read, Capability::Write};
    CHECK(mcp.has(Capability::Read) && mcp.has(Capability::Write), "mcp may read and write");
    // §6.3: the append-only guarantee moves from the MCP service's own code
    // into the core, so a prompt-injected agent cannot delete however
    // convincingly it is instructed to.
    CHECK(!mcp.has(Capability::Delete), "mcp may not delete");
    CHECK(!mcp.has(Capability::Destroy), "nor destroy");
    CHECK(!mcp.is_cli(), "and it is not a cli identity");

    ServiceIdentity audit;
    audit.service_id   = "audit_service";
    audit.capabilities = {Capability::Accountability};
    CHECK(!audit.has(Capability::Write),
          "audit_service is a reader — today nothing stops it writing files");
}

// ── Loopback (§6.1) ─────────────────────────────────────────────────────────

static void test_loopback_detection() {
    std::cout << "test_loopback_detection\n";
    CHECK(peer_is_loopback("ipv4:127.0.0.1:54321"), "v4 loopback");
    CHECK(peer_is_loopback("ipv6:[::1]:54321"), "v6 loopback");
    CHECK(peer_is_loopback("ipv6:[::ffff:127.0.0.1]:54321"), "v4-mapped loopback");
    CHECK(peer_is_loopback("unix:/run/fileengine/bootstrap.sock"),
          "a unix socket peer is local by definition, and more bounded than loopback TCP");
    CHECK(!peer_is_loopback("ipv4:10.0.0.7:54321"), "a private LAN address is not loopback");
    CHECK(!peer_is_loopback("ipv4:172.17.0.3:54321"), "nor a container address");
    // The near-miss that a prefix check done carelessly would let through.
    CHECK(!peer_is_loopback("ipv4:127.0.0.1.evil.com:80"), "not a lookalike hostname");
}

// ── Erasure capability split (§5.4.3) ───────────────────────────────────────

static void test_erasure_attestation_is_not_destroy() {
    std::cout << "test_erasure_attestation_is_not_destroy\n";
    // csai, discussion and difference all have to read the erasures they owe and
    // report back. If that rode on `destroy`, every one of them would also be
    // able to erase arbitrary files — a far larger grant than the job needs, and
    // handed to the services most exposed to untrusted document content.
    Capability c;
    CHECK(capability_for_method("/fileengine_rpc.FileService/ListPendingErasures", c) &&
          c == Capability::Erasure, "reading the queue is `erasure`, not `destroy`");
    CHECK(capability_for_method("/fileengine_rpc.FileService/AcknowledgeErasure", c) &&
          c == Capability::Erasure, "acknowledging is `erasure`, not `destroy`");
    CHECK(capability_for_method("/fileengine_rpc.FileService/GetErasureStatus", c) &&
          c == Capability::Erasure, "reading status is `erasure`, not `destroy`");

    // The destructive act itself stays with the irreversible operations.
    CHECK(capability_for_method("/fileengine_rpc.FileService/EraseFile", c) &&
          c == Capability::Erase, "erasing a file is `erase`, not `destroy`");
    CHECK(capability_for_method("/fileengine_rpc.FileService/PurgeOldVersions", c) &&
          c == Capability::Destroy, "culling versions stays `destroy`");

    // Both are high risk, for different reasons: `destroy` removes committed
    // data, `erasure` can close a contractual obligation that was never met.
    // Neither may ride along with an ordinary credential issue.
    CHECK(is_high_risk(Capability::Destroy), "destroy is high risk");
    CHECK(is_high_risk(Capability::Erase), "erase is high risk");
    CHECK(is_high_risk(Capability::Erasure),
          "erasure is high risk — a false acknowledgement is a false compliance claim");
    CHECK(!is_high_risk(Capability::Read), "read is not high risk");
}

// ── The coverage test (§9.4, §9.8) ──────────────────────────────────────────

static void test_every_rpc_in_the_descriptor_is_classified() {
    std::cout << "test_every_rpc_in_the_descriptor_is_classified\n";
    // Enumerate the COMPILED service descriptor rather than a hand-written
    // list. That is the whole point of enforcing centrally: coverage should be
    // a property of the mechanism, not of anyone remembering to update a list
    // when they add an RPC.
    //
    // Read from the generated descriptor pool, which is populated by linking
    // the generated code — so this sees exactly the service the server
    // registers, and a method added to the proto appears here with no change to
    // this test.
    const auto* pool = google::protobuf::DescriptorPool::generated_pool();
    const auto* descriptor = pool->FindServiceByName("fileengine_rpc.FileService");
    CHECK(descriptor != nullptr, "the service descriptor is available");
    if (descriptor == nullptr) return;

    std::vector<std::string> unclassified;
    for (int i = 0; i < descriptor->method_count(); ++i) {
        const std::string name = descriptor->method(i)->name();
        Capability c;
        if (!capability_for_method("/fileengine_rpc.FileService/" + name, c)) {
            unclassified.push_back(name);
        }
    }

    if (!unclassified.empty()) {
        std::cout << "    unclassified RPCs (add each to method_map() in "
                     "service_credential.cpp):\n";
        for (const auto& n : unclassified) std::cout << "      - " << n << "\n";
    }
    CHECK(unclassified.empty(),
          "every RPC belongs to a capability — an unclassified one is unreachable "
          "by everyone, including cli, and must not ship");

    CHECK(descriptor->method_count() == static_cast<int>(classified_methods().size()),
          "the classification covers the descriptor exactly, with nothing left over "
          "for a method that no longer exists");
}

int main() {
    std::cout << "=== service_credential_tests ===\n";
    test_token_round_trips();
    test_service_ids_containing_underscores_parse();
    test_malformed_tokens_are_refused();
    test_secrets_are_unique();
    test_hashing_is_peppered_and_constant_time();
    test_an_absent_pepper_refuses_rather_than_degrades();
    test_constant_time_compare_rejects_mismatched_lengths();
    test_capability_names_are_stable();
    test_high_risk_capabilities_are_flagged();
    test_methods_map_to_their_capability();
    test_an_unclassified_method_is_denied_to_everyone();
    test_identity_capability_checks();
    test_loopback_detection();
    test_erasure_attestation_is_not_destroy();
    test_every_rpc_in_the_descriptor_is_classified();

    std::cout << "\n=== " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed ===\n";
    return g_failures == 0 ? 0 : 1;
}
