#include "fileengine/connection_pool.h"
#include "fileengine/logger.h"
#include <sstream>

namespace fileengine {

DatabaseConnection::DatabaseConnection(const std::string& conninfo) {
    conn_ = PQconnectdb(conninfo.c_str());
    if (!is_valid()) {
        throw std::runtime_error("Failed to connect to database: " + std::string(PQerrorMessage(conn_)));
    }
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
                << " password=" << password;
    connection_info_ = conn_stream.str();
}

ConnectionPool::~ConnectionPool() {
    shutdown();
}

bool ConnectionPool::initialize() {
    LOG_DEBUG("ConnectionPool", "Initializing connection pool with size: " + std::to_string(pool_size_));
    for (int i = 0; i < pool_size_; ++i) {
        try {
            auto conn = std::make_shared<DatabaseConnection>(connection_info_);
            available_connections_.push(conn);
            LOG_INFO("ConnectionPool", "Successfully initialized connection #" + std::to_string(i + 1) + " for pool.");
        } catch (const std::exception& e) {
            LOG_ERROR("ConnectionPool", "Failed to initialize database connection #" + std::to_string(i + 1) + ": " + std::string(e.what()));
            // Clear any connections that might have been successfully created before the failure
            while (!available_connections_.empty()) {
                available_connections_.pop();
            }
            return false;
        }
    }
    LOG_INFO("ConnectionPool", "Successfully initialized all " + std::to_string(pool_size_) + " connections in the pool.");
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
    std::unique_lock<std::mutex> lock(pool_mutex_);
    
    while (!shutdown_flag_ && available_connections_.empty()) {
        pool_cv_.wait(lock);
    }
    
    if (shutdown_flag_) {
        return nullptr;
    }
    
    auto conn = available_connections_.front();
    available_connections_.pop();
    return conn;
}

void ConnectionPool::release(std::shared_ptr<DatabaseConnection> conn) {
    if (conn && conn->is_valid()) {
        std::unique_lock<std::mutex> lock(pool_mutex_);
        if (!shutdown_flag_) {
            available_connections_.push(conn);
        }
    } else {
        // If the connection is invalid, create a new one to replace it
        if (!shutdown_flag_) {
            try {
                auto new_conn = std::make_shared<DatabaseConnection>(connection_info_);
                std::unique_lock<std::mutex> lock(pool_mutex_);
                available_connections_.push(new_conn);
            } catch (const std::exception& e) {
                // Log error as appropriate
            }
        }
    }
}

} // namespace fileengine