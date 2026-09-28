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

// Unit tests for the compression decision — storage_pipeline.md §3 / §10 (S2).
//
// Two things are being verified, and the second is the one that will actually
// regress: that already-compressed payloads are recognised, AND that highly
// compressible content is still compressed. SR-9 names that second set
// explicitly because getting it wrong is quiet and expensive — this platform
// stores enormous ASCII (IFC, CityJSON, OBJ, SQL) that zlib earns 70-90% on.

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "fileengine/compression_policy.h"

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

static std::vector<uint8_t> bytes(std::initializer_list<int> in) {
    std::vector<uint8_t> v;
    for (int b : in) v.push_back(static_cast<uint8_t>(b));
    return v;
}

// Pad a signature out so the payload is long enough to be realistic; the tail
// is highly compressible zeros, which is exactly the trap — if the signature
// is NOT honoured the trial would say "compress" and the test would catch it.
static std::vector<uint8_t> with_tail(std::vector<uint8_t> head, size_t total = 4096) {
    head.resize(total, 0);
    return head;
}

// ---------------------------------------------------------------------------
// SR-6a: the signature fast path
// ---------------------------------------------------------------------------
static void test_known_compressed_signatures() {
    std::cout << "signatures: formats compressed by construction are skipped..." << std::endl;

    struct Case { const char* name; std::vector<uint8_t> head; };
    const std::vector<Case> cases = {
        {"zip/docx/xlsx/odf", bytes({0x50,0x4B,0x03,0x04, 0x14,0x00,0x00,0x00})},
        {"zip empty",         bytes({0x50,0x4B,0x05,0x06})},
        {"zip spanned",       bytes({0x50,0x4B,0x07,0x08})},
        {"gzip",              bytes({0x1F,0x8B,0x08,0x00})},
        {"bzip2",             bytes({0x42,0x5A,0x68,0x39})},
        {"xz",                bytes({0xFD,0x37,0x7A,0x58,0x5A,0x00})},
        {"zstd",              bytes({0x28,0xB5,0x2F,0xFD})},
        {"7z",                bytes({0x37,0x7A,0xBC,0xAF,0x27,0x1C})},
        {"rar",               bytes({0x52,0x61,0x72,0x21,0x1A,0x07,0x00})},
        {"jpeg",              bytes({0xFF,0xD8,0xFF,0xE0})},
        {"png",               bytes({0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A})},
        {"gif87a",            bytes({'G','I','F','8','7','a'})},
        {"gif89a",            bytes({'G','I','F','8','9','a'})},
        {"matroska/webm",     bytes({0x1A,0x45,0xDF,0xA3})},
        {"ogg",               bytes({'O','g','g','S'})},
        {"mp3 id3",           bytes({'I','D','3',0x04})},
        {"flac",              bytes({'f','L','a','C'})},
    };

    for (const auto& c : cases) {
        auto payload = with_tail(c.head);
        CHECK(looks_already_compressed(payload), std::string("signature: ") + c.name);
        auto d = decide_compression(payload);
        CHECK(!d.compress, std::string("skip compression: ") + c.name);
        CHECK(d.reason == CompressionDecisionReason::KnownCompressed,
              std::string("reason is known-compressed, not measured: ") + c.name);
        // The fast path must SKIP the trial, not merely agree with it. If the
        // trial had run, sample_ratio would be set.
        CHECK(d.sample_ratio < 0.0,
              std::string("no trial deflate ran for: ") + c.name);
    }
}

static void test_offset_signatures() {
    std::cout << "signatures: patterns at a non-zero offset..." << std::endl;

    // ISO-BMFF: "ftyp" at offset 4 — mp4, mov, 3gp, heic, avif.
    for (const char* brand : {"isom", "mp42", "qt  ", "heic", "avif", "mif1"}) {
        std::vector<uint8_t> h = {0x00,0x00,0x00,0x20, 'f','t','y','p'};
        h.insert(h.end(), brand, brand + 4);
        auto d = decide_compression(with_tail(h));
        CHECK(!d.compress, std::string("iso-bmff brand ") + brand + " is skipped");
        CHECK(d.reason == CompressionDecisionReason::KnownCompressed, "iso-bmff via signature");
    }

    // WebP needs BOTH "RIFF" at 0 and "WEBP" at 8.
    std::vector<uint8_t> webp = {'R','I','F','F', 0x10,0x00,0x00,0x00, 'W','E','B','P'};
    CHECK(looks_already_compressed(with_tail(webp)), "RIFF....WEBP is webp");

    // ...and a bare RIFF must NOT match: WAV and AVI are RIFF and are typically
    // uncompressed. This is the case a careless "RIFF" entry would break.
    std::vector<uint8_t> wav = {'R','I','F','F', 0x10,0x00,0x00,0x00, 'W','A','V','E'};
    CHECK(!looks_already_compressed(with_tail(wav)), "RIFF....WAVE is NOT skipped");
    auto dwav = decide_compression(with_tail(wav));
    CHECK(dwav.reason != CompressionDecisionReason::KnownCompressed,
          "a WAV reaches the measurement");
}

// SR-6b: the deliberate omissions. These are the entries a well-meaning
// extension of the table would add, and each would lose real storage.
static void test_deliberate_exclusions() {
    std::cout << "signatures: PDF, TIFF and legacy Office reach the measurement..." << std::endl;

    struct Case { const char* name; std::vector<uint8_t> head; };
    const std::vector<Case> cases = {
        {"pdf",         bytes({'%','P','D','F','-','1','.','7'})},
        {"tiff-le",     bytes({0x49,0x49,0x2A,0x00})},
        {"tiff-be",     bytes({0x4D,0x4D,0x00,0x2A})},
        {"ole2 .doc",   bytes({0xD0,0xCF,0x11,0xE0,0xA1,0xB1,0x1A,0xE1})},
    };
    for (const auto& c : cases) {
        auto payload = with_tail(c.head);
        CHECK(!looks_already_compressed(payload),
              std::string("not on the skip list: ") + c.name);
        auto d = decide_compression(payload);
        CHECK(d.reason != CompressionDecisionReason::KnownCompressed,
              std::string("measured, not skipped on a signature: ") + c.name);
        CHECK(d.sample_ratio >= 0.0,
              std::string("the trial actually ran for: ") + c.name);
    }
}

static void test_coincidental_prefix_is_safe() {
    std::cout << "signatures: a coincidental prefix only ever skips compression..." << std::endl;
    // Text that happens to start with "ID3" — the fast path may cost a missed
    // compression, and must never change how bytes are stored or read.
    std::string text = "ID3 is also how this sentence starts, and it repeats. ";
    std::vector<uint8_t> payload;
    while (payload.size() < 8192) payload.insert(payload.end(), text.begin(), text.end());
    auto d = decide_compression(payload);
    CHECK(!d.compress, "a coincidental match skips compression (accepted cost)");
    CHECK(d.reason == CompressionDecisionReason::KnownCompressed, "via the signature");
    // The point of the assertion: the decision is only ever a compress/skip
    // boolean. Nothing else about the payload changes.
}

// ---------------------------------------------------------------------------
// SR-9: highly compressible content MUST still be compressed
// ---------------------------------------------------------------------------
static void test_compressible_content_is_compressed() {
    std::cout << "SR-9: IFC / JSON / XML / SQL / text are still compressed..." << std::endl;

    // A realistic IFC (STEP) prologue plus repeated entity lines. Real IFC is
    // enormous ASCII and zlib earns 70-90% on it; this is the regression that
    // would be quiet and expensive.
    std::string ifc =
        "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION((''),'2;1');\n"
        "FILE_NAME('model.ifc','2026-09-26T00:00:00',(''),(''),'','','');\n"
        "FILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n";
    for (int i = 0; i < 2000; ++i) {
        ifc += "#" + std::to_string(i) +
               "=IFCCARTESIANPOINT((0.,0.,0.));\n"
               "#" + std::to_string(i + 100000) +
               "=IFCWALLSTANDARDCASE('2O2Fr$t4X7Zf8NOew3FNr2',#41,'Wall',$,$,#1,#2,$);\n";
    }
    std::vector<uint8_t> ifc_bytes(ifc.begin(), ifc.end());
    auto d_ifc = decide_compression(ifc_bytes);
    CHECK(d_ifc.compress, "IFC is compressed");
    CHECK(d_ifc.reason == CompressionDecisionReason::MeasuredGood, "IFC measured good");
    // Band check, not just the boolean: if this drops far, something changed in
    // the sampling and the decision is no longer meaningful.
    CHECK(d_ifc.sample_ratio < 0.35,
          "IFC compresses to well under 35% of the sample");

    struct Case { const char* name; std::string body; };
    std::string json = "{\"objects\":[";
    for (int i = 0; i < 1500; ++i) {
        json += "{\"id\":" + std::to_string(i) +
                ",\"type\":\"Building\",\"attributes\":{\"height\":12.5,\"floors\":3}},";
    }
    json += "]}";
    std::string xml;
    for (int i = 0; i < 1500; ++i) {
        xml += "<element id=\"" + std::to_string(i) + "\"><name>value</name></element>\n";
    }
    std::string sql;
    for (int i = 0; i < 1500; ++i) {
        sql += "INSERT INTO files (uid, name, size) VALUES ('" + std::to_string(i) +
               "', 'document.txt', 1024);\n";
    }
    std::string obj;
    for (int i = 0; i < 2000; ++i) {
        obj += "v 1.000000 2.000000 3.000000\nvn 0.000000 1.000000 0.000000\n";
    }
    const std::vector<Case> cases = {
        {"json", json}, {"xml", xml}, {"sql", sql}, {"obj", obj},
    };
    for (const auto& c : cases) {
        std::vector<uint8_t> b(c.body.begin(), c.body.end());
        auto d = decide_compression(b);
        CHECK(d.compress, std::string("compressed: ") + c.name);
        CHECK(d.reason == CompressionDecisionReason::MeasuredGood,
              std::string("measured good: ") + c.name);
        CHECK(d.sample_ratio < 0.5,
              std::string("meaningful ratio: ") + c.name);
    }
}

// ---------------------------------------------------------------------------
// SR-6: the trial deflate
// ---------------------------------------------------------------------------
static void test_incompressible_unknown_format() {
    std::cout << "trial: high-entropy bytes with no signature are measured and skipped..." << std::endl;
    // A deterministic pseudo-random stream: no signature, no redundancy.
    std::vector<uint8_t> noise(64 * 1024);
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (auto& b : noise) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        b = static_cast<uint8_t>(x);
    }
    auto d = decide_compression(noise);
    CHECK(!d.compress, "incompressible noise is not compressed");
    CHECK(d.reason == CompressionDecisionReason::MeasuredPoor, "measured poor");
    CHECK(d.sample_ratio > 0.9, "noise does not shrink");
}

static void test_threshold_boundary() {
    std::cout << "trial: the min-gain threshold is honoured in both directions..." << std::endl;
    std::vector<uint8_t> text(32 * 1024, 'a');   // compresses to almost nothing
    auto measured = decide_compression(text);
    CHECK(measured.sample_ratio > 0.0 && measured.sample_ratio < 0.01,
          "a run of one byte value compresses to well under 1%");

    // Straddle the observed ratio from both sides, so the threshold is shown to
    // be the thing deciding rather than the content.
    const double saved = 1.0 - measured.sample_ratio;
    CompressionPolicyConfig just_under;
    just_under.min_gain = saved - 0.0001;
    CHECK(decide_compression(text, just_under).compress,
          "a demand just below the achieved saving is met");

    CompressionPolicyConfig just_over;
    just_over.min_gain = saved + 0.0001;
    auto d_over = decide_compression(text, just_over);
    CHECK(!d_over.compress, "a demand just above the achieved saving is refused");
    CHECK(d_over.reason == CompressionDecisionReason::MeasuredPoor, "…as measured-poor");

    CompressionPolicyConfig impossible;
    impossible.min_gain = 2.0;                   // unreachable by construction
    auto d_imp = decide_compression(text, impossible);
    CHECK(!d_imp.compress, "an unreachable threshold refuses");

    // And the default threshold sits between the two extremes for real content:
    // noise refuses, text accepts. Covered in the trial tests; asserted here as
    // the boundary being a property of the config rather than of the payload.
    CHECK(decide_compression(text).compress, "the default threshold accepts this");
}

static void test_sample_window() {
    std::cout << "trial: only the sample window is measured (SR-7)..." << std::endl;
    // Compressible head, incompressible tail. With a small window the decision
    // follows the head — which is the documented limitation of sampling, and
    // the behaviour the streaming writer depends on (it sees only the head).
    std::vector<uint8_t> payload(4096, 'x');
    uint64_t x = 12345;
    for (int i = 0; i < 200000; ++i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        payload.push_back(static_cast<uint8_t>(x));
    }
    CompressionPolicyConfig small;
    small.sample_bytes = 4096;
    auto d = decide_compression(payload, small);
    CHECK(d.compress, "a compressible head within the window decides the write");

    CompressionPolicyConfig big;
    big.sample_bytes = 128 * 1024;
    auto d2 = decide_compression(payload, big);
    CHECK(!d2.compress, "a larger window sees the incompressible body");
}

static void test_edge_cases() {
    std::cout << "edges: empty, tiny, disabled..." << std::endl;

    auto d_empty = decide_compression(std::vector<uint8_t>{});
    CHECK(!d_empty.compress, "an empty payload is not compressed");
    CHECK(d_empty.reason == CompressionDecisionReason::NoData, "…reported as no-data");

    CHECK(!looks_already_compressed(nullptr, 0), "null head does not match");
    CHECK(!looks_already_compressed(std::vector<uint8_t>{0x50}), "a 1-byte prefix of PK does not match");
    CHECK(!looks_already_compressed(bytes({0x50, 0x4B})), "a 2-byte prefix of PK does not match");
    CHECK(looks_already_compressed(bytes({0x50,0x4B,0x03,0x04})), "the full 4-byte PK does match");

    // A payload shorter than the window must not read out of range: run the
    // whole table against every prefix length of a realistic header.
    std::vector<uint8_t> head = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D};
    for (size_t n = 0; n <= head.size(); ++n) {
        (void)looks_already_compressed(head.data(), n);   // must not crash
        (void)decide_compression(head.data(), n);
    }
    CHECK(true, "short buffers do not read out of range");

    CompressionPolicyConfig off;
    off.enabled = false;
    auto d_off = decide_compression(std::vector<uint8_t>(1024, 'a'), off);
    CHECK(!d_off.compress, "the deployment flag still wins");
    CHECK(d_off.reason == CompressionDecisionReason::Disabled, "…reported as disabled");

    auto d_tiny = decide_compression(bytes({'h','i'}));
    CHECK(d_tiny.reason == CompressionDecisionReason::MeasuredPoor ||
          d_tiny.reason == CompressionDecisionReason::MeasuredGood,
          "a 2-byte payload is measured, not special-cased");
}

static void test_reason_names_are_total() {
    std::cout << "reason_name() covers every enumerator..." << std::endl;
    const CompressionDecisionReason all[] = {
        CompressionDecisionReason::NoData,
        CompressionDecisionReason::Disabled,
        CompressionDecisionReason::KnownCompressed,
        CompressionDecisionReason::MeasuredPoor,
        CompressionDecisionReason::MeasuredGood,
        CompressionDecisionReason::TrialFailed,
    };
    for (auto r : all) {
        CompressionDecision d; d.reason = r;
        CHECK(std::strcmp(d.reason_name(), "unknown") != 0, "every reason has a name");
    }
}

static void test_determinism() {
    std::cout << "the decision is deterministic for the same bytes..." << std::endl;
    std::vector<uint8_t> payload;
    for (int i = 0; i < 20000; ++i) payload.push_back(static_cast<uint8_t>(i * 7));
    auto a = decide_compression(payload);
    for (int i = 0; i < 20; ++i) {
        auto b = decide_compression(payload);
        CHECK(a.compress == b.compress && a.reason == b.reason, "same input, same decision");
    }
}

int main() {
    test_known_compressed_signatures();
    test_offset_signatures();
    test_deliberate_exclusions();
    test_coincidental_prefix_is_safe();
    test_compressible_content_is_compressed();
    test_incompressible_unknown_format();
    test_threshold_boundary();
    test_sample_window();
    test_edge_cases();
    test_reason_names_are_total();
    test_determinism();
    std::cout << "compression_policy_tests: all passed (" << g_checks << " checks)" << std::endl;
    return 0;
}
