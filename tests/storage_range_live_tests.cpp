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

// Byte-range reads end to end through a real FileSystem, a real Postgres and
// real on-disk storage — storage_pipeline.md S3a, acceptance §10.
//
// The assertions that matter most are the ones that would pass trivially if
// ranges were broken and nobody looked:
//
//   * a range is BYTE-IDENTICAL to the same window of a full download, under
//     every combination of compression and encryption (SR-10);
//   * a request with no offset/length behaves exactly as before (SR-21),
//     asserted against the recorded behaviour of get_stream rather than
//     against get_range's own idea of it;
//   * a FULL read still verifies the GCM tag (SR-15) and a ranged read says it
//     did not (SR-16/SR-17) — the regression that would matter most and be
//     completely invisible.
//
// Skips (77) when no Postgres is reachable, like the other live suites.

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "fileengine/accountability.h"
#include "fileengine/database.h"
#include "fileengine/acl_manager.h"
#include "fileengine/filesystem.h"
#include "fileengine/tenant_manager.h"
#include "fileengine/utils.h"

using namespace fileengine;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::cerr << "  FAIL: " << what << std::endl;
    }
}

static std::string env_or(const char* key, const std::string& dflt) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : dflt;
}

static const std::string USER = "range-tester";
// Creating directly under the filesystem root is admin-only, so the fixture's
// principal carries tenant_admin. Nothing here is testing the ACL model; this
// is only how a file gets created at the root.
static const std::vector<std::string> ROLES = {"tenant_admin"};

// A payload with both characters: a compressible head and an incompressible
// tail, so one file exercises both sides of the S2 decision.
static std::vector<uint8_t> mixed_payload(size_t n) {
    std::vector<uint8_t> v;
    const std::string unit = "range payload block; ";
    while (v.size() < n / 2) v.insert(v.end(), unit.begin(), unit.end());
    uint64_t x = 0x1234567890ABCDEFull;
    while (v.size() < n) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        v.push_back(static_cast<uint8_t>(x));
    }
    v.resize(n);
    return v;
}

struct Fixture {
    // TenantManager refuses to initialise a tenant without a shared database —
    // the platform never opens connections outside the pool — so the fixture
    // owns one and hands it over.
    std::shared_ptr<Database> db;
    std::shared_ptr<TenantManager> tm;
    std::unique_ptr<FileSystem> fs;
    std::string tenant;
    std::string root;
};

static bool make_fixture(Fixture& f, bool encrypt, bool compress, const std::string& suffix) {
    TenantConfig cfg;
    cfg.db_host = env_or("FILEENGINE_PG_HOST", env_or("FE_TEST_PG_HOST", "localhost"));
    cfg.db_port = std::stoi(env_or("FILEENGINE_PG_PORT", env_or("FE_TEST_PG_PORT", "5434")));
    cfg.db_name = env_or("FILEENGINE_PG_DATABASE", env_or("FE_TEST_PG_DB", "fileengine"));
    cfg.db_user = env_or("FILEENGINE_PG_USER", env_or("FE_TEST_PG_USER", "postgres"));
    cfg.db_password = env_or("FILEENGINE_PG_PASSWORD", env_or("FE_TEST_PG_PASSWORD", "postgres"));
    f.root = "/tmp/fe_range_" + std::to_string(::getpid()) + "_" + suffix;
    cfg.storage_base_path = f.root;
    cfg.s3_endpoint = "";
    cfg.s3_region = "";
    cfg.s3_bucket = "";
    cfg.s3_access_key = "";
    cfg.s3_secret_key = "";
    cfg.s3_path_style = true;
    cfg.encrypt_data = encrypt;
    cfg.compress_data = compress;
    cfg.encryption_key =
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

    f.tenant = "rng_" + std::to_string(::getpid()) + "_" + suffix;
    f.db = std::make_shared<Database>(cfg.db_host, cfg.db_port, cfg.db_name,
                                      cfg.db_user, cfg.db_password, /*pool_size=*/8);
    if (!f.db->connect()) return false;
    if (!f.db->create_schema().success) return false;
    f.tm = std::make_shared<TenantManager>(cfg, f.db);
    if (!f.tm->initialize_tenant(f.tenant)) return false;
    f.fs = std::make_unique<FileSystem>(f.tm);
    // Without an AclManager, FileSystem fails closed on every authorization
    // question — including the admin check that guards creating at the root.
    f.fs->set_acl_manager(std::make_shared<AclManager>(f.db));
    return true;
}

static void destroy_fixture(Fixture& f) {
    f.fs.reset();
    f.tm.reset();
    if (f.db && !f.tenant.empty()) {
        // Best-effort: a cleanup failure must not turn a passing run red, and
        // the pid-suffixed names mean leftovers never affect a later run.
        f.db->cleanup_tenant_data(f.tenant, AccountabilityContext::system());
    }
    f.db.reset();
    std::error_code ec;
    std::filesystem::remove_all(f.root, ec);
}

// Read the whole file through get_stream — the pre-existing path, used as the
// reference every range is compared against.
static bool read_all(FileSystem& fs, const std::string& uid, const std::string& tenant,
                     std::vector<uint8_t>& out) {
    out.clear();
    auto r = fs.get_stream(uid, [&out](const uint8_t* p, size_t n) {
        out.insert(out.end(), p, p + n);
        return true;
    }, USER, ROLES, tenant, "");
    return r.success;
}

static bool read_range(FileSystem& fs, const std::string& uid, const std::string& tenant,
                       int64_t off, int64_t len, std::vector<uint8_t>& out,
                       FileSystem::RangeReport* rep = nullptr) {
    out.clear();
    FileSystem::RangeReport local;
    auto r = fs.get_range(uid, off, len, [&out](const uint8_t* p, size_t n) {
        out.insert(out.end(), p, p + n);
        return true;
    }, USER, ROLES, tenant, "", rep ? rep : &local);
    return r.success;
}

static void run_combination(bool encrypt, bool compress, const std::string& label,
                            bool exhaustive) {
    std::cout << "ranges end-to-end: " << label << "..." << std::endl;

    Fixture f;
    if (!make_fixture(f, encrypt, compress, label)) {
        check(false, "fixture " + label);
        return;
    }

    const size_t N = 40000;
    const auto payload = mixed_payload(N);

    auto created = f.fs->touch("", "ranged.bin", USER, ROLES, f.tenant);
    check(created.success, label + ": touch — " + created.error);
    if (!created.success) { destroy_fixture(f); return; }
    const std::string uid = created.value;

    check(f.fs->put(uid, payload, USER, ROLES, f.tenant).success, label + ": put");

    // SR-21: the pre-existing whole-file path is unchanged and is the reference.
    std::vector<uint8_t> whole;
    check(read_all(*f.fs, uid, f.tenant, whole), label + ": get_stream");
    check(whole == payload, label + ": get_stream returns the payload unchanged");

    // SR-10: a range is byte-identical to the same window of the full download.
    struct Window { int64_t off; int64_t len; const char* name; };
    const Window windows[] = {
        {0, 1, "first byte"},
        {0, 1000, "head"},
        {0, static_cast<int64_t>(N), "explicit whole"},
        {1, 1, "second byte"},
        {12345, 1, "one byte mid-file"},
        {12345, 5000, "mid-file run"},
        {static_cast<int64_t>(N) - 1, 1, "last byte"},
        {static_cast<int64_t>(N) - 100, 100, "tail"},
        {static_cast<int64_t>(N) / 2, 0, "half to end (len 0)"},
        {0, 0, "whole via 0,0"},
    };
    for (const auto& w : windows) {
        std::vector<uint8_t> got;
        FileSystem::RangeReport rep;
        check(read_range(*f.fs, uid, f.tenant, w.off, w.len, got, &rep),
              label + std::string(": range ") + w.name);
        const size_t expect_len = (w.len == 0)
            ? (N - static_cast<size_t>(w.off))
            : std::min<size_t>(static_cast<size_t>(w.len), N - static_cast<size_t>(w.off));
        std::vector<uint8_t> expect(whole.begin() + w.off, whole.begin() + w.off + expect_len);
        check(got == expect, label + std::string(": range content ") + w.name);

        // SR-12: total_size is the whole version, not what this read touched.
        check(rep.total_size == static_cast<int64_t>(N),
              label + std::string(": total_size on ") + w.name +
                  " = " + std::to_string(rep.total_size));
        check(rep.range_length == static_cast<int64_t>(expect_len),
              label + std::string(": range_length on ") + w.name);
        // SR-13: tier 1 satisfies every format by scanning, and says so.
        check(rep.range_method == "scan", label + ": range_method is reported");
    }

    // SR-14: overlong clamps rather than failing.
    {
        std::vector<uint8_t> got;
        FileSystem::RangeReport rep;
        check(read_range(*f.fs, uid, f.tenant, static_cast<int64_t>(N) - 10, 1000000, got, &rep),
              label + ": overlong length");
        check(got.size() == 10, label + ": overlong clamps to the end");
    }

    // SR-15 / SR-16 / SR-17. This is the pair that must not blur.
    {
        FileSystem::RangeReport full;
        std::vector<uint8_t> got;
        check(read_range(*f.fs, uid, f.tenant, 0, 0, got, &full), label + ": full read");
        check(full.authenticated,
              label + ": a FULL read is authenticated (the tag was verified)");
        check(!full.ranged, label + ": 0,0 is not reported as a range");

        FileSystem::RangeReport part;
        check(read_range(*f.fs, uid, f.tenant, 0, 100, got, &part), label + ": head range");
        check(part.ranged, label + ": a window is reported as a range");
        if (encrypt) {
            check(!part.authenticated,
                  label + ": an early-terminated ranged read reports UNAUTHENTICATED");
        } else {
            check(part.authenticated,
                  label + ": unencrypted content has no tag to miss");
        }
    }

    // A negative offset or length is an error, not a silent reinterpretation.
    {
        std::vector<uint8_t> got;
        auto bad = f.fs->get_range(uid, -1, 10, [&got](const uint8_t* p, size_t n) {
            got.insert(got.end(), p, p + n); return true;
        }, USER, ROLES, f.tenant, "", nullptr);
        check(!bad.success, label + ": a negative offset is refused");
    }

    // Every byte offset, in a smaller file, so no window is untested by luck.
    // Run for two representative combinations only: the sweep is O(n^2) reads
    // and each read re-opens and re-decodes the blob, so doing it for all four
    // turns a 40-second suite into a five-minute one for no extra coverage —
    // "plain" and "compressed-encrypted" are the two extremes of the pipeline.
    if (exhaustive) {
        auto small_created = f.fs->touch("", "small.bin", USER, ROLES, f.tenant);
        check(small_created.success, label + ": touch small");
        const auto small = mixed_payload(129);
        check(f.fs->put(small_created.value, small, USER, ROLES, f.tenant).success,
              label + ": put small");
        std::vector<uint8_t> ref;
        check(read_all(*f.fs, small_created.value, f.tenant, ref), label + ": read small");
        check(ref == small, label + ": small payload round-trips");

        bool all_good = true;
        for (int64_t off = 0; off < 129 && all_good; ++off) {
            for (int64_t len = 1; off + len <= 129 && all_good; ++len) {
                std::vector<uint8_t> got;
                if (!read_range(*f.fs, small_created.value, f.tenant, off, len, got)) {
                    all_good = false;
                    check(false, label + ": exhaustive range read failed at " +
                                     std::to_string(off) + "," + std::to_string(len));
                    break;
                }
                std::vector<uint8_t> expect(ref.begin() + off, ref.begin() + off + len);
                if (got != expect) {
                    all_good = false;
                    check(false, label + ": exhaustive range mismatch at " +
                                     std::to_string(off) + "," + std::to_string(len));
                }
            }
        }
        check(all_good, label + ": every (offset,length) over a 129-byte file is exact");
    }

    destroy_fixture(f);
}

int main() {
    std::cout << "=== storage_range_live_tests ===\n";

    // Probe first so an absent database is a SKIP, not a wall of failures.
    {
        Fixture probe;
        if (!make_fixture(probe, false, false, "probe")) {
            std::cout << "SKIP: could not initialise a tenant (no Postgres?)\n";
            destroy_fixture(probe);
            return 77;
        }
        destroy_fixture(probe);
    }

    run_combination(false, false, "plain", /*exhaustive=*/true);
    run_combination(true,  false, "encrypted", /*exhaustive=*/false);
    run_combination(false, true,  "compressed", /*exhaustive=*/false);
    run_combination(true,  true,  "compressed-encrypted", /*exhaustive=*/true);

    std::cout << "\n=== " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed ===\n";
    return g_failures == 0 ? 0 : 1;
}
