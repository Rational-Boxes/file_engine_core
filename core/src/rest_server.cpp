#include "fileengine/rest_server.h"
#include "fileengine/cache_manager.h"
#include "fileengine/file_culler.h"
#include "fileengine/server_logger.h"
#include "fileengine/build_info.h"

// cpp-httplib lives in third_party/ — single header.
#include "httplib.h"
// nlohmann/json lives there too.
#include "json.hpp"

namespace fileengine {

namespace {
using json = nlohmann::json;
} // namespace

RestServer::RestServer(std::shared_ptr<IDatabase> db,
                       CacheManager* cache_manager,
                       FileCuller* file_culler)
    : db_(std::move(db)),
      cache_manager_(cache_manager),
      file_culler_(file_culler),
      http_(std::make_unique<httplib::Server>()) {
    install_routes();
}

RestServer::~RestServer() {
    stop();
}

void RestServer::install_routes() {
    // ---------------------------------------------------------------------
    // /healthz — liveness. Always 200 as long as the process is responding.
    // ---------------------------------------------------------------------
    http_->Get("/healthz", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("ok\n", "text/plain");
    });

    // ---------------------------------------------------------------------
    // /readyz — readiness. 200 once DB is connected; 503 otherwise.
    // ---------------------------------------------------------------------
    http_->Get("/readyz", [this](const httplib::Request&, httplib::Response& res) {
        const bool db_ok = db_ && db_->is_connected();
        if (db_ok) {
            res.status = 200;
            res.set_content("ready\n", "text/plain");
        } else {
            res.status = 503;
            res.set_content("not ready: database unreachable\n", "text/plain");
        }
    });

    // ---------------------------------------------------------------------
    // /v1/version — CMake-baked build info.
    // ---------------------------------------------------------------------
    http_->Get("/v1/version", [](const httplib::Request&, httplib::Response& res) {
        json j = {
            {"version",     kBuildVersion},
            {"git_sha",     kBuildGitSha},
            {"built_at",    kBuildTimestamp},
            {"otel_enabled", kBuildOtelEnabled},
        };
        res.set_content(j.dump(2) + "\n", "application/json");
    });

    // ---------------------------------------------------------------------
    // /v1/status — JSON snapshot of server state. Cheap; just reads
    // in-memory counters and runs one or two small SQL queries.
    // ---------------------------------------------------------------------
    http_->Get("/v1/status", [this](const httplib::Request&, httplib::Response& res) {
        json j;
        j["version"]   = kBuildVersion;
        j["git_sha"]   = kBuildGitSha;
        j["built_at"]  = kBuildTimestamp;

        // Uptime from this REST listener's start (the gRPC server starts
        // before this listener; close enough for an operator-facing value).
        auto now = std::chrono::steady_clock::now();
        auto up_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            now - started_at_).count();
        j["uptime_seconds"] = up_seconds;

        // Database / connection-pool state. The IDatabase interface only
        // exposes is_connected(); primary/secondary failover state is a
        // concrete-Database concern and not surfaced here. server-in-
        // readonly-mode lives on the ConnectionPoolManager singleton.
        json db;
        db["connected"]     = db_ && db_->is_connected();
        db["readonly_mode"] = ConnectionPoolManager::get_instance()
                                  .is_server_in_readonly_mode();
        j["database"] = std::move(db);

        // Cache state, if a CacheManager was wired in.
        if (cache_manager_) {
            json c;
            c["size_bytes"]     = cache_manager_->get_cache_size_bytes();
            c["max_bytes"]      = cache_manager_->get_max_cache_size_bytes();
            c["usage_pct"]      = cache_manager_->get_cache_usage_percentage() * 100.0;
            j["cache"] = std::move(c);
        }

        // Culler state.
        if (file_culler_) {
            json cu;
            cu["culled_files_total"] = file_culler_->get_culled_file_count();
            cu["culled_bytes_total"] = file_culler_->get_culled_byte_count();
            j["culler"] = std::move(cu);
        }

        // Tenant list — from the registry. Per-tenant byte/file counts
        // are deferred to /v1/tenants/{id}/usage (Phase A optional).
        if (db_) {
            auto tenants_result = db_->list_tenants();
            if (tenants_result.success) {
                j["tenants"] = tenants_result.value;
            } else {
                j["tenants"] = json::array();
                j["tenants_error"] = tenants_result.error;
            }
        }

        res.set_content(j.dump(2) + "\n", "application/json");
    });

    // 404 default.
    http_->set_error_handler([](const httplib::Request& req, httplib::Response& res) {
        json j = {{"error", "not found"}, {"path", req.path}};
        res.status = 404;
        res.set_content(j.dump() + "\n", "application/json");
    });
}

bool RestServer::start(const std::string& bind_addr, int port) {
    if (running_.load()) {
        SERVER_LOG_WARN("RestServer", "start() called while already running");
        return true;
    }
    started_at_ = std::chrono::steady_clock::now();
    running_.store(true);

    // Pre-bind so the caller knows immediately whether we can listen.
    // listen_after_bind() lets the listener thread run() while we already
    // hold the bound socket.
    if (!http_->bind_to_port(bind_addr.c_str(), port)) {
        SERVER_LOG_ERROR("RestServer",
                         "Failed to bind monitoring listener to " +
                         bind_addr + ":" + std::to_string(port));
        running_.store(false);
        return false;
    }

    listener_thread_ = std::thread([this, bind_addr, port]() {
        SERVER_LOG_INFO("RestServer",
                        "Monitoring REST listener on " + bind_addr + ":" +
                        std::to_string(port));
        http_->listen_after_bind();
        SERVER_LOG_INFO("RestServer", "Monitoring REST listener stopped");
    });

    return true;
}

void RestServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (http_) http_->stop();
    if (listener_thread_.joinable()) listener_thread_.join();
}

} // namespace fileengine
