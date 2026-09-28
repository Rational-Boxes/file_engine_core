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

#include "fileengine/storage_format_v2.h"

#include <algorithm>
#include <cstring>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <zlib.h>

#include "fileengine/compression_policy.h"
#include "fileengine/crypto_utils.h"

namespace fileengine {
namespace v2 {

namespace {

// --- little-endian primitives ------------------------------------------------
void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
void put_u64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
uint16_t get_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
uint32_t get_u32(const uint8_t* p) {
    uint32_t x = 0;
    for (int i = 0; i < 4; ++i) x |= static_cast<uint32_t>(p[i]) << (8 * i);
    return x;
}
uint64_t get_u64(const uint8_t* p) {
    uint64_t x = 0;
    for (int i = 0; i < 8; ++i) x |= static_cast<uint64_t>(p[i]) << (8 * i);
    return x;
}

Error ok() { return Error{Status::Ok, ""}; }
Error err(Status s, const std::string& d) { return Error{s, d}; }

std::vector<uint8_t> key_to_bytes(const std::string& key) {
    if (key.length() == 64) return CryptoUtils::hex_string_to_bytes(key);
    return CryptoUtils::base64_decode(key);
}

// --- header ------------------------------------------------------------------
//
// Exactly kHeaderSize bytes, and the SERIALIZED FORM is what is authenticated:
// every block's AAD carries these bytes verbatim, so tampering with the block
// size, the salt, the key id or the capability bits invalidates every tag in
// the blob (SR-24).
void serialize_header(const Header& h, std::vector<uint8_t>& out) {
    const size_t start = out.size();
    out.insert(out.end(), kMagic, kMagic + 4);
    put_u16(out, h.format_version);
    put_u16(out, h.required_caps);
    put_u16(out, h.advisory_caps);
    out.push_back(0);                       // pad, keeps block_size 4-aligned
    out.push_back(0);
    put_u32(out, h.block_size);
    out.insert(out.end(), h.salt, h.salt + kSaltSize);
    put_u32(out, h.key_id);
    while (out.size() - start < kHeaderSize) out.push_back(0);   // reserved

    // The identity block, when kCapIdentity is set. Length-prefixed rather than
    // fixed-width: a uid is a UUID today and a version timestamp's format is
    // the platform's to change, and neither should be able to make this header
    // wrong by growing.
    if (h.has_identity()) {
        put_u16(out, static_cast<uint16_t>(h.file_uid.size()));
        put_u16(out, static_cast<uint16_t>(h.version_timestamp.size()));
        out.insert(out.end(), h.file_uid.begin(), h.file_uid.end());
        out.insert(out.end(), h.version_timestamp.begin(), h.version_timestamp.end());
    }
}

Error parse_header(const uint8_t* p, size_t n, Header& h) {
    if (n < kHeaderSize) return err(Status::ShortInput, "header truncated");
    // SR-38: magic and version are validated before any other field is read.
    if (std::memcmp(p, kMagic, 4) != 0) return err(Status::BadMagic, "bad header magic");
    h.format_version = get_u16(p + 4);
    if (h.format_version != kFormatVersion) {
        return err(Status::UnsupportedVersion,
                   "format version " + std::to_string(h.format_version));
    }
    h.required_caps = get_u16(p + 6);
    h.advisory_caps = get_u16(p + 8);       // SR-37: unknown advisory bits ignored
    h.block_size = get_u32(p + 12);
    std::memcpy(h.salt, p + 16, kSaltSize);
    h.key_id = get_u32(p + 24);

    // SR-36: a required capability this build does not implement means refuse,
    // naming what is unsupported. Never attempt the read.
    const uint16_t unknown = static_cast<uint16_t>(h.required_caps & ~kSupportedRequiredCaps);
    if (unknown != 0) {
        return err(Status::UnsupportedCapability,
                   "unsupported required capability bits 0x" +
                       CryptoUtils::bytes_to_hex_string(
                           {static_cast<uint8_t>(unknown >> 8), static_cast<uint8_t>(unknown)}));
    }
    if (h.block_size == 0) return err(Status::BadTrailer, "block_size is zero");
    return ok();
}

// The identity block that follows the fixed header. `p` points at it and `n` is
// what is available. Returns the number of bytes consumed via `used`.
//
// Every bound is checked BEFORE anything is allocated or copied: this is the
// one part of the header a reader must parse before any tag has verified, so it
// is the one part that has to be hostile-input-safe on its own.
Error parse_identity(const uint8_t* p, size_t n, Header& h, size_t& used) {
    used = 0;
    if (n < 4) return err(Status::ShortInput, "identity block truncated");
    const size_t uid_len = get_u16(p);
    const size_t ver_len = get_u16(p + 2);
    if (uid_len > kMaxIdentityFieldBytes || ver_len > kMaxIdentityFieldBytes) {
        return err(Status::BadMagic, "identity field implausibly long");
    }
    if (n < 4 + uid_len + ver_len) return err(Status::ShortInput, "identity block truncated");
    h.file_uid.assign(reinterpret_cast<const char*>(p + 4), uid_len);
    h.version_timestamp.assign(reinterpret_cast<const char*>(p + 4 + uid_len), ver_len);
    used = 4 + uid_len + ver_len;
    return ok();
}

// --- AAD ---------------------------------------------------------------------
//
// SR-24 / SR-25 / SR-27. Per-block tags prove each block is genuine and nothing
// else; the AAD is what proves WHERE the block belongs. It binds:
//
//   the header bytes      — block size, salt, caps, key id cannot be altered
//   a domain tag          — a block AAD can never collide with a trailer AAD
//   the block index       — blocks cannot be reordered
//   the final-block flag  — the file cannot be truncated undetectably
//   file_uid + version    — a block cannot be spliced in from another version
//                           encrypted under the same key
std::vector<uint8_t> block_aad(const std::vector<uint8_t>& header_bytes,
                               const VersionIdentity& id,
                               uint32_t index, bool final_block) {
    std::vector<uint8_t> aad;
    aad.reserve(header_bytes.size() + id.file_uid.size() +
                id.version_timestamp.size() + 32);
    aad.push_back('B');                                  // domain separation
    aad.insert(aad.end(), header_bytes.begin(), header_bytes.end());
    put_u32(aad, index);
    aad.push_back(final_block ? 1 : 0);
    put_u32(aad, static_cast<uint32_t>(id.file_uid.size()));
    aad.insert(aad.end(), id.file_uid.begin(), id.file_uid.end());
    put_u32(aad, static_cast<uint32_t>(id.version_timestamp.size()));
    aad.insert(aad.end(), id.version_timestamp.begin(), id.version_timestamp.end());
    return aad;
}

//: SR-26. The trailer gets its own tag over the header and the trailer body,
//: because otherwise an attacker rewrites the index — reordering or dropping
//: blocks — without touching a single block tag.
std::vector<uint8_t> trailer_aad(const std::vector<uint8_t>& header_bytes,
                                 const VersionIdentity& id) {
    std::vector<uint8_t> aad;
    aad.push_back('T');                                  // domain separation
    aad.insert(aad.end(), header_bytes.begin(), header_bytes.end());
    put_u32(aad, static_cast<uint32_t>(id.file_uid.size()));
    aad.insert(aad.end(), id.file_uid.begin(), id.file_uid.end());
    put_u32(aad, static_cast<uint32_t>(id.version_timestamp.size()));
    aad.insert(aad.end(), id.version_timestamp.begin(), id.version_timestamp.end());
    return aad;
}

// SR-23: nonce = salt(8) ‖ block_index(4). The salt is fresh per WRITE, so two
// writes of the same payload never share a nonce, and no two blocks within one
// version can (the index differs). Nonce reuse under one key is catastrophic
// for GCM, which is why the salt is generated in the Writer's constructor and
// never derived from anything reproducible.
//
// Index 0xFFFFFFFF is reserved for the trailer so it can never collide with a
// block; a blob is refused above that many blocks long before it gets here.
constexpr uint32_t kTrailerNonceIndex = 0xFFFFFFFFu;

void make_nonce(const uint8_t* salt, uint32_t index, uint8_t out[kNonceSize]) {
    std::memcpy(out, salt, kSaltSize);
    for (int i = 0; i < 4; ++i) out[kSaltSize + i] = static_cast<uint8_t>(index >> (8 * i));
}

// --- AEAD --------------------------------------------------------------------
bool gcm_seal(const std::vector<uint8_t>& key, const uint8_t nonce[kNonceSize],
              const std::vector<uint8_t>& aad, const uint8_t* pt, size_t pt_len,
              std::vector<uint8_t>& out) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    bool good = false;
    do {
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceSize, nullptr) != 1) break;
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce) != 1) break;
        int len = 0;
        if (!aad.empty() &&
            EVP_EncryptUpdate(ctx, nullptr, &len, aad.data(), static_cast<int>(aad.size())) != 1) break;
        const size_t base = out.size();
        out.resize(base + pt_len + kTagSize);
        if (pt_len > 0 &&
            EVP_EncryptUpdate(ctx, out.data() + base, &len, pt, static_cast<int>(pt_len)) != 1) break;
        int fin = 0;
        if (EVP_EncryptFinal_ex(ctx, out.data() + base + pt_len, &fin) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagSize),
                                out.data() + base + pt_len) != 1) break;
        good = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    return good;
}

bool gcm_open(const std::vector<uint8_t>& key, const uint8_t nonce[kNonceSize],
              const std::vector<uint8_t>& aad, const uint8_t* ct, size_t ct_len,
              std::vector<uint8_t>& out) {
    if (ct_len < kTagSize) return false;
    const size_t body = ct_len - kTagSize;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    bool good = false;
    do {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceSize, nullptr) != 1) break;
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce) != 1) break;
        int len = 0;
        if (!aad.empty() &&
            EVP_DecryptUpdate(ctx, nullptr, &len, aad.data(), static_cast<int>(aad.size())) != 1) break;
        const size_t base = out.size();
        out.resize(base + body);
        if (body > 0 &&
            EVP_DecryptUpdate(ctx, out.data() + base, &len, ct, static_cast<int>(body)) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagSize),
                                const_cast<uint8_t*>(ct + body)) != 1) break;
        int fin = 0;
        // The tag check. A reordered, spliced, truncated or corrupted block
        // fails here and nowhere else, so this return value is the whole of
        // SR-28 and must never be ignored.
        if (EVP_DecryptFinal_ex(ctx, out.data() + base + body, &fin) <= 0) {
            out.resize(base);
            break;
        }
        good = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    return good;
}

// --- per-block compression (SR-31) -------------------------------------------
//
// Decided per block, so a compressible header and an incompressible body are
// each stored appropriately. A block that does not shrink is stored raw and
// flagged in its index entry.
bool deflate_block(const uint8_t* p, size_t n, std::vector<uint8_t>& out) {
    uLongf bound = compressBound(static_cast<uLong>(n));
    out.resize(bound);
    if (compress2(out.data(), &bound, p, static_cast<uLong>(n), Z_DEFAULT_COMPRESSION) != Z_OK) {
        return false;
    }
    out.resize(bound);
    return true;
}

bool inflate_block(const uint8_t* p, size_t n, size_t expected, std::vector<uint8_t>& out) {
    out.resize(expected);
    uLongf dst = static_cast<uLongf>(expected);
    if (expected == 0) { out.clear(); return n == 0; }
    if (uncompress(out.data(), &dst, p, static_cast<uLong>(n)) != Z_OK) return false;
    if (dst != expected) return false;
    return true;
}

} // namespace

const char* status_name(Status s) {
    switch (s) {
        case Status::Ok:                    return "ok";
        case Status::ShortInput:            return "short-input";
        case Status::BadMagic:              return "bad-magic";
        case Status::UnsupportedVersion:    return "unsupported-version";
        case Status::UnsupportedCapability: return "unsupported-capability";
        case Status::BadTrailer:            return "bad-trailer";
        case Status::BadBlock:              return "bad-block";
        case Status::Truncated:             return "truncated";
        case Status::RangeNotSatisfiable:   return "range-not-satisfiable";
        case Status::BadKey:                return "bad-key";
        case Status::IdentityMismatch:      return "identity-mismatch";
        case Status::Internal:              return "internal";
    }
    return "unknown";
}

bool is_v2(const uint8_t* head, size_t n) {
    return n >= 6 && std::memcmp(head, kMagic, 4) == 0 && get_u16(head + 4) == kFormatVersion;
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

struct Writer::Impl {
    Sink sink;
    std::vector<uint8_t> key;
    VersionIdentity id;
    Header header;
    std::vector<uint8_t> header_bytes;
    Options opts;

    std::vector<uint8_t> pending;       // partial plaintext block
    std::vector<uint32_t> index;
    uint64_t plaintext_size = 0;
    uint64_t stored_size = 0;
    uint32_t next_index = 0;
    bool header_written = false;
    bool finished = false;
    bool uniform = true;                // still true iff nothing was compressed
    Error fault = ok();

    bool emit(const uint8_t* p, size_t n) {
        if (!sink(p, n)) return false;
        stored_size += n;
        return true;
    }

    // A block is only ever flushed as "final" by finish(), so a writer that is
    // never finished produces a blob whose last block lacks the final flag and
    // therefore fails SR-27 on read. That is the correct outcome: a truncated
    // write must not read back as a short but valid file.
    Error flush_block(bool final_block) {
        std::vector<uint8_t> payload;
        bool raw = true;
        if (opts.compress && !pending.empty()) {
            std::vector<uint8_t> z;
            if (deflate_block(pending.data(), pending.size(), z) &&
                z.size() < pending.size()) {
                payload.swap(z);
                raw = false;
                uniform = false;
            }
        }
        if (raw) payload.assign(pending.begin(), pending.end());

        std::vector<uint8_t> out;
        if (opts.encrypt) {
            uint8_t nonce[kNonceSize];
            make_nonce(header.salt, next_index, nonce);
            const auto aad = block_aad(header_bytes, id, next_index, final_block);
            if (!gcm_seal(key, nonce, aad, payload.data(), payload.size(), out)) {
                return err(Status::Internal, "block seal failed");
            }
        } else {
            out.swap(payload);
        }
        if (out.size() > kBlockLenMask) return err(Status::Internal, "block too large");

        uint32_t entry = static_cast<uint32_t>(out.size());
        if (raw) entry |= kBlockStoredRaw;
        index.push_back(entry);
        if (!emit(out.data(), out.size())) return err(Status::Internal, "sink refused");
        ++next_index;
        pending.clear();
        return ok();
    }
};

Writer::Writer(Sink sink, const std::string& key, const VersionIdentity& id,
               const Options& opts)
    : impl_(new Impl()) {
    impl_->sink = std::move(sink);
    impl_->id = id;
    impl_->opts = opts;
    impl_->header.block_size = opts.block_size ? opts.block_size : kDefaultBlockSize;
    impl_->header.key_id = opts.key_id;
    impl_->header.required_caps = kCapIdentity;
    impl_->header.file_uid = id.file_uid;
    impl_->header.version_timestamp = id.version_timestamp;
    if (opts.compress) impl_->header.required_caps |= kCapCompressedBlocks;
    if (opts.encrypt)  impl_->header.required_caps |= kCapEncrypted;

    if (opts.encrypt) {
        impl_->key = key_to_bytes(key);
        if (impl_->key.size() != 32) {
            impl_->fault = err(Status::BadKey, "AES-256 needs a 32-byte key");
            return;
        }
        // SR-23: fresh per WRITE. Never derived from the version name, the uid,
        // or anything else a rewrite would reproduce.
        if (RAND_bytes(impl_->header.salt, static_cast<int>(kSaltSize)) != 1) {
            impl_->fault = err(Status::Internal, "RAND_bytes failed");
            return;
        }
    }
    impl_->pending.reserve(impl_->header.block_size);
}

Writer::~Writer() = default;

uint64_t Writer::plaintext_size() const { return impl_->plaintext_size; }
uint64_t Writer::stored_size() const { return impl_->stored_size; }

Error Writer::write(const uint8_t* data, size_t n) {
    if (impl_->fault) return impl_->fault;
    if (impl_->finished) return err(Status::Internal, "write after finish");

    if (!impl_->header_written) {
        // The header's SERIALIZED bytes are what every AAD binds, so they are
        // built once and kept — re-serializing per block would risk the two
        // diverging silently.
        serialize_header(impl_->header, impl_->header_bytes);
        if (!impl_->emit(impl_->header_bytes.data(), impl_->header_bytes.size())) {
            return err(Status::Internal, "sink refused header");
        }
        impl_->header_written = true;
    }

    // ONE BLOCK OF LOOKAHEAD. A full block is flushed only once more input has
    // arrived to follow it, so the final-block flag (SR-27) lands on the block
    // that really is last. Flushing eagerly on "pending is full" would emit a
    // spurious empty final block whenever the payload is an exact multiple of
    // the block size — wasting a block, and making the plaintext length
    // indistinguishable from a blob whose last block was dropped.
    size_t off = 0;
    while (off < n) {
        if (impl_->pending.size() == impl_->header.block_size) {
            auto e = impl_->flush_block(false);
            if (e) return e;
        }
        const size_t room = impl_->header.block_size - impl_->pending.size();
        const size_t take = std::min(room, n - off);
        impl_->pending.insert(impl_->pending.end(), data + off, data + off + take);
        off += take;
        impl_->plaintext_size += take;
    }
    return ok();
}

Error Writer::finish() {
    if (impl_->fault) return impl_->fault;
    if (impl_->finished) return ok();

    if (!impl_->header_written) {
        serialize_header(impl_->header, impl_->header_bytes);
        if (!impl_->emit(impl_->header_bytes.data(), impl_->header_bytes.size())) {
            return err(Status::Internal, "sink refused header");
        }
        impl_->header_written = true;
    }

    // Always flush a final block, even for an empty payload: the final-block
    // flag is what SR-27 checks, and a zero-block blob would have nowhere to
    // carry it. An empty version is therefore one empty final block.
    auto e = impl_->flush_block(true);
    if (e) return e;

    if (impl_->uniform && impl_->opts.encrypt) {
        impl_->header.required_caps |= kCapUniformStoredLength;
    }

    // Trailer body, then its own tag (SR-26).
    std::vector<uint8_t> body;
    put_u32(body, static_cast<uint32_t>(impl_->index.size()));
    put_u64(body, impl_->plaintext_size);
    put_u16(body, impl_->header.required_caps);   // final caps, incl. uniform
    for (uint32_t x : impl_->index) put_u32(body, x);

    std::vector<uint8_t> sealed;
    if (impl_->opts.encrypt) {
        uint8_t nonce[kNonceSize];
        make_nonce(impl_->header.salt, kTrailerNonceIndex, nonce);
        const auto aad = trailer_aad(impl_->header_bytes, impl_->id);
        if (!gcm_seal(impl_->key, nonce, aad, body.data(), body.size(), sealed)) {
            return err(Status::Internal, "trailer seal failed");
        }
    } else {
        sealed.swap(body);
    }

    const uint64_t trailer_offset = impl_->stored_size;
    if (!impl_->emit(sealed.data(), sealed.size())) {
        return err(Status::Internal, "sink refused trailer");
    }

    std::vector<uint8_t> footer;
    put_u64(footer, trailer_offset);
    footer.insert(footer.end(), kMagic, kMagic + 4);
    put_u32(footer, 0);
    if (!impl_->emit(footer.data(), footer.size())) {
        return err(Status::Internal, "sink refused footer");
    }

    impl_->finished = true;
    return ok();
}

Error encode(const std::vector<uint8_t>& plaintext, const std::string& key,
             const VersionIdentity& id, std::vector<uint8_t>& out,
             const Options& opts) {
    out.clear();
    Writer w([&out](const uint8_t* p, size_t n) {
        out.insert(out.end(), p, p + n);
        return true;
    }, key, id, opts);
    auto e = w.write(plaintext.data(), plaintext.size());
    if (e) return e;
    return w.finish();
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

struct Reader::Impl {
    Source source;
    std::vector<uint8_t> key;
    VersionIdentity id;
    uint64_t stored_size = 0;
    Header header;
    std::vector<uint8_t> header_bytes;
    size_t header_total = kHeaderSize;   // fixed part + identity block
    Trailer trailer;
    std::vector<uint64_t> block_offset;   // stored offset of each block
    bool open = false;

    bool fetch(uint64_t off, size_t len, std::vector<uint8_t>& out) const {
        if (len == 0) { out.clear(); return true; }
        if (off > stored_size || stored_size - off < len) return false;
        out.resize(len);
        return source(off, len, out.data());
    }

    //: Decrypt + inflate one block, appending its plaintext to `out`.
    Error load_block(uint32_t i, std::vector<uint8_t>& out) const {
        const uint32_t entry = trailer.index[i];
        const size_t stored_len = entry & kBlockLenMask;
        const bool raw = (entry & kBlockStoredRaw) != 0;
        const bool final_block = (i + 1 == trailer.block_count);

        std::vector<uint8_t> ct;
        if (!fetch(block_offset[i], stored_len, ct)) {
            return err(Status::ShortInput, "block " + std::to_string(i) + " unreadable");
        }

        std::vector<uint8_t> payload;
        if (header.encrypted()) {
            uint8_t nonce[kNonceSize];
            make_nonce(header.salt, i, nonce);
            const auto aad = block_aad(header_bytes, id, i, final_block);
            if (!gcm_open(key, nonce, aad, ct.data(), ct.size(), payload)) {
                // SR-24/25/27/28 all land here: corrupted, reordered, spliced
                // from another version, or the final-block flag disagreeing.
                return err(Status::BadBlock,
                           "block " + std::to_string(i) + " failed authentication");
            }
        } else {
            payload.swap(ct);
        }

        // Plaintext length of this block is implicit: full blocks are
        // block_size, the last one is whatever remains.
        const uint64_t begin = static_cast<uint64_t>(i) * header.block_size;
        const size_t expect = static_cast<size_t>(
            std::min<uint64_t>(header.block_size, trailer.plaintext_size - begin));

        if (raw) {
            if (payload.size() != expect) {
                return err(Status::BadBlock, "block " + std::to_string(i) + " wrong length");
            }
            out.swap(payload);
        } else {
            if (!inflate_block(payload.data(), payload.size(), expect, out)) {
                return err(Status::BadBlock, "block " + std::to_string(i) + " inflate failed");
            }
        }
        return ok();
    }
};

Reader::Reader() : impl_(new Impl()) {}
Reader::~Reader() = default;

const Header& Reader::header() const { return impl_->header; }
const Trailer& Reader::trailer() const { return impl_->trailer; }
uint64_t Reader::plaintext_size() const { return impl_->trailer.plaintext_size; }
uint64_t Reader::data_offset() const { return impl_->header_total; }

Error Reader::open(Source source, uint64_t stored_size, const std::string& key,
                   const VersionIdentity& id) {
    impl_->source = std::move(source);
    impl_->stored_size = stored_size;
    impl_->id = id;   // provisional; replaced below once the header is read

    if (stored_size < kHeaderSize + kFooterSize) {
        return err(Status::ShortInput, "blob too small to be v2");
    }

    // Header first — SR-38 validates magic and version before anything else is
    // read, and SR-36 refuses an unknown required capability here rather than
    // attempting the read.
    std::vector<uint8_t> hb;
    if (!impl_->fetch(0, kHeaderSize, hb)) return err(Status::ShortInput, "header unreadable");
    auto e = parse_header(hb.data(), hb.size(), impl_->header);
    if (e) return e;

    // The identity block, when the header says there is one. Its LENGTH decides
    // where the blocks start, which is why kCapIdentity is a required rather
    // than an advisory capability: a reader that ignored the bit would compute
    // every block offset wrongly and fail with a puzzling tag error instead of
    // a clear one.
    if (impl_->header.has_identity()) {
        std::vector<uint8_t> lens;
        if (!impl_->fetch(kHeaderSize, 4, lens)) {
            return err(Status::ShortInput, "identity lengths unreadable");
        }
        const size_t uid_len = get_u16(lens.data());
        const size_t ver_len = get_u16(lens.data() + 2);
        if (uid_len > kMaxIdentityFieldBytes || ver_len > kMaxIdentityFieldBytes) {
            return err(Status::BadMagic, "identity field implausibly long");
        }
        std::vector<uint8_t> idblock;
        if (!impl_->fetch(kHeaderSize, 4 + uid_len + ver_len, idblock)) {
            return err(Status::ShortInput, "identity block unreadable");
        }
        size_t used = 0;
        auto ie = parse_identity(idblock.data(), idblock.size(), impl_->header, used);
        if (ie) return ie;
        hb.insert(hb.end(), idblock.begin(), idblock.begin() + used);
    }
    impl_->header_bytes = hb;
    impl_->header_total = hb.size();

    // Cross-check, or adopt. An empty caller identity means "this blob is all I
    // have" — the recovery path — and the header's own answer is used. When both
    // are present and disagree, refuse: either the wrong blob was fetched or the
    // database and the bytes have diverged, and both are worth stopping for
    // rather than discovering as an authentication failure three blocks in.
    VersionIdentity effective = id;
    if (impl_->header.has_identity()) {
        const bool caller_supplied = !id.file_uid.empty() || !id.version_timestamp.empty();
        if (!caller_supplied) {
            effective.file_uid = impl_->header.file_uid;
            effective.version_timestamp = impl_->header.version_timestamp;
        } else if (id.file_uid != impl_->header.file_uid ||
                   id.version_timestamp != impl_->header.version_timestamp) {
            return err(Status::IdentityMismatch,
                       "blob records " + impl_->header.file_uid + "@" +
                       impl_->header.version_timestamp + ", caller asked for " +
                       id.file_uid + "@" + id.version_timestamp);
        }
    }
    impl_->id = effective;

    if (impl_->header.encrypted()) {
        impl_->key = key_to_bytes(key);
        if (impl_->key.size() != 32) return err(Status::BadKey, "AES-256 needs a 32-byte key");
    }

    // Footer, then trailer. The footer's magic and offset are validated before
    // the trailer is trusted (SR-38).
    std::vector<uint8_t> fb;
    if (!impl_->fetch(stored_size - kFooterSize, kFooterSize, fb)) {
        return err(Status::ShortInput, "footer unreadable");
    }
    if (std::memcmp(fb.data() + 8, kMagic, 4) != 0) {
        return err(Status::BadMagic, "bad footer magic");
    }
    const uint64_t trailer_offset = get_u64(fb.data());
    if (trailer_offset < impl_->header_total || trailer_offset > stored_size - kFooterSize) {
        return err(Status::BadTrailer, "trailer offset out of range");
    }
    const size_t trailer_len = static_cast<size_t>(stored_size - kFooterSize - trailer_offset);

    std::vector<uint8_t> tb;
    if (!impl_->fetch(trailer_offset, trailer_len, tb)) {
        return err(Status::ShortInput, "trailer unreadable");
    }

    std::vector<uint8_t> body;
    if (impl_->header.encrypted()) {
        uint8_t nonce[kNonceSize];
        make_nonce(impl_->header.salt, kTrailerNonceIndex, nonce);
        const auto aad = trailer_aad(impl_->header_bytes, impl_->id);
        if (!gcm_open(impl_->key, nonce, aad, tb.data(), tb.size(), body)) {
            // SR-26: without this an attacker rewrites the index and reorders
            // or drops blocks without touching a single block tag.
            return err(Status::BadTrailer, "trailer failed authentication");
        }
    } else {
        body.swap(tb);
    }

    if (body.size() < 14) return err(Status::BadTrailer, "trailer too short");
    impl_->trailer.block_count = get_u32(body.data());
    impl_->trailer.plaintext_size = get_u64(body.data() + 4);
    const uint16_t final_caps = get_u16(body.data() + 12);
    // The trailer's capability word is authenticated; the header's copy is too,
    // but the uniform bit is only known at finish(). Take the trailer's.
    impl_->header.required_caps = final_caps;
    if ((final_caps & ~kSupportedRequiredCaps) != 0) {
        return err(Status::UnsupportedCapability, "unsupported required capability in trailer");
    }

    const size_t need = 14 + static_cast<size_t>(impl_->trailer.block_count) * 4;
    if (body.size() < need) return err(Status::BadTrailer, "trailer index truncated");
    impl_->trailer.index.resize(impl_->trailer.block_count);
    for (uint32_t i = 0; i < impl_->trailer.block_count; ++i) {
        impl_->trailer.index[i] = get_u32(body.data() + 14 + i * 4);
    }

    // The index must describe exactly the bytes between the header and the
    // trailer — a short or long index is a structural inconsistency even if
    // every tag would verify.
    uint64_t off = impl_->header_total;
    impl_->block_offset.resize(impl_->trailer.block_count);
    for (uint32_t i = 0; i < impl_->trailer.block_count; ++i) {
        impl_->block_offset[i] = off;
        off += (impl_->trailer.index[i] & kBlockLenMask);
        if (off > trailer_offset) return err(Status::BadTrailer, "index overruns trailer");
    }
    if (off != trailer_offset) return err(Status::BadTrailer, "index does not cover the blocks");

    // SR-27: the block count and the plaintext size must agree. A blob claiming
    // more plaintext than its blocks can hold has been truncated.
    const uint64_t capacity =
        static_cast<uint64_t>(impl_->trailer.block_count) * impl_->header.block_size;
    if (impl_->trailer.block_count == 0 ||
        impl_->trailer.plaintext_size > capacity ||
        (impl_->trailer.block_count > 1 &&
         impl_->trailer.plaintext_size <=
             static_cast<uint64_t>(impl_->trailer.block_count - 1) * impl_->header.block_size)) {
        return err(Status::Truncated, "block count inconsistent with plaintext size");
    }

    impl_->open = true;
    return ok();
}

Error Reader::read_range(uint64_t offset, uint64_t length, const Emit& emit) const {
    if (!impl_->open) return err(Status::Internal, "reader not open");
    const uint64_t total = impl_->trailer.plaintext_size;

    // SR-14: past the end is not satisfiable; overlong is clamped.
    if (offset > total || (offset == total && total > 0)) {
        return err(Status::RangeNotSatisfiable, "offset beyond end of file");
    }
    if (length == 0 || offset + length > total) length = total - offset;
    if (length == 0) return ok();

    const uint32_t bs = impl_->header.block_size;
    const uint32_t first = static_cast<uint32_t>(offset / bs);
    const uint32_t last = static_cast<uint32_t>((offset + length - 1) / bs);

    // SR-29: at most one block beyond each end of the window — which is what
    // the loop bounds already give, since first/last are the blocks the window
    // lands in.
    for (uint32_t i = first; i <= last; ++i) {
        std::vector<uint8_t> pt;
        auto e = impl_->load_block(i, pt);
        if (e) return e;

        const uint64_t block_begin = static_cast<uint64_t>(i) * bs;
        const uint64_t want_begin = std::max(offset, block_begin);
        const uint64_t want_end = std::min(offset + length, block_begin + pt.size());
        if (want_end <= want_begin) continue;
        if (!emit(pt.data() + (want_begin - block_begin),
                  static_cast<size_t>(want_end - want_begin))) {
            return err(Status::Internal, "emit refused");
        }
    }
    return ok();
}

Error Reader::read_all(const Emit& emit) const { return read_range(0, 0, emit); }

Error decode(const std::vector<uint8_t>& blob, const std::string& key,
             const VersionIdentity& id, std::vector<uint8_t>& out) {
    return decode_range(blob, key, id, 0, 0, out);
}

Error decode_range(const std::vector<uint8_t>& blob, const std::string& key,
                   const VersionIdentity& id, uint64_t offset, uint64_t length,
                   std::vector<uint8_t>& out) {
    out.clear();
    Reader r;
    auto e = r.open([&blob](uint64_t off, size_t len, uint8_t* dst) {
        if (off + len > blob.size()) return false;
        std::memcpy(dst, blob.data() + off, len);
        return true;
    }, blob.size(), key, id);
    if (e) return e;
    return r.read_range(offset, length, [&out](const uint8_t* p, size_t n) {
        out.insert(out.end(), p, p + n);
        return true;
    });
}

} // namespace v2
} // namespace fileengine
