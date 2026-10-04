// Live tests for the one-shot enrolment path's CLOSURE (PROPOSAL_service_
// authentication.md §3.6).
//
// Production ran for weeks with twelve credentials and no bootstrap marker:
// every credential was issued with fileengine_cli, which did not write the
// marker — only socket enrolment did. So every core start tried to open the
// unauthenticated enrolment socket (it failed to bind only because /run/fileengine
// does not exist in the image), and emptying the credential map would have
// re-armed enrolment for real: the back door the separate marker exists to deny.
//
// These tests EMPTY the global credential and marker tables, so they refuse to
// run against a database named "fileengine" (the dev stack's). Point them at a
// throwaway one:
//
//   createdb -h localhost -p 5434 -U postgres fe_bootstrap_test
//   FILEENGINE_PG_DATABASE=fe_bootstrap_test ./service_bootstrap_live_tests
//
// Skips cleanly (exit 77) without Postgres or without a throwaway database.

#include "fileengine/accountability.h"
#include "fileengine/database.h"

#include <cstdlib>
#include <iostream>
#include <libpq-fe.h>
#include <string>

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

const char* kPepper = "bootstrap-live-test-pepper";

std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

AccountabilityContext operator_ctx() {
    AccountabilityContext c;
    c.actor        = "cli:test-operator";
    c.source_iface = "cli";
    return c;
}

// Raw SQL for setup and assertions: the Database API deliberately has no way
// to delete the marker except the recorded break-glass reopen.
struct Sql {
    PGconn* c = nullptr;
    explicit Sql(const std::string& conninfo) { c = PQconnectdb(conninfo.c_str()); }
    ~Sql() { if (c) PQfinish(c); }
    bool ok() const { return c && PQstatus(c) == CONNECTION_OK; }
    void exec(const std::string& sql) {
        PGresult* r = PQexec(c, sql.c_str());
        PQclear(r);
    }
    std::string scalar(const std::string& sql) {
        PGresult* r = PQexec(c, sql.c_str());
        std::string out;
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1) out = PQgetvalue(r, 0, 0);
        PQclear(r);
        return out;
    }
    long long count(const std::string& sql) {
        const std::string v = scalar(sql);
        return v.empty() ? -1 : std::atoll(v.c_str());
    }
};

void reset(Sql& sql) {
    sql.exec("DELETE FROM service_auth_capability;");
    sql.exec("DELETE FROM service_auth_credential;");
    sql.exec("DELETE FROM service_auth_bootstrap;");
}

long long closed_records(Sql& sql) {
    return sql.count("SELECT count(*) FROM accountability_record_global "
                     "WHERE action = 'service_token.bootstrap_closed';");
}

bool complete(Database& db) {
    auto r = db.service_bootstrap_complete();
    return r.success && r.value;
}

void test_a_fresh_system_keeps_its_enrolment_path(Database& db, Sql& sql) {
    std::cout << "test_a_fresh_system_keeps_its_enrolment_path\n";
    reset(sql);
    CHECK(!complete(db), "empty map, no marker: bootstrap is open");
    auto closed = db.close_service_bootstrap_if_issued(AccountabilityContext::system());
    CHECK(closed.success && !closed.value, "the startup backfill leaves a fresh system alone");
    CHECK(!complete(db), "and enrolment is still available");
    CHECK(sql.count("SELECT count(*) FROM service_auth_bootstrap;") == 0, "no marker written");
}

void test_issuing_a_credential_closes_bootstrap(Database& db, Sql& sql) {
    std::cout << "test_issuing_a_credential_closes_bootstrap\n";
    reset(sql);
    const long long before = closed_records(sql);
    auto issued = db.issue_service_credential("http_bridge", kPepper, 1, operator_ctx(), false);
    CHECK(issued.success, "the credential issues");
    CHECK(sql.count("SELECT count(*) FROM service_auth_bootstrap;") == 1,
          "a direct issue writes the marker — the production gap");
    CHECK(sql.scalar("SELECT enrolled_id FROM service_auth_bootstrap;") == "http_bridge",
          "naming the credential that closed it");
    CHECK(closed_records(sql) == before + 1, "and the closure is on the record");
    CHECK(complete(db), "bootstrap is complete");

    auto second = db.issue_service_credential("csai", kPepper, 1, operator_ctx(), false);
    CHECK(second.success, "a second issue works");
    CHECK(closed_records(sql) == before + 1, "but records no second closure");
}

void test_production_state_is_backfilled_once(Database& db, Sql& sql) {
    std::cout << "test_production_state_is_backfilled_once\n";
    reset(sql);
    db.issue_service_credential("share", kPepper, 1, operator_ctx(), false);
    // Recreate what every deployment issued before the fix looks like.
    sql.exec("DELETE FROM service_auth_bootstrap;");
    CHECK(complete(db), "credentials alone already count as complete — no socket opens");

    const long long before = closed_records(sql);
    auto closed = db.close_service_bootstrap_if_issued(AccountabilityContext::system());
    CHECK(closed.success && closed.value, "the backfill writes the missing marker");
    CHECK(sql.scalar("SELECT enrolled_id FROM service_auth_bootstrap;") == "*pre-existing*",
          "marked as closed by pre-existing credentials");
    CHECK(closed_records(sql) == before + 1, "and recorded by the system actor");
    CHECK(sql.scalar("SELECT actor FROM accountability_record_global WHERE action = "
                     "'service_token.bootstrap_closed' ORDER BY seq DESC LIMIT 1;") == "system",
          "attributed to the system, not an operator");

    auto again = db.close_service_bootstrap_if_issued(AccountabilityContext::system());
    CHECK(again.success && !again.value, "a second start is a no-op");
    CHECK(closed_records(sql) == before + 1, "and records nothing");
}

void test_emptying_the_map_does_not_reopen_enrolment(Database& db, Sql& sql) {
    std::cout << "test_emptying_the_map_does_not_reopen_enrolment\n";
    reset(sql);
    db.issue_service_credential("mcp", kPepper, 1, operator_ctx(), false);
    sql.exec("DELETE FROM service_auth_capability;");
    sql.exec("DELETE FROM service_auth_credential;");
    CHECK(complete(db), "the marker keeps bootstrap closed with an empty map");
    auto enrol = db.enrol_bootstrap_credential("cli:intruder", kPepper, 1,
                                               AccountabilityContext::system("bootstrap"));
    CHECK(!enrol.success, "and enrolment refuses");
}

void test_enrolment_refuses_beside_credentials(Database& db, Sql& sql) {
    std::cout << "test_enrolment_refuses_beside_credentials\n";
    reset(sql);
    db.issue_service_credential("audit_service", kPepper, 1, operator_ctx(), false);
    sql.exec("DELETE FROM service_auth_bootstrap;");
    auto enrol = db.enrol_bootstrap_credential("cli:late", kPepper, 1,
                                               AccountabilityContext::system("bootstrap"));
    CHECK(!enrol.success, "credentials present: enrolment refuses even with no marker");
}

}  // namespace

int main() {
    std::cout << "=== service_bootstrap_live_tests ===\n";

    const std::string host = env_or("FILEENGINE_PG_HOST", env_or("FE_TEST_PG_HOST", "localhost"));
    const int port = std::stoi(env_or("FILEENGINE_PG_PORT", env_or("FE_TEST_PG_PORT", "5434")));
    const std::string name = env_or("FILEENGINE_PG_DATABASE", env_or("FE_TEST_PG_DB", "fe_bootstrap_test"));
    const std::string user = env_or("FILEENGINE_PG_USER", env_or("FE_TEST_PG_USER", "postgres"));
    const std::string pass = env_or("FILEENGINE_PG_PASSWORD", env_or("FE_TEST_PG_PASSWORD", "postgres"));

    if (name == "fileengine") {
        std::cout << "SKIP: these tests empty the global credential tables; refusing the shared "
                     "'fileengine' database. Set FILEENGINE_PG_DATABASE to a throwaway one.\n";
        return 77;
    }

    Database db(host, port, name, user, pass, /*pool_size=*/4);
    if (!db.connect()) {
        std::cout << "SKIP: no Postgres database '" << name << "' at " << host << ":" << port
                  << " — create it, or set FILEENGINE_PG_* to point at one.\n";
        return 77;
    }
    if (!db.create_schema().success) {
        std::cout << "SKIP: could not create/verify the global schema.\n";
        return 77;
    }
    Sql sql("host=" + host + " port=" + std::to_string(port) + " dbname=" + name +
            " user=" + user + " password=" + pass);
    if (!sql.ok()) {
        std::cout << "SKIP: could not open a raw connection for setup.\n";
        return 77;
    }

    test_a_fresh_system_keeps_its_enrolment_path(db, sql);
    test_issuing_a_credential_closes_bootstrap(db, sql);
    test_production_state_is_backfilled_once(db, sql);
    test_emptying_the_map_does_not_reopen_enrolment(db, sql);
    test_enrolment_refuses_beside_credentials(db, sql);
    reset(sql);

    std::cout << "\n=== " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed ===\n";
    return g_failures == 0 ? 0 : 1;
}
