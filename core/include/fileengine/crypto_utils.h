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

#include <vector>
#include <string>
#include <cstdint>
#include <memory>

namespace fileengine {

// ---------------------------------------------------------------------------
// Streaming transforms — chunk-at-a-time equivalents of the whole-buffer
// CryptoUtils helpers below. They let the storage pipeline compress/encrypt (and
// decrypt/decompress) a file as it flows through, so the whole payload is never
// held in memory. Each transform produces/consumes the SAME byte format as the
// one-shot helpers, so streaming and one-shot are cross-compatible and existing
// stored files remain readable.
//
// Usage: call update() for each input chunk (output is appended to `out`), then
// finish() exactly once to flush. `out` is the caller's buffer (cleared by the
// transform on entry to each call).

// zlib deflate, streaming. Output is a standard zlib stream (decompressible by
// the one-shot CryptoUtils::decompress_data and by DecompressStream alike).
class CompressStream {
public:
    CompressStream();
    ~CompressStream();
    CompressStream(const CompressStream&) = delete;
    CompressStream& operator=(const CompressStream&) = delete;
    void update(const uint8_t* data, size_t n, std::vector<uint8_t>& out);
    void finish(std::vector<uint8_t>& out);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// zlib inflate, streaming.
class DecompressStream {
public:
    DecompressStream();
    ~DecompressStream();
    DecompressStream(const DecompressStream&) = delete;
    DecompressStream& operator=(const DecompressStream&) = delete;
    void update(const uint8_t* data, size_t n, std::vector<uint8_t>& out);
    void finish(std::vector<uint8_t>& out);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// AES-256-GCM encrypt, streaming. Emits the 12-byte IV before any ciphertext and
// the 16-byte tag at finish() — i.e. the same [IV || ciphertext || tag] layout
// as CryptoUtils::encrypt_data. Zero total input produces zero output (matching
// the one-shot empty-blob convention).
class EncryptStream {
public:
    explicit EncryptStream(const std::string& key);
    ~EncryptStream();
    EncryptStream(const EncryptStream&) = delete;
    EncryptStream& operator=(const EncryptStream&) = delete;
    void update(const uint8_t* data, size_t n, std::vector<uint8_t>& out);
    void finish(std::vector<uint8_t>& out);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// AES-256-GCM decrypt, streaming. Consumes [IV || ciphertext || tag]; the GCM
// tag is verified at finish() (throws on mismatch). NOTE: because the tag trails
// the ciphertext, plaintext chunks are emitted before authentication completes —
// a caller streaming to a network MUST treat a finish() failure as "discard what
// was sent" (abort the stream with an error). Empty input yields empty output.
class DecryptStream {
public:
    explicit DecryptStream(const std::string& key);
    ~DecryptStream();
    DecryptStream(const DecryptStream&) = delete;
    DecryptStream& operator=(const DecryptStream&) = delete;
    void update(const uint8_t* data, size_t n, std::vector<uint8_t>& out);
    void finish(std::vector<uint8_t>& out);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class CryptoUtils {
public:
    // Compression functions
    static std::vector<uint8_t> compress_data(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> decompress_data(const std::vector<uint8_t>& compressed_data);
    
    // Encryption functions
    static std::vector<uint8_t> encrypt_data(const std::vector<uint8_t>& data, const std::string& key);
    static std::vector<uint8_t> decrypt_data(const std::vector<uint8_t>& encrypted_data, const std::string& key);
    
    // Utility function to convert hex string to bytes
    static std::vector<uint8_t> hex_string_to_bytes(const std::string& hex);
    
    // Utility function to convert bytes to hex string
    static std::string bytes_to_hex_string(const std::vector<uint8_t>& bytes);

    // Utility function to decode base64
    static std::vector<uint8_t> base64_decode(const std::string& input);
};

} // namespace fileengine