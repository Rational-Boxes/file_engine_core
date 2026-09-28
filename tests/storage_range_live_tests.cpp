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

static bool make_fixture(Fixture& f, bool encrypt, bool compress, const std::string& suffix,
                         int write_format = 1, bool selective = true) {
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
    cfg.storage_write_format = write_format;
    cfg.storage_selective_compression = selective;
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
                            bool exhaustive, int write_format = 1) {
    std::cout << "ranges end-to-end: " << label << "..." << std::endl;

    Fixture f;
    if (!make_fixture(f, encrypt, compress, label, write_format, /*selective=*/true)) {
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
        // SR-13. v1 satisfies every window by scanning; v2 seeks. Asserting
        // the value rather than merely its presence is what would catch a v2
        // read silently falling back to the v1 path.
        const char* expect_method = (write_format == 2) ? "seek" : "scan";
        check(rep.range_method == expect_method,
              label + std::string(": range_method is ") + expect_method +
                  " (got " + rep.range_method + ")");
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
        if (write_format == 2) {
            // SR-28: v2 verifies every block a range touches, so a ranged read
            // is authenticated. This is the trade the format exists to remove.
            check(part.authenticated,
                  label + ": a v2 ranged read IS authenticated (SR-28)");
        } else if (encrypt) {
            check(!part.authenticated,
                  label + ": an early-terminated v1 ranged read reports UNAUTHENTICATED");
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

// §7.1 / SR-33: the switch is what decides the format, and the default must be
// 1 so that deploying the code does not start writing v2.
static void test_write_format_is_opt_in() {
    std::cout << "SR-33: v2 is written only when configured..." << std::endl;

    // Default config: a fresh TenantConfig must not write v2.
    TenantConfig fresh;
    check(fresh.storage_write_format == 1,
          "TenantConfig defaults to format 1 — deploying the code writes no v2");

    Fixture f;
    if (!make_fixture(f, /*encrypt=*/true, /*compress=*/true, "fmtdefault", /*write_format=*/1)) {
        check(false, "fixture");
        return;
    }
    auto c = f.fs->touch("", "v1.bin", USER, ROLES, f.tenant);
    check(c.success, "touch");
    const auto payload = mixed_payload(5000);
    check(f.fs->put(c.value, payload, USER, ROLES, f.tenant).success, "put");

    // The version row must say 1, and the stored bytes must NOT be v2.
    auto rec = f.db->get_version_transform(c.value, f.fs->stat(c.value, USER, ROLES, f.tenant).value.version,
                                           f.tenant);
    check(rec.success && rec.value.has_value() && rec.value->storage_format == 1,
          "a default deployment records storage_format = 1");

    destroy_fixture(f);
}

// A tenant that switches the setting mid-life ends up with both formats. That
// is the steady state P6/P7 create, and §6.9 says it is permanent — so it has
// to read correctly, per version, forever.
static void test_mixed_corpus() {
    std::cout << "§6.9: v1 and v2 versions of the same file both read..." << std::endl;

    Fixture f;
    if (!make_fixture(f, /*encrypt=*/true, /*compress=*/true, "mixed", /*write_format=*/1)) {
        check(false, "fixture");
        return;
    }
    auto c = f.fs->touch("", "mixed.bin", USER, ROLES, f.tenant);
    check(c.success, "touch");
    const std::string uid = c.value;

    const auto v1_payload = mixed_payload(9000);
    check(f.fs->put(uid, v1_payload, USER, ROLES, f.tenant).success, "put v1");
    const std::string v1_version = f.fs->stat(uid, USER, ROLES, f.tenant).value.version;

    // Flip the switch, as P6 does, and write another version of the SAME file.
    TenantContext* ctx = f.tm->get_tenant_context(f.tenant);
    check(ctx != nullptr, "tenant context");
    if (ctx) ctx->config.storage_write_format = 2;

    const auto v2_payload = mixed_payload(11000);
    check(f.fs->put(uid, v2_payload, USER, ROLES, f.tenant).success, "put v2");
    const std::string v2_version = f.fs->stat(uid, USER, ROLES, f.tenant).value.version;
    check(v1_version != v2_version, "two distinct versions");

    auto r1 = f.db->get_version_transform(uid, v1_version, f.tenant);
    auto r2 = f.db->get_version_transform(uid, v2_version, f.tenant);
    check(r1.value.has_value() && r1.value->storage_format == 1, "the first version is v1");
    check(r2.value.has_value() && r2.value->storage_format == 2, "the second version is v2");

    // The current version (v2) reads through every path.
    std::vector<uint8_t> whole;
    check(read_all(*f.fs, uid, f.tenant, whole), "get_stream on v2");
    check(whole == v2_payload, "v2 content is correct through get_stream");

    auto got = f.fs->get(uid, USER, ROLES, f.tenant);
    check(got.success && got.value == v2_payload, "v2 content is correct through get");

    // And the OLD v1 version still reads, selected by its own record (SR-32).
    auto old = f.fs->get_version(uid, v1_version, USER, ROLES, f.tenant);
    check(old.success, "the v1 version still reads: " + old.error);
    check(old.success && old.value == v1_payload, "…and is byte-identical");

    // Ranges over the v2 version seek and are authenticated.
    FileSystem::RangeReport rep;
    std::vector<uint8_t> window;
    check(read_range(*f.fs, uid, f.tenant, 4321, 1000, window, &rep), "range on v2");
    check(window == std::vector<uint8_t>(v2_payload.begin() + 4321,
                                         v2_payload.begin() + 5321),
          "v2 range content");
    check(rep.range_method == "seek", "v2 ranges seek");
    check(rep.authenticated, "v2 ranges are authenticated");
    check(rep.total_size == static_cast<int64_t>(v2_payload.size()), "v2 total_size");

    destroy_fixture(f);
}

// THE ROLLBACK PROPERTY. With the measurement off — the default, and what the
// first deploy runs — an already-compressed payload must still be stored
// compressed, exactly as the previous binary stored it. That is what keeps the
// deploy reversible: a binary that decides by configuration can still read
// everything the new one wrote.
static void test_default_deploy_writes_what_the_old_binary_reads() {
    std::cout << "rollback: with the measurement off, writes are unchanged..." << std::endl;

    // A PNG: the case the measurement would store uncompressed, and therefore
    // the case a rollback would be unable to read.
    std::vector<uint8_t> png = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};
    png.resize(20000, 0x5A);

    // Default: selective compression OFF.
    {
        Fixture f;
        if (!make_fixture(f, /*encrypt=*/true, /*compress=*/true, "rollback_off",
                          /*write_format=*/1, /*selective=*/false)) {
            check(false, "fixture"); return;
        }
        TenantConfig fresh;
        check(!fresh.storage_selective_compression,
              "TenantConfig defaults to the ORIGINAL compression behaviour");

        auto c = f.fs->touch("", "image.png", USER, ROLES, f.tenant);
        check(c.success, "touch");
        check(f.fs->put(c.value, png, USER, ROLES, f.tenant).success, "put");

        const std::string ver = f.fs->stat(c.value, USER, ROLES, f.tenant).value.version;
        auto rec = f.db->get_version_transform(c.value, ver, f.tenant);
        check(rec.success && rec.value.has_value(), "transform recorded");
        check(rec.value.has_value() && rec.value->compressed.value_or(false),
              "a PNG is still COMPRESSED with the measurement off — so a binary "
              "that decides by configuration can still read it");
        check(rec.value.has_value() && rec.value->storage_format == 1,
              "and it is still format 1");

        std::vector<uint8_t> back;
        check(read_all(*f.fs, c.value, f.tenant, back), "read back");
        check(back == png, "round-trips");
        destroy_fixture(f);
    }

    // Switched on: the same payload is now stored uncompressed, which is the
    // saving — and the reason the switch has to exist.
    {
        Fixture f;
        if (!make_fixture(f, /*encrypt=*/true, /*compress=*/true, "rollback_on",
                          /*write_format=*/1, /*selective=*/true)) {
            check(false, "fixture"); return;
        }
        auto c = f.fs->touch("", "image.png", USER, ROLES, f.tenant);
        check(c.success, "touch");
        check(f.fs->put(c.value, png, USER, ROLES, f.tenant).success, "put");
        const std::string ver = f.fs->stat(c.value, USER, ROLES, f.tenant).value.version;
        auto rec = f.db->get_version_transform(c.value, ver, f.tenant);
        check(rec.value.has_value() && !rec.value->compressed.value_or(true),
              "with the measurement on, a PNG is stored uncompressed");

        std::vector<uint8_t> back;
        check(read_all(*f.fs, c.value, f.tenant, back), "read back");
        check(back == png, "and still round-trips through the record");
        destroy_fixture(f);
    }
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

    // The same battery again, with the format switch on (§7.1 / SR-33).
    run_combination(true,  true,  "v2-compressed-encrypted", /*exhaustive=*/true,  /*write_format=*/2);
    run_combination(true,  false, "v2-encrypted",            /*exhaustive=*/false, /*write_format=*/2);
    run_combination(false, true,  "v2-compressed",           /*exhaustive=*/false, /*write_format=*/2);

    test_write_format_is_opt_in();
    test_default_deploy_writes_what_the_old_binary_reads();
    test_mixed_corpus();

    std::cout << "\n=== " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed ===\n";
    return g_failures == 0 ? 0 : 1;
}
