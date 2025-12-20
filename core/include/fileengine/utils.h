#pragma once

#include <string>

namespace fileengine {

class Utils {
public:
    static std::string generate_uuid();
    static std::string get_timestamp_string();
    static std::string sha256_hash(const std::string& input);
};

} // namespace fileengine