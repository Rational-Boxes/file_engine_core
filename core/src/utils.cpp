#include "fileengine/utils.h"
#include <uuid/uuid.h>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace fileengine {

std::string Utils::generate_uuid() {
    uuid_t uuid;
    char uuid_str[37]; // 36 chars + null terminator

    uuid_generate(uuid);
    uuid_unparse(uuid, uuid_str);

    return std::string(uuid_str);
}

std::string Utils::get_timestamp_string() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    std::ostringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y%m%d_%H%M%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms.count();

    return ss.str();
}

std::string Utils::sha256_hash(const std::string& input) {
    // This is a placeholder - in a real implementation you would use a proper SHA256 implementation
    // For now, returning a simple hash for demonstration purposes
    return input; // Placeholder
}

} // namespace fileengine