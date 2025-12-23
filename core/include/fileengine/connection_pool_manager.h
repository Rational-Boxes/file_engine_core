#ifndef FILEENGINE_CONNECTION_POOL_MANAGER_H
#define FILEENGINE_CONNECTION_POOL_MANAGER_H

#include "types.h"
#include "connection_pool.h"
#include <memory>
#include <string>

namespace fileengine {

// Singleton connection pool manager to ensure all database instances share the same pool
class ConnectionPoolManager {
public:
    static ConnectionPoolManager& get_instance();

    // Initialize the shared connection pool
    Result<void> initialize_pool(const std::string& host, int port, const std::string& dbname,
                                const std::string& user, const std::string& password, int pool_size = 10);

    // Get the shared connection pool
    std::shared_ptr<ConnectionPool> get_pool();

    // Shutdown the connection pool
    Result<void> shutdown_pool();

    // Server state tracking methods
    void set_server_in_readonly_mode(bool readonly) { server_in_readonly_mode_ = readonly; }
    bool is_server_in_readonly_mode() const { return server_in_readonly_mode_; }

private:
    ConnectionPoolManager() = default;
    ~ConnectionPoolManager() = default;

    std::shared_ptr<ConnectionPool> pool_;
    bool initialized_{false};
    bool server_in_readonly_mode_{false};  // Track if server is in disconnected read-only mode
};

} // namespace fileengine

#endif // FILEENGINE_CONNECTION_POOL_MANAGER_H