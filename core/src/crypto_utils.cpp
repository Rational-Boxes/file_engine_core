#include "fileengine/crypto_utils.h"
#include <zlib.h>
#include <openssl/evp.h>
#include <openssl/aes.h>
#include <openssl/rand.h>
#include <cstring>
#include <stdexcept>
#include <iostream>

namespace fileengine {

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