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

#include "fileengine/IDatabase.h"
#include "fileengine/connection_pool_manager.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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
// Trust model: no in-process auth or TLS. Defends the port in depth by binding
// loopback-only by default (see config http_metrics_addr) and, optionally, an
// IP allowlist (set_allowed_ips) enforced before routing. The deployment still
// relies on the network perimeter (firewall / VPC security groups). See
// design_documents/monitoring_and_telemetry.md §10 for the rationale.
class RestServer {
public:
    RestServer(std::shared_ptr<IDatabase> db,
               CacheManager* cache_manager,
               FileCuller* file_culler);
    ~RestServer();

    RestServer(const RestServer&) = delete;
    RestServer& operator=(const RestServer&) = delete;

    // Optional client-IP allowlist (exact-match). When non-empty, a request
    // whose remote address is not listed is refused with 403 before routing.
    // Call before start(). Empty (default) = allow any host that reaches the
    // bound address. (Security review L2.)
    void set_allowed_ips(std::vector<std::string> ips);

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
    std::vector<std::string> allow_ips_;
    std::thread listener_thread_;
    std::atomic<bool> running_{false};
    std::chrono::steady_clock::time_point started_at_;
};

} // namespace fileengine
