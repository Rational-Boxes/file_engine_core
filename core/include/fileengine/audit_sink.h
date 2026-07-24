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

#pragma once

#include "audit_entry.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

struct redisContext;  // forward-declared; hiredis is pulled in only by the .cpp

namespace fileengine {

// The durable audit sink. Unlike IEventSink (fail-open, may drop), publish()
// returns whether the entry was DURABLY captured, so fail-closed callers can
// reject the guarded op when it was not (usage_logging_and_auditing.md §6).
class IAuditSink {
public:
    virtual ~IAuditSink() = default;
    // Returns true iff the entry is durably captured (WAL-committed). The sink
    // fills event_id/ts when empty, hence by-value.
    virtual bool publish(AuditEntry entry) = 0;
    virtual void start() {}
    virtual void stop() {}
};

// Disabled/unavailable sink: pretends every entry is durable so callers need no
// null-checks and auditing being off never blocks an operation.
class NullAuditSink : public IAuditSink {
public:
    bool publish(AuditEntry) override { return true; }
};

// WAL-backed durable sink over a Redis Stream. publish() appends the serialized
// envelope to an append-only, fsync'd local WAL (that IS the durability point,
// so it returns true the moment the WAL commit succeeds) and hands the line to a
// worker thread that XADDs it to the aggregating audit stream. On XADD failure
// the worker retries — nothing is dropped; the WAL holds it. When the backlog
// fully drains the WAL is truncated. On start() any un-truncated WAL is replayed
// (re-delivered; the consumer's (event_id, ts) key absorbs the duplicates), so a
// crash never loses a durably-published entry.
class RedisAuditSink : public IAuditSink {
public:
    struct Options {
        std::string host = "localhost";
        int         port = 6379;
        std::string password;
        int         db = 0;
        std::string stream = "fileengine:audit";
        long long   stream_maxlen = 1000000;
        std::string wal_path = "audit.wal";
        int         retry_ms = 500;
    };

    explicit RedisAuditSink(Options options);
    ~RedisAuditSink() override;

    bool publish(AuditEntry entry) override;
    void start() override;
    void stop() override;

    std::uint64_t delivered() const { return delivered_.load(); }
    std::size_t   buffered();

private:
    void run();
    bool wal_append(const std::string& line);  // append + fsync; false on I/O error
    void wal_truncate();                        // drop the fully-delivered backlog
    void wal_recover();                         // reload un-truncated lines into pending_
    void ensure_connected();                    // throws on failure
    void disconnect();
    bool xadd(const std::string& line);         // deliver one line; false on failure

    Options opts_;
    int wal_fd_ = -1;
    std::deque<std::string> pending_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    redisContext* ctx_ = nullptr;
    std::atomic<std::uint64_t> delivered_{0};
};

} // namespace fileengine
