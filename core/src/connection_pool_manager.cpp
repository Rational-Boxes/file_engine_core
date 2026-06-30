#include "fileengine/connection_pool_manager.h"
#include "fileengine/connection_pool.h"
#include <mutex>

// Disconnected read-only mode (REPLICATION_FAILOVER.md): the global flag below is
// driven by Database's connection monitor — set when the primary is unreachable and
// a secondary is configured, cleared when the primary is restored. The gRPC service
// rejects writes while it is set. (Read routing to the secondary is tracked in the
// design doc as the remaining step.)

namespace fileengine {

ConnectionPoolManager& ConnectionPoolManager::get_instance() {
    static ConnectionPoolManager instance;
    return instance;
}

Result<void> ConnectionPoolManager::initialize_pool(const std::string& host, int port, const std::string& dbname,
                                                   const std::string& user, const std::string& password, int pool_size) {
    if (initialized_) {
        return Result<void>::ok(); // Already initialized
    }

    try {
        pool_ = std::make_shared<ConnectionPool>(host, port, dbname, user, password, pool_size);
        if (pool_->initialize()) {
            initialized_ = true;
            return Result<void>::ok();
        } else {
            return Result<void>::err("Failed to initialize connection pool");
        }
    } catch (const std::exception& e) {
        return Result<void>::err("Exception initializing connection pool: " + std::string(e.what()));
    }
}

std::shared_ptr<ConnectionPool> ConnectionPoolManager::get_pool() {
    return pool_;
}

Result<void> ConnectionPoolManager::shutdown_pool() {
    if (pool_) {
        pool_->shutdown();
        initialized_ = false;
        pool_.reset();
        return Result<void>::ok();
    }
    return Result<void>::err("Pool not initialized");
}

} // namespace fileengine