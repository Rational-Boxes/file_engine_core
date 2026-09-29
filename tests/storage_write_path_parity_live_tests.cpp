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

// THE TWO WRITE PATHS MUST PRODUCE THE SAME STORAGE FORMAT.
//
// This suite exists because that was not true in production and nothing caught
// it. Core 1.9.36 shipped with FILEENGINE_STORAGE_WRITE_FORMAT=2, the config
// was read correctly, the core started clean — and every file uploaded through
// a door was still written as v1, because FileSystem::put_stream hard-coded
// storage_format=1 and never consulted the setting. Measured on the live
// deployment after the deploy: 0 v2 versions anywhere, across four tenants.
//
// WHY THE EXISTING TESTS DID NOT SEE IT. storage_format_v2_tests.cpp has
// ~186,000 adversarial checks on the FORMAT — block swaps, cross-version
// splices, truncation at every seventh byte, every bit of the header flipped.
// storage_range_live_tests.cpp drives a real FileSystem with write_format=2.
// Both were thorough and both were blind in the same way: they only ever called
// FileSystem::put(). Every door uploads through StreamFileUpload, which lands
// in put_stream(), and no test in the tree called it with write_format=2.
//
// So the assertions here are deliberately not about v2's internals. They are
// about PARITY: whatever put() does with a given configuration, put_stream()
// must do too, and the recorded storage_format must match the bytes actually on
// disk. A test that checks only the database record would have passed
// throughout the incident, because put_stream wrote `1` and then honestly
// stored a v1 blob — the record was accurate, and the deployment was still not
// doing what it was configured to do.
//
// Skips (77) rather than fails when no Postgres is reachable, like the other
// live suites.

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "fileengine/accountability.h"
#include "fileengine/acl_manager.h"
#include "fileengine/database.h"
#include "fileengine/filesystem.h"
#include "fileengine/storage_format_v2.h"
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

// Deterministic, compressible-or-not on demand: a run of repeated text
// compresses, a PRNG stream does not.
static std::vector<uint8_t> make_payload(size_t n, bool compressible) {
    std::vector<uint8_t> v;
    v.reserve(n);
    if (compressible) {
        const std::string unit = "the same line over and over, which zlib enjoys. ";
        while (v.size() < n) v.insert(v.end(), unit.begin(), unit.end());
        v.resize(n);
        return v;
    }
    uint64_t x = 0x9E3779B97F4A7C15ull ^ n;
    while (v.size() < n) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        v.push_back(static_cast<uint8_t>(x));
    }
    v.resize(n);
    return v;
}

struct Fixture {
    std::shared_ptr<Database> db;
    std::shared_ptr<TenantManager> tm;
    std::unique_ptr<FileSystem> fs;
    std::string tenant;
    std::string root;
};

static bool make_fixture(Fixture& f, const std::string& suffix,
                         int write_format, bool encrypt, bool compress,
                         bool selective) {
    TenantConfig cfg;
    cfg.db_host = env_or("FILEENGINE_PG_HOST", env_or("FE_TEST_PG_HOST", "localhost"));
    cfg.db_port = std::stoi(env_or("FILEENGINE_PG_PORT", env_or("FE_TEST_PG_PORT", "5434")));
    cfg.db_name = env_or("FILEENGINE_PG_DATABASE", env_or("FE_TEST_PG_DB", "fileengine"));
    cfg.db_user = env_or("FILEENGINE_PG_USER", env_or("FE_TEST_PG_USER", "postgres"));
    cfg.db_password = env_or("FILEENGINE_PG_PASSWORD", env_or("FE_TEST_PG_PASSWORD", "postgres"));
    f.root = "/tmp/fe_parity_" + std::to_string(::getpid()) + "_" + suffix;
    cfg.storage_base_path = f.root;
    cfg.s3_endpoint = ""; cfg.s3_region = ""; cfg.s3_bucket = "";
    cfg.s3_access_key = ""; cfg.s3_secret_key = ""; cfg.s3_path_style = true;
    cfg.encrypt_data = encrypt;
    cfg.compress_data = compress;
    cfg.storage_write_format = write_format;
    cfg.storage_selective_compression = selective;
    cfg.encryption_key =
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

    f.tenant = "par_" + std::to_string(::getpid()) + "_" + suffix;
    f.db = std::make_shared<Database>(cfg.db_host, cfg.db_port, cfg.db_name,
                                      cfg.db_user, cfg.db_password, /*pool_size=*/8);
    if (!f.db->connect()) return false;
    if (!f.db->create_schema().success) return false;
    f.tm = std::make_shared<TenantManager>(cfg, f.db);
    if (!f.tm->initialize_tenant(f.tenant)) return false;
    f.fs = std::make_unique<FileSystem>(f.tm);
    f.fs->set_acl_manager(std::make_shared<AclManager>(f.db));
    return true;
}

static void destroy_fixture(Fixture& f) {
    f.fs.reset();
    f.tm.reset();
    if (f.db && !f.tenant.empty()) {
        f.db->cleanup_tenant_data(f.tenant, AccountabilityContext::system());
    }
    f.db.reset();
    std::error_code ec;
    std::filesystem::remove_all(f.root, ec);
}

static const char* kUser = "parity@rationalboxes.com";
static const std::vector<std::string> kRoles = {"system_admin"};

// Create a file node and return its uid.
static std::string make_node(Fixture& f, const std::string& name) {
    auto r = f.fs->touch("", name, kUser, kRoles, f.tenant);
    return r.success ? r.value : std::string();
}

// Write through put_stream, handing the payload over in small pieces so the
// block-cutting logic is actually exercised rather than receiving one buffer.
static bool write_streaming(Fixture& f, const std::string& uid,
                            const std::vector<uint8_t>& data, size_t piece) {
    size_t at = 0;
    auto next = [&](std::vector<uint8_t>& out) -> bool {
        if (at >= data.size()) return false;
        const size_t n = std::min(piece, data.size() - at);
        out.assign(data.begin() + at, data.begin() + at + n);
        at += n;
        return true;
    };
    return f.fs->put_stream(uid, next, kUser, kRoles, f.tenant).success;
}

static bool read_all(Fixture& f, const std::string& uid, std::vector<uint8_t>& out) {
    out.clear();
    auto r = f.fs->get_stream(uid, [&](const uint8_t* p, size_t n) {
        out.insert(out.end(), p, p + n);
        return true;
    }, kUser, kRoles, f.tenant);
    return r.success;
}

static bool newest_version(Fixture& f, const std::string& uid, std::string& out) {
    auto vers = f.db->list_versions(uid, f.tenant);
    if (!vers.success || vers.value.empty()) return false;
    out = vers.value.front();
    for (const auto& v : vers.value) if (v > out) out = v;
    return true;
}

// The recorded transform for a file's current version.
static bool recorded(Fixture& f, const std::string& uid, VersionTransform& vt) {
    std::string newest;
    if (!newest_version(f, uid, newest)) return false;
    auto t = f.db->get_version_transform(uid, newest, f.tenant);
    if (!t.success || !t.value.has_value()) return false;
    vt = *t.value;
    return true;
}

// The first bytes actually on disk for a file's current version.
static bool stored_head(Fixture& f, const std::string& uid, std::vector<uint8_t>& head) {
    std::string newest;
    if (!newest_version(f, uid, newest)) return false;
    auto p = f.db->get_version_storage_path(uid, newest, f.tenant);
    if (!p.success || !p.value.has_value() || p.value->empty()) return false;
    std::ifstream in(*p.value, std::ios::binary);
    if (!in) return false;
    head.assign(64, 0);
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(in.gcount()));
    return true;
}

// ---------------------------------------------------------------------------

// The regression, stated as plainly as it can be: configure v2, write through
// the path a door uses, and the bytes on disk must be v2.
static void test_streaming_write_honours_the_configured_format(Fixture& f) {
    const auto data = make_payload(3 * 1024 * 1024 + 7777, /*compressible=*/false);
    const std::string uid = make_node(f, "streamed-v2.bin");
    check(!uid.empty(), "touch created a node for the streamed write");
    if (uid.empty()) return;

    check(write_streaming(f, uid, data, 64 * 1024), "put_stream accepted the payload");

    VersionTransform vt;
    check(recorded(f, uid, vt), "a transform row was recorded for the streamed write");
    check(vt.storage_format == 2,
          "put_stream RECORDED storage_format=2 when configured for v2 (got " +
          std::to_string(vt.storage_format) + ")");

    std::vector<uint8_t> head;
    check(stored_head(f, uid, head), "the streamed version's blob is readable from disk");
    check(!head.empty() && v2::is_v2(head.data(), head.size()),
          "put_stream WROTE a v2 blob when configured for v2 "
          "(the bytes on disk, not the database's opinion of them)");

    std::vector<uint8_t> back;
    check(read_all(f, uid, back), "the streamed v2 version reads back");
    check(back == data, "the streamed v2 version round-trips byte for byte");
}

// Parity is the invariant. Asserting put_stream alone would let a later change
// fix one path and regress the other.
static void test_both_write_paths_agree(Fixture& f) {
    const auto data = make_payload(2 * 1024 * 1024, /*compressible=*/true);

    const std::string a = make_node(f, "buffered.bin");
    const std::string b = make_node(f, "streamed.bin");
    check(!a.empty() && !b.empty(), "both nodes created");
    if (a.empty() || b.empty()) return;

    check(f.fs->put(a, data, kUser, kRoles, f.tenant).success, "put accepted the payload");
    check(write_streaming(f, b, data, 100 * 1024), "put_stream accepted the payload");

    VersionTransform va{}, vb{};
    check(recorded(f, a, va) && recorded(f, b, vb), "both transforms recorded");
    check(va.storage_format == vb.storage_format,
          "the two write paths recorded the SAME storage_format (put=" +
          std::to_string(va.storage_format) + ", put_stream=" +
          std::to_string(vb.storage_format) + ")");
    check(va.encrypted == vb.encrypted, "the two write paths agree on encryption");

    std::vector<uint8_t> ha, hb;
    check(stored_head(f, a, ha) && stored_head(f, b, hb), "both blobs readable");
    const bool a_is_v2 = !ha.empty() && v2::is_v2(ha.data(), ha.size());
    const bool b_is_v2 = !hb.empty() && v2::is_v2(hb.data(), hb.size());
    check(a_is_v2 == b_is_v2,
          "the two write paths produced the SAME FORMAT ON DISK (put v2=" +
          std::string(a_is_v2 ? "yes" : "no") + ", put_stream v2=" +
          std::string(b_is_v2 ? "yes" : "no") + ")");

    std::vector<uint8_t> ra, rb;
    check(read_all(f, a, ra) && read_all(f, b, rb), "both read back");
    check(ra == data && rb == data, "both round-trip byte for byte");
}

// The record must describe the bytes. A record that says v1 over a v1 blob is
// self-consistent and still wrong when the deployment asked for v2 — so this
// checks the record against the DISK, and the caller checks the disk against
// the configuration.
static void test_record_matches_the_bytes(Fixture& f, int configured) {
    const auto data = make_payload(700 * 1024, /*compressible=*/false);
    const std::string uid = make_node(f, "record-vs-bytes.bin");
    if (uid.empty()) { check(false, "touch created a node"); return; }
    check(write_streaming(f, uid, data, 32 * 1024), "streamed write accepted");

    VersionTransform vt;
    std::vector<uint8_t> head;
    if (!recorded(f, uid, vt) || !stored_head(f, uid, head)) {
        check(false, "transform and blob both available");
        return;
    }
    const bool on_disk_v2 = !head.empty() && v2::is_v2(head.data(), head.size());
    check((vt.storage_format == 2) == on_disk_v2,
          "the recorded storage_format describes the bytes actually stored");
    check(vt.storage_format == configured,
          "the recorded storage_format matches the configured write format (configured=" +
          std::to_string(configured) + ", recorded=" + std::to_string(vt.storage_format) + ")");
}

// v1 must stay reachable and stay v1 — the switch is a switch, not a ratchet.
static void test_v1_configuration_still_writes_v1(Fixture& f) {
    const auto data = make_payload(300 * 1024, /*compressible=*/true);
    const std::string uid = make_node(f, "streamed-v1.bin");
    if (uid.empty()) { check(false, "touch created a node"); return; }
    check(write_streaming(f, uid, data, 16 * 1024), "streamed write accepted");

    VersionTransform vt;
    check(recorded(f, uid, vt), "transform recorded");
    check(vt.storage_format == 1, "configured for v1, recorded v1");

    std::vector<uint8_t> head;
    check(stored_head(f, uid, head), "blob readable");
    check(head.empty() || !v2::is_v2(head.data(), head.size()),
          "configured for v1, did NOT write a v2 header");

    std::vector<uint8_t> back;
    check(read_all(f, uid, back) && back == data, "v1 streamed write round-trips");
}

// A ranged read of a STREAMED v2 payload must be able to seek. This is the
// payoff the whole format exists for, and while put_stream wrote v1 it was
// unreachable for every file a door had ever written.
static void test_streamed_v2_supports_a_seeking_range(Fixture& f) {
    const auto data = make_payload(4 * 1024 * 1024 + 11, /*compressible=*/false);
    const std::string uid = make_node(f, "streamed-range.bin");
    if (uid.empty()) { check(false, "touch created a node"); return; }
    check(write_streaming(f, uid, data, 128 * 1024), "streamed write accepted");

    const int64_t off = 3 * 1024 * 1024 + 5;
    const int64_t len = 4096;
    std::vector<uint8_t> got;
    FileSystem::RangeReport rep;
    auto r = f.fs->get_range(uid, off, len, [&](const uint8_t* p, size_t n) {
        got.insert(got.end(), p, p + n);
        return true;
    }, kUser, kRoles, f.tenant, "", &rep);
    check(r.success, "ranged read of a streamed v2 payload succeeded");
    check(got.size() == static_cast<size_t>(len), "the range returned the requested length");
    check(std::memcmp(got.data(), data.data() + off, got.size()) == 0,
          "the range is byte-identical to the same window of the original");
    check(rep.total_size == static_cast<int64_t>(data.size()),
          "the range report carries the whole version's size");
    check(rep.range_method == "seek",
          "a streamed v2 payload seeks rather than scans (got '" + rep.range_method + "')");
}

// Sizes at and around the block boundary, fed in chunkings that deliberately do
// NOT align with it. A streaming block-cutter breaks here or nowhere: the
// earlier v2 writer emitted a spurious empty final block when the payload was an
// exact multiple of block_size, and that was found by a round-trip matrix rather
// than by review. The streaming path re-opens that risk, because now the CALLER
// also picks arbitrary boundaries.
static void test_block_boundaries_under_every_chunking(Fixture& f, bool expect_v2) {
    const size_t B = v2::kDefaultBlockSize;
    const size_t sizes[] = {0, 1, 2, B - 1, B, B + 1, 2 * B, 2 * B + 1, 2 * B + 7};
    const size_t chunks[] = {7, 4096, 64 * 1024, static_cast<size_t>(-1)};

    int n = 0;
    for (size_t sz : sizes) {
        for (size_t ck : chunks) {
            // Tiny chunkings over multi-megabyte payloads add minutes and no
            // information — the boundary logic is already exercised by the
            // small sizes. Skip that corner deliberately rather than silently.
            if (ck == 7 && sz > 128 * 1024) continue;
            const size_t piece = (ck == static_cast<size_t>(-1)) ? (sz ? sz : 1) : ck;
            const auto data = make_payload(sz, /*compressible=*/(sz % 2) == 0);
            const std::string name = "bb_" + std::to_string(sz) + "_" + std::to_string(piece) + ".bin";
            const std::string uid = make_node(f, name);
            if (uid.empty()) { check(false, "touch " + name); continue; }
            if (!write_streaming(f, uid, data, piece)) { check(false, "stream " + name); continue; }

            std::vector<uint8_t> back;
            const bool ok = read_all(f, uid, back);
            check(ok && back == data,
                  "round-trip size=" + std::to_string(sz) + " chunk=" + std::to_string(piece));

            std::vector<uint8_t> head;
            if (stored_head(f, uid, head)) {
                const bool is2 = !head.empty() && v2::is_v2(head.data(), head.size());
                check(is2 == expect_v2,
                      "format on disk size=" + std::to_string(sz) +
                      " chunk=" + std::to_string(piece) +
                      " (expected v2=" + (expect_v2 ? "yes" : "no") + ")");
            } else {
                // A zero-byte v1 blob legitimately has no head to read.
                check(sz == 0 && !expect_v2, "blob readable for " + name);
            }
            ++n;
        }
    }
    check(n > 0, "the boundary matrix actually ran");
}

// A ranged read must agree with the same window of a full read, for a STREAMED
// payload, at boundaries. This is the assertion that would catch an off-by-one
// in the block index that a whole-file round trip cannot see.
static void test_streamed_ranges_match_the_whole_read(Fixture& f) {
    const size_t B = v2::kDefaultBlockSize;
    const auto data = make_payload(2 * B + 4321, /*compressible=*/false);
    const std::string uid = make_node(f, "range-windows.bin");
    if (uid.empty()) { check(false, "touch for range windows"); return; }
    if (!write_streaming(f, uid, data, 33 * 1024)) { check(false, "stream for range windows"); return; }

    struct W { int64_t off; int64_t len; const char* what; };
    const W windows[] = {
        {0, 16, "at the start"},
        {static_cast<int64_t>(B) - 8, 16, "straddling the first block boundary"},
        {static_cast<int64_t>(B), 32, "exactly on a block boundary"},
        {static_cast<int64_t>(2 * B) - 1, 2, "straddling the second boundary"},
        {static_cast<int64_t>(data.size()) - 5, 5, "the last five bytes"},
        {static_cast<int64_t>(data.size()) - 1, 0, "to the end from one byte short"},
    };
    for (const auto& w : windows) {
        std::vector<uint8_t> got;
        FileSystem::RangeReport rep;
        auto r = f.fs->get_range(uid, w.off, w.len, [&](const uint8_t* p, size_t n) {
            got.insert(got.end(), p, p + n);
            return true;
        }, kUser, kRoles, f.tenant, "", &rep);
        const size_t want = (w.len == 0) ? (data.size() - static_cast<size_t>(w.off))
                                         : static_cast<size_t>(w.len);
        check(r.success, std::string("ranged read ") + w.what);
        check(got.size() == want, std::string("range length ") + w.what);
        check(got.size() == want &&
              std::memcmp(got.data(), data.data() + w.off, got.size()) == 0,
              std::string("range bytes match the original ") + w.what);
    }
}

int main() {
    Fixture probe;
    if (!make_fixture(probe, "probe", 1, false, false, true)) {
        std::cout << "storage write-path parity: no Postgres reachable — skipping\n";
        return 77;
    }
    destroy_fixture(probe);

    struct Case { const char* name; bool encrypt; bool compress; };
    const Case cases[] = {
        {"plain",    false, false},
        {"enc",      true,  false},
        {"comp",     false, true},
        {"enc_comp", true,  true},
    };

    for (const auto& c : cases) {
        Fixture f2;
        if (make_fixture(f2, std::string("v2_") + c.name, 2, c.encrypt, c.compress, true)) {
            std::cout << "-- write_format=2, " << c.name << "\n";
            test_streaming_write_honours_the_configured_format(f2);
            test_both_write_paths_agree(f2);
            test_record_matches_the_bytes(f2, 2);
            test_streamed_v2_supports_a_seeking_range(f2);
            test_block_boundaries_under_every_chunking(f2, /*expect_v2=*/true);
            test_streamed_ranges_match_the_whole_read(f2);
            destroy_fixture(f2);
        } else {
            check(false, std::string("fixture for v2/") + c.name);
        }

        Fixture f1;
        if (make_fixture(f1, std::string("v1_") + c.name, 1, c.encrypt, c.compress, true)) {
            std::cout << "-- write_format=1, " << c.name << "\n";
            test_v1_configuration_still_writes_v1(f1);
            test_both_write_paths_agree(f1);
            test_record_matches_the_bytes(f1, 1);
            test_block_boundaries_under_every_chunking(f1, /*expect_v2=*/false);
            destroy_fixture(f1);
        } else {
            check(false, std::string("fixture for v1/") + c.name);
        }
    }

    std::cout << "storage write-path parity: " << g_checks << " checks, "
              << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
