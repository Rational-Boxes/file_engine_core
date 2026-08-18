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

#include "fileengine/connection_pool.h"
#include "fileengine/server_logger.h"
#include <sstream>

namespace fileengine {

DatabaseConnection::DatabaseConnection(const std::string& conninfo) {
    SERVER_LOG_DEBUG("DatabaseConnection", "Attempting to connect to database using conninfo: " + conninfo);
    conn_ = PQconnectdb(conninfo.c_str());
    if (!is_valid()) {
        std::string error_message = "Failed to connect to database: " + std::string(PQerrorMessage(conn_));
        SERVER_LOG_ERROR("DatabaseConnection", error_message);
        throw std::runtime_error(error_message);
    }
    SERVER_LOG_INFO("DatabaseConnection", "Successfully connected to database.");
}

DatabaseConnection::~DatabaseConnection() {
    if (conn_) {
        PQfinish(conn_);
    }
}

ConnectionPool::ConnectionPool(const std::string& host, int port, const std::string& dbname,
                               const std::string& user, const std::string& password, int pool_size)
    : pool_size_(pool_size), shutdown_flag_(false) {
    std::ostringstream conn_stream;
    conn_stream << "host=" << host << " port=" << port
                << " dbname=" << dbname << " user=" << user
                << " password=" << password
                << " connect_timeout=5";
    connection_info_ = conn_stream.str();
}

ConnectionPool::~ConnectionPool() {
    shutdown();
}

bool ConnectionPool::initialize() {
    SERVER_LOG_DEBUG("ConnectionPool", "Initializing connection pool with size: " + std::to_string(pool_size_));
    for (int i = 0; i < pool_size_; ++i) {
        try {
            auto conn = std::make_shared<DatabaseConnection>(connection_info_);
            available_connections_.push(conn);
            SERVER_LOG_INFO("ConnectionPool", "Successfully initialized connection #" + std::to_string(i + 1) + " for pool.");
        } catch (const std::exception& e) {
            SERVER_LOG_ERROR("ConnectionPool", "Failed to initialize database connection #" + std::to_string(i + 1) + ": " + std::string(e.what()));
            // Clear any connections that might have been successfully created before the failure
            while (!available_connections_.empty()) {
                available_connections_.pop();
            }
            return false;
        }
    }
    SERVER_LOG_INFO("ConnectionPool", "Successfully initialized all " + std::to_string(pool_size_) + " connections in the pool.");
    return true;
}

void ConnectionPool::shutdown() {
    std::unique_lock<std::mutex> lock(pool_mutex_);
    shutdown_flag_ = true;
    pool_cv_.notify_all();
    
    while (!available_connections_.empty()) {
        available_connections_.pop();
    }
}

std::shared_ptr<DatabaseConnection> ConnectionPool::acquire() {
    const auto started = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(pool_mutex_);

    bool waited = false;
    while (!shutdown_flag_ && available_connections_.empty()) {
        if (!waited) {
            waited = true;
            wait_events_.fetch_add(1);
            waiters_.fetch_add(1);
            // Exhaustion is the interesting event, so say so once per wait rather
            // than per loop. in_use at this moment IS the whole pool, by
            // definition of being here.
            SERVER_LOG_WARN("ConnectionPool",
                            "pool exhausted: all " + std::to_string(pool_size_) +
                            " connections in use, " + std::to_string(waiters_.load()) +
                            " caller(s) waiting");
        }
        // A bounded wait, for two reasons.
        //
        // A leaked connection — acquired on a path that returns without releasing
        // — permanently shrinks the pool. Waiting forever turns that into a server
        // that stops answering with no diagnosis available; giving up produces an
        // error a caller can report and a counter an operator can see.
        //
        // It also makes the wait robust against a missed notification, which this
        // pool was silently suffering: release() pushed a connection back without
        // ever notifying, so a caller that started waiting was never woken by a
        // returning connection and blocked until shutdown. That is fixed below,
        // and the timeout means a future regression degrades instead of hanging.
        if (acquire_timeout_.count() > 0) {
            if (pool_cv_.wait_for(lock, acquire_timeout_) == std::cv_status::timeout &&
                available_connections_.empty() && !shutdown_flag_) {
                waiters_.fetch_sub(1);
                wait_timeouts_.fetch_add(1);
                SERVER_LOG_ERROR("ConnectionPool",
                                 "timed out after " + std::to_string(acquire_timeout_.count()) +
                                 "ms waiting for a database connection; " +
                                 std::to_string(in_use_.load()) + " of " +
                                 std::to_string(pool_size_) + " still checked out " +
                                 "(a connection acquired but never released will look like this)");
                return nullptr;
            }
        } else {
            pool_cv_.wait(lock);
        }
    }

    if (waited) {
        waiters_.fetch_sub(1);
        const auto ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count());
        // Plain compare-exchange loop: a high-water mark, not a running total.
        std::uint64_t prev = longest_wait_ms_.load();
        while (ms > prev && !longest_wait_ms_.compare_exchange_weak(prev, ms)) {}
    }

    if (shutdown_flag_) {
        return nullptr;
    }

    auto conn = available_connections_.front();
    available_connections_.pop();
    in_use_.fetch_add(1);
    acquired_total_.fetch_add(1);
    return conn;
}

void ConnectionPool::release(std::shared_ptr<DatabaseConnection> conn) {
    if (!conn) {
        // Releasing nothing is not an error (acquire() can return nullptr), but it
        // must not be counted as a connection coming back.
        return;
    }

    if (conn->is_valid()) {
        {
            std::unique_lock<std::mutex> lock(pool_mutex_);
            if (!shutdown_flag_) {
                available_connections_.push(conn);
            }
        }
    } else {
        // Replace a dead connection so the pool does not shrink over time — an
        // outage that invalidates connections would otherwise leave the server
        // permanently short of them.
        if (!shutdown_flag_) {
            try {
                auto new_conn = std::make_shared<DatabaseConnection>(connection_info_);
                {
                    std::unique_lock<std::mutex> lock(pool_mutex_);
                    available_connections_.push(new_conn);
                }
                replaced_total_.fetch_add(1);
                SERVER_LOG_INFO("ConnectionPool",
                                "replaced a dead connection on release");
            } catch (const std::exception& e) {
                SERVER_LOG_ERROR("ConnectionPool",
                                 "could not replace a dead connection on release: " +
                                 std::string(e.what()) + " — pool is now one short");
            }
        }
    }

    in_use_.fetch_sub(1);
    released_total_.fetch_add(1);
    // THE fix: a connection coming back has to wake somebody waiting for one.
    // Without this, release() returned a connection to a queue that a blocked
    // acquire() would never look at again, and the server wedged as soon as
    // demand touched the pool size.
    pool_cv_.notify_one();
}

ConnectionPoolStats ConnectionPool::stats() const {
    ConnectionPoolStats s;
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        s.available     = static_cast<int>(available_connections_.size());
        s.shutting_down = shutdown_flag_;
    }
    s.size            = pool_size_;
    s.in_use          = in_use_.load();
    s.waiters         = waiters_.load();
    s.acquired_total  = acquired_total_.load();
    s.released_total  = released_total_.load();
    s.wait_events     = wait_events_.load();
    s.wait_timeouts   = wait_timeouts_.load();
    s.replaced_total  = replaced_total_.load();
    s.longest_wait_ms = longest_wait_ms_.load();
    return s;
}

} // namespace fileengine