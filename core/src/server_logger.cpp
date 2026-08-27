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
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <system_error>
#include <vector>

#include <openssl/evp.h>

namespace fileengine {

ServerLogger& ServerLogger::getInstance() {
    static ServerLogger instance;
    return instance;
}

void ServerLogger::initialize(const std::string& log_level, const std::string& log_file_path,
                       bool log_to_console, bool log_to_file,
                       size_t rotation_size_mb, int retention_days,
                       bool redact_names) {
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
    redact_names_ = redact_names;

    // A fresh salt per process. Tags stay stable for the life of a run — which
    // is what makes them correlatable within a trace — but cannot be matched
    // against a dictionary of likely filenames precomputed by anyone who gets
    // hold of the logs, and cannot be joined across runs to rebuild a corpus.
    if (redaction_salt_.empty()) {
        std::random_device rd;
        std::ostringstream salt;
        for (int i = 0; i < 4; ++i) salt << std::hex << rd();
        redaction_salt_ = salt.str();
    }

    // Close any file from a previous initialize() FIRST.
    //
    // Without this, opening an already-open ofstream fails and sets failbit
    // while is_open() stays true — so a re-initialization silently keeps
    // writing to the OLD path. Every subsequent line lands in a file the
    // operator believes they moved away from, and nothing reports an error.
    if (log_file_.is_open()) {
        log_file_.flush();
        log_file_.close();
    }
    log_file_.clear();          // drop any failbit from a prior open

    // Open log file if logging to file
    if (log_to_file_) {
        log_file_.open(log_file_path_, std::ios::app);
        if (!log_file_.is_open()) {
            // If we can't open the log file, disable file logging but continue
            log_to_file_ = false;
            std::cout << "Warning: Could not open log file " << log_file_path_ << ". File logging disabled." << std::endl;
        } else {
            // Adopt the existing file's size so an append to an already-large
            // log rotates on the next write rather than after another full
            // rotation_size_mb — otherwise a restarting process can double the
            // intended ceiling every time it comes up.
            std::error_code ec;
            const auto existing = std::filesystem::file_size(log_file_path_, ec);
            current_size_ = ec ? 0 : static_cast<size_t>(existing);
            prune_old_rolls();
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

    // Log to console if enabled. A SECURITY entry goes to the console even when
    // console logging is off: in a container the console IS the log shipper, and
    // dropping it there would leave the event only in a file nobody collects.
    if (log_to_console_ || level == ServerLogLevel::SECURITY) {
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
        current_size_ += formatted_message.size() + 1;
        // Rotation is checked on every write, and it is checked HERE rather
        // than on a timer because an unbounded log is a silent failure of the
        // same family as a dropped record: the disk fills, the logger stops
        // being able to write, and the process that notices is the one that
        // crashes. It matters more now that this file carries the SECURITY
        // channel — the one signal that must survive.
        if (rotation_size_mb_ > 0 &&
            current_size_ >= rotation_size_mb_ * 1024ULL * 1024ULL) {
            rotate_log_file();
        }
    }
}

void ServerLogger::security(const std::string& component, const std::string& message) {
    security_events_.fetch_add(1, std::memory_order_relaxed);
    log(ServerLogLevel::SECURITY, component, message);
}

// Caller holds log_mutex_.
void ServerLogger::rotate_log_file() {
    if (!log_file_.is_open()) return;
    log_file_.close();

    // Roll to a timestamped name rather than a rolling .1/.2 shuffle: renaming
    // N files on every rotation is N chances to lose one, and a timestamped
    // name is what makes retention answerable without parsing an index.
    std::ostringstream rolled;
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    rolled << log_file_path_ << "." << std::put_time(&tm_buf, "%Y%m%d-%H%M%S");

    std::error_code ec;
    std::filesystem::rename(log_file_path_, rolled.str(), ec);
    if (ec) {
        // Could not roll — reopen the original and keep writing. Losing lines
        // because rotation failed would be worse than an oversized file.
        log_file_.open(log_file_path_, std::ios::app);
        return;
    }

    log_file_.open(log_file_path_, std::ios::trunc);
    current_size_ = 0;
    prune_old_rolls();
}

// Caller holds log_mutex_.
void ServerLogger::prune_old_rolls() {
    if (retention_days_ <= 0) return;   // 0 or negative = keep everything

    std::error_code ec;
    const std::filesystem::path active(log_file_path_);
    const auto dir = active.has_parent_path() ? active.parent_path()
                                              : std::filesystem::path(".");
    const std::string prefix = active.filename().string() + ".";

    const auto cutoff = std::filesystem::file_time_type::clock::now() -
                        std::chrono::hours(24 * retention_days_);

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) return;
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0) continue;      // not one of our rolls
        std::error_code stat_ec;
        const auto written = std::filesystem::last_write_time(entry.path(), stat_ec);
        if (stat_ec || written >= cutoff) continue;
        std::error_code rm_ec;
        std::filesystem::remove(entry.path(), rm_ec);  // best-effort
    }
}

std::string ServerLogger::redact(const std::string& value) const {
    if (!redact_names_) return value;
    // Nothing to disclose, and "name#<hash of empty>" would be worse than
    // useless — it would read as a real name that happens to recur everywhere.
    if (value.empty()) return value;

    const std::string salted = redaction_salt_ + "\x1f" + value;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) return "name#?";
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
              EVP_DigestUpdate(ctx, salted.data(), salted.size()) == 1 &&
              EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok || digest_len < 4) return "name#?";

    // Four bytes is enough to follow one file through a trace and far too few
    // to be useful for anything else.
    static const char* kHex = "0123456789abcdef";
    std::string tag = "name#";
    for (unsigned int i = 0; i < 4; ++i) {
        tag.push_back(kHex[digest[i] >> 4]);
        tag.push_back(kHex[digest[i] & 0x0F]);
    }
    return tag;
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
        case ServerLogLevel::SECURITY: return "SECURITY";
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
    // SECURITY is never filtered. An operator raising the log level to quieten
    // a noisy service must not thereby silence the signal that the
    // accountability guarantee is failing — that is precisely the condition
    // where the absence of output is indistinguishable from health.
    if (level == ServerLogLevel::SECURITY) return true;
    return level >= current_level_;
}

uint64_t ServerLogger::get_thread_id() const {
    return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

} // namespace fileengine