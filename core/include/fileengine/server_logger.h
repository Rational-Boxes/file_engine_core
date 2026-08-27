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
#include <atomic>
#include <cstdint>

namespace fileengine {

enum class ServerLogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3,
    FATAL = 4,

    // Above FATAL, and deliberately not reachable from FILEENGINE_LOG_LEVEL.
    //
    // This is the operational signal of the accountability guarantee: a
    // permission change refused because its record could not be written, a
    // chain-integrity failure, a rejected attempt to read the security log.
    // PROPOSAL_accountability_record.md §4.2 guarantee 3 says no configuration
    // disables the record; the same has to be true of its alarm, or an operator
    // can silence the one signal that says the guarantee is not holding — and
    // silencing it looks exactly like nothing being wrong.
    //
    // shouldLog() always returns true for this level. Reserve it for security
    // events; it is not a louder ERROR.
    SECURITY = 5
};

class ServerLogger {
public:
    static ServerLogger& getInstance();

    void initialize(const std::string& log_level, const std::string& log_file_path,
                   bool log_to_console, bool log_to_file,
                   size_t rotation_size_mb = 10, int retention_days = 7,
                   bool redact_names = true);

    void log(ServerLogLevel level, const std::string& component, const std::string& message);

    void debug(const std::string& component, const std::string& message);
    void info(const std::string& component, const std::string& message);
    void warn(const std::string& component, const std::string& message);
    void error(const std::string& component, const std::string& message);
    void fatal(const std::string& component, const std::string& message);

    // The unsuppressable channel (ServerLogLevel::SECURITY). Emitted at any
    // configured level, to console AND file when file logging is on, and
    // flushed immediately — a security event that was buffered when the process
    // died was not recorded.
    void security(const std::string& component, const std::string& message);

    // How many security events this process has emitted, for the monitoring
    // endpoint. A log line nobody greps is not an alarm; a counter is scrapeable.
    std::uint64_t security_event_count() const { return security_events_.load(); }

    // ── Identifiers, never payload ──────────────────────────────────────────
    // The audit chain was just stopped from storing filenames, because they are
    // party data and that log is immutable
    // (PROPOSAL_accountability_record.md §5.4.7). The operational log has the
    // same problem for a different reason: it prints names on nearly every RPC,
    // and it is rotated, shipped and archived — so an erasure obligation cannot
    // reach what it already copied out.
    //
    // redact() replaces a name with a short, stable, non-reversible tag:
    //
    //     redact("Acme_Corp_Contract_J_Smith.pdf")  ->  "name#3f9c1a2e"
    //
    // Stable, so the same name correlates across lines and a support engineer
    // can still follow one file through a trace. Non-reversible, so the log
    // never holds the name itself. An empty string stays empty (nothing to
    // disclose), and the tag is derived from a per-process salt so tags cannot
    // be matched against a precomputed dictionary of likely filenames.
    //
    // Disable with FILEENGINE_LOG_REDACT_NAMES=false when debugging something
    // that genuinely needs the literal name. Default ON: the cheapest way to
    // survive an erasure request is to have nothing to erase.
    std::string redact(const std::string& value) const;
    bool redact_names() const { return redact_names_; }

    // Helper method for detailed logging prefixes
    std::string detailed_log_prefix();

private:
    ServerLogger() = default;
    ~ServerLogger();

    std::string levelToString(ServerLogLevel level);
    std::string getCurrentTimestamp();
    bool shouldLog(ServerLogLevel level) const;

    // Roll the file when it passes rotation_size_mb and prune rolls older than
    // retention_days. Callers hold log_mutex_.
    void rotate_log_file();
    void prune_old_rolls();
    uint64_t get_thread_id() const;

    std::ofstream log_file_;
    ServerLogLevel current_level_{ServerLogLevel::INFO};
    bool log_to_console_{true};
    bool log_to_file_{false};
    std::string log_file_path_;
    size_t rotation_size_mb_{10};
    int retention_days_{7};
    size_t current_size_{0};

    bool redact_names_{true};
    std::string redaction_salt_;               // per-process; makes tags undictionaryable
    std::atomic<std::uint64_t> security_events_{0};

    std::mutex log_mutex_;
    bool initialized_{false};
};

// Macro for easy logging
#define SERVER_LOG_DEBUG(component, msg) fileengine::ServerLogger::getInstance().debug(component, msg)
#define SERVER_LOG_INFO(component, msg) fileengine::ServerLogger::getInstance().info(component, msg)
#define SERVER_LOG_WARN(component, msg) fileengine::ServerLogger::getInstance().warn(component, msg)
#define SERVER_LOG_ERROR(component, msg) fileengine::ServerLogger::getInstance().error(component, msg)
#define SERVER_LOG_FATAL(component, msg) fileengine::ServerLogger::getInstance().fatal(component, msg)
// The unsuppressable channel. Use it ONLY for security events — a refused
// accountability write, a chain-integrity failure, a rejected read of the
// security log. Everything else has a level.
#define SERVER_LOG_SECURITY(component, msg) fileengine::ServerLogger::getInstance().security(component, msg)
// Wrap any name, path or other party data that reaches a log line.
#define SERVER_LOG_REDACT(value) fileengine::ServerLogger::getInstance().redact(value)

} // namespace fileengine

#endif // FILEENGINE_SERVER_LOGGER_H