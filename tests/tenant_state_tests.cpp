// Copyright (C) 2026 James Hickman
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// Tenant lifecycle state — the pure half. The stored spelling, the parse, and
// the one question every door will ask: does this state admit a user?
//
// These matter more than they look. The spelling is shared between a door, the
// registry and a log line, and a parse that quietly succeeds on an unknown
// value is how a suspension becomes advisory.

#include <cassert>
#include <cstring>
#include <iostream>
#include <set>
#include <string>

#include "fileengine/IDatabase.h"

using namespace fileengine;

static int g_checks = 0;
#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::cerr << "FAIL: " << (what) << "  [" << __FILE__ << ":"          \
                      << __LINE__ << "]" << std::endl;                           \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

static const TenantState kAll[] = {
    TenantState::Requested, TenantState::AwaitingDns, TenantState::Provisioning,
    TenantState::Live, TenantState::Suspended, TenantState::Decommissioning,
    TenantState::Decommissioned,
};

static void test_only_live_admits() {
    std::cout << "only `live` admits a user..." << std::endl;
    for (auto s : kAll) {
        const bool admits = tenant_state_admits(s);
        CHECK(admits == (s == TenantState::Live),
              std::string("admits(") + tenant_state_name(s) + ") == (state is live)");
    }
    // Spelled out, because these are the two that look like they might admit
    // and must not: a half-built tenant is not reachable, and a row that
    // outlives its data is not a way back in.
    CHECK(!tenant_state_admits(TenantState::Provisioning), "provisioning does not admit");
    CHECK(!tenant_state_admits(TenantState::Decommissioned), "decommissioned does not admit");
}

static void test_names_are_distinct_and_never_unknown() {
    std::cout << "every state has its own name..." << std::endl;
    std::set<std::string> seen;
    for (auto s : kAll) {
        const std::string n = tenant_state_name(s);
        CHECK(n != "unknown", "a state without a name would be stored as 'unknown'");
        CHECK(seen.insert(n).second, "names are distinct: " + n);
    }
}

static void test_round_trip() {
    std::cout << "name -> parse -> same state..." << std::endl;
    for (auto s : kAll) {
        TenantState back;
        CHECK(tenant_state_from_string(tenant_state_name(s), back), "parses its own name");
        CHECK(back == s, std::string("round-trips: ") + tenant_state_name(s));
    }
}

static void test_unknown_values_are_refused() {
    std::cout << "an unrecognised value does not parse..." << std::endl;
    // The case that matters: a value written by a NEWER build. Accepting it as
    // some default is how a state nobody here understands ends up admitting a
    // user. The caller must treat a false return as "do not admit".
    TenantState out = TenantState::Live;
    for (const char* bad : {"", " ", "LIVE", "Live", "active", "enabled", "deleted",
                            "live ", "suspended_pending_review", "0", "true"}) {
        out = TenantState::Live;
        CHECK(!tenant_state_from_string(bad, out),
              std::string("refuses '") + bad + "'");
    }
    // Case matters: the stored spelling is lowercase, and a case-insensitive
    // parse would make two spellings mean one state in a column with no
    // constraint enforcing either.
    CHECK(!tenant_state_from_string("Suspended", out), "parse is case-sensitive");
}

static void test_default_record_is_live() {
    // Matches the column default. A record constructed and not filled in must
    // not accidentally deny every tenant, and must not accidentally admit one
    // either — it is the same value the migration writes, and the migration's
    // reasoning is that every tenant existing at that moment is in service.
    TenantStateRecord rec;
    CHECK(rec.state == TenantState::Live, "a default record matches the column default");
    CHECK(tenant_state_admits(rec.state), "and therefore admits");
}

int main() {
    test_only_live_admits();
    test_names_are_distinct_and_never_unknown();
    test_round_trip();
    test_unknown_values_are_refused();
    test_default_record_is_live();
    std::cout << "tenant_state_tests: all passed (" << g_checks << " checks)" << std::endl;
    return 0;
}
