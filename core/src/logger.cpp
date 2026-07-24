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

#include "fileengine/server_logger.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <thread>
#include <chrono>
#include <cstdarg>

namespace fileengine {

Logger& ServerLogger::getInstance() {
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
        if (!log_file_.is_open()) {
            // If we can't open the log file, disable file logging but continue
            log_to_file_ = false;
            std::cout << "Warning: Could not open log file " << log_file_path_ << ". File logging disabled." << std::endl;
        }
    }

    initialized_ = true;
}

void Logger::log(LogLevel level, const std::string& component, const std::string& message) {
    if (!shouldLog(level)) {
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
    }
}

void Logger::rotate_log_file() {
    // For this minimal implementation, we'll just close and reopen the file
    if (log_file_.is_open()) {
        log_file_.close();
        log_file_.open(log_file_path_, std::ios::app);
    }
}

void Logger::debug(const std::string& component, const std::string& message) {
    log(LogLevel::DEBUG, component, message);
}

std::string Logger::detailed_log_prefix() {
    std::ostringstream prefix;
    prefix << "[thread:" << std::this_thread::get_id() << "] ";
    return prefix.str();
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

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

bool Logger::shouldLog(LogLevel level) const {
    return level >= current_level_;
}

uint64_t Logger::get_thread_id() const {
    return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

} // namespace fileengine