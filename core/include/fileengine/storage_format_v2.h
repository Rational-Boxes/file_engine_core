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

#pragma once

// Storage format v2 — authenticated random access.
// design_documents/storage_pipeline.md §6, SR-23 – SR-38.
//
// v1 stores a version as one zlib stream inside one AES-256-GCM object. That
// format cannot serve a byte range cheaply (zlib has no seek points) and cannot
// authenticate a partial read (the GCM tag covers the whole object). v2 stores
// the version as INDEPENDENT BLOCKS over fixed-size plaintext windows, each
// compressed, encrypted and authenticated on its own, so the unit of
// authentication becomes the unit of access.
//
// Layout (§6.2):
//
//   header   32 bytes, cleartext, authenticated as AAD by every block
//   blocks   [ ciphertext ‖ tag(16) ] × n, in plaintext order
//   trailer  block_count, plaintext_size, index[], trailer_tag
//   footer   16 bytes, always last: trailer_offset ‖ "FEV2" ‖ reserved
//
// Three things about this shape are deliberate:
//
//  * PLAINTEXT OFFSETS ARE IMPLICIT. Block i covers
//    [i*block_size, (i+1)*block_size), so the index stores only each block's
//    STORED length — 4 bytes per block.
//  * AN UNCOMPRESSED VERSION NEEDS NO INDEX. With kCapUniformStoredLength every
//    block is block_size + 16 stored, so the stored offset is arithmetic. That
//    is the media case, which is the case that most needs O(1) seek.
//  * COUNT AND INDEX LIVE IN THE TRAILER, not the header, so writing stays
//    single-pass and streaming. A writer that had to patch a count back into
//    the header could not stream to an object store. The footer is fixed-size
//    and last, so a reader finds the trailer with one read from the end.
//
// WHAT THE PER-BLOCK TAGS DO NOT GIVE YOU, and what this format adds:
// a tag proves a block is genuine. It proves nothing about ORDER,
// COMPLETENESS, or WHICH FILE the block came from. So every block's AAD binds
// the header bytes, the block index, a final-block flag, and the version
// identity (SR-24/SR-25/SR-27), and the trailer carries its own tag (SR-26) —
// without that last one an attacker rewrites the index to reorder or drop
// blocks without touching a single block tag.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fileengine {
namespace v2 {

constexpr char     kMagic[4]      = {'F', 'E', 'V', '2'};
constexpr uint16_t kFormatVersion = 2;
constexpr size_t   kHeaderSize    = 32;
constexpr size_t   kFooterSize    = 16;
constexpr size_t   kTagSize       = 16;
constexpr size_t   kSaltSize      = 8;
constexpr size_t   kNonceSize     = 12;   // salt(8) ‖ block_index(4)

//: §6.6 / D6.
constexpr uint32_t kDefaultBlockSize = 1024 * 1024;

//: Bound on each identity string, so a malformed header cannot make a reader
//: allocate arbitrarily before anything has been authenticated.
constexpr size_t kMaxIdentityFieldBytes = 512;

// Capability bits (§6.2, SR-36/SR-37).
//
// REQUIRED bits describe something a reader must implement to read the blob at
// all; an unknown required bit means refuse, loudly, naming what is unsupported.
// ADVISORY bits are hints that affect efficiency and never correctness; an
// unknown advisory bit is ignored. Same split as PNG's critical/ancillary
// chunks, and it prevents the specific failure of an old binary silently
// misreading a newer blob.
constexpr uint16_t kCapCompressedBlocks    = 1u << 0;
constexpr uint16_t kCapEncrypted           = 1u << 1;
constexpr uint16_t kCapUniformStoredLength = 1u << 2;
//: An identity block follows the fixed header: the version's file_uid and
//: version_timestamp, length-prefixed. Required rather than advisory because it
//: changes how the header is PARSED — a reader that ignored it would compute
//: every block offset wrongly.
constexpr uint16_t kCapIdentity            = 1u << 3;
//: Everything this build knows how to honour. A required bit outside this mask
//: is refused.
constexpr uint16_t kSupportedRequiredCaps =
    kCapCompressedBlocks | kCapEncrypted | kCapUniformStoredLength | kCapIdentity;

//: Per-block index flag, stored in the high bit of the index entry: this block
//: is stored uncompressed even though the blob as a whole allows compression
//: (SR-31 — the decision is made per block, so a compressible header and an
//: incompressible body are each stored appropriately).
constexpr uint32_t kBlockStoredRaw = 0x80000000u;
constexpr uint32_t kBlockLenMask   = 0x7FFFFFFFu;

struct Header {
    uint16_t format_version = kFormatVersion;
    uint16_t required_caps = 0;
    uint16_t advisory_caps = 0;
    uint32_t block_size = kDefaultBlockSize;
    uint8_t  salt[kSaltSize] = {0};
    uint32_t key_id = 0;

    // The version this blob IS, recorded in the blob (kCapIdentity).
    //
    // These are the two identifiers the AAD binds (SR-25), and recording them
    // is what makes SR-34's claim complete: without them a recovered blob can
    // be decoded only by someone who already knows which version it is, which
    // in practice meant relying on the storage PATH to carry it — an
    // undocumented dependency on a layout convention.
    //
    // The original FILE NAME is deliberately absent. It is mutable, so a rename
    // would leave the header stating something false rather than merely
    // incomplete; and a filename is content-grade PII on this platform (audit
    // events reference files by uid for exactly that reason), so putting one in
    // a cleartext header would push PII into the object store and the offsite
    // mirror, where erasing a database row does not reach it.
    std::string file_uid;
    std::string version_timestamp;

    bool compressed() const { return (required_caps & kCapCompressedBlocks) != 0; }
    bool encrypted()  const { return (required_caps & kCapEncrypted) != 0; }
    bool uniform()    const { return (required_caps & kCapUniformStoredLength) != 0; }
    bool has_identity() const { return (required_caps & kCapIdentity) != 0; }
};

//: Everything a read needs that is not in the header. Recovered from the
//: trailer, which is verified before any of it is trusted.
struct Trailer {
    uint32_t block_count = 0;
    uint64_t plaintext_size = 0;
    std::vector<uint32_t> index;   // stored length per block, |kBlockStoredRaw
};

//: The version this blob belongs to. Bound into every block's AAD (SR-25) so a
//: block cannot be moved between versions encrypted under the same key.
struct VersionIdentity {
    std::string file_uid;
    std::string version_timestamp;
};

struct Options {
    uint32_t block_size = kDefaultBlockSize;
    bool encrypt = true;
    //: Allow per-block compression at all. Individual blocks are still measured
    //: (SR-31); this only says whether to try.
    bool compress = true;
    uint32_t key_id = 0;
};

enum class Status {
    Ok = 0,
    ShortInput,             // truncated before a structure could be read
    BadMagic,
    UnsupportedVersion,
    UnsupportedCapability,  // SR-36
    BadTrailer,             // SR-26: trailer tag failed, or it is inconsistent
    BadBlock,               // a block tag failed — corrupt, reordered or spliced
    Truncated,              // SR-27: final block never reached
    RangeNotSatisfiable,
    BadKey,
    IdentityMismatch,       // the blob says it is a different version
    Internal,
};

const char* status_name(Status s);

struct Error {
    Status status = Status::Ok;
    std::string detail;
    explicit operator bool() const { return status != Status::Ok; }
};

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

//: Single-pass streaming writer (§6.2). Never seeks backwards: the header goes
//: out first, blocks follow as they are fed, and the trailer + footer are
//: emitted by finish(). That is what keeps a direct-to-object-store upload
//: possible, and there is a test asserting it.
class Writer {
public:
    using Sink = std::function<bool(const uint8_t*, size_t)>;

    Writer(Sink sink, const std::string& key, const VersionIdentity& id,
           const Options& opts = {});
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    //: Feed plaintext. May be called with any chunking; blocks are cut at
    //: block_size regardless of how the input arrives.
    Error write(const uint8_t* data, size_t n);
    //: Flush the final partial block, then the trailer and footer.
    Error finish();

    uint64_t plaintext_size() const;
    uint64_t stored_size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

//: Whole-buffer convenience over Writer, for callers that already hold the
//: payload (FileSystem::put) and for tests.
Error encode(const std::vector<uint8_t>& plaintext, const std::string& key,
             const VersionIdentity& id, std::vector<uint8_t>& out,
             const Options& opts = {});

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

//: True when `head` begins with a v2 header. Used only to double-check a blob
//: the version row already claimed was v2 — selection is by the recorded
//: storage_format (SR-32), never by sniffing.
bool is_v2(const uint8_t* head, size_t n);

//: Random-access reader over a byte source. `source` must fill `out` with
//: `len` stored bytes at `offset`, returning false if it cannot.
//
// SR-34: a v2 blob is interpretable from the blob and the key ALONE — no
// database and no configuration. That is the property this class exists to
// provide, and it is why open() takes nothing but a source, a key and the
// identity bound into the AAD.
class Reader {
public:
    using Source = std::function<bool(uint64_t offset, size_t len, uint8_t* out)>;

    Reader();
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    //: Read and verify the footer, header and trailer. Nothing from the trailer
    //: is trusted before its tag verifies (SR-26).
    //
    //: `id` may be left EMPTY, in which case the identity recorded in the
    //: header is adopted — the recovery path, where a blob is all anyone has.
    //: When `id` is supplied and the header disagrees, the read is refused with
    //: IdentityMismatch rather than being attempted: the two disagreeing means
    //: either the wrong blob was fetched or the database and the bytes have
    //: diverged, and both are worth stopping for.
    Error open(Source source, uint64_t stored_size, const std::string& key,
               const VersionIdentity& id);

    const Header& header() const;
    const Trailer& trailer() const;
    uint64_t plaintext_size() const;
    //: Stored offset of the first block — the fixed header plus the identity
    //: block. Exposed because it is not a constant any more, and a caller
    //: (or a test) that re-derives it is one format change from being wrong.
    uint64_t data_offset() const;

    //: Emit plaintext [offset, offset+length) — half-open, clamped to the end
    //: (SR-14). length 0 means "to the end". Every block touched is
    //: authenticated (SR-28); a failure anywhere aborts the read.
    using Emit = std::function<bool(const uint8_t*, size_t)>;
    Error read_range(uint64_t offset, uint64_t length, const Emit& emit) const;

    //: The whole payload. Equivalent to read_range(0, 0, emit).
    Error read_all(const Emit& emit) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

//: Whole-buffer convenience over Reader, for tests and small payloads.
Error decode(const std::vector<uint8_t>& blob, const std::string& key,
             const VersionIdentity& id, std::vector<uint8_t>& out);

//: Decode a range out of an in-memory blob.
Error decode_range(const std::vector<uint8_t>& blob, const std::string& key,
                   const VersionIdentity& id, uint64_t offset, uint64_t length,
                   std::vector<uint8_t>& out);

} // namespace v2
} // namespace fileengine
