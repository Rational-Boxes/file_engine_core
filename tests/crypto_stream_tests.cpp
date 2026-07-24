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

// Unit tests for the streaming crypto primitives (CompressStream /
// DecompressStream / EncryptStream / DecryptStream). The central guarantee is
// cross-compatibility with the one-shot CryptoUtils helpers and independence
// from chunk boundaries, so existing stored files round-trip and the streaming
// storage pipeline produces byte-equivalent content.
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "fileengine/crypto_utils.h"

using fileengine::CryptoUtils;
using fileengine::CompressStream;
using fileengine::DecompressStream;
using fileengine::EncryptStream;
using fileengine::DecryptStream;

// 32-byte key as 64 hex chars.
static const std::string KEY = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

static std::vector<uint8_t> make_data(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>((i * 31 + 7) & 0xff);
    return v;
}

// Run an input through a streaming transform in fixed-size chunks + finish().
template <typename Stream, typename... Args>
static std::vector<uint8_t> run_stream(const std::vector<uint8_t>& in, size_t chunk, Args&&... args) {
    Stream s(std::forward<Args>(args)...);
    std::vector<uint8_t> out, total;
    for (size_t off = 0; off < in.size(); off += chunk) {
        size_t n = std::min(chunk, in.size() - off);
        s.update(in.data() + off, n, out);
        total.insert(total.end(), out.begin(), out.end());
    }
    s.finish(out);
    total.insert(total.end(), out.begin(), out.end());
    return total;
}

static void test_compress_cross_compat() {
    std::cout << "compress: streaming <-> one-shot cross round-trip..." << std::endl;
    for (size_t sz : {0u, 1u, 100u, 65536u, 1000000u}) {
        auto data = make_data(sz);
        // streaming compress -> one-shot decompress
        for (size_t chunk : {1u, 7u, 4096u, 100000u}) {
            auto comp = run_stream<CompressStream>(data, chunk);
            auto back = CryptoUtils::decompress_data(comp);
            assert(back == data);
        }
        // one-shot compress -> streaming decompress
        if (!data.empty()) {
            auto comp = CryptoUtils::compress_data(data);
            for (size_t chunk : {1u, 7u, 4096u}) {
                auto back = run_stream<DecompressStream>(comp, chunk);
                assert(back == data);
            }
        }
    }
    std::cout << "  ok" << std::endl;
}

static void test_encrypt_cross_compat() {
    std::cout << "encrypt: streaming <-> one-shot cross round-trip..." << std::endl;
    for (size_t sz : {0u, 1u, 16u, 4096u, 1000003u}) {
        auto data = make_data(sz);
        // streaming encrypt -> one-shot decrypt
        for (size_t chunk : {1u, 13u, 4096u, 100000u}) {
            auto blob = run_stream<EncryptStream>(data, chunk, KEY);
            auto back = CryptoUtils::decrypt_data(blob, KEY);
            assert(back == data);
        }
        // one-shot encrypt -> streaming decrypt
        auto blob = CryptoUtils::encrypt_data(data, KEY);
        for (size_t chunk : {1u, 13u, 4096u}) {
            auto back = run_stream<DecryptStream>(blob, chunk, KEY);
            assert(back == data);
        }
    }
    std::cout << "  ok" << std::endl;
}

static void test_full_pipeline() {
    std::cout << "pipeline: compress->encrypt then decrypt->decompress (streaming)..." << std::endl;
    for (size_t sz : {0u, 1u, 9999u, 5000000u}) {  // includes a >4 MiB payload
        auto data = make_data(sz);
        // Write side: plaintext -> compress(stream) -> encrypt(stream) -> blob
        CompressStream comp;
        EncryptStream enc(KEY);
        std::vector<uint8_t> blob, cbuf, ebuf;
        const size_t WCHUNK = 33333;
        for (size_t off = 0; off < data.size(); off += WCHUNK) {
            size_t n = std::min(WCHUNK, data.size() - off);
            comp.update(data.data() + off, n, cbuf);
            if (!cbuf.empty()) { enc.update(cbuf.data(), cbuf.size(), ebuf); blob.insert(blob.end(), ebuf.begin(), ebuf.end()); }
        }
        comp.finish(cbuf);
        if (!cbuf.empty()) { enc.update(cbuf.data(), cbuf.size(), ebuf); blob.insert(blob.end(), ebuf.begin(), ebuf.end()); }
        enc.finish(ebuf); blob.insert(blob.end(), ebuf.begin(), ebuf.end());

        // Read side: blob -> decrypt(stream) -> decompress(stream) -> plaintext
        DecryptStream dec(KEY);
        DecompressStream dcomp;
        std::vector<uint8_t> out, dbuf, ddbuf;
        const size_t RCHUNK = 7777;
        for (size_t off = 0; off < blob.size(); off += RCHUNK) {
            size_t n = std::min(RCHUNK, blob.size() - off);
            dec.update(blob.data() + off, n, dbuf);
            if (!dbuf.empty()) { dcomp.update(dbuf.data(), dbuf.size(), ddbuf); out.insert(out.end(), ddbuf.begin(), ddbuf.end()); }
        }
        dec.finish(dbuf);
        if (!dbuf.empty()) { dcomp.update(dbuf.data(), dbuf.size(), ddbuf); out.insert(out.end(), ddbuf.begin(), ddbuf.end()); }
        dcomp.finish(ddbuf); out.insert(out.end(), ddbuf.begin(), ddbuf.end());

        assert(out == data);
    }
    std::cout << "  ok" << std::endl;
}

static void test_tag_tamper_detected() {
    std::cout << "decrypt: tampered tag is rejected at finish()..." << std::endl;
    auto data = make_data(5000);
    auto blob = CryptoUtils::encrypt_data(data, KEY);
    blob.back() ^= 0x01;  // flip a bit in the trailing GCM tag
    bool threw = false;
    try {
        (void)run_stream<DecryptStream>(blob, 256, KEY);
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
    std::cout << "  ok" << std::endl;
}

int main() {
    test_compress_cross_compat();
    test_encrypt_cross_compat();
    test_full_pipeline();
    test_tag_tamper_detected();
    std::cout << "All crypto streaming tests passed!" << std::endl;
    return 0;
}
