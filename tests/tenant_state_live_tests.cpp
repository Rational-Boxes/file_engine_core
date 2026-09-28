// Copyright (C) 2026 James Hickman
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// Tenant lifecycle state against a real Postgres: the migration, the backfill
// that must not lock anyone out, and the accessors.
//
// The assertion that matters most is test_existing_tenants_default_to_live. A
// migration cannot know what it was not told, so it may assert only what is
// already true — and every tenant existing when this runs IS in service.
// Defaulting to anything else would refuse every login on the deployment after
// the first restart.
//
// Skips (77) without a database, like the other live suites.

#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

#include "fileengine/accountability.h"
#include "fileengine/database.h"

using namespace fileengine;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::cerr << "  FAIL: " << what << std::endl; }
}

static std::string env_or(const char* k, const std::string& d) {
    const char* v = std::getenv(k);
    return (v && *v) ? std::string(v) : d;
}

static void test_existing_tenants_default_to_live(Database& db, const std::string& t) {
    std::cout << "the migration backfills existing tenants as live..." << std::endl;
    // The tenant was created BEFORE anything set a state — which is the shape
    // of every tenant already on the deployment when this ships.
    auto r = db.get_tenant_state(t);
    check(r.success, "read state: " + r.error);
    check(r.value.has_value(), "a registered tenant has a state row");
    if (r.value.has_value()) {
        check(r.value->state == TenantState::Live,
              "an existing tenant is live, not locked out by its own migration");
        check(tenant_state_admits(r.value->state), "and therefore admits users");
        check(r.value->state_by.empty(), "with no actor, because nobody moved it");
    }
}

static void test_absence_is_distinguishable_from_error(Database& db) {
    std::cout << "a tenant that does not exist reports absent, not a state..." << std::endl;
    auto r = db.get_tenant_state("no-such-tenant-" + std::to_string(::getpid()));
    check(r.success, "the query succeeded");
    check(!r.value.has_value(), "and reported no row rather than inventing one");
    // This is the property a door depends on: absent and errored are different
    // answers, and only one of them is a verdict.
}

static void test_transitions_round_trip(Database& db, const std::string& t) {
    std::cout << "every state round-trips through the column..." << std::endl;
    const TenantState all[] = {
        TenantState::Requested, TenantState::AwaitingDns, TenantState::Provisioning,
        TenantState::Suspended, TenantState::Decommissioning, TenantState::Decommissioned,
        TenantState::Live,
    };
    for (auto s : all) {
        auto w = db.set_tenant_state(t, s, "tester", "because");
        check(w.success, std::string("set ") + tenant_state_name(s) + ": " + w.error);
        auto r = db.get_tenant_state(t);
        check(r.success && r.value.has_value(), "read back");
        if (r.value.has_value()) {
            check(r.value->state == s, std::string("round-trips: ") + tenant_state_name(s));
            check(r.value->state_by == "tester", "records who moved it");
            check(r.value->note == "because", "records why");
            check(!r.value->state_since.empty(), "records when");
        }
    }
}

static void test_suspension_denies_and_is_reversible(Database& db, const std::string& t) {
    std::cout << "suspend denies, and un-suspending restores..." << std::endl;
    check(db.set_tenant_state(t, TenantState::Suspended, "admin", "unpaid").success, "suspend");
    auto s = db.get_tenant_state(t);
    check(s.value.has_value() && !tenant_state_admits(s.value->state),
          "a suspended tenant does not admit");

    check(db.set_tenant_state(t, TenantState::Live, "admin", "paid").success, "restore");
    auto l = db.get_tenant_state(t);
    check(l.value.has_value() && tenant_state_admits(l.value->state),
          "and suspension is reversible, unlike decommissioning");
}

static void test_setting_a_missing_tenant_is_an_error(Database& db) {
    std::cout << "a transition against a missing tenant fails loudly..." << std::endl;
    // An UPDATE matching nothing looks exactly like one that worked. Reported
    // rather than silently successful, or the administration application would
    // believe it had suspended something.
    auto r = db.set_tenant_state("ghost-" + std::to_string(::getpid()), TenantState::Suspended);
    check(!r.success, "no such tenant is refused: " + r.error);
}

static void test_state_is_per_tenant(Database& db, const std::string& a, const std::string& b) {
    std::cout << "suspending one tenant does not touch another..." << std::endl;
    check(db.set_tenant_state(a, TenantState::Suspended, "admin").success, "suspend a");
    check(db.set_tenant_state(b, TenantState::Live, "admin").success, "b live");
    auto ra = db.get_tenant_state(a);
    auto rb = db.get_tenant_state(b);
    check(ra.value.has_value() && ra.value->state == TenantState::Suspended, "a stays suspended");
    check(rb.value.has_value() && rb.value->state == TenantState::Live, "b stays live");
}

int main() {
    std::cout << "=== tenant_state_live_tests ===\n";

    const std::string host = env_or("FILEENGINE_PG_HOST", env_or("FE_TEST_PG_HOST", "localhost"));
    const int port = std::stoi(env_or("FILEENGINE_PG_PORT", env_or("FE_TEST_PG_PORT", "5434")));
    const std::string name = env_or("FILEENGINE_PG_DATABASE", env_or("FE_TEST_PG_DB", "fileengine"));
    const std::string user = env_or("FILEENGINE_PG_USER", env_or("FE_TEST_PG_USER", "postgres"));
    const std::string pass = env_or("FILEENGINE_PG_PASSWORD", env_or("FE_TEST_PG_PASSWORD", "postgres"));

    Database db(host, port, name, user, pass, /*pool_size=*/6);
    if (!db.connect()) { std::cout << "SKIP: no Postgres at " << host << ":" << port << "\n"; return 77; }
    if (!db.create_schema().success) { std::cout << "SKIP: could not create the global schema.\n"; return 77; }

    const std::string a = "ts_" + std::to_string(::getpid()) + "_a";
    const std::string b = "ts_" + std::to_string(::getpid()) + "_b";
    for (const auto& t : {a, b}) {
        if (!db.create_tenant_schema(t, AccountabilityContext::system()).success) {
            std::cout << "SKIP: could not create tenant schema " << t << "\n";
            return 77;
        }
    }

    test_existing_tenants_default_to_live(db, a);
    test_absence_is_distinguishable_from_error(db);
    test_transitions_round_trip(db, a);
    test_suspension_denies_and_is_reversible(db, a);
    test_setting_a_missing_tenant_is_an_error(db);
    test_state_is_per_tenant(db, a, b);

    for (const auto& t : {a, b}) db.cleanup_tenant_data(t, AccountabilityContext::system());

    std::cout << "\n=== " << (g_checks - g_failures) << "/" << g_checks << " checks passed ===\n";
    return g_failures == 0 ? 0 : 1;
}
