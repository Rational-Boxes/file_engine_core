#include "fileengine/server_logger.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <thread>
#include <chrono>
#include <cstdarg>
#include <algorithm>

namespace fileengine {

ServerLogger& ServerLogger::getInstance() {
    static ServerLogger instance;
    return instance;
}

void ServerLogger::initialize(const std::string& log_level, const std::string& log_file_path,
                       bool log_to_console, bool log_to_file,
                       size_t rotation_size_mb, int retention_days) {
    std::lock_guard<std::mutex> lock(log_mutex_);

    // Parse log level
    std::string upper_level = log_level;
    std::transform(upper_level.begin(), upper_level.end(), upper_level.begin(), ::toupper);

    if (upper_level == "DEBUG") {
        current_level_ = ServerLogLevel::DEBUG;
    } else if (upper_level == "INFO") {
        current_level_ = ServerLogLevel::INFO;
    } else if (upper_level == "WARN") {
        current_level_ = ServerLogLevel::WARN;
    } else if (upper_level == "ERROR") {
        current_level_ = ServerLogLevel::ERROR;
    } else if (upper_level == "FATAL") {
        current_level_ = ServerLogLevel::FATAL;
    } else {
        current_level_ = ServerLogLevel::INFO; // default
    }

    log_to_console_ = log_to_console;
    log_to_file_ = log_to_file;
    log_file_path_ = log_file_path;
    rotation_size_mb_ = rotation_size_mb;
    retention_days_ = retention_days;

    // Open log file if logging to file
    if (log_to_file_) {
        log_file_.open(log_file_path_, std::ios::app);
        if (!log_file_.is_open()) {
            // If we can't open the log file, disable file logging but continue
            log_to_file_ = false;
            std::cout << "Warning: Could not open log file " << log_file_path_ << ". File logging disabled." << std::endl;
        }
    }

    initialized_ = true;
}

void ServerLogger::log(ServerLogLevel level, const std::string& component, const std::string& message) {
    if (!shouldLog(level)) {
        return;
    }

    std::lock_guard<std::mutex> lock(log_mutex_);

    std::string formatted_message = "[" + getCurrentTimestamp() + "] " +
                                   "[" + levelToString(level) + "] " +
                                   "[" + component + "] " + message;

    // Log to console if enabled
    if (log_to_console_) {
        if (level >= ServerLogLevel::ERROR) {
            std::cerr << formatted_message << std::endl;
        } else {
            std::cout << formatted_message << std::endl;
        }
    }

    // Log to file if enabled
    if (log_to_file_ && log_file_.is_open()) {
        log_file_ << formatted_message << std::endl;
        log_file_.flush();
    }
}

void ServerLogger::rotate_log_file() {
    // For this minimal implementation, we'll just close and reopen the file
    if (log_file_.is_open()) {
        log_file_.close();
        log_file_.open(log_file_path_, std::ios::app);
    }
}

void ServerLogger::debug(const std::string& component, const std::string& message) {
    log(ServerLogLevel::DEBUG, component, message);
}

std::string ServerLogger::detailed_log_prefix() {
    std::ostringstream prefix;
    prefix << "[thread:" << std::this_thread::get_id() << "] ";
    return prefix.str();
}

void ServerLogger::info(const std::string& component, const std::string& message) {
    log(ServerLogLevel::INFO, component, message);
}

void ServerLogger::warn(const std::string& component, const std::string& message) {
    log(ServerLogLevel::WARN, component, message);
}

void ServerLogger::error(const std::string& component, const std::string& message) {
    log(ServerLogLevel::ERROR, component, message);
}

void ServerLogger::fatal(const std::string& component, const std::string& message) {
    log(ServerLogLevel::FATAL, component, message);
}

ServerLogger::~ServerLogger() {
    if (log_file_.is_open()) {
        log_file_.close();
    }
}

std::string ServerLogger::levelToString(ServerLogLevel level) {
    switch (level) {
        case ServerLogLevel::DEBUG: return "DEBUG";
        case ServerLogLevel::INFO: return "INFO";
        case ServerLogLevel::WARN: return "WARN";
        case ServerLogLevel::ERROR: return "ERROR";
        case ServerLogLevel::FATAL: return "FATAL";
        default: return "UNKNOWN";
    }
}

std::string ServerLogger::getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

bool ServerLogger::shouldLog(ServerLogLevel level) const {
    return level >= current_level_;
}

uint64_t ServerLogger::get_thread_id() const {
    return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

} // namespace fileengine