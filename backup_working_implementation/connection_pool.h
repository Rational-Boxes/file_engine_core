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

private:
    std::string connection_info_;
    int pool_size_;

    std::queue<std::shared_ptr<DatabaseConnection>> available_connections_;
    std::mutex pool_mutex_;
    std::condition_variable pool_cv_;
    bool shutdown_flag_;
};

} // namespace fileengine