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

// COPY AND RESTORE MUST LEAVE A FILE THAT READS BACK, AT ITS REAL SIZE.
//
// Both reused a version's STORED bytes. copy() byte-copied each blob under the
// new uid and recorded the blob's stored length as the size, with no transform
// row; restore_to_version() inserted a new-timestamp row pointing at the old
// blob, also with no transform. Under storage v2 every block's AAD binds
// {file_uid, version_timestamp} (SR-25), so neither result could be decrypted:
// production writes v2, and a copied or restored file answered 500 "inflate
// failed". Under v1 the content survived by luck of the config fallback, but
// the recorded size was the ciphertext length, so the bridge promised a
// Content-Length the body never reached and clients saw a terminated download.
//
// Why nothing caught it: the parity suite proves put() and put_stream() write
// the same format, and the range suite reads what put() wrote. Nothing read a
// file that copy() or restore had produced, and the dev stack writes v1.
//
// So every assertion here is about what a CLIENT gets back after the
// operation, under each configuration the core can run with, plus the mixed
// history production actually has: versions written as v1 before the switch,
// copied and restored after it.
//
// Skips (77) rather than fails when no Postgres is reachable.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "fileengine/accountability.h"
#include "fileengine/acl_manager.h"
#include "fileengine/database.h"
#include "fileengine/filesystem.h"
#include "fileengine/storage_format_v2.h"
#include "fileengine/tenant_manager.h"

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

static std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

// Incompressible and large enough to span several v2 blocks.
static std::vector<uint8_t> big_payload(size_t n) {
    std::vector<uint8_t> v;
    v.reserve(n);
    uint64_t x = 0x9E3779B97F4A7C15ull ^ n;
    while (v.size() < n) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        v.push_back(static_cast<uint8_t>(x));
    }
    return v;
}

struct Fixture {
    std::shared_ptr<Database> db;
    std::shared_ptr<TenantManager> tm;
    std::unique_ptr<FileSystem> fs;
    std::string tenant;
    std::string root;
    int write_format = 1;
};

static TenantConfig config_for(const std::string& root, int write_format, bool encrypt, bool compress) {
    TenantConfig cfg;
    cfg.db_host = env_or("FILEENGINE_PG_HOST", env_or("FE_TEST_PG_HOST", "localhost"));
    cfg.db_port = std::stoi(env_or("FILEENGINE_PG_PORT", env_or("FE_TEST_PG_PORT", "5434")));
    cfg.db_name = env_or("FILEENGINE_PG_DATABASE", env_or("FE_TEST_PG_DB", "fileengine"));
    cfg.db_user = env_or("FILEENGINE_PG_USER", env_or("FE_TEST_PG_USER", "postgres"));
    cfg.db_password = env_or("FILEENGINE_PG_PASSWORD", env_or("FE_TEST_PG_PASSWORD", "postgres"));
    cfg.storage_base_path = root;
    cfg.s3_endpoint = ""; cfg.s3_region = ""; cfg.s3_bucket = "";
    cfg.s3_access_key = ""; cfg.s3_secret_key = ""; cfg.s3_path_style = true;
    cfg.encrypt_data = encrypt;
    cfg.compress_data = compress;
    cfg.storage_write_format = write_format;
    cfg.storage_selective_compression = true;
    cfg.encryption_key =
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    return cfg;
}

// `tenant`/`root` empty = fresh ones; pass an existing fixture's to reopen the
// SAME tenant and storage under a different configuration (the format switch).
static bool make_fixture(Fixture& f, const std::string& suffix, int write_format,
                         bool encrypt, bool compress,
                         const std::string& tenant = "", const std::string& root = "") {
    f.root = root.empty() ? "/tmp/fe_copyrestore_" + std::to_string(::getpid()) + "_" + suffix : root;
    f.tenant = tenant.empty() ? "cpr_" + std::to_string(::getpid()) + "_" + suffix : tenant;
    f.write_format = write_format;
    TenantConfig cfg = config_for(f.root, write_format, encrypt, compress);
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

static void close_fixture(Fixture& f) {
    f.fs.reset();
    f.tm.reset();
    f.db.reset();
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

static const char* kUser = "copyrestore@rationalboxes.com";
static const std::vector<std::string> kRoles = {"system_admin"};

// Version timestamps have millisecond resolution; keep consecutive writes apart.
static void tick() { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }

static std::string make_file(Fixture& f, const std::string& parent, const std::string& name) {
    auto r = f.fs->touch(parent, name, kUser, kRoles, f.tenant);
    return r.success ? r.value : std::string();
}

static bool put(Fixture& f, const std::string& uid, const std::vector<uint8_t>& data) {
    tick();
    return f.fs->put(uid, data, kUser, kRoles, f.tenant).success;
}

// What a door serves: the streamed current content.
static bool read_current(Fixture& f, const std::string& uid, std::vector<uint8_t>& out) {
    out.clear();
    auto r = f.fs->get_stream(uid, [&](const uint8_t* p, size_t n) {
        out.insert(out.end(), p, p + n);
        return true;
    }, kUser, kRoles, f.tenant);
    return r.success;
}

static std::vector<std::string> versions_newest_first(Fixture& f, const std::string& uid) {
    auto r = f.fs->list_versions(uid, kUser, kRoles, f.tenant);
    return r.success ? r.value : std::vector<std::string>{};
}

static std::string child_named(Fixture& f, const std::string& dir, const std::string& name) {
    auto r = f.db->list_files_in_directory(dir, f.tenant);
    if (!r.success) return {};
    for (const auto& e : r.value) if (e.name == name) return e.uid;
    return {};
}

// Every recorded fact about a version agrees with its content and the
// deployment: the size column is the CONTENT size, a transform row exists, and
// the format recorded is the one the bytes on disk are in.
static void check_version_record(Fixture& f, const std::string& uid, const std::string& ver,
                                 size_t content_size, int expect_format, const std::string& what) {
    auto sz = f.db->get_version_size(uid, ver, f.tenant);
    check(sz.success && sz.value.has_value() &&
              *sz.value == static_cast<int64_t>(content_size),
          what + ": versions.size is the content size (" + std::to_string(content_size) + ", got " +
              (sz.success && sz.value.has_value() ? std::to_string(*sz.value) : std::string("none")) + ")");
    auto vt = f.db->get_version_transform(uid, ver, f.tenant);
    check(vt.success && vt.value.has_value(), what + ": a transform row was recorded");
    if (vt.success && vt.value.has_value()) {
        check(vt.value->storage_format == expect_format,
              what + ": recorded storage_format " + std::to_string(expect_format) + " (got " +
                  std::to_string(vt.value->storage_format) + ")");
    }
    auto p = f.db->get_version_storage_path(uid, ver, f.tenant);
    if (p.success && p.value.has_value()) {
        std::ifstream in(*p.value, std::ios::binary);
        std::vector<uint8_t> head(64, 0);
        in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<size_t>(in.gcount()));
        const bool on_disk_v2 = !head.empty() && v2::is_v2(head.data(), head.size());
        check(on_disk_v2 == (expect_format == 2), what + ": the bytes on disk are in the recorded format");
    } else {
        check(false, what + ": the version has a storage path");
    }
}

// ---------------------------------------------------------------------------

static void test_copy_keeps_every_version_readable(Fixture& f) {
    const std::string dir = f.fs->mkdir("", "dst", kUser, kRoles, 0755, f.tenant).value;
    const std::string src = make_file(f, "", "versioned.txt");
    check(!dir.empty() && !src.empty(), "copy: nodes created");
    if (dir.empty() || src.empty()) return;
    const std::vector<std::string> contents = {"v1-one", "v2-two", "v3-three"};
    for (const auto& c : contents) check(put(f, src, bytes(c)), "copy: put " + c);

    check(f.fs->copy(src, dir, kUser, kRoles, f.tenant).success, "copy: copy() succeeded");
    const std::string cp = child_named(f, dir, "versioned.txt");
    check(!cp.empty(), "copy: the copy exists in the destination");
    if (cp.empty()) return;

    std::vector<uint8_t> cur;
    check(read_current(f, cp, cur), "copy: the copy's current content reads back");
    check(cur == bytes("v3-three"), "copy: the copy's current content is the latest version");

    auto st = f.fs->stat(cp, kUser, kRoles, f.tenant);
    check(st.success && st.value.size == 8,
          "copy: stat size is the content size, so Content-Length is honest (got " +
              (st.success ? std::to_string(st.value.size) : std::string("error")) + ")");

    const auto src_vers = versions_newest_first(f, src);
    const auto cp_vers = versions_newest_first(f, cp);
    check(cp_vers == src_vers, "copy: the copy keeps every version, with the same timestamps");
    for (size_t i = 0; i < cp_vers.size() && i < contents.size(); ++i) {
        const std::string& want = contents[contents.size() - 1 - i];   // newest-first
        auto got = f.fs->get_version(cp, cp_vers[i], kUser, kRoles, f.tenant);
        check(got.success && got.value == bytes(want), "copy: version " + cp_vers[i] + " reads back as " + want);
        check_version_record(f, cp, cp_vers[i], want.size(), f.write_format, "copy " + cp_vers[i]);
    }
    // The source is untouched.
    std::vector<uint8_t> src_cur;
    check(read_current(f, src, src_cur) && src_cur == bytes("v3-three"), "copy: the source still reads back");
}

static void test_copy_of_a_multi_block_file(Fixture& f) {
    const std::string dir = f.fs->mkdir("", "dst-big", kUser, kRoles, 0755, f.tenant).value;
    const std::string src = make_file(f, "", "model.ifc");
    const auto data = big_payload(3 * 1024 * 1024 + 4321);
    check(put(f, src, data), "copy-big: put a multi-block payload");
    check(f.fs->copy(src, dir, kUser, kRoles, f.tenant).success, "copy-big: copy() succeeded");
    const std::string cp = child_named(f, dir, "model.ifc");
    std::vector<uint8_t> cur;
    check(!cp.empty() && read_current(f, cp, cur) && cur == data,
          "copy-big: the copy round-trips byte for byte");
    auto st = f.fs->stat(cp, kUser, kRoles, f.tenant);
    check(st.success && st.value.size == static_cast<int64_t>(data.size()), "copy-big: stat size is the content size");
}

static void test_copy_carries_the_current_rendition(Fixture& f) {
    const std::string dir = f.fs->mkdir("", "dst-rend", kUser, kRoles, 0755, f.tenant).value;
    const std::string src = make_file(f, "", "drawing.pdf");
    check(put(f, src, bytes("the drawing")), "rendition: put the file");
    const std::string rend = make_file(f, src, "thumbnail.png");
    check(!rend.empty(), "rendition: a rendition node under the file");
    if (rend.empty()) return;
    check(put(f, rend, bytes("old-thumb")), "rendition: first rendition version");
    check(put(f, rend, bytes("new-thumbnail")), "rendition: newer rendition version");

    check(f.fs->copy(src, dir, kUser, kRoles, f.tenant).success, "rendition: copy() succeeded");
    const std::string cp = child_named(f, dir, "drawing.pdf");
    const std::string cp_rend = cp.empty() ? std::string() : child_named(f, cp, "thumbnail.png");
    check(!cp_rend.empty(), "rendition: the copy has the rendition");
    if (cp_rend.empty()) return;
    std::vector<uint8_t> got;
    check(read_current(f, cp_rend, got), "rendition: the copied rendition reads back");
    check(got == bytes("new-thumbnail"), "rendition: the copy carries the CURRENT rendition, not the oldest");
    auto st = f.fs->stat(cp_rend, kUser, kRoles, f.tenant);
    check(st.success && st.value.size == 13, "rendition: its size is the content size");
}

static void test_restore_makes_a_readable_new_version(Fixture& f) {
    const std::string uid = make_file(f, "", "restore-me.txt");
    check(put(f, uid, bytes("first-version")), "restore: put first");
    check(put(f, uid, bytes("second-version")), "restore: put second");
    const auto before = versions_newest_first(f, uid);
    check(before.size() == 2, "restore: two versions before restoring");
    if (before.size() != 2) return;
    const std::string oldest = before.back();

    tick();
    auto r = f.fs->restore_to_version(uid, oldest, kUser, kRoles, f.tenant);
    check(r.success && r.value, "restore: restore_to_version succeeded" + (r.success ? std::string() : ": " + r.error));

    const auto after = versions_newest_first(f, uid);
    check(after.size() == 3, "restore: restoring adds a version (history is kept)");
    std::vector<uint8_t> cur;
    check(read_current(f, uid, cur), "restore: the restored file's current content reads back");
    check(cur == bytes("first-version"), "restore: the current content is the restored version's");
    auto st = f.fs->stat(uid, kUser, kRoles, f.tenant);
    check(st.success && st.value.size == 13, "restore: stat size is the restored content's size");
    if (!after.empty()) {
        check(after.front() != oldest, "restore: the restored content is a NEW version");
        check_version_record(f, uid, after.front(), 13, f.write_format, "restored version");
    }
    // Every earlier version is still intact.
    for (const auto& v : before) {
        auto got = f.fs->get_version(uid, v, kUser, kRoles, f.tenant);
        check(got.success, "restore: earlier version " + v + " still reads back");
    }
}

static void test_restore_of_a_missing_version_refuses(Fixture& f) {
    const std::string uid = make_file(f, "", "restore-missing.txt");
    check(put(f, uid, bytes("only")), "restore-missing: put");
    auto r = f.fs->restore_to_version(uid, "19700101_000000.000", kUser, kRoles, f.tenant);
    check(!r.success, "restore-missing: restoring a version that does not exist is refused");
    check(versions_newest_first(f, uid).size() == 1, "restore-missing: a refused restore adds no version");
}

// Production's actual history: versions written as v1, the deployment then
// switched to v2. Copy and restore must READ by the recorded (v1) format and
// WRITE in the deployment's current (v2) one.
static void test_v1_history_copied_and_restored_after_switching_to_v2(bool encrypt, bool compress,
                                                                      const std::string& name) {
    Fixture f1;
    if (!make_fixture(f1, "mix_" + name, 1, encrypt, compress)) {
        check(false, "mixed/" + name + ": v1 fixture");
        return;
    }
    const std::string uid = make_file(f1, "", "legacy.txt");
    check(put(f1, uid, bytes("legacy-one")), "mixed/" + name + ": v1 put one");
    check(put(f1, uid, bytes("legacy-two!")), "mixed/" + name + ": v1 put two");
    const std::string tenant = f1.tenant, root = f1.root;
    close_fixture(f1);

    Fixture f2;
    if (!make_fixture(f2, "mix_" + name, 2, encrypt, compress, tenant, root)) {
        check(false, "mixed/" + name + ": reopened under v2");
        return;
    }
    const auto vers = versions_newest_first(f2, uid);
    check(vers.size() == 2, "mixed/" + name + ": the v1 history is there under v2");

    const std::string dir = f2.fs->mkdir("", "dst", kUser, kRoles, 0755, f2.tenant).value;
    check(f2.fs->copy(uid, dir, kUser, kRoles, f2.tenant).success, "mixed/" + name + ": copy under v2");
    const std::string cp = child_named(f2, dir, "legacy.txt");
    std::vector<uint8_t> cur;
    check(!cp.empty() && read_current(f2, cp, cur) && cur == bytes("legacy-two!"),
          "mixed/" + name + ": the copy of a v1 file reads back under v2");
    if (!cp.empty() && !vers.empty()) {
        check_version_record(f2, cp, vers.front(), 11, 2, "mixed/" + name + " copy (re-encoded as v2)");
    }

    tick();
    if (vers.size() == 2) {
        auto r = f2.fs->restore_to_version(uid, vers.back(), kUser, kRoles, f2.tenant);
        check(r.success, "mixed/" + name + ": restore a v1 version under v2");
        check(read_current(f2, uid, cur) && cur == bytes("legacy-one"),
              "mixed/" + name + ": the restored v1 content reads back");
        const auto after = versions_newest_first(f2, uid);
        if (!after.empty()) check_version_record(f2, uid, after.front(), 10, 2, "mixed/" + name + " restored");
        // The v1 versions themselves still read by their recorded format.
        auto old = f2.fs->get_version(uid, vers.front(), kUser, kRoles, f2.tenant);
        check(old.success && old.value == bytes("legacy-two!"), "mixed/" + name + ": a v1 version still reads under v2");
    }
    destroy_fixture(f2);
}

int main() {
    Fixture probe;
    if (!make_fixture(probe, "probe", 1, false, false)) {
        std::cout << "copy/restore: no Postgres reachable — skipping\n";
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

    for (int fmt : {1, 2}) {
        for (const auto& c : cases) {
            Fixture f;
            const std::string suffix = "v" + std::to_string(fmt) + "_" + c.name;
            if (!make_fixture(f, suffix, fmt, c.encrypt, c.compress)) {
                check(false, "fixture for " + suffix);
                continue;
            }
            std::cout << "-- write_format=" << fmt << ", " << c.name << "\n";
            test_copy_keeps_every_version_readable(f);
            test_copy_of_a_multi_block_file(f);
            test_copy_carries_the_current_rendition(f);
            test_restore_makes_a_readable_new_version(f);
            test_restore_of_a_missing_version_refuses(f);
            destroy_fixture(f);
        }
    }
    for (const auto& c : cases) {
        std::cout << "-- v1 history, then write_format=2, " << c.name << "\n";
        test_v1_history_copied_and_restored_after_switching_to_v2(c.encrypt, c.compress, c.name);
    }

    std::cout << "copy/restore: " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
