#ifndef FILEENGINE_LOGGER_H
#define FILEENGINE_LOGGER_H

#include <string>
#include <fstream>
#include <sstream>
#include <mutex>
#include <memory>
#include <iostream>
#include <chrono>
#include <iomanip>

namespace fileengine {

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3,
    FATAL = 4
};

class Logger {
public:
    static Logger& getInstance();

    void initialize(const std::string& log_level, const std::string& log_file_path, 
                   bool log_to_console, bool log_to_file, 
                   size_t rotation_size_mb = 10, int retention_days = 7);

    void log(LogLevel level, const std::string& component, const std::string& message);

    void debug(const std::string& component, const std::string& message);
    void info(const std::string& component, const std::string& message);
    void warn(const std::string& component, const std::string& message);
    void error(const std::string& component, const std::string& message);
    void fatal(const std::string& component, const std::string& message);

    // Helper method for detailed logging prefixes
    std::string detailed_log_prefix();

private:
    Logger() = default;
    ~Logger();

    std::string levelToString(LogLevel level);
    std::string getCurrentTimestamp();
    bool shouldLog(LogLevel level) const;
    void rotate_log_file();

    std::ofstream log_file_;
    LogLevel current_level_{LogLevel::INFO};
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
#define LOG_DEBUG(component, msg) fileengine::Logger::getInstance().debug(component, msg)
#define LOG_INFO(component, msg) fileengine::Logger::getInstance().info(component, msg)
#define LOG_WARN(component, msg) fileengine::Logger::getInstance().warn(component, msg)
#define LOG_ERROR(component, msg) fileengine::Logger::getInstance().error(component, msg)
#define LOG_FATAL(component, msg) fileengine::Logger::getInstance().fatal(component, msg)

} // namespace fileengine

#endif // FILEENGINE_LOGGER_H