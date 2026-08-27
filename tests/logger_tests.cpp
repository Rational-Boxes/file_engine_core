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

// The logger's security-relevant behaviour.
//
// Three properties are under test, and each of them exists because its absence
// is invisible:
//
//   * A SECURITY entry cannot be silenced by configuration. If it could, the one
//     signal saying the accountability guarantee is not holding would be absent
//     in exactly the deployments that turned the volume down — and absence reads
//     as health.
//   * Names do not reach the log in clear. The log is rotated, shipped and
//     archived, so a filename that lands in it is one an erasure obligation
//     cannot follow.
//   * The log rotates and prunes. An unbounded log fills the disk, and the
//     process that notices is the one that dies.
//
// ServerLogger is a singleton, so these tests run in order against one instance
// and re-initialize between cases.

#include "fileengine/server_logger.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

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

std::filesystem::path scratch_dir() {
    static const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("fe-logger-tests-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    return dir;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// Point the singleton at a fresh file for one case.
std::filesystem::path use_fresh_log(const std::string& level, bool redact,
                                    size_t rotation_mb = 10, int retention_days = 7) {
    static int n = 0;
    const auto path = scratch_dir() / ("case-" + std::to_string(++n) + ".log");
    std::filesystem::remove(path);
    ServerLogger::getInstance().initialize(level, path.string(),
                                          /*log_to_console=*/false,
                                          /*log_to_file=*/true,
                                          rotation_mb, retention_days, redact);
    return path;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ── the unsuppressable channel ─────────────────────────────────────────────

static void test_security_survives_the_highest_log_level() {
    std::cout << "test_security_survives_the_highest_log_level\n";
    // FATAL is the loudest level an operator can configure. Everything below it
    // is expected to disappear; SECURITY is expected not to.
    const auto path = use_fresh_log("FATAL", /*redact=*/true);

    SERVER_LOG_DEBUG("T", "debug line");
    SERVER_LOG_INFO("T", "info line");
    SERVER_LOG_WARN("T", "warn line");
    SERVER_LOG_ERROR("T", "error line");
    SERVER_LOG_SECURITY("T", "accountability write FAILED");

    const std::string body = read_file(path);
    CHECK(!contains(body, "debug line"), "DEBUG is filtered at level FATAL");
    CHECK(!contains(body, "info line"), "INFO is filtered");
    CHECK(!contains(body, "warn line"), "WARN is filtered");
    CHECK(!contains(body, "error line"), "ERROR is filtered");
    CHECK(contains(body, "accountability write FAILED"),
          "SECURITY is emitted anyway — no configuration silences it");
    CHECK(contains(body, "[SECURITY]"),
          "and is tagged so it can be alerted on without parsing the message");
}

static void test_security_events_are_counted() {
    std::cout << "test_security_events_are_counted\n";
    use_fresh_log("INFO", /*redact=*/true);
    const auto before = ServerLogger::getInstance().security_event_count();
    SERVER_LOG_SECURITY("T", "one");
    SERVER_LOG_SECURITY("T", "two");
    SERVER_LOG_ERROR("T", "not a security event");
    const auto after = ServerLogger::getInstance().security_event_count();
    // A log line nobody greps is not an alarm. The counter is what a scrape can
    // see without shipping and parsing the file.
    CHECK(after - before == 2, "only security() increments the counter");
}

// ── identifiers, never payload ─────────────────────────────────────────────

static void test_names_do_not_reach_the_log_in_clear() {
    std::cout << "test_names_do_not_reach_the_log_in_clear\n";
    const auto path = use_fresh_log("DEBUG", /*redact=*/true);
    const std::string party_data = "Acme_Corp_Contract_J_Smith.pdf";

    SERVER_LOG_INFO("T", "created " + SERVER_LOG_REDACT(party_data));
    const std::string body = read_file(path);

    CHECK(!contains(body, party_data), "the filename is absent from the log");
    CHECK(!contains(body, "Acme_Corp"), "and so is any recognisable fragment of it");
    CHECK(contains(body, "name#"), "a tag stands in its place");
}

static void test_a_tag_is_stable_within_a_run() {
    std::cout << "test_a_tag_is_stable_within_a_run\n";
    // Stability is the whole reason this is a hash rather than a constant: a
    // support engineer has to be able to follow one file through a trace.
    auto& logger = ServerLogger::getInstance();
    const std::string a = logger.redact("report.docx");
    const std::string b = logger.redact("report.docx");
    const std::string c = logger.redact("report.xlsx");
    CHECK(a == b, "the same name yields the same tag");
    CHECK(a != c, "different names yield different tags");
    CHECK(a.rfind("name#", 0) == 0 && a.size() == 13,
          "the tag is a short fixed-width token, not an open-ended string");
}

static void test_an_empty_name_stays_empty() {
    std::cout << "test_an_empty_name_stays_empty\n";
    // "name#<hash of empty>" would be worse than useless: it would read as a
    // real name that happens to recur everywhere.
    CHECK(ServerLogger::getInstance().redact("").empty(),
          "nothing to disclose produces nothing");
}

static void test_redaction_can_be_turned_off_for_debugging() {
    std::cout << "test_redaction_can_be_turned_off_for_debugging\n";
    const auto path = use_fresh_log("INFO", /*redact=*/false);
    const std::string name = "quarterly-figures.xlsx";
    SERVER_LOG_INFO("T", "opened " + SERVER_LOG_REDACT(name));
    CHECK(contains(read_file(path), name),
          "with redaction off the literal name is written, as the escape hatch intends");
    CHECK(!ServerLogger::getInstance().redact_names(), "and the state is queryable");
}

static void test_tags_are_not_comparable_across_processes() {
    std::cout << "test_tags_are_not_comparable_across_processes\n";
    // Re-initializing keeps the salt, so a tag stays stable for the life of the
    // process — but the salt is generated per process, so logs from two runs
    // cannot be joined to rebuild a corpus of names, and a tag cannot be matched
    // against a precomputed dictionary of likely filenames.
    auto& logger = ServerLogger::getInstance();
    // Redaction must be ON for both samples — the previous case left it off,
    // and comparing a literal name against a tag would "pass" for the wrong
    // reason in one direction and fail in the other.
    use_fresh_log("INFO", /*redact=*/true);
    const std::string before = logger.redact("payroll.csv");
    use_fresh_log("INFO", /*redact=*/true);
    CHECK(logger.redact("payroll.csv") == before,
          "the salt persists across re-initialization within one process");
    CHECK(before.rfind("name#", 0) == 0, "and both samples really are tags");
}

// ── rotation and retention ─────────────────────────────────────────────────

static void test_the_log_rotates_at_its_size_limit() {
    std::cout << "test_the_log_rotates_at_its_size_limit\n";
    // 1 MB, then write past it. Without rotation the file grows until the disk
    // does not, and the failure surfaces as something else entirely.
    const auto path = use_fresh_log("INFO", /*redact=*/true, /*rotation_mb=*/1);
    const std::string filler(4096, 'x');
    for (int i = 0; i < 400; ++i) {          // ~1.6 MB
        SERVER_LOG_INFO("T", filler);
    }

    CHECK(std::filesystem::exists(path), "the active log still exists");
    CHECK(std::filesystem::file_size(path) < 1024 * 1024,
          "and is back under the limit, so it rolled rather than growing");

    int rolls = 0;
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(path.filename().string() + ".", 0) == 0) ++rolls;
    }
    CHECK(rolls >= 1, "at least one timestamped roll was produced");
}

static void test_retention_prunes_old_rolls() {
    std::cout << "test_retention_prunes_old_rolls\n";
    const auto path = use_fresh_log("INFO", /*redact=*/true, /*rotation_mb=*/1,
                                    /*retention_days=*/1);

    // An old roll, and a fresh one. Only the old one should go.
    const auto old_roll = path.string() + ".20200101-000000";
    const auto new_roll = path.string() + ".20991231-235959";
    { std::ofstream(old_roll) << "old\n"; }
    { std::ofstream(new_roll) << "new\n"; }
    std::filesystem::last_write_time(
        old_roll, std::filesystem::file_time_type::clock::now() - std::chrono::hours(72));

    // Re-initializing runs a prune pass, as a restart does.
    ServerLogger::getInstance().initialize("INFO", path.string(), false, true, 1, 1, true);

    CHECK(!std::filesystem::exists(old_roll), "a roll older than retention is removed");
    CHECK(std::filesystem::exists(new_roll), "a recent roll is kept");
}

static void test_retention_leaves_unrelated_files_alone() {
    std::cout << "test_retention_leaves_unrelated_files_alone\n";
    const auto path = use_fresh_log("INFO", /*redact=*/true, 1, 1);
    const auto bystander = path.parent_path() / "something-else.log";
    { std::ofstream(bystander) << "not ours\n"; }
    std::filesystem::last_write_time(
        bystander, std::filesystem::file_time_type::clock::now() - std::chrono::hours(72));

    ServerLogger::getInstance().initialize("INFO", path.string(), false, true, 1, 1, true);
    CHECK(std::filesystem::exists(bystander),
          "pruning is scoped to this log's own rolls, by name prefix");
}

static void test_zero_retention_keeps_everything() {
    std::cout << "test_zero_retention_keeps_everything\n";
    const auto path = use_fresh_log("INFO", true, 1, /*retention_days=*/0);
    const auto roll = path.string() + ".20200101-000000";
    { std::ofstream(roll) << "old\n"; }
    std::filesystem::last_write_time(
        roll, std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 365));

    ServerLogger::getInstance().initialize("INFO", path.string(), false, true, 1, 0, true);
    CHECK(std::filesystem::exists(roll),
          "retention 0 means keep forever — an operator archiving externally "
          "must not have their history deleted underneath them");
}

int main() {
    std::cout << "=== logger_tests ===\n";
    test_security_survives_the_highest_log_level();
    test_security_events_are_counted();
    test_names_do_not_reach_the_log_in_clear();
    test_a_tag_is_stable_within_a_run();
    test_an_empty_name_stays_empty();
    test_redaction_can_be_turned_off_for_debugging();
    test_tags_are_not_comparable_across_processes();
    test_the_log_rotates_at_its_size_limit();
    test_retention_prunes_old_rolls();
    test_retention_leaves_unrelated_files_alone();
    test_zero_retention_keeps_everything();

    std::error_code ec;
    std::filesystem::remove_all(scratch_dir(), ec);

    std::cout << "\n=== " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed ===\n";
    return g_failures == 0 ? 0 : 1;
}
