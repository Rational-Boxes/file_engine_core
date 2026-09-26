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

// End-to-end tests for the per-version transform record, against a real
// Postgres — storage_pipeline.md S0 (SR-1 / SR-2 / SR-3), acceptance §10.
//
// THE TEST THAT MATTERS is test_flag_flip_does_not_corrupt_reads. Before S0 the
// read path asked the deployment's CURRENT configuration what to undo, so
// turning compression off made every read hand the raw zlib stream back as the
// file's content — with no error. That is a silent corruption, and it is the
// direction an operator takes to improve performance. Everything else here
// supports it.
//
// Skips (77) rather than fails when no Postgres is reachable, like the other
// live suites.

#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "fileengine/database.h"
#include "fileengine/accountability.h"
#include "fileengine/crypto_utils.h"
#include "fileengine/compression_policy.h"

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

static std::string unique_tenant(const char* suffix) {
    return "sp_" + std::to_string(::getpid()) + "_" + suffix;
}

static const std::string KEY =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

// The exact write-side pipeline: compress (maybe) then encrypt (maybe).
static std::vector<uint8_t> apply_transform(const std::vector<uint8_t>& plain,
                                            bool compress, bool encrypt) {
    std::vector<uint8_t> out = plain;
    if (compress) out = CryptoUtils::compress_data(out);
    if (encrypt) out = CryptoUtils::encrypt_data(out, KEY);
    return out;
}

// The exact read-side pipeline, given what it BELIEVES was applied.
static bool undo_transform(const std::vector<uint8_t>& stored, bool decrypt,
                           bool decompress, std::vector<uint8_t>& out) {
    out = stored;
    try {
        if (decrypt) out = CryptoUtils::decrypt_data(out, KEY);
        if (decompress) out = CryptoUtils::decompress_data(out);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

static std::vector<uint8_t> compressible_payload(size_t n) {
    const std::string unit = "FileEngine storage pipeline regression payload. ";
    std::vector<uint8_t> v;
    while (v.size() < n) v.insert(v.end(), unit.begin(), unit.end());
    v.resize(n);
    return v;
}

// ---------------------------------------------------------------------------
// SR-1: the record round-trips through the database
// ---------------------------------------------------------------------------
static void test_record_roundtrip(Database& db, const std::string& tenant) {
    std::cout << "SR-1: the transform record round-trips..." << std::endl;

    const std::string uid = "11111111-1111-1111-1111-111111111111";
    const std::string ver = "2026-09-26T10:00:00.000000Z";
    auto ins = db.insert_version(uid, ver, 1234, "/tmp/sp/one", "tester", tenant);
    check(ins.success, "insert_version");

    // Before anything is recorded the answer is "unknown" — NOT false. Reading
    // an absent record as "not compressed" is exactly the defect.
    auto before = db.get_version_transform(uid, ver, tenant);
    check(before.success, "get_version_transform before");
    check(before.value.has_value(), "the row exists");
    if (before.value.has_value()) {
        check(!before.value->compressed.has_value(), "compressed is unknown, not false");
        check(!before.value->encrypted.has_value(), "encrypted is unknown, not false");
        check(before.value->storage_format == 1, "storage_format defaults to 1");
        check(before.value->key_id == 0, "key_id defaults to 0");
    }

    VersionTransform t;
    t.compressed = false;
    t.encrypted = true;
    t.storage_format = 1;
    t.key_id = 7;
    check(db.set_version_transform(uid, ver, t, tenant).success, "set_version_transform");

    auto after = db.get_version_transform(uid, ver, tenant);
    check(after.success && after.value.has_value(), "get_version_transform after");
    if (after.value.has_value()) {
        check(after.value->compressed.has_value() && *after.value->compressed == false,
              "compressed=false round-trips as false, not as unknown");
        check(after.value->encrypted.has_value() && *after.value->encrypted == true,
              "encrypted=true round-trips");
        check(after.value->key_id == 7, "key_id round-trips");
    }

    auto missing = db.get_version_transform(uid, "no-such-version", tenant);
    check(missing.success && !missing.value.has_value(), "an absent version reports absent");
}

// ---------------------------------------------------------------------------
// THE DEFECT. SR-2.
// ---------------------------------------------------------------------------
static void test_flag_flip_does_not_corrupt_reads(Database& db, const std::string& tenant) {
    std::cout << "SR-2: flipping the deployment flag does not corrupt reads..." << std::endl;

    const auto plain = compressible_payload(4096);

    struct Case { const char* name; bool compress; bool encrypt; };
    const Case written[] = {
        {"compressed+encrypted", true,  true},
        {"compressed only",      true,  false},
        {"encrypted only",       false, true},
        {"plain",                false, false},
    };

    int i = 0;
    for (const auto& w : written) {
        const std::string uid = "22222222-2222-2222-2222-22222222222" + std::to_string(i);
        const std::string ver = "2026-09-26T11:0" + std::to_string(i) + ":00.000000Z";
        ++i;

        const auto stored = apply_transform(plain, w.compress, w.encrypt);
        check(db.insert_version(uid, ver, static_cast<int64_t>(plain.size()),
                                "/tmp/sp/" + uid, "tester", tenant).success,
              std::string("insert ") + w.name);

        VersionTransform t;
        t.compressed = w.compress;
        t.encrypted = w.encrypt;
        check(db.set_version_transform(uid, ver, t, tenant).success,
              std::string("record ") + w.name);

        // Now read it back under EVERY possible current configuration — the
        // operator having since flipped either flag, or both.
        for (bool cfg_compress : {false, true}) {
            for (bool cfg_encrypt : {false, true}) {
                auto rec = db.get_version_transform(uid, ver, tenant);
                check(rec.success && rec.value.has_value(), "record readable");
                if (!rec.value.has_value()) continue;

                // SR-2: resolve from the RECORD, falling back to config only
                // when the record is absent.
                const bool do_decompress = rec.value->compressed.value_or(cfg_compress);
                const bool do_decrypt = rec.value->encrypted.value_or(cfg_encrypt);

                std::vector<uint8_t> out;
                const bool okread = undo_transform(stored, do_decrypt, do_decompress, out);
                check(okread && out == plain,
                      std::string("read '") + w.name + "' correctly with config " +
                          (cfg_compress ? "C" : "-") + (cfg_encrypt ? "E" : "-"));

                // And demonstrate the defect the record prevents: the OLD
                // behaviour, deriving from configuration, on the case where
                // they disagree.
                if (cfg_compress != w.compress || cfg_encrypt != w.encrypt) {
                    std::vector<uint8_t> bad;
                    const bool old_ok = undo_transform(stored, cfg_encrypt, cfg_compress, bad);
                    const bool old_correct = old_ok && bad == plain;
                    check(!old_correct,
                          std::string("config-derived read of '") + w.name +
                              "' under config " + (cfg_compress ? "C" : "-") +
                              (cfg_encrypt ? "E" : "-") +
                              " is wrong — which is the defect SR-2 fixes");

                    // The nastier half: when it is wrong WITHOUT erroring, the
                    // caller receives the intermediate representation as if it
                    // were the file. That is the silent corruption.
                    if (old_ok && bad != plain) {
                        check(true, "…and in this combination it failed SILENTLY");
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// SR-3: the backfill
// ---------------------------------------------------------------------------
static void test_backfill(Database& db, const std::string& tenant) {
    std::cout << "SR-3: the backfill fills only unknown rows, idempotently..." << std::endl;

    // Two rows with no record (as if written before S0), one with a record.
    const std::string a = "33333333-3333-3333-3333-333333333331";
    const std::string b = "33333333-3333-3333-3333-333333333332";
    const std::string c = "33333333-3333-3333-3333-333333333333";
    const std::string ver = "2026-09-26T12:00:00.000000Z";
    for (const auto& uid : {a, b, c}) {
        check(db.insert_version(uid, ver, 10, "/tmp/sp/" + uid, "tester", tenant).success,
              "insert " + uid);
    }
    VersionTransform known;
    known.compressed = false;          // deliberately the OPPOSITE of the flags
    known.encrypted = false;
    check(db.set_version_transform(c, ver, known, tenant).success, "record c");

    auto n1 = db.backfill_version_transforms(true, true, tenant);
    check(n1.success, "backfill runs");
    check(n1.value >= 2, "…and wrote at least the two unknown rows");

    auto ra = db.get_version_transform(a, ver, tenant);
    check(ra.success && ra.value.has_value() &&
          ra.value->compressed.value_or(false) && ra.value->encrypted.value_or(false),
          "an unknown row now carries the live flags");

    // The row that already had a record must be untouched: the backfill must
    // never rewrite history, or a later flag change would corrupt it.
    auto rc = db.get_version_transform(c, ver, tenant);
    check(rc.success && rc.value.has_value() &&
          rc.value->compressed.has_value() && *rc.value->compressed == false &&
          rc.value->encrypted.has_value() && *rc.value->encrypted == false,
          "a row that already had a record is left alone");

    // Idempotent: a second run, with different flags, writes nothing.
    auto n2 = db.backfill_version_transforms(false, false, tenant);
    check(n2.success && n2.value == 0, "a second backfill writes nothing");
    auto ra2 = db.get_version_transform(a, ver, tenant);
    check(ra2.success && ra2.value.has_value() &&
          ra2.value->compressed.value_or(false) && ra2.value->encrypted.value_or(false),
          "…and does not change what the first one wrote");
}

// ---------------------------------------------------------------------------
// S2 against real content, through the database record
// ---------------------------------------------------------------------------
static void test_decision_is_recorded_and_honoured(Database& db, const std::string& tenant) {
    std::cout << "S2: the compression decision is recorded and honoured..." << std::endl;

    struct Case { const char* name; std::vector<uint8_t> payload; bool expect_compress; };
    std::vector<uint8_t> png = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};
    png.resize(8192, 0x11);
    std::vector<Case> cases = {
        {"text",  compressible_payload(8192), true},
        {"png",   png,                        false},
    };

    int i = 0;
    for (auto& c : cases) {
        const std::string uid = "44444444-4444-4444-4444-44444444444" + std::to_string(i);
        const std::string ver = "2026-09-26T13:0" + std::to_string(i) + ":00.000000Z";
        ++i;

        CompressionPolicyConfig cfg;              // enabled, defaults
        const auto decision = decide_compression(c.payload, cfg);
        check(decision.compress == c.expect_compress,
              std::string("decision for ") + c.name);

        const auto stored = apply_transform(c.payload, decision.compress, true);
        check(db.insert_version(uid, ver, static_cast<int64_t>(c.payload.size()),
                                "/tmp/sp/" + uid, "tester", tenant).success, "insert");
        VersionTransform t;
        t.compressed = decision.compress;
        t.encrypted = true;
        check(db.set_version_transform(uid, ver, t, tenant).success, "record");

        auto rec = db.get_version_transform(uid, ver, tenant);
        check(rec.success && rec.value.has_value(), "read back");
        if (!rec.value.has_value()) continue;

        std::vector<uint8_t> out;
        const bool okread = undo_transform(stored, rec.value->encrypted.value_or(true),
                                           rec.value->compressed.value_or(true), out);
        check(okread && out == c.payload,
              std::string("round-trip through the record: ") + c.name);
    }

    // A mixed corpus — compressed and uncompressed versions OF THE SAME FILE —
    // must both read correctly. This is the steady state S2 creates, and the
    // state the configuration-derived read path could never survive.
    const std::string uid = "55555555-5555-5555-5555-555555555555";
    const auto text = compressible_payload(4096);
    for (int k = 0; k < 2; ++k) {
        const bool compress = (k == 0);
        const std::string ver = "2026-09-26T14:0" + std::to_string(k) + ":00.000000Z";
        const auto stored = apply_transform(text, compress, true);
        check(db.insert_version(uid, ver, static_cast<int64_t>(text.size()),
                                "/tmp/sp/mixed" + std::to_string(k), "tester", tenant).success,
              "insert mixed");
        VersionTransform t;
        t.compressed = compress;
        t.encrypted = true;
        check(db.set_version_transform(uid, ver, t, tenant).success, "record mixed");

        auto rec = db.get_version_transform(uid, ver, tenant);
        std::vector<uint8_t> out;
        check(rec.value.has_value() &&
                  undo_transform(stored, *rec.value->encrypted, *rec.value->compressed, out) &&
                  out == text,
              std::string("mixed-corpus version ") + std::to_string(k) + " reads correctly");
    }
}

// ---------------------------------------------------------------------------
// Tenant isolation of the record
// ---------------------------------------------------------------------------
static void test_record_is_tenant_scoped(Database& db, const std::string& t1,
                                         const std::string& t2) {
    std::cout << "the transform record is per tenant..." << std::endl;
    const std::string uid = "66666666-6666-6666-6666-666666666666";
    const std::string ver = "2026-09-26T15:00:00.000000Z";

    check(db.insert_version(uid, ver, 1, "/tmp/sp/t1", "tester", t1).success, "insert t1");
    check(db.insert_version(uid, ver, 1, "/tmp/sp/t2", "tester", t2).success, "insert t2");

    VersionTransform a; a.compressed = true;  a.encrypted = true;
    VersionTransform b; b.compressed = false; b.encrypted = false;
    check(db.set_version_transform(uid, ver, a, t1).success, "record t1");
    check(db.set_version_transform(uid, ver, b, t2).success, "record t2");

    auto r1 = db.get_version_transform(uid, ver, t1);
    auto r2 = db.get_version_transform(uid, ver, t2);
    check(r1.value.has_value() && *r1.value->compressed == true, "t1 keeps its own answer");
    check(r2.value.has_value() && *r2.value->compressed == false, "t2 keeps its own answer");

    // And a backfill in one tenant does not reach the other.
    const std::string only_t1 = "77777777-7777-7777-7777-777777777777";
    check(db.insert_version(only_t1, ver, 1, "/tmp/sp/x", "tester", t2).success, "insert in t2");
    auto n = db.backfill_version_transforms(true, true, t1);
    check(n.success, "backfill t1");
    auto still_unknown = db.get_version_transform(only_t1, ver, t2);
    check(still_unknown.value.has_value() && !still_unknown.value->compressed.has_value(),
          "a backfill in one tenant leaves the other's rows unknown");
}

int main() {
    std::cout << "=== storage_pipeline_live_tests ===\n";

    const std::string host = env_or("FILEENGINE_PG_HOST", env_or("FE_TEST_PG_HOST", "localhost"));
    const int port = std::stoi(env_or("FILEENGINE_PG_PORT", env_or("FE_TEST_PG_PORT", "5434")));
    const std::string name = env_or("FILEENGINE_PG_DATABASE", env_or("FE_TEST_PG_DB", "fileengine"));
    const std::string user = env_or("FILEENGINE_PG_USER", env_or("FE_TEST_PG_USER", "postgres"));
    const std::string pass = env_or("FILEENGINE_PG_PASSWORD", env_or("FE_TEST_PG_PASSWORD", "postgres"));

    Database db(host, port, name, user, pass, /*pool_size=*/8);
    if (!db.connect()) {
        std::cout << "SKIP: no Postgres at " << host << ":" << port << "\n";
        return 77;
    }
    if (!db.create_schema().success) {
        std::cout << "SKIP: could not create/verify the global schema.\n";
        return 77;
    }

    const std::string t1 = unique_tenant("a");
    const std::string t2 = unique_tenant("b");
    for (const auto& t : {t1, t2}) {
        if (!db.create_tenant_schema(t, AccountabilityContext::system()).success) {
            std::cout << "SKIP: could not create tenant schema " << t << "\n";
            return 77;
        }
    }

    test_record_roundtrip(db, t1);
    test_flag_flip_does_not_corrupt_reads(db, t1);
    test_backfill(db, t1);
    test_decision_is_recorded_and_honoured(db, t1);
    test_record_is_tenant_scoped(db, t1, t2);

    // Best-effort teardown: a cleanup failure must not turn a passing run red,
    // and the pid-suffixed tenant names mean leftovers never affect a later run.
    for (const auto& t : {t1, t2}) {
        db.cleanup_tenant_data(t, AccountabilityContext::system());
    }

    std::cout << "\n=== " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed ===\n";
    return g_failures == 0 ? 0 : 1;
}
