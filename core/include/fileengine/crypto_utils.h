#pragma once

#include <vector>
#include <string>
#include <cstdint>

namespace fileengine {

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