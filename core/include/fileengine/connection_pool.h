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

#include "types.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <libpq-fe.h>

namespace fileengine {

class DatabaseConnection {
public:
    explicit DatabaseConnection(const std::string& conninfo);
    ~DatabaseConnection();

    PGconn* get_connection() { return conn_; }
    bool is_valid() const { return conn_ && PQstatus(conn_) == CONNECTION_OK; }

private:
    PGconn* conn_;
};

// A point-in-time picture of the pool, for the monitoring listener.
//
// `in_use` is the number a caller is holding right now, and it is the number to
// watch: a healthy server returns to 0 once traffic stops. A value that only ever
// climbs is a leaked connection — some path acquired one and returned without
// releasing it — and since every acquire waits for a free connection, a pool that
// leaks its way to empty stops the server dead.
struct ConnectionPoolStats {
    int  size          = 0;   // connections the pool was built with
    int  available     = 0;   // idle, ready to hand out
    int  in_use        = 0;   // held by a caller right now
    int  waiters       = 0;   // callers blocked waiting for one
    std::uint64_t acquired_total   = 0;
    std::uint64_t released_total   = 0;
    std::uint64_t wait_events      = 0;  // acquires that had to block at all
    std::uint64_t wait_timeouts    = 0;  // acquires that gave up (see acquire())
    std::uint64_t replaced_total   = 0;  // dead connections rebuilt on release
    std::uint64_t longest_wait_ms  = 0;  // worst single wait since start
    bool shutting_down = false;
};

class ConnectionPool {
public:
    ConnectionPool(const std::string& host, int port, const std::string& dbname,
                   const std::string& user, const std::string& password, int pool_size = 10);
    ~ConnectionPool();

    // Blocks until a connection is free, the pool shuts down, or the wait budget
    // is exhausted; nullptr on the latter two. Returning nullptr instead of
    // waiting forever is deliberate — see the note in the .cpp.
    std::shared_ptr<DatabaseConnection> acquire();
    void release(std::shared_ptr<DatabaseConnection> conn);

    bool initialize();
    void shutdown();

    // Connection information access
    std::string get_connection_info() const { return connection_info_; }

    // Cheap enough to call from a monitoring endpoint: one mutex acquisition and
    // a few atomic loads.
    ConnectionPoolStats stats() const;

    // How long acquire() will wait before giving up. 0 restores the old
    // wait-forever behaviour (not recommended — a leak then presents as a hang).
    void set_acquire_timeout(std::chrono::milliseconds t) { acquire_timeout_ = t; }

private:
    std::string connection_info_;
    int pool_size_;

    std::queue<std::shared_ptr<DatabaseConnection>> available_connections_;
    mutable std::mutex pool_mutex_;
    std::condition_variable pool_cv_;
    bool shutdown_flag_;

    // --- telemetry (§ pool lifecycle) -----------------------------------
    std::atomic<int>           in_use_{0};
    std::atomic<int>           waiters_{0};
    std::atomic<std::uint64_t> acquired_total_{0};
    std::atomic<std::uint64_t> released_total_{0};
    std::atomic<std::uint64_t> wait_events_{0};
    std::atomic<std::uint64_t> wait_timeouts_{0};
    std::atomic<std::uint64_t> replaced_total_{0};
    std::atomic<std::uint64_t> longest_wait_ms_{0};

    std::chrono::milliseconds acquire_timeout_{std::chrono::seconds(30)};
};

} // namespace fileengine