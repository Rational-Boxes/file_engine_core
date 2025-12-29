#ifndef CLI_LOGGING_H
#define CLI_LOGGING_H

#include <iostream>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace fileengine {

enum class LogLevel {
    QUIET = 0,      // No logging
    NORMAL = 1,     // Standard output only
    VERBOSE = 2,    // Additional info
    VERY_VERBOSE = 3, // Detailed info
    EXTREMELY_VERBOSE = 4 // Maximum detail
};

class Logger {
private:
    static LogLevel current_level;

public:
    static void set_level(LogLevel level) {
        current_level = level;
    }

    static LogLevel get_level() {
        return current_level;
    }

    template<typename... Args>
    static void log(LogLevel level, const std::string& prefix, const Args&... args) {
        if (level <= current_level) {
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;

            std::stringstream ss;
            ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
            ss << '.' << std::setfill('0') << std::setw(3) << ms.count();

            std::cout << "[" << ss.str() << "] [" << prefix << "] ";
            log_impl(args...);
            std::cout << std::endl;
        }
    }

    template<typename... Args>
    static void debug(const std::string& prefix, const Args&... args) {
        log(LogLevel::VERBOSE, prefix, args...);
    }

    template<typename... Args>
    static void trace(const std::string& prefix, const Args&... args) {
        log(LogLevel::VERY_VERBOSE, prefix, args...);
    }

    template<typename... Args>
    static void detail(const std::string& prefix, const Args&... args) {
        log(LogLevel::EXTREMELY_VERBOSE, prefix, args...);
    }

private:
    template<typename T>
    static void log_impl(const T& value) {
        std::cout << value;
    }

    template<typename T, typename... Args>
    static void log_impl(const T& first, const Args&... rest) {
        std::cout << first;
        log_impl(rest...);
    }
};

// Initialize static member
LogLevel Logger::current_level = LogLevel::NORMAL;

} // namespace fileengine

#endif // CLI_LOGGING_H