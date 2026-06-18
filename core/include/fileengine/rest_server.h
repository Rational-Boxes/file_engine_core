#pragma once

#include "fileengine/IDatabase.h"
#include "fileengine/connection_pool_manager.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

// Forward-declare cpp-httplib's Server so this header doesn't drag the
// whole httplib.h compile cost into every translation unit that includes it.
namespace httplib { class Server; }

namespace fileengine {

class CacheManager;
class FileCuller;

// Embedded HTTP monitoring listener for the fileengine server.
//
// Phase A endpoints (this file): /healthz, /readyz, /v1/version, /v1/status.
// Phase B will add /metrics on the same listener.
//
// Trust model: no in-process auth or TLS. The deployment relies on the
// network perimeter (firewall / VPC security groups) to restrict the port
// to trusted operators and scrapers. See design_documents/
// monitoring_and_telemetry.md §10 for the rationale.
class RestServer {
public:
    RestServer(std::shared_ptr<IDatabase> db,
               CacheManager* cache_manager,
               FileCuller* file_culler);
    ~RestServer();

    RestServer(const RestServer&) = delete;
    RestServer& operator=(const RestServer&) = delete;

    // Start the listener on the given address/port. Returns false on bind
    // failure so the caller (server.cpp) can choose to fail the boot.
    // Non-blocking: the listener runs on a dedicated thread.
    bool start(const std::string& bind_addr, int port);

    // Stop and join the listener thread. Safe to call before start() or
    // multiple times.
    void stop();

private:
    void install_routes();

    std::shared_ptr<IDatabase> db_;
    CacheManager* cache_manager_;
    FileCuller* file_culler_;

    std::unique_ptr<httplib::Server> http_;
    std::thread listener_thread_;
    std::atomic<bool> running_{false};
    std::chrono::steady_clock::time_point started_at_;
};

} // namespace fileengine
