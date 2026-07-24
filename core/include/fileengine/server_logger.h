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

#ifndef FILEENGINE_SERVER_LOGGER_H
#define FILEENGINE_SERVER_LOGGER_H

#include <string>
#include <fstream>
#include <sstream>
#include <mutex>
#include <memory>
#include <iostream>
#include <chrono>
#include <iomanip>

namespace fileengine {

enum class ServerLogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3,
    FATAL = 4
};

class ServerLogger {
public:
    static ServerLogger& getInstance();

    void initialize(const std::string& log_level, const std::string& log_file_path,
                   bool log_to_console, bool log_to_file,
                   size_t rotation_size_mb = 10, int retention_days = 7);

    void log(ServerLogLevel level, const std::string& component, const std::string& message);

    void debug(const std::string& component, const std::string& message);
    void info(const std::string& component, const std::string& message);
    void warn(const std::string& component, const std::string& message);
    void error(const std::string& component, const std::string& message);
    void fatal(const std::string& component, const std::string& message);

    // Helper method for detailed logging prefixes
    std::string detailed_log_prefix();

private:
    ServerLogger() = default;
    ~ServerLogger();

    std::string levelToString(ServerLogLevel level);
    std::string getCurrentTimestamp();
    bool shouldLog(ServerLogLevel level) const;
    void rotate_log_file();
    uint64_t get_thread_id() const;

    std::ofstream log_file_;
    ServerLogLevel current_level_{ServerLogLevel::INFO};
    bool log_to_console_{true};
    bool log_to_file_{false};
    std::string log_file_path_;
    size_t rotation_size_mb_{10};
    int retention_days_{7};
    size_t current_size_{0};

    std::mutex log_mutex_;
    bool initialized_{false};
};

// Macro for easy logging
#define SERVER_LOG_DEBUG(component, msg) fileengine::ServerLogger::getInstance().debug(component, msg)
#define SERVER_LOG_INFO(component, msg) fileengine::ServerLogger::getInstance().info(component, msg)
#define SERVER_LOG_WARN(component, msg) fileengine::ServerLogger::getInstance().warn(component, msg)
#define SERVER_LOG_ERROR(component, msg) fileengine::ServerLogger::getInstance().error(component, msg)
#define SERVER_LOG_FATAL(component, msg) fileengine::ServerLogger::getInstance().fatal(component, msg)

} // namespace fileengine

#endif // FILEENGINE_SERVER_LOGGER_H