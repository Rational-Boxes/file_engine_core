#include "logger.h"
#include <sys/stat.h>
#include <algorithm>
#include <thread>

namespace fileengine {

Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

void Logger::initialize(const std::string& log_level, const std::string& log_file_path, 
                       bool log_to_console, bool log_to_file, 
                       size_t rotation_size_mb, int retention_days) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    
    // Parse log level
    std::string upper_level = log_level;
    std::transform(upper_level.begin(), upper_level.end(), upper_level.begin(), ::toupper);
    
    if (upper_level == "DEBUG") {
        current_level_ = LogLevel::DEBUG;
    } else if (upper_level == "INFO") {
        current_level_ = LogLevel::INFO;
    } else if (upper_level == "WARN") {
        current_level_ = LogLevel::WARN;
    } else if (upper_level == "ERROR") {
        current_level_ = LogLevel::ERROR;
    } else if (upper_level == "FATAL") {
        current_level_ = LogLevel::FATAL;
    } else {
        current_level_ = LogLevel::INFO; // default
    }

    log_to_console_ = log_to_console;
    log_to_file_ = log_to_file;
    log_file_path_ = log_file_path;
    rotation_size_mb_ = rotation_size_mb;
    retention_days_ = retention_days;

    // Open log file if logging to file
    if (log_to_file_) {
        log_file_.open(log_file_path_, std::ios::app);
        if (log_file_.is_open()) {
            // Get current file size
            log_file_.seekp(0, std::ios::end);
            current_size_ = static_cast<size_t>(log_file_.tellp());
        }
    }

    initialized_ = true;
}

void Logger::log(LogLevel level, const std::string& component, const std::string& message) {
    if (!shouldLog(level) && !initialized_) {
        return;
    }

    std::lock_guard<std::mutex> lock(log_mutex_);
    
    std::string formatted_message = "[" + getCurrentTimestamp() + "] " +
                                   "[" + levelToString(level) + "] " +
                                   "[" + component + "] " + message;

    // Log to console if enabled
    if (log_to_console_) {
        if (level >= LogLevel::ERROR) {
            std::cerr << formatted_message << std::endl;
        } else {
            std::cout << formatted_message << std::endl;
        }
    }

    // Log to file if enabled
    if (log_to_file_ && log_file_.is_open()) {
        log_file_ << formatted_message << std::endl;
        log_file_.flush();
        
        // Update current size
        current_size_ += formatted_message.length() + 1; // +1 for newline
        
        // Check if rotation is needed
        if (current_size_ >= rotation_size_mb_ * 1024 * 1024) { // Convert MB to bytes
            log_file_.close();
            rotate_log_file();
            log_file_.open(log_file_path_, std::ios::app);
            if (log_file_.is_open()) {
                current_size_ = 0;
            }
        }
    }
}

void Logger::rotate_log_file() {
    // Simple rotation by renaming the current file with a timestamp
    std::string rotated_name = log_file_path_ + "." + getCurrentTimestamp();
    
    // Close and rename the current log file
    if (std::rename(log_file_path_.c_str(), rotated_name.c_str()) != 0) {
        // If renaming fails, just continue with a new file
        std::cout << "Warning: Could not rotate log file: " << log_file_path_ << std::endl;
    }
}

void Logger::debug(const std::string& component, const std::string& message) {
    // In debug mode, capture additional context information for more detailed logging
    if (current_level_ <= LogLevel::DEBUG) {
        #ifdef __linux__
        // Include thread ID, timestamp with microseconds, and memory usage for detailed debugging
        std::stringstream ss;
        ss << "[thread:" << std::this_thread::get_id()
           << "] " << message;
        log(LogLevel::DEBUG, component, ss.str());
        #else
        log(LogLevel::DEBUG, component, message);
        #endif
    }
}

std::string Logger::detailed_log_prefix() {
    if (current_level_ > LogLevel::DEBUG) {
        return "";
    }

    std::stringstream ss;
    #ifdef __linux__
    ss << "[thread:" << std::this_thread::get_id() << "] ";
    #endif

    return ss.str();
}

void Logger::info(const std::string& component, const std::string& message) {
    log(LogLevel::INFO, component, message);
}

void Logger::warn(const std::string& component, const std::string& message) {
    log(LogLevel::WARN, component, message);
}

void Logger::error(const std::string& component, const std::string& message) {
    log(LogLevel::ERROR, component, message);
}

void Logger::fatal(const std::string& component, const std::string& message) {
    log(LogLevel::FATAL, component, message);
}

Logger::~Logger() {
    if (log_file_.is_open()) {
        log_file_.close();
    }
}

std::string Logger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO: return "INFO";
        case LogLevel::WARN: return "WARN";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
        default: return "UNKNOWN";
    }
}

std::string Logger::getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

bool Logger::shouldLog(LogLevel level) const {
    return level >= current_level_;
}

} // namespace fileengine