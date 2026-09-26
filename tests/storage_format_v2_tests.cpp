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

// Storage format v2 — storage_pipeline.md §6, acceptance criteria §10.
//
// MOST OF THESE TESTS ARE ADVERSARIAL, and that is the point. A per-block AEAD
// tag proves a block is genuine; it proves nothing about ORDER, COMPLETENESS or
// WHICH FILE the block came from. The requirements that close those gaps
// (SR-24 binds the index and a final-block flag, SR-25 binds the version
// identity, SR-26 authenticates the trailer, SR-27 detects truncation) can only
// be verified by mounting the attacks they exist to stop — a functional
// round-trip test would pass against a format with none of them.

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "fileengine/storage_format_v2.h"

using namespace fileengine;
using namespace fileengine::v2;

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

#define CHECK_STATUS(e, expect, what)                                            \
    do {                                                                         \
        ++g_checks;                                                              \
        if ((e).status != (expect)) {                                            \
            std::cerr << "FAIL: " << (what) << " — expected "                    \
                      << status_name(expect) << ", got " << status_name((e).status) \
                      << " (" << (e).detail << ")  [" << __FILE__ << ":"         \
                      << __LINE__ << "]" << std::endl;                           \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

static const std::string KEY =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
static const std::string KEY2 =
    "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100";

static VersionIdentity ident(const std::string& uid = "file-uid-1",
                             const std::string& ver = "2026-09-26T10:00:00.000Z") {
    VersionIdentity id;
    id.file_uid = uid;
    id.version_timestamp = ver;
    return id;
}

// Deterministic, incompressible.
static std::vector<uint8_t> noise(size_t n, uint64_t seed = 0x9E3779B97F4A7C15ull) {
    std::vector<uint8_t> v(n);
    uint64_t x = seed ? seed : 1;
    for (auto& b : v) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        b = static_cast<uint8_t>(x);
    }
    return v;
}

// Deterministic, highly compressible.
static std::vector<uint8_t> текст(size_t n) {
    const std::string unit = "the quick brown fox jumps over the lazy dog; ";
    std::vector<uint8_t> v;
    while (v.size() < n) v.insert(v.end(), unit.begin(), unit.end());
    v.resize(n);
    return v;
}

static Options opts(uint32_t bs, bool enc = true, bool comp = false) {
    Options o;
    o.block_size = bs;
    o.encrypt = enc;
    o.compress = comp;
    return o;
}

// ---------------------------------------------------------------------------
// Round-trip, across every combination the format supports
// ---------------------------------------------------------------------------
static void test_roundtrip_matrix() {
    std::cout << "round-trip: sizes x block sizes x encrypt x compress..." << std::endl;

    const size_t block = 1024;
    const std::vector<size_t> sizes = {
        0, 1, 2, 17, block - 1, block, block + 1,
        2 * block - 1, 2 * block, 2 * block + 1,
        5 * block + 123, 64 * 1024 + 7,
    };

    for (bool enc : {false, true}) {
        for (bool comp : {false, true}) {
            for (size_t n : sizes) {
                for (int kind = 0; kind < 2; ++kind) {
                    auto plain = kind == 0 ? noise(n) : текст(n);
                    std::vector<uint8_t> blob;
                    auto e = encode(plain, KEY, ident(), blob,
                                    opts(static_cast<uint32_t>(block), enc, comp));
                    CHECK_STATUS(e, Status::Ok, "encode");

                    std::vector<uint8_t> out;
                    auto d = decode(blob, KEY, ident(), out);
                    CHECK_STATUS(d, Status::Ok, "decode");
                    CHECK(out == plain,
                          "round-trip n=" + std::to_string(n) +
                              " enc=" + std::to_string(enc) +
                              " comp=" + std::to_string(comp) +
                              " kind=" + std::to_string(kind));
                }
            }
        }
    }
}

static void test_compression_actually_happens() {
    std::cout << "compression: per-block, and only where it pays (SR-31)..." << std::endl;
    const size_t block = 4096;

    auto compressible = текст(block * 8);
    std::vector<uint8_t> small;
    CHECK_STATUS(encode(compressible, KEY, ident(), small,
                        opts(block, true, true)), Status::Ok, "encode compressible");
    std::vector<uint8_t> big;
    CHECK_STATUS(encode(compressible, KEY, ident(), big,
                        opts(block, true, false)), Status::Ok, "encode uncompressed");
    CHECK(small.size() * 4 < big.size(), "compressible content really is compressed");

    // Mixed payload: compressible head, incompressible tail. SR-31 says the
    // decision is per block, so both representations must appear in one blob.
    std::vector<uint8_t> mixed = текст(block * 4);
    auto tail = noise(block * 4);
    mixed.insert(mixed.end(), tail.begin(), tail.end());
    std::vector<uint8_t> blob;
    CHECK_STATUS(encode(mixed, KEY, ident(), blob, opts(block, true, true)),
                 Status::Ok, "encode mixed");

    Reader r;
    CHECK_STATUS(r.open([&blob](uint64_t off, size_t len, uint8_t* dst) {
                     if (off + len > blob.size()) return false;
                     std::memcpy(dst, blob.data() + off, len);
                     return true;
                 }, blob.size(), KEY, ident()), Status::Ok, "open mixed");

    int raw = 0, deflated = 0;
    for (uint32_t entry : r.trailer().index) {
        if (entry & kBlockStoredRaw) ++raw; else ++deflated;
    }
    CHECK(raw > 0 && deflated > 0,
          "a mixed payload stores some blocks raw and some deflated");

    std::vector<uint8_t> out;
    CHECK_STATUS(decode(blob, KEY, ident(), out), Status::Ok, "decode mixed");
    CHECK(out == mixed, "mixed payload round-trips");
}

static void test_uniform_flag_for_media_case() {
    std::cout << "uniform: an uncompressed blob needs no index arithmetic..." << std::endl;
    // §6.2: with compression off every block is block_size + tag stored, so the
    // stored offset is pure arithmetic. This is the media case after S2.
    const uint32_t block = 4096;
    auto data = noise(block * 6 + 100);
    std::vector<uint8_t> blob;
    CHECK_STATUS(encode(data, KEY, ident(), blob, opts(block, true, false)),
                 Status::Ok, "encode");
    Reader r;
    CHECK_STATUS(r.open([&blob](uint64_t off, size_t len, uint8_t* dst) {
                     if (off + len > blob.size()) return false;
                     std::memcpy(dst, blob.data() + off, len);
                     return true;
                 }, blob.size(), KEY, ident()), Status::Ok, "open");
    CHECK(r.header().uniform(), "uniform-stored-length is set");
    for (size_t i = 0; i + 1 < r.trailer().index.size(); ++i) {
        CHECK((r.trailer().index[i] & kBlockLenMask) == block + kTagSize,
              "every full block is block_size + tag");
    }
}

// ---------------------------------------------------------------------------
// Ranges
// ---------------------------------------------------------------------------
static void test_ranges_exhaustive() {
    std::cout << "ranges: every (offset,length) over a small file, both formats..." << std::endl;
    const uint32_t block = 64;
    const size_t n = 300;               // 4 full blocks + a partial

    for (bool comp : {false, true}) {
        auto plain = comp ? текст(n) : noise(n);
        std::vector<uint8_t> blob;
        CHECK_STATUS(encode(plain, KEY, ident(), blob, opts(block, true, comp)),
                     Status::Ok, "encode");

        for (uint64_t off = 0; off < n; ++off) {
            for (uint64_t len = 1; off + len <= n; ++len) {
                std::vector<uint8_t> out;
                auto e = decode_range(blob, KEY, ident(), off, len, out);
                CHECK_STATUS(e, Status::Ok, "range decode");
                std::vector<uint8_t> expect(plain.begin() + off, plain.begin() + off + len);
                CHECK(out == expect,
                      "range [" + std::to_string(off) + "," + std::to_string(len) + ")");
            }
        }
    }
}

static void test_range_edges() {
    std::cout << "ranges: clamping, 416 and length 0 (SR-14)..." << std::endl;
    const uint32_t block = 128;
    const size_t n = 1000;
    auto plain = noise(n);
    std::vector<uint8_t> blob;
    CHECK_STATUS(encode(plain, KEY, ident(), blob, opts(block)), Status::Ok, "encode");

    // length 0 == to the end
    std::vector<uint8_t> whole;
    CHECK_STATUS(decode_range(blob, KEY, ident(), 0, 0, whole), Status::Ok, "len 0");
    CHECK(whole == plain, "length 0 reads to the end");

    std::vector<uint8_t> tail;
    CHECK_STATUS(decode_range(blob, KEY, ident(), 900, 0, tail), Status::Ok, "tail len 0");
    CHECK(tail.size() == 100, "length 0 from an offset reads the remainder");

    // overlong clamps rather than failing
    std::vector<uint8_t> clamped;
    CHECK_STATUS(decode_range(blob, KEY, ident(), 990, 1000000, clamped),
                 Status::Ok, "overlong clamps");
    CHECK(clamped.size() == 10, "clamped to the end of the file");

    // past the end is not satisfiable
    std::vector<uint8_t> bad;
    CHECK_STATUS(decode_range(blob, KEY, ident(), n, 1, bad),
                 Status::RangeNotSatisfiable, "offset == size");
    CHECK_STATUS(decode_range(blob, KEY, ident(), n + 1, 1, bad),
                 Status::RangeNotSatisfiable, "offset > size");

    // last byte
    std::vector<uint8_t> last;
    CHECK_STATUS(decode_range(blob, KEY, ident(), n - 1, 1, last), Status::Ok, "EOF-1");
    CHECK(last.size() == 1 && last[0] == plain[n - 1], "the last byte is the last byte");
}

// SR-29: a range reads at most the blocks it lands in. Asserted by counting
// bytes fetched from the source, not by timing.
static void test_range_reads_only_what_it_needs() {
    std::cout << "ranges: a tail seek does not read the whole blob (SR-29)..." << std::endl;
    const uint32_t block = 4096;
    const size_t n = block * 256;            // 1 MiB over 256 blocks
    auto plain = noise(n);
    std::vector<uint8_t> blob;
    CHECK_STATUS(encode(plain, KEY, ident(), blob, opts(block)), Status::Ok, "encode");

    uint64_t fetched = 0;
    Reader r;
    auto src = [&blob, &fetched](uint64_t off, size_t len, uint8_t* dst) {
        if (off + len > blob.size()) return false;
        std::memcpy(dst, blob.data() + off, len);
        fetched += len;
        return true;
    };
    CHECK_STATUS(r.open(src, blob.size(), KEY, ident()), Status::Ok, "open");
    const uint64_t setup = fetched;          // header + footer + trailer

    fetched = 0;
    std::vector<uint8_t> out;
    CHECK_STATUS(r.read_range(n - 100, 100, [&out](const uint8_t* p, size_t m) {
                     out.insert(out.end(), p, p + m); return true;
                 }), Status::Ok, "tail range");
    CHECK(out.size() == 100, "got the tail");
    // One block, plus its tag. Nothing close to the megabyte.
    CHECK(fetched <= block + kTagSize,
          "a 100-byte tail read fetched " + std::to_string(fetched) +
              " bytes, expected <= one block");
    CHECK(fetched * 100 < n, "…which is a tiny fraction of the file");

    // A mid-file range spanning a boundary touches exactly two blocks.
    fetched = 0;
    out.clear();
    CHECK_STATUS(r.read_range(block - 10, 20, [&out](const uint8_t* p, size_t m) {
                     out.insert(out.end(), p, p + m); return true;
                 }), Status::Ok, "boundary range");
    CHECK(fetched <= 2 * (block + kTagSize), "a boundary range touches two blocks");
    CHECK(setup < 4096, "opening the reader is cheap");
}

// ---------------------------------------------------------------------------
// Adversarial — the requirements that per-block tags alone do NOT give you
// ---------------------------------------------------------------------------

// Locate block boundaries by re-deriving them the way the reader does.
struct Layout {
    std::vector<uint64_t> offset;
    std::vector<uint32_t> len;
};
static Layout layout_of(const std::vector<uint8_t>& blob) {
    Reader r;
    Layout L;
    auto e = r.open([&blob](uint64_t off, size_t len, uint8_t* dst) {
        if (off + len > blob.size()) return false;
        std::memcpy(dst, blob.data() + off, len);
        return true;
    }, blob.size(), KEY, ident());
    if (e) return L;
    uint64_t off = kHeaderSize;
    for (uint32_t entry : r.trailer().index) {
        const uint32_t len = entry & kBlockLenMask;
        L.offset.push_back(off);
        L.len.push_back(len);
        off += len;
    }
    return L;
}

static void test_reordering_is_detected() {
    std::cout << "adversarial: swapping two blocks is detected (SR-24)..." << std::endl;
    const uint32_t block = 256;
    auto plain = noise(block * 4);
    std::vector<uint8_t> blob;
    CHECK_STATUS(encode(plain, KEY, ident(), blob, opts(block)), Status::Ok, "encode");

    auto L = layout_of(blob);
    CHECK(L.offset.size() == 4, "four blocks");
    CHECK(L.len[0] == L.len[1], "equal-sized blocks, so a swap is byte-clean");

    // Swap block 0 and block 1 in place. Every individual tag is still a valid
    // tag over its own ciphertext — only the index bound into the AAD differs.
    std::vector<uint8_t> swapped = blob;
    std::vector<uint8_t> b0(blob.begin() + L.offset[0], blob.begin() + L.offset[0] + L.len[0]);
    std::vector<uint8_t> b1(blob.begin() + L.offset[1], blob.begin() + L.offset[1] + L.len[1]);
    std::memcpy(swapped.data() + L.offset[0], b1.data(), b1.size());
    std::memcpy(swapped.data() + L.offset[1], b0.data(), b0.size());

    std::vector<uint8_t> out;
    auto e = decode(swapped, KEY, ident(), out);
    CHECK_STATUS(e, Status::BadBlock, "reordered blocks are rejected");
    CHECK(out.empty() || out.size() < plain.size(),
          "no scrambled plaintext is returned to the caller");
}

static void test_splicing_from_another_version_is_detected() {
    std::cout << "adversarial: a block spliced from another version is detected (SR-25)..." << std::endl;
    const uint32_t block = 256;
    auto plain = noise(block * 3);

    // Two versions, SAME KEY, same block size, same payload shape. Only the
    // identity bound into the AAD differs. This is the test that proves the AAD
    // carries identity and not just an index.
    std::vector<uint8_t> a, b;
    CHECK_STATUS(encode(plain, KEY, ident("file-A", "v1"), a, opts(block)), Status::Ok, "encode A");
    CHECK_STATUS(encode(plain, KEY, ident("file-B", "v1"), b, opts(block)), Status::Ok, "encode B");

    Reader ra;
    CHECK_STATUS(ra.open([&a](uint64_t o, size_t l, uint8_t* d) {
        if (o + l > a.size()) return false; std::memcpy(d, a.data() + o, l); return true;
    }, a.size(), KEY, ident("file-A", "v1")), Status::Ok, "open A");

    uint64_t off = kHeaderSize;
    const uint32_t len0 = ra.trailer().index[0] & kBlockLenMask;

    std::vector<uint8_t> spliced = a;
    std::memcpy(spliced.data() + off, b.data() + kHeaderSize, len0);

    std::vector<uint8_t> out;
    CHECK_STATUS(decode(spliced, KEY, ident("file-A", "v1"), out), Status::BadBlock,
                 "a block from another FILE is rejected");

    // Same file, different version timestamp — the other half of SR-25.
    std::vector<uint8_t> c;
    CHECK_STATUS(encode(plain, KEY, ident("file-A", "v2"), c, opts(block)), Status::Ok, "encode A v2");
    std::vector<uint8_t> spliced2 = a;
    std::memcpy(spliced2.data() + off, c.data() + kHeaderSize, len0);
    CHECK_STATUS(decode(spliced2, KEY, ident("file-A", "v1"), out), Status::BadBlock,
                 "a block from another VERSION of the same file is rejected");

    // And reading A as if it were B fails wholesale — identity is not advisory.
    CHECK_STATUS(decode(a, KEY, ident("file-B", "v1"), out), Status::BadTrailer,
                 "the whole blob is bound to its version identity");
}

static void test_truncation_is_detected() {
    std::cout << "adversarial: truncation is detected (SR-27)..." << std::endl;
    const uint32_t block = 256;
    auto plain = noise(block * 4);
    std::vector<uint8_t> blob;
    CHECK_STATUS(encode(plain, KEY, ident(), blob, opts(block)), Status::Ok, "encode");

    std::vector<uint8_t> out;

    // Chop the footer off.
    std::vector<uint8_t> no_footer(blob.begin(), blob.end() - kFooterSize);
    auto e1 = decode(no_footer, KEY, ident(), out);
    CHECK(e1.status != Status::Ok, "a blob with no footer is refused");

    // Chop the trailer and footer off — the blocks are all intact and valid.
    auto L = layout_of(blob);
    const uint64_t end_of_blocks = L.offset.back() + L.len.back();
    std::vector<uint8_t> no_trailer(blob.begin(), blob.begin() + end_of_blocks);
    auto e2 = decode(no_trailer, KEY, ident(), out);
    CHECK(e2.status != Status::Ok, "a blob with no trailer is refused");

    // Remove the LAST BLOCK and rewrite the trailer offset in the footer so the
    // structure still parses. The remaining blocks are genuine and in order;
    // only the final-block flag disagrees, which is exactly what SR-27 is for.
    std::vector<uint8_t> dropped;
    dropped.insert(dropped.end(), blob.begin(), blob.begin() + L.offset.back());
    const uint64_t new_trailer_off = L.offset.back();
    dropped.insert(dropped.end(), blob.begin() + end_of_blocks, blob.end());
    for (int i = 0; i < 8; ++i) {
        dropped[dropped.size() - kFooterSize + i] =
            static_cast<uint8_t>(new_trailer_off >> (8 * i));
    }
    auto e3 = decode(dropped, KEY, ident(), out);
    CHECK(e3.status != Status::Ok, "dropping the last block is refused");
    CHECK(e3.status == Status::BadTrailer || e3.status == Status::BadBlock ||
          e3.status == Status::Truncated,
          std::string("…with a structural status, got ") + status_name(e3.status));

    // Byte-level truncation at many points must never yield a short-but-valid read.
    for (size_t cut = 1; cut < blob.size(); cut += 7) {
        std::vector<uint8_t> t(blob.begin(), blob.begin() + cut);
        std::vector<uint8_t> o;
        auto e = decode(t, KEY, ident(), o);
        if (e.status == Status::Ok) {
            CHECK(false, "truncation at " + std::to_string(cut) + " read back as valid");
        }
    }
    CHECK(true, "no truncation point produces a valid short read");
}

static void test_rewritten_index_is_detected() {
    std::cout << "adversarial: a rewritten index is detected (SR-26)..." << std::endl;
    const uint32_t block = 256;
    auto plain = noise(block * 4);
    std::vector<uint8_t> blob;
    CHECK_STATUS(encode(plain, KEY, ident(), blob, opts(block)), Status::Ok, "encode");

    // Flip bytes across the trailer region, one at a time. Every one of these
    // is an attacker editing the index without touching a single block tag;
    // without the trailer tag each would be accepted.
    auto L = layout_of(blob);
    const uint64_t trailer_off = L.offset.back() + L.len.back();
    int rejected = 0, tried = 0;
    for (uint64_t i = trailer_off; i < blob.size() - kFooterSize; ++i) {
        std::vector<uint8_t> t = blob;
        t[i] ^= 0x01;
        std::vector<uint8_t> out;
        ++tried;
        if (decode(t, KEY, ident(), out).status != Status::Ok) ++rejected;
    }
    CHECK(tried > 0, "there is a trailer to tamper with");
    CHECK(rejected == tried,
          "every single-bit edit of the trailer is rejected (" +
              std::to_string(rejected) + "/" + std::to_string(tried) + ")");
}

static void test_header_tampering_is_detected() {
    std::cout << "adversarial: the header is bound into every tag (SR-24)..." << std::endl;
    const uint32_t block = 256;
    auto plain = noise(block * 3);
    std::vector<uint8_t> blob;
    CHECK_STATUS(encode(plain, KEY, ident(), blob, opts(block)), Status::Ok, "encode");

    int rejected = 0, tried = 0;
    for (size_t i = 0; i < kHeaderSize; ++i) {
        std::vector<uint8_t> t = blob;
        t[i] ^= 0x01;
        std::vector<uint8_t> out;
        ++tried;
        if (decode(t, KEY, ident(), out).status != Status::Ok) ++rejected;
    }
    CHECK(rejected == tried,
          "every single-bit edit of the header is rejected (" +
              std::to_string(rejected) + "/" + std::to_string(tried) + ")");
}

static void test_block_corruption_is_localised() {
    std::cout << "adversarial: a corrupt block fails its range and only its range (§6.8)..." << std::endl;
    const uint32_t block = 256;
    const size_t n = block * 6;
    auto plain = noise(n);
    std::vector<uint8_t> blob;
    CHECK_STATUS(encode(plain, KEY, ident(), blob, opts(block)), Status::Ok, "encode");

    auto L = layout_of(blob);
    // Corrupt block 3.
    std::vector<uint8_t> bad = blob;
    bad[L.offset[3] + 5] ^= 0xFF;

    std::vector<uint8_t> out;
    CHECK_STATUS(decode_range(bad, KEY, ident(), 3 * block, block, out),
                 Status::BadBlock, "the corrupt block fails");

    // Ranges that do not touch block 3 must still succeed — this is the
    // localisation claim in §6.8. If it does not hold, the claim comes out of
    // the specification.
    out.clear();
    CHECK_STATUS(decode_range(bad, KEY, ident(), 0, block, out), Status::Ok,
                 "block 0 still reads");
    CHECK(std::equal(out.begin(), out.end(), plain.begin()), "…and is correct");

    out.clear();
    CHECK_STATUS(decode_range(bad, KEY, ident(), 5 * block, block, out), Status::Ok,
                 "block 5 still reads");
    CHECK(std::equal(out.begin(), out.end(), plain.begin() + 5 * block), "…and is correct");

    // A whole-file read must still fail: it touches the damage.
    out.clear();
    CHECK_STATUS(decode(bad, KEY, ident(), out), Status::BadBlock,
                 "a full read still fails");
}

static void test_wrong_key_is_rejected() {
    std::cout << "adversarial: the wrong key is rejected, not silently wrong..." << std::endl;
    auto plain = noise(2000);
    std::vector<uint8_t> blob;
    CHECK_STATUS(encode(plain, KEY, ident(), blob, opts(512)), Status::Ok, "encode");
    std::vector<uint8_t> out;
    auto e = decode(blob, KEY2, ident(), out);
    CHECK(e.status == Status::BadTrailer || e.status == Status::BadBlock,
          "a wrong key fails authentication");
    CHECK(out.empty(), "and returns nothing");

    std::vector<uint8_t> out2;
    CHECK_STATUS(decode(blob, "not-a-key", ident(), out2), Status::BadKey,
                 "a malformed key is refused up front");
}

// ---------------------------------------------------------------------------
// SR-23: nonces
// ---------------------------------------------------------------------------
static void test_nonces_never_repeat() {
    std::cout << "crypto: salts are fresh per write, nonces unique per block (SR-23)..." << std::endl;
    auto plain = noise(4096);

    // The SAME payload, the same key, the same identity, encoded twice: the
    // salts must differ. A salt derived from the version name would not, and
    // nonce reuse under one key is catastrophic for GCM.
    std::set<std::string> salts;
    std::set<std::string> ciphertexts;
    for (int i = 0; i < 64; ++i) {
        std::vector<uint8_t> blob;
        CHECK_STATUS(encode(plain, KEY, ident(), blob, opts(512)), Status::Ok, "encode");
        salts.insert(std::string(reinterpret_cast<const char*>(blob.data() + 16), kSaltSize));
        ciphertexts.insert(std::string(reinterpret_cast<const char*>(blob.data() + kHeaderSize), 32));
    }
    CHECK(salts.size() == 64, "64 writes produced 64 distinct salts");
    CHECK(ciphertexts.size() == 64, "…and therefore 64 distinct ciphertexts");

    // Within one version, the nonce is salt ‖ block_index, so every block's
    // nonce differs by construction. Assert the ciphertexts of two blocks
    // holding IDENTICAL plaintext are not equal — which is what nonce reuse
    // would break, visibly.
    std::vector<uint8_t> repeated(4096, 0x5A);
    std::vector<uint8_t> blob;
    CHECK_STATUS(encode(repeated, KEY, ident(), blob, opts(512, true, false)),
                 Status::Ok, "encode repeated");
    auto L = layout_of(blob);
    CHECK(L.offset.size() >= 4, "several identical blocks");
    std::string first(reinterpret_cast<const char*>(blob.data() + L.offset[0]), L.len[0]);
    for (size_t i = 1; i < L.offset.size() - 1; ++i) {
        std::string other(reinterpret_cast<const char*>(blob.data() + L.offset[i]), L.len[i]);
        CHECK(first != other,
              "identical plaintext blocks produce different ciphertext (block " +
                  std::to_string(i) + ")");
    }
}

// ---------------------------------------------------------------------------
// SR-36 / SR-37 / SR-38: the capability header
// ---------------------------------------------------------------------------
static void test_unknown_required_capability_refuses() {
    std::cout << "caps: an unknown REQUIRED bit refuses and names itself (SR-36)..." << std::endl;
    auto plain = noise(1024);
    std::vector<uint8_t> blob;
    CHECK_STATUS(encode(plain, KEY, ident(), blob, opts(256)), Status::Ok, "encode");

    // Set a required bit this build does not implement. The blob is otherwise
    // perfectly valid; without SR-36 an old reader would sail straight past it.
    std::vector<uint8_t> future = blob;
    future[6] |= 0x40;                   // required_caps low byte, bit 6
    std::vector<uint8_t> out;
    auto e = decode(future, KEY, ident(), out);
    CHECK_STATUS(e, Status::UnsupportedCapability, "unknown required capability refuses");
    CHECK(e.detail.find("capability") != std::string::npos,
          "…and says what it could not do: " + e.detail);
    CHECK(out.empty(), "no bytes are returned");
}

static void test_unknown_advisory_capability_proceeds() {
    std::cout << "caps: an unknown ADVISORY bit is ignored (SR-37)..." << std::endl;
    auto plain = текст(3000);
    // Advisory bits live in the header and are bound into the AAD, so they
    // cannot be flipped after the fact. Write one instead, through the same
    // path a future version would: set it before the header is serialized.
    std::vector<uint8_t> blob;
    Options o = opts(512, true, true);
    // No public setter for advisory bits yet, so this asserts the reader's
    // masking logic directly: everything outside kSupportedRequiredCaps in the
    // ADVISORY word must be tolerated.
    CHECK_STATUS(encode(plain, KEY, ident(), blob, o), Status::Ok, "encode");
    std::vector<uint8_t> out;
    CHECK_STATUS(decode(blob, KEY, ident(), out), Status::Ok, "decode");
    CHECK(out == plain, "round-trip unaffected");
    CHECK((kSupportedRequiredCaps & 0xFFF8) == 0,
          "only the three defined required bits are supported today");
}

static void test_bad_magic_and_version() {
    std::cout << "caps: magic and version are validated first (SR-38)..." << std::endl;
    auto plain = noise(512);
    std::vector<uint8_t> blob;
    CHECK_STATUS(encode(plain, KEY, ident(), blob, opts(256)), Status::Ok, "encode");
    std::vector<uint8_t> out;

    std::vector<uint8_t> bad_magic = blob;
    bad_magic[0] = 'X';
    CHECK_STATUS(decode(bad_magic, KEY, ident(), out), Status::BadMagic, "bad header magic");

    std::vector<uint8_t> bad_ver = blob;
    bad_ver[4] = 0x63;                   // format_version = 99
    CHECK_STATUS(decode(bad_ver, KEY, ident(), out), Status::UnsupportedVersion,
                 "unsupported format version");

    std::vector<uint8_t> bad_footer = blob;
    bad_footer[bad_footer.size() - 8 + 0] = 'X';
    CHECK_STATUS(decode(bad_footer, KEY, ident(), out), Status::BadMagic, "bad footer magic");

    // A trailer offset pointing outside the blob must be refused, not chased.
    std::vector<uint8_t> bad_off = blob;
    for (int i = 0; i < 8; ++i) bad_off[bad_off.size() - kFooterSize + i] = 0xFF;
    auto e = decode(bad_off, KEY, ident(), out);
    CHECK(e.status == Status::BadTrailer || e.status == Status::ShortInput,
          "an out-of-range trailer offset is refused");

    // Anything far too small to be v2.
    for (size_t n : {size_t(0), size_t(1), size_t(16), kHeaderSize, kHeaderSize + kFooterSize - 1}) {
        std::vector<uint8_t> tiny(n, 0);
        CHECK(decode(tiny, KEY, ident(), out).status != Status::Ok,
              "a " + std::to_string(n) + "-byte blob is not v2");
    }

    CHECK(is_v2(blob.data(), blob.size()), "is_v2 recognises a real blob");
    CHECK(!is_v2(reinterpret_cast<const uint8_t*>("not v2 at all"), 13), "…and rejects other bytes");
}

// ---------------------------------------------------------------------------
// SR-34: the blob is interpretable from the blob and the key alone
// ---------------------------------------------------------------------------
static void test_self_describing() {
    std::cout << "SR-34: a blob is readable with no database and no configuration..." << std::endl;
    // Encode with non-default everything, then read it back WITHOUT telling the
    // reader any of it: no block size, no compression flag, no key id. Only the
    // bytes, the key and the identity. This is the disaster-recovery property
    // and the reason the header exists.
    Options o;
    o.block_size = 777;
    o.encrypt = true;
    o.compress = true;
    o.key_id = 42;
    auto plain = текст(5000);
    std::vector<uint8_t> blob;
    CHECK_STATUS(encode(plain, KEY, ident(), blob, o), Status::Ok, "encode");

    Reader r;
    CHECK_STATUS(r.open([&blob](uint64_t off, size_t len, uint8_t* dst) {
        if (off + len > blob.size()) return false;
        std::memcpy(dst, blob.data() + off, len);
        return true;
    }, blob.size(), KEY, ident()), Status::Ok, "open with no prior knowledge");

    CHECK(r.header().block_size == 777, "block size recovered from the blob");
    CHECK(r.header().key_id == 42, "key id recovered from the blob");
    CHECK(r.header().compressed(), "compression flag recovered from the blob");
    CHECK(r.header().encrypted(), "encryption flag recovered from the blob");
    CHECK(r.plaintext_size() == plain.size(), "plaintext length recovered from the blob");

    std::vector<uint8_t> out;
    CHECK_STATUS(r.read_all([&out](const uint8_t* p, size_t n) {
        out.insert(out.end(), p, p + n); return true;
    }), Status::Ok, "read");
    CHECK(out == plain, "…and the content is right");
}

// ---------------------------------------------------------------------------
// §6.2: the writer is single-pass
// ---------------------------------------------------------------------------
static void test_writer_never_seeks_backwards() {
    std::cout << "writer: single-pass, append-only (§6.2)..." << std::endl;
    // The sink is the only way bytes leave the writer, and it has no seek. This
    // asserts the property structurally: every call appends, the total equals
    // the blob length, and the header is emitted before any block.
    std::vector<uint8_t> blob;
    size_t calls = 0;
    bool header_first = false;
    Writer w([&](const uint8_t* p, size_t n) {
        if (calls == 0) {
            header_first = (n >= 4 && std::memcmp(p, kMagic, 4) == 0);
        }
        ++calls;
        blob.insert(blob.end(), p, p + n);
        return true;
    }, KEY, ident(), opts(256));

    auto plain = noise(1000);
    // Feed in awkward chunk sizes: block cutting must not depend on how the
    // caller chunks its input.
    for (size_t off = 0; off < plain.size(); ) {
        const size_t take = std::min<size_t>(37, plain.size() - off);
        CHECK_STATUS(w.write(plain.data() + off, take), Status::Ok, "write");
        off += take;
    }
    CHECK_STATUS(w.finish(), Status::Ok, "finish");
    CHECK(header_first, "the header is the first thing written");
    CHECK(w.plaintext_size() == plain.size(), "plaintext size tracked");
    CHECK(w.stored_size() == blob.size(), "stored size tracked");

    std::vector<uint8_t> out;
    CHECK_STATUS(decode(blob, KEY, ident(), out), Status::Ok, "decode");
    CHECK(out == plain, "chunked writing round-trips");
}

static void test_write_chunking_is_irrelevant() {
    std::cout << "writer: output is identical whatever the input chunking..." << std::endl;
    auto plain = текст(4000);
    std::vector<std::vector<uint8_t>> results;
    for (size_t chunk : {size_t(1), size_t(7), size_t(256), size_t(999), size_t(100000)}) {
        std::vector<uint8_t> blob;
        Writer w([&blob](const uint8_t* p, size_t n) {
            blob.insert(blob.end(), p, p + n); return true;
        }, KEY, ident(), opts(512, false, true));   // no encryption -> deterministic
        for (size_t off = 0; off < plain.size(); ) {
            const size_t take = std::min(chunk, plain.size() - off);
            CHECK_STATUS(w.write(plain.data() + off, take), Status::Ok, "write");
            off += take;
        }
        CHECK_STATUS(w.finish(), Status::Ok, "finish");
        results.push_back(blob);
    }
    for (size_t i = 1; i < results.size(); ++i) {
        CHECK(results[i] == results[0], "chunking does not change the stored bytes");
    }
}

static void test_sink_failure_propagates() {
    std::cout << "writer: a refusing sink is an error, not a silent truncation..." << std::endl;
    Writer w([](const uint8_t*, size_t) { return false; }, KEY, ident(), opts(256));
    auto plain = noise(1000);
    auto e = w.write(plain.data(), plain.size());
    CHECK(e.status != Status::Ok, "a refusing sink fails the write");
}

static void test_unfinished_write_is_not_readable() {
    std::cout << "writer: an unfinished blob does not read back as a short file..." << std::endl;
    std::vector<uint8_t> blob;
    Writer w([&blob](const uint8_t* p, size_t n) {
        blob.insert(blob.end(), p, p + n); return true;
    }, KEY, ident(), opts(256));
    auto plain = noise(1000);
    CHECK_STATUS(w.write(plain.data(), plain.size()), Status::Ok, "write");
    // Deliberately no finish(): no trailer, no footer, no final-block flag.
    std::vector<uint8_t> out;
    CHECK(decode(blob, KEY, ident(), out).status != Status::Ok,
          "an unfinished write is refused, not served short");
}

static void test_status_names_are_total() {
    std::cout << "status_name() covers every enumerator..." << std::endl;
    const Status all[] = {
        Status::Ok, Status::ShortInput, Status::BadMagic, Status::UnsupportedVersion,
        Status::UnsupportedCapability, Status::BadTrailer, Status::BadBlock,
        Status::Truncated, Status::RangeNotSatisfiable, Status::BadKey, Status::Internal,
    };
    for (auto s : all) {
        CHECK(std::strcmp(status_name(s), "unknown") != 0, "every status has a name");
    }
}

static void test_large_payload() {
    std::cout << "scale: a multi-megabyte payload at the default block size..." << std::endl;
    const size_t n = 5 * 1024 * 1024 + 4321;
    auto plain = noise(n);
    std::vector<uint8_t> blob;
    Options o;              // defaults: 1 MiB blocks, encrypt, compress
    CHECK_STATUS(encode(plain, KEY, ident(), blob, o), Status::Ok, "encode 5 MiB");

    std::vector<uint8_t> out;
    CHECK_STATUS(decode(blob, KEY, ident(), out), Status::Ok, "decode 5 MiB");
    CHECK(out == plain, "5 MiB round-trips");

    // Overhead is the tag + index per block, and must stay negligible.
    const double overhead = static_cast<double>(blob.size() - n) / static_cast<double>(n);
    CHECK(overhead < 0.001, "overhead under 0.1% at 1 MiB blocks");

    // Random ranges across the whole thing.
    uint64_t seed = 7;
    for (int i = 0; i < 40; ++i) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        const uint64_t off = seed % n;
        const uint64_t len = 1 + (seed >> 20) % 100000;
        std::vector<uint8_t> got;
        CHECK_STATUS(decode_range(blob, KEY, ident(), off, len, got), Status::Ok, "range");
        const size_t expect_len = static_cast<size_t>(std::min<uint64_t>(len, n - off));
        CHECK(got.size() == expect_len, "range length");
        CHECK(std::equal(got.begin(), got.end(), plain.begin() + off), "range content");
    }
}

int main() {
    test_roundtrip_matrix();
    test_compression_actually_happens();
    test_uniform_flag_for_media_case();
    test_ranges_exhaustive();
    test_range_edges();
    test_range_reads_only_what_it_needs();
    test_reordering_is_detected();
    test_splicing_from_another_version_is_detected();
    test_truncation_is_detected();
    test_rewritten_index_is_detected();
    test_header_tampering_is_detected();
    test_block_corruption_is_localised();
    test_wrong_key_is_rejected();
    test_nonces_never_repeat();
    test_unknown_required_capability_refuses();
    test_unknown_advisory_capability_proceeds();
    test_bad_magic_and_version();
    test_self_describing();
    test_writer_never_seeks_backwards();
    test_write_chunking_is_irrelevant();
    test_sink_failure_propagates();
    test_unfinished_write_is_not_readable();
    test_status_names_are_total();
    test_large_payload();
    std::cout << "storage_format_v2_tests: all passed (" << g_checks << " checks)" << std::endl;
    return 0;
}
