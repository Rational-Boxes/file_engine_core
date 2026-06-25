#include "fileengine/crypto_utils.h"
#include <zlib.h>
#include <openssl/evp.h>
#include <openssl/aes.h>
#include <openssl/rand.h>
#include <cstring>
#include <stdexcept>
#include <iostream>

namespace fileengine {

namespace {
// Parse a 32-byte AES-256 key from a 64-char hex string or base64 (mirrors the
// one-shot encrypt_data/decrypt_data logic).
std::vector<uint8_t> parse_aes256_key(const std::string& key) {
    std::vector<uint8_t> key_bytes;
    if (key.length() == 64) {
        key_bytes = CryptoUtils::hex_string_to_bytes(key);
    } else {
        key_bytes = CryptoUtils::base64_decode(key);
    }
    if (key_bytes.size() != 32) {
        throw std::runtime_error("Invalid key length for AES-256, expected 32 bytes, got " +
                                 std::to_string(key_bytes.size()));
    }
    return key_bytes;
}
} // namespace

// ----------------------------- CompressStream ------------------------------
struct CompressStream::Impl {
    z_stream zs;
    bool active = false;
    std::vector<uint8_t> buf;
};

CompressStream::CompressStream() : impl_(std::make_unique<Impl>()) {
    memset(&impl_->zs, 0, sizeof(impl_->zs));
    if (deflateInit(&impl_->zs, Z_DEFAULT_COMPRESSION) != Z_OK) {
        throw std::runtime_error("deflateInit failed");
    }
    impl_->active = true;
    impl_->buf.resize(32768);
}

CompressStream::~CompressStream() {
    if (impl_ && impl_->active) deflateEnd(&impl_->zs);
}

void CompressStream::update(const uint8_t* data, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    if (n == 0) return;
    impl_->zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    impl_->zs.avail_in = static_cast<uInt>(n);
    do {
        impl_->zs.next_out = impl_->buf.data();
        impl_->zs.avail_out = static_cast<uInt>(impl_->buf.size());
        int ret = deflate(&impl_->zs, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR) throw std::runtime_error("deflate failed");
        size_t produced = impl_->buf.size() - impl_->zs.avail_out;
        out.insert(out.end(), impl_->buf.begin(), impl_->buf.begin() + produced);
    } while (impl_->zs.avail_out == 0);
}

void CompressStream::finish(std::vector<uint8_t>& out) {
    out.clear();
    impl_->zs.next_in = nullptr;
    impl_->zs.avail_in = 0;
    int ret;
    do {
        impl_->zs.next_out = impl_->buf.data();
        impl_->zs.avail_out = static_cast<uInt>(impl_->buf.size());
        ret = deflate(&impl_->zs, Z_FINISH);
        size_t produced = impl_->buf.size() - impl_->zs.avail_out;
        out.insert(out.end(), impl_->buf.begin(), impl_->buf.begin() + produced);
    } while (ret == Z_OK);
    if (ret != Z_STREAM_END) throw std::runtime_error("deflate finish failed");
    deflateEnd(&impl_->zs);
    impl_->active = false;
}

// ---------------------------- DecompressStream -----------------------------
struct DecompressStream::Impl {
    z_stream zs;
    bool active = false;
    bool ended = false;
    std::vector<uint8_t> buf;
};

DecompressStream::DecompressStream() : impl_(std::make_unique<Impl>()) {
    memset(&impl_->zs, 0, sizeof(impl_->zs));
    if (inflateInit(&impl_->zs) != Z_OK) {
        throw std::runtime_error("inflateInit failed");
    }
    impl_->active = true;
    impl_->buf.resize(32768);
}

DecompressStream::~DecompressStream() {
    if (impl_ && impl_->active) inflateEnd(&impl_->zs);
}

void DecompressStream::update(const uint8_t* data, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    if (n == 0 || impl_->ended) return;
    impl_->zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    impl_->zs.avail_in = static_cast<uInt>(n);
    do {
        impl_->zs.next_out = impl_->buf.data();
        impl_->zs.avail_out = static_cast<uInt>(impl_->buf.size());
        int ret = inflate(&impl_->zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            throw std::runtime_error("inflate failed");
        }
        size_t produced = impl_->buf.size() - impl_->zs.avail_out;
        out.insert(out.end(), impl_->buf.begin(), impl_->buf.begin() + produced);
        if (ret == Z_STREAM_END) { impl_->ended = true; break; }
        if (ret == Z_BUF_ERROR) break;  // need more input
    } while (impl_->zs.avail_out == 0);
}

void DecompressStream::finish(std::vector<uint8_t>& out) {
    out.clear();
    // ended==false with no input ever fed is fine (empty); otherwise the stream
    // should have reached its end marker.
    if (impl_->active) { inflateEnd(&impl_->zs); impl_->active = false; }
}

// ----------------------------- EncryptStream -------------------------------
struct EncryptStream::Impl {
    EVP_CIPHER_CTX* ctx = nullptr;
    unsigned char iv[12];
    bool iv_emitted = false;
    bool any_input = false;
};

EncryptStream::EncryptStream(const std::string& key) : impl_(std::make_unique<Impl>()) {
    std::vector<uint8_t> key_bytes = parse_aes256_key(key);
    impl_->ctx = EVP_CIPHER_CTX_new();
    if (!impl_->ctx) throw std::runtime_error("Could not create cipher context");
    if (EVP_EncryptInit_ex(impl_->ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
        RAND_bytes(impl_->iv, 12) <= 0 ||
        EVP_EncryptInit_ex(impl_->ctx, NULL, NULL, key_bytes.data(), impl_->iv) != 1) {
        EVP_CIPHER_CTX_free(impl_->ctx);
        impl_->ctx = nullptr;
        throw std::runtime_error("Could not initialize encryption");
    }
}

EncryptStream::~EncryptStream() {
    if (impl_ && impl_->ctx) EVP_CIPHER_CTX_free(impl_->ctx);
}

void EncryptStream::update(const uint8_t* data, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    if (n == 0) return;
    impl_->any_input = true;
    if (!impl_->iv_emitted) {  // [IV] prefixes the ciphertext
        out.insert(out.end(), impl_->iv, impl_->iv + 12);
        impl_->iv_emitted = true;
    }
    size_t base = out.size();
    out.resize(base + n);  // GCM is a stream cipher: ciphertext length == input
    int len = 0;
    if (EVP_EncryptUpdate(impl_->ctx, out.data() + base, &len, data, static_cast<int>(n)) != 1) {
        throw std::runtime_error("Could not encrypt data");
    }
    out.resize(base + static_cast<size_t>(len));
}

void EncryptStream::finish(std::vector<uint8_t>& out) {
    out.clear();
    if (!impl_->any_input) return;  // empty input -> empty blob (one-shot parity)
    unsigned char fin[16];
    int len = 0;
    if (EVP_EncryptFinal_ex(impl_->ctx, fin, &len) != 1) {
        throw std::runtime_error("Could not finalize encryption");
    }
    if (len > 0) out.insert(out.end(), fin, fin + len);  // GCM final emits 0 bytes
    unsigned char tag[16];
    if (EVP_CIPHER_CTX_ctrl(impl_->ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) {
        throw std::runtime_error("Could not get tag");
    }
    out.insert(out.end(), tag, tag + 16);  // [... || tag]
}

// ----------------------------- DecryptStream -------------------------------
struct DecryptStream::Impl {
    EVP_CIPHER_CTX* ctx = nullptr;
    std::vector<uint8_t> key_bytes;
    std::vector<uint8_t> iv_buf;   // accumulates the leading 12-byte IV
    std::vector<uint8_t> tail;     // rolling last <=16 bytes (the trailing tag)
    bool initialized = false;
    bool any_input = false;
};

DecryptStream::DecryptStream(const std::string& key) : impl_(std::make_unique<Impl>()) {
    impl_->key_bytes = parse_aes256_key(key);  // validated up front
}

DecryptStream::~DecryptStream() {
    if (impl_ && impl_->ctx) EVP_CIPHER_CTX_free(impl_->ctx);
}

void DecryptStream::update(const uint8_t* data, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    if (n == 0) return;
    impl_->any_input = true;
    const uint8_t* p = data;
    size_t rem = n;

    // 1) Consume the 12-byte IV prefix, then init the cipher.
    if (!impl_->initialized) {
        size_t need = 12 - impl_->iv_buf.size();
        size_t take = std::min(need, rem);
        impl_->iv_buf.insert(impl_->iv_buf.end(), p, p + take);
        p += take;
        rem -= take;
        if (impl_->iv_buf.size() < 12) return;  // need more bytes for the IV
        impl_->ctx = EVP_CIPHER_CTX_new();
        if (!impl_->ctx ||
            EVP_DecryptInit_ex(impl_->ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
            EVP_DecryptInit_ex(impl_->ctx, NULL, NULL, impl_->key_bytes.data(), impl_->iv_buf.data()) != 1) {
            throw std::runtime_error("Could not initialize decryption");
        }
        impl_->initialized = true;
    }
    if (rem == 0) return;

    // 2) Feed ciphertext, always holding back the last 16 bytes (the tag).
    impl_->tail.insert(impl_->tail.end(), p, p + rem);
    if (impl_->tail.size() > 16) {
        size_t feed = impl_->tail.size() - 16;
        size_t base = out.size();
        out.resize(base + feed);
        int len = 0;
        if (EVP_DecryptUpdate(impl_->ctx, out.data() + base, &len, impl_->tail.data(),
                              static_cast<int>(feed)) != 1) {
            throw std::runtime_error("Could not decrypt data");
        }
        out.resize(base + static_cast<size_t>(len));
        impl_->tail.erase(impl_->tail.begin(), impl_->tail.begin() + feed);
    }
}

void DecryptStream::finish(std::vector<uint8_t>& out) {
    out.clear();
    if (!impl_->any_input) return;  // empty blob -> empty content (one-shot parity)
    if (!impl_->initialized || impl_->tail.size() != 16) {
        throw std::runtime_error("Encrypted stream truncated");
    }
    if (EVP_CIPHER_CTX_ctrl(impl_->ctx, EVP_CTRL_GCM_SET_TAG, 16, impl_->tail.data()) != 1) {
        throw std::runtime_error("Could not set tag");
    }
    unsigned char fin[16];
    int len = 0;
    if (EVP_DecryptFinal_ex(impl_->ctx, fin, &len) <= 0) {
        throw std::runtime_error("Could not finalize decryption - tag verification failed");
    }
    if (len > 0) out.insert(out.end(), fin, fin + len);  // GCM final emits 0 bytes
}

// Compression functions
std::vector<uint8_t> CryptoUtils::compress_data(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return std::vector<uint8_t>();
    }

    z_stream zs;
    memset(&zs, 0, sizeof(zs));

    if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK) {
        throw std::runtime_error("deflateInit failed");
    }

    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());

    int ret;
    std::vector<uint8_t> outbuffer(32768);
    std::vector<uint8_t> compressed;

    do {
        zs.next_out = outbuffer.data();
        zs.avail_out = static_cast<uInt>(outbuffer.size());

        ret = deflate(&zs, Z_SYNC_FLUSH);

        if (zs.total_out > compressed.size()) {
            size_t new_bytes = zs.total_out - compressed.size();
            compressed.insert(compressed.end(), outbuffer.begin(), outbuffer.begin() + new_bytes);
        }
    } while (ret == Z_OK);

    // Finish the compression
    int finish_ret;
    do {
        zs.next_out = outbuffer.data();
        zs.avail_out = static_cast<uInt>(outbuffer.size());

        finish_ret = deflate(&zs, Z_FINISH);

        if (zs.total_out > compressed.size()) {
            size_t new_bytes = zs.total_out - compressed.size();
            compressed.insert(compressed.end(), outbuffer.begin(), outbuffer.begin() + new_bytes);
        }
    } while (finish_ret == Z_OK);

    deflateEnd(&zs);

    if (finish_ret != Z_STREAM_END) {
        throw std::runtime_error("Exception during zlib compression");
    }

    return compressed;
}

std::vector<uint8_t> CryptoUtils::decompress_data(const std::vector<uint8_t>& compressed_data) {
    if (compressed_data.empty()) {
        return std::vector<uint8_t>();
    }

    z_stream zs;
    memset(&zs, 0, sizeof(zs));

    if (inflateInit(&zs) != Z_OK) {
        throw std::runtime_error("inflateInit failed");
    }

    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(compressed_data.data()));
    zs.avail_in = static_cast<uInt>(compressed_data.size());

    int ret;
    std::vector<uint8_t> outbuffer(32768);
    std::vector<uint8_t> decompressed;

    do {
        zs.next_out = outbuffer.data();
        zs.avail_out = static_cast<uInt>(outbuffer.size());

        ret = inflate(&zs, Z_SYNC_FLUSH);

        if (zs.total_out > decompressed.size()) {
            size_t new_bytes = zs.total_out - decompressed.size();
            decompressed.insert(decompressed.end(), outbuffer.begin(), outbuffer.begin() + new_bytes);
        }
    } while (ret == Z_OK && zs.avail_in > 0);

    inflateEnd(&zs);

    if (ret != Z_STREAM_END) {
        throw std::runtime_error("Exception during zlib decompression");
    }

    return decompressed;
}

// Encryption functions
std::vector<uint8_t> CryptoUtils::encrypt_data(const std::vector<uint8_t>& data, const std::string& key) {
    if (data.empty()) {
        return std::vector<uint8_t>();
    }

    // Convert key to bytes - first try hex, then base64
    std::vector<uint8_t> key_bytes;
    if (key.length() == 64) { // Hex string of 32 bytes = 64 hex chars
        key_bytes = hex_string_to_bytes(key);
    } else {
        // Assume it's base64 encoded
        key_bytes = base64_decode(key);
    }

    if (key_bytes.size() != 32) { // AES-256 requires 32-byte key
        throw std::runtime_error("Invalid key length for AES-256, expected 32 bytes, got " + std::to_string(key_bytes.size()));
    }

    // Create context
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Could not create cipher context");
    }

    // Initialize cipher
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Could not initialize cipher");
    }

    // Set key and IV
    unsigned char iv[12]; // 96-bit IV for GCM
    if (RAND_bytes(iv, 12) <= 0) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Could not generate random IV");
    }

    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key_bytes.data(), iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Could not set key and IV");
    }

    // Encrypt the data
    std::vector<uint8_t> ciphertext(data.size() + AES_BLOCK_SIZE);
    int len;
    int ciphertext_len;

    if (EVP_EncryptUpdate(ctx, ciphertext.data(), &len, data.data(), data.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Could not encrypt data");
    }
    ciphertext_len = len;

    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Could not finalize encryption");
    }
    ciphertext_len += len;

    // Get the tag
    unsigned char tag[16];
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Could not get tag");
    }

    // Clean up
    EVP_CIPHER_CTX_free(ctx);

    // Prepare result: IV + ciphertext + tag
    std::vector<uint8_t> result;
    result.insert(result.end(), iv, iv + 12);         // 12-byte IV
    result.insert(result.end(), ciphertext.begin(), ciphertext.begin() + ciphertext_len);  // ciphertext
    result.insert(result.end(), tag, tag + 16);       // 16-byte tag

    return result;
}

std::vector<uint8_t> CryptoUtils::decrypt_data(const std::vector<uint8_t>& encrypted_data, const std::string& key) {
    // Empty plaintext encrypts to an empty blob (see encrypt_data), and a null
    // payload can also reach the store from a 0-byte upload. Treat an empty
    // stored blob as empty content rather than failing the read. Only a
    // non-empty-but-truncated blob (1..27 bytes) is genuinely corrupt.
    if (encrypted_data.empty()) {
        return std::vector<uint8_t>();
    }
    if (encrypted_data.size() < 12 + 16) { // At least IV + tag
        throw std::runtime_error("Encrypted data too short");
    }

    // Convert key to bytes - first try hex, then base64
    std::vector<uint8_t> key_bytes;
    if (key.length() == 64) { // Hex string of 32 bytes = 64 hex chars
        key_bytes = hex_string_to_bytes(key);
    } else {
        // Assume it's base64 encoded
        key_bytes = base64_decode(key);
    }

    if (key_bytes.size() != 32) { // AES-256 requires 32-byte key
        throw std::runtime_error("Invalid key length for AES-256, expected 32 bytes, got " + std::to_string(key_bytes.size()));
    }

    // Extract IV, ciphertext, and tag
    std::vector<uint8_t> iv(encrypted_data.begin(), encrypted_data.begin() + 12);
    std::vector<uint8_t> tag(encrypted_data.end() - 16, encrypted_data.end());
    std::vector<uint8_t> ciphertext(encrypted_data.begin() + 12, encrypted_data.end() - 16);

    // Create context
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Could not create cipher context");
    }

    // Initialize cipher
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Could not initialize cipher");
    }

    // Set key and IV
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key_bytes.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Could not set key and IV");
    }

    // Decrypt the data
    std::vector<uint8_t> plaintext(ciphertext.size() + AES_BLOCK_SIZE);
    int len;
    int plaintext_len;

    if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext.data(), ciphertext.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Could not decrypt data");
    }
    plaintext_len = len;

    // Set expected tag
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Could not set tag");
    }

    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len) <= 0) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Could not finalize decryption - tag verification failed");
    }
    plaintext_len += len;

    // Clean up
    EVP_CIPHER_CTX_free(ctx);

    // Resize to actual plaintext length
    plaintext.resize(plaintext_len);

    return plaintext;
}

std::vector<uint8_t> CryptoUtils::hex_string_to_bytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    
    for (unsigned int i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(strtol(byteString.c_str(), NULL, 16));
        bytes.push_back(byte);
    }
    
    return bytes;
}

std::string CryptoUtils::bytes_to_hex_string(const std::vector<uint8_t>& bytes) {
    std::string result;

    for (uint8_t byte : bytes) {
        char buf[3];
        snprintf(buf, 3, "%02x", byte);
        result += buf;
    }

    return result;
}

std::vector<uint8_t> CryptoUtils::base64_decode(const std::string& input) {
    const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> result;
    int val = 0, valb = -8;

    for (char c : input) {
        if (c == '=') break; // Stop at padding

        int pos = chars.find(c);
        if (pos == std::string::npos) break; // Invalid character

        val = (val << 6) + pos;
        valb += 6;

        if (valb >= 0) {
            result.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }

    return result;
}

} // namespace fileengine