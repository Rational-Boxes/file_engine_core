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

// TWO WRITES TO ONE FILE IN ONE MILLISECOND MUST BE TWO VERSIONS.
//
// A version id was the write's start time to the millisecond, and the blob path
// is made from it. Two writes that started in the same millisecond opened the
// same path with truncate and streamed into it together, and the version
// insert's ON CONFLICT DO UPDATE let both succeed: one version whose bytes
// interleave two uploads, two callers told their save landed (version_id.h).
//
// Unit part (no infrastructure, `--unit-only`): the id sequence a frozen clock
// produces, that it still sorts as text in the order claimed, and that a full
// millisecond rolls over to the next.
//
// Live part (skips with 77 without Postgres), under v1 and the v2+encrypt+
// compress production configuration:
//   * a write into a millisecond whose bare id is already taken gets "-1",
//     reads back as its own bytes, and leaves the other blob untouched;
//   * a write into milliseconds whose ten ids are all taken moves past them;
//   * many concurrent put_stream/put calls on one file each become exactly one
//     version holding exactly that caller's bytes;
//   * the database refuses a second row for an existing (file, version).

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "fileengine/acl_manager.h"
#include "fileengine/accountability.h"
#include "fileengine/database.h"
#include "fileengine/filesystem.h"
#include "fileengine/storage.h"
#include "fileengine/tenant_manager.h"
#include "fileengine/version_id.h"

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

// Utils::get_timestamp_string's format, for an arbitrary moment.
static std::string ts_at(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y%m%d_%H%M%S") << "." << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

// ---------------------------------------------------------------- unit part

static void unit_tests() {
    std::cout << "[unit] id sequence under a frozen clock" << std::endl;
    const std::string root = "/tmp/fe_vid_unit_" + std::to_string(::getpid());
    Storage storage(root);
    const std::string uid = "0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0";

    check(version_id::with_suffix("20261029_120000.123", 0) == "20261029_120000.123", "suffix 0 is bare");
    check(version_id::with_suffix("20261029_120000.123", 3) == "20261029_120000.123-3", "suffix 3");

    std::string frozen = "20261029_120000.123";
    std::vector<std::string> got;
    for (int i = 0; i <= version_id::kMaxSuffix; ++i) {
        auto r = version_id::reserve(storage, uid, "t", [&] { return frozen; });
        check(r.success, "reserve #" + std::to_string(i));
        if (!r.success) break;
        check(std::filesystem::exists(r.value.path), "reservation creates the blob path");
        check(r.value.path == storage.get_storage_path(uid, r.value.version, "t"), "path matches the id");
        got.push_back(r.value.version);
    }
    check(got.size() == 10 && got.front() == frozen && got.back() == frozen + "-9",
          "ten ids in one millisecond: bare, -1 .. -9");
    for (size_t i = 1; i < got.size(); ++i) {
        check(got[i - 1] < got[i], "claimed ids sort as text in claim order: " + got[i - 1] + " < " + got[i]);
    }
    check(frozen + "-9" < "20261029_120000.124", "a suffixed id sorts before the next millisecond");

    // The eleventh write in that millisecond waits for the next one.
    int calls = 0;
    auto r = version_id::reserve(storage, uid, "t", [&] {
        return ++calls < 5 ? frozen : std::string("20261029_120000.124");
    });
    check(r.success && r.value.version == "20261029_120000.124",
          "a full millisecond rolls over to the next (got " + (r.success ? r.value.version : r.error) + ")");

    // A clock that never moves on fails rather than spinning.
    auto stuck = version_id::reserve(storage, uid, "t", [&] { return frozen; });
    check(!stuck.success, "a clock stuck on a full millisecond gives up");

    // A different file never competes for the same ids.
    auto other = version_id::reserve(storage, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "t",
                                     [&] { return frozen; });
    check(other.success && other.value.version == frozen, "ids are per file");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// ---------------------------------------------------------------- live part

struct Fixture {
    std::shared_ptr<Database> db;
    std::shared_ptr<TenantManager> tm;
    std::unique_ptr<FileSystem> fs;
    std::string tenant;
    std::string root;
};

static bool make_fixture(Fixture& f, const std::string& suffix, int write_format, bool encrypt, bool compress) {
    f.root = "/tmp/fe_vid_" + std::to_string(::getpid()) + "_" + suffix;
    f.tenant = "vid_" + std::to_string(::getpid()) + "_" + suffix;
    TenantConfig cfg;
    cfg.db_host = env_or("FILEENGINE_PG_HOST", env_or("FE_TEST_PG_HOST", "localhost"));
    cfg.db_port = std::stoi(env_or("FILEENGINE_PG_PORT", env_or("FE_TEST_PG_PORT", "5434")));
    cfg.db_name = env_or("FILEENGINE_PG_DATABASE", env_or("FE_TEST_PG_DB", "fileengine"));
    cfg.db_user = env_or("FILEENGINE_PG_USER", env_or("FE_TEST_PG_USER", "postgres"));
    cfg.db_password = env_or("FILEENGINE_PG_PASSWORD", env_or("FE_TEST_PG_PASSWORD", "postgres"));
    cfg.storage_base_path = f.root;
    cfg.s3_endpoint = ""; cfg.s3_region = ""; cfg.s3_bucket = "";
    cfg.s3_access_key = ""; cfg.s3_secret_key = ""; cfg.s3_path_style = true;
    cfg.encrypt_data = encrypt;
    cfg.compress_data = compress;
    cfg.storage_write_format = write_format;
    cfg.storage_selective_compression = true;
    cfg.encryption_key = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    f.db = std::make_shared<Database>(cfg.db_host, cfg.db_port, cfg.db_name,
                                      cfg.db_user, cfg.db_password, /*pool_size=*/16);
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
    if (f.db && !f.tenant.empty()) f.db->cleanup_tenant_data(f.tenant, AccountabilityContext::system());
    f.db.reset();
    std::error_code ec;
    std::filesystem::remove_all(f.root, ec);
}

static const char* kUser = "versionid@rationalboxes.com";
static const std::vector<std::string> kRoles = {"system_admin"};

// Distinct, incompressible, several v2 blocks and several stream chunks long,
// so concurrent writers really do interleave if they share a blob.
static std::vector<uint8_t> payload(uint64_t seed, size_t n) {
    std::vector<uint8_t> v;
    v.reserve(n);
    uint64_t x = 0x9E3779B97F4A7C15ull ^ (seed * 0xD1B54A32D192ED03ull);
    while (v.size() < n) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        v.push_back(static_cast<uint8_t>(x));
    }
    return v;
}

static Result<void> stream_put(Fixture& f, const std::string& uid, const std::vector<uint8_t>& data) {
    size_t off = 0;
    const size_t chunk = 64 * 1024;
    return f.fs->put_stream(uid, [&](std::vector<uint8_t>& out) -> bool {
        if (off >= data.size()) return false;
        size_t n = std::min(chunk, data.size() - off);
        out.assign(data.begin() + off, data.begin() + off + n);
        off += n;
        return true;
    }, kUser, kRoles, f.tenant);
}

static bool read_version(Fixture& f, const std::string& uid, const std::string& ver, std::vector<uint8_t>& out) {
    out.clear();
    auto r = f.fs->get_stream(uid, [&](const uint8_t* p, size_t n) {
        out.insert(out.end(), p, p + n);
        return true;
    }, kUser, kRoles, f.tenant, ver);
    return r.success;
}

static std::vector<std::string> versions(Fixture& f, const std::string& uid) {
    auto r = f.fs->list_versions(uid, kUser, kRoles, f.tenant);
    return r.success ? r.value : std::vector<std::string>{};
}

static std::string make_file(Fixture& f, const std::string& name) {
    auto r = f.fs->touch("", name, kUser, kRoles, f.tenant);
    return r.success ? r.value : std::string();
}

// Occupy every id from `first_suffix` to `last_suffix` for each millisecond in
// [now, now + window) with a sentinel blob, as another writer would have.
static std::vector<std::pair<std::string, std::string>> occupy(Fixture& f, const std::string& uid,
                                                               int window_ms, int last_suffix) {
    std::vector<std::pair<std::string, std::string>> taken;   // path, sentinel text
    auto ctx = f.tm->get_tenant_context(f.tenant);
    auto start = std::chrono::system_clock::now();
    for (int ms = 0; ms < window_ms; ++ms) {
        const std::string base = ts_at(start + std::chrono::milliseconds(ms));
        for (int n = 0; n <= last_suffix; ++n) {
            const std::string ver = version_id::with_suffix(base, n);
            const std::string path = ctx->storage->get_storage_path(uid, ver, f.tenant);
            std::filesystem::create_directories(std::filesystem::path(path).parent_path());
            std::ofstream o(path, std::ios::binary);
            const std::string text = "SENTINEL " + ver;
            o << text;
            taken.emplace_back(path, text);
        }
    }
    return taken;
}

static bool sentinels_intact(const std::vector<std::pair<std::string, std::string>>& taken) {
    for (const auto& [path, text] : taken) {
        std::ifstream i(path, std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
        if (got != text) return false;
    }
    return true;
}

static void live_case(const std::string& label, int write_format, bool encrypt, bool compress) {
    std::cout << "[live] " << label << std::endl;
    Fixture f;
    if (!make_fixture(f, label, write_format, encrypt, compress)) {
        check(false, label + ": fixture");
        destroy_fixture(f);
        return;
    }

    // 1. The bare id of this millisecond (and the next 400) is already taken.
    {
        const std::string uid = make_file(f, "occupied.bin");
        check(!uid.empty(), label + ": touch");
        auto taken = occupy(f, uid, 400, 0);
        auto data = payload(1, 300 * 1024);
        auto r = stream_put(f, uid, data);
        check(r.success, label + ": put_stream into an occupied millisecond (" + r.error + ")");
        auto v = versions(f, uid);
        check(v.size() == 1, label + ": exactly one recorded version");
        if (!v.empty()) {
            check(v[0].size() > 2 && v[0].substr(v[0].size() - 2) == "-1",
                  label + ": took the -1 id (got " + v[0] + ")");
            std::vector<uint8_t> back;
            check(read_version(f, uid, v[0], back) && back == data, label + ": reads back its own bytes");
        }
        check(sentinels_intact(taken), label + ": the other writer's blobs are untouched");

        // put() takes the same path.
        auto data2 = payload(2, 1000);
        auto r2 = f.fs->put(uid, data2, kUser, kRoles, f.tenant);
        check(r2.success, label + ": put into an occupied millisecond");
        auto v2 = versions(f, uid);
        check(v2.size() == 2 && v2[0].substr(v2[0].size() - 2) == "-1",
              label + ": put took a -1 id too (got " + (v2.empty() ? "" : v2[0]) + ")");
        std::vector<uint8_t> back2;
        check(!v2.empty() && read_version(f, uid, v2[0], back2) && back2 == data2,
              label + ": put reads back its own bytes");
        check(sentinels_intact(taken), label + ": still untouched after put");
    }

    // 2. Every id of the next 50 milliseconds is taken: the write moves past them.
    {
        const std::string uid = make_file(f, "exhausted.bin");
        const std::string before = ts_at(std::chrono::system_clock::now());
        auto taken = occupy(f, uid, 50, version_id::kMaxSuffix);
        const std::string window_end = ts_at(std::chrono::system_clock::now() + std::chrono::milliseconds(50));
        auto data = payload(3, 200 * 1024);
        auto r = stream_put(f, uid, data);
        check(r.success, label + ": put_stream past full milliseconds (" + r.error + ")");
        auto v = versions(f, uid);
        check(v.size() == 1 && v[0] > before, label + ": recorded one later version");
        std::vector<uint8_t> back;
        check(!v.empty() && read_version(f, uid, v[0], back) && back == data, label + ": reads back");
        check(sentinels_intact(taken), label + ": full milliseconds untouched");
        (void)window_end;
    }

    // 3. Concurrent writers on one file, released together, several rounds.
    {
        const std::string uid = make_file(f, "concurrent.bin");
        const int kThreads = 8, kRounds = 6;
        std::map<std::string, int> owner_of_version;   // filled from reads below
        std::vector<std::vector<uint8_t>> sent;
        int suffixed = 0, ok_writes = 0;
        for (int round = 0; round < kRounds; ++round) {
            std::vector<std::vector<uint8_t>> data;
            for (int t = 0; t < kThreads; ++t) data.push_back(payload(1000 + round * kThreads + t, 256 * 1024 + t));
            std::atomic<int> ready{0};
            std::atomic<bool> go{false};
            std::vector<int> ok(kThreads, 0);
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&, t] {
                    ready.fetch_add(1);
                    while (!go.load()) {}
                    auto r = (t % 2 == 0) ? stream_put(f, uid, data[t])
                                          : f.fs->put(uid, data[t], kUser, kRoles, f.tenant);
                    ok[t] = r.success ? 1 : 0;
                });
            }
            while (ready.load() < kThreads) {}
            go.store(true);
            for (auto& th : threads) th.join();
            for (int t = 0; t < kThreads; ++t) ok_writes += ok[t];
            for (auto& d : data) sent.push_back(std::move(d));
        }
        check(ok_writes == kThreads * kRounds, label + ": every concurrent write succeeded (" +
              std::to_string(ok_writes) + "/" + std::to_string(kThreads * kRounds) + ")");

        auto v = versions(f, uid);
        check(v.size() == sent.size(), label + ": one version per write (" + std::to_string(v.size()) +
              " for " + std::to_string(sent.size()) + ")");
        std::set<size_t> matched;
        bool all_read = true;
        for (const auto& ver : v) {
            if (ver.find('-') != std::string::npos) ++suffixed;
            std::vector<uint8_t> back;
            if (!read_version(f, uid, ver, back)) { all_read = false; check(false, label + ": read " + ver); continue; }
            bool found = false;
            for (size_t i = 0; i < sent.size(); ++i) {
                if (!matched.count(i) && back.size() == sent[i].size() && back == sent[i]) {
                    matched.insert(i); found = true; break;
                }
            }
            check(found, label + ": version " + ver + " holds exactly one caller's bytes");
        }
        check(all_read && matched.size() == sent.size(), label + ": every caller's bytes are a version");
        std::cout << "  concurrent: " << v.size() << " versions, " << suffixed
                  << " of them from a same-millisecond clash" << std::endl;
    }

    // 4. The database never re-points an existing version row.
    {
        const std::string uid = make_file(f, "dbguard.bin");
        const std::string ver = "20261029_120000.123";
        auto a = f.db->insert_version(uid, ver, 10, "/first/path", kUser, f.tenant);
        auto b = f.db->insert_version(uid, ver, 20, "/second/path", kUser, f.tenant);
        check(a.success, label + ": first insert");
        check(!b.success, label + ": second insert of the same (file, version) is refused");
        auto p = f.db->get_version_storage_path(uid, ver, f.tenant);
        check(p.success && p.value.has_value() && *p.value == "/first/path",
              label + ": the row still points at the first blob");
    }

    destroy_fixture(f);
}

int main(int argc, char** argv) {
    const bool unit_only = argc > 1 && std::string(argv[1]) == "--unit-only";
    unit_tests();
    if (!unit_only) {
        Database probe(env_or("FILEENGINE_PG_HOST", env_or("FE_TEST_PG_HOST", "localhost")),
                       std::stoi(env_or("FILEENGINE_PG_PORT", env_or("FE_TEST_PG_PORT", "5434"))),
                       env_or("FILEENGINE_PG_DATABASE", env_or("FE_TEST_PG_DB", "fileengine")),
                       env_or("FILEENGINE_PG_USER", env_or("FE_TEST_PG_USER", "postgres")),
                       env_or("FILEENGINE_PG_PASSWORD", env_or("FE_TEST_PG_PASSWORD", "postgres")), 1);
        if (!probe.connect()) {
            std::cout << "SKIP live part: no Postgres (" << g_checks << " unit checks, "
                      << g_failures << " failures)" << std::endl;
            return g_failures ? 1 : 77;
        }
        live_case("v1", 1, false, false);
        live_case("v2", 2, true, true);
    }
    std::cout << g_checks << " checks, " << g_failures << " failures" << std::endl;
    return g_failures ? 1 : 0;
}
