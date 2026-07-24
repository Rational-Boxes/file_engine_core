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

class ConnectionPool {
public:
    ConnectionPool(const std::string& host, int port, const std::string& dbname,
                   const std::string& user, const std::string& password, int pool_size = 10);
    ~ConnectionPool();

    std::shared_ptr<DatabaseConnection> acquire();
    void release(std::shared_ptr<DatabaseConnection> conn);

    bool initialize();
    void shutdown();

    // Connection information access
    std::string get_connection_info() const { return connection_info_; }

private:
    std::string connection_info_;
    int pool_size_;

    std::queue<std::shared_ptr<DatabaseConnection>> available_connections_;
    std::mutex pool_mutex_;
    std::condition_variable pool_cv_;
    bool shutdown_flag_;
};

} // namespace fileengine