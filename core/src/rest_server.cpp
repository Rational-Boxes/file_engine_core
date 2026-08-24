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

#include "fileengine/rest_server.h"
#include "fileengine/transfer_tracker.h"
#include "fileengine/cache_manager.h"
#include "fileengine/file_culler.h"
#include "fileengine/server_logger.h"
#include "fileengine/build_info.h"
#include "fileengine/connection_pool_manager.h"
#include "fileengine/database.h"
#include "fileengine/rpc_tracker.h"
#include "fileengine/thread_stats.h"

#include <iomanip>
#include <set>
#include <sstream>

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

void RestServer::set_allowed_ips(std::vector<std::string> ips) {
    allow_ips_ = std::move(ips);
}

namespace {

// --- Prometheus text exposition (0.0.4) ----------------------------------
//
// The shape matters as much as the numbers. Prometheus exposition is what
// monitoring platforms and load-balancer autoscalers already read — Prometheus
// itself, Grafana Agent, the OpenTelemetry collector, Datadog's OpenMetrics
// check — so publishing it here means no bespoke adapter has to exist for this
// service to be scraped, alerted on, or used as a scaling input.
//
// Conventions followed deliberately, because tooling depends on them: snake_case
// names under one `fileengine_` namespace, base units (seconds, not
// milliseconds), `_total` suffix on monotonic counters, and HELP/TYPE on every
// family.
class MetricsWriter {
public:
    void gauge(const std::string& name, const std::string& help, double value,
               const std::string& labels = "") {
        family(name, help, "gauge");
        emit(name, labels, value);
    }

    void counter(const std::string& name, const std::string& help, double value,
                 const std::string& labels = "") {
        family(name, help, "counter");
        emit(name, labels, value);
    }

    // For a family with several label sets, declare it once then add members.
    void family(const std::string& name, const std::string& help, const std::string& type) {
        if (declared_.count(name)) return;
        declared_.insert(name);
        out_ << "# HELP " << name << " " << help << "\n"
             << "# TYPE " << name << " " << type << "\n";
    }

    void emit(const std::string& name, const std::string& labels, double value) {
        out_ << name;
        if (!labels.empty()) out_ << "{" << labels << "}";
        // Plain decimal: Prometheus accepts it and it stays readable by eye.
        out_ << " " << format(value) << "\n";
    }

    std::string str() const { return out_.str(); }

private:
    static std::string format(double v) {
        std::ostringstream o;
        if (v == static_cast<long long>(v)) {
            o << static_cast<long long>(v);
        } else {
            o << std::fixed << std::setprecision(3) << v;
        }
        return o.str();
    }

    std::ostringstream out_;
    std::set<std::string> declared_;
};

// A label value must not break the exposition format.
std::string esc(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (char c : v) {
        if (c == '\\' || c == '"') { out += '\\'; out += c; }
        else if (c == '\n') { out += "\\n"; }
        else { out += c; }
    }
    return out;
}

} // namespace

void RestServer::install_routes() {
    // Optional IP allowlist, enforced before any route runs. Reads allow_ips_
    // live so set_allowed_ips() may be called after construction. Empty list =
    // no restriction (the bind address is the primary control). (L2.)
    http_->set_pre_routing_handler(
        [this](const httplib::Request& req, httplib::Response& res) {
            if (!allow_ips_.empty()) {
                bool allowed = false;
                for (const auto& ip : allow_ips_) {
                    if (ip == req.remote_addr) { allowed = true; break; }
                }
                if (!allowed) {
                    SERVER_LOG_WARN("RestServer",
                                    "monitoring request from " + req.remote_addr +
                                    " rejected (not in allowlist)");
                    res.status = 403;
                    res.set_content("forbidden\n", "text/plain");
                    return httplib::Server::HandlerResponse::Handled;
                }
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

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


    // ---------------------------------------------------------------------
    // /metrics — Prometheus exposition. The integration surface: scrapers,
    // dashboards and load-balancer autoscalers read this shape natively.
    // ---------------------------------------------------------------------
    http_->Get("/metrics", [this](const httplib::Request&, httplib::Response& res) {
        MetricsWriter m;

        auto now = std::chrono::steady_clock::now();
        m.gauge("fileengine_uptime_seconds", "Seconds since the monitoring listener started",
                static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(
                    now - started_at_).count()));

        m.gauge("fileengine_build_info", "Build identity; the value is always 1", 1,
                "version=\"" + esc(kBuildVersion) + "\",git_sha=\"" + esc(kBuildGitSha) + "\"");

        // --- threads ------------------------------------------------------
        // A high total is normal: the gRPC sync server parks a fixed pool of
        // pollers for the life of the process. What matters is the split, and
        // especially `uninterruptible` — a thread in D state is blocked in the
        // kernel and cannot be timed out or cancelled.
        const auto th = read_thread_stats();
        m.gauge("fileengine_thread_state_available",
                "1 when per-thread state could be read from /proc, 0 when it could not",
                th.available ? 1 : 0);
        if (th.available) {
            m.gauge("fileengine_threads", "Threads in this process, by kernel state",
                    th.running, "state=\"running\"");
            m.emit("fileengine_threads", "state=\"sleeping\"", th.sleeping);
            m.emit("fileengine_threads", "state=\"uninterruptible\"", th.uninterruptible);
            m.emit("fileengine_threads", "state=\"stopped\"", th.stopped);
            m.emit("fileengine_threads", "state=\"zombie\"", th.zombie);
            m.emit("fileengine_threads", "state=\"other\"", th.other);
            m.gauge("fileengine_threads_total", "Total threads in this process", th.total);
            m.gauge("fileengine_threads_not_waiting",
                    "Threads not in interruptible sleep. An idle server should hold this near zero",
                    th.not_waiting());
        }

        // --- large object-store transfers --------------------------------
        //
        // Only multipart uploads reach these counters: small files take the
        // single-PutObject path. So everything here is a BIG transfer, which is
        // exactly the population worth watching — a failure costs the user a
        // long wait and produces nothing they can see.
        //
        // A failed multipart is otherwise invisible. No object appears, nothing
        // references it, the abandoned parts are absent from a normal listing,
        // and the local copy survives so no data is lost. Without these the only
        // symptom is a user saying a large file "didn't work".
        {
            const auto tx = TransferTracker::instance().stats();
            m.counter("fileengine_multipart_completed_total",
                      "Large (multipart) object-store uploads that committed",
                      static_cast<double>(tx.completed));
            m.counter("fileengine_multipart_completed_bytes_total",
                      "Bytes committed by multipart uploads",
                      static_cast<double>(tx.completed_bytes));

            // Split by stage: where a big upload dies says what is wrong.
            // open = local, part = the link, complete = the store itself.
            m.family("fileengine_multipart_aborted_total",
                     "Large uploads abandoned before commit, by the stage they reached",
                     "counter");
            m.emit("fileengine_multipart_aborted_total", "stage=\"open\"",
                   static_cast<double>(tx.aborted_open));
            m.emit("fileengine_multipart_aborted_total", "stage=\"part\"",
                   static_cast<double>(tx.aborted_part));
            m.emit("fileengine_multipart_aborted_total", "stage=\"complete\"",
                   static_cast<double>(tx.aborted_complete));

            // The frustration signal: bytes a user waited to send, discarded.
            // Rate over time is the number to chart; a step change means large
            // uploads have started failing for somebody.
            m.counter("fileengine_multipart_aborted_bytes_total",
                      "Bytes transferred by uploads that were then abandoned — work the "
                      "user waited for and did not get",
                      static_cast<double>(tx.aborted_bytes));

            // Cleanup that failed, so parts are stranded in the bucket until the
            // AbortIncompleteMultipartUpload lifecycle rule reaps them. Distinct
            // from the failure itself: this one accumulates billable residue.
            m.counter("fileengine_multipart_abort_failed_total",
                      "Aborts that themselves failed, leaving parts to be garbage-collected "
                      "by the bucket lifecycle rule",
                      static_cast<double>(tx.abort_failed));
        }

        // --- in-flight RPCs ----------------------------------------------
        const auto rpc = RpcTracker::instance().stats();
        m.gauge("fileengine_rpc_in_flight_total", "RPCs being served right now", rpc.in_flight);
        m.counter("fileengine_rpc_started_total", "RPCs started since boot",
                  static_cast<double>(rpc.started_total));
        m.counter("fileengine_rpc_completed_total", "RPCs completed since boot",
                  static_cast<double>(rpc.completed_total));
        m.gauge("fileengine_rpc_oldest_in_flight_seconds",
                "Age of the longest-running RPC still in flight. File operations finish in "
                "milliseconds, so a value in minutes means a wedged request",
                static_cast<double>(rpc.oldest_age_ms) / 1000.0);
        if (!rpc.by_method.empty()) {
            m.family("fileengine_rpc_in_flight", "RPCs in flight, by method", "gauge");
            for (const auto& [method, count] : rpc.by_method) {
                m.emit("fileengine_rpc_in_flight", "method=\"" + esc(method) + "\"", count);
            }
        }

        // --- database connection pool -------------------------------------
        // The pool actually serving traffic belongs to the Database; the manager
        // singleton is never initialized (see Database::pool_stats).
        if (auto concrete_db = std::dynamic_pointer_cast<Database>(db_)) {
            const auto ps = concrete_db->pool_stats();
            m.gauge("fileengine_db_pool_size", "Connections the pool was built with", ps.size);
            m.gauge("fileengine_db_pool_available", "Idle connections ready to hand out", ps.available);
            m.gauge("fileengine_db_pool_in_use", "Connections held by a caller right now", ps.in_use);
            m.gauge("fileengine_db_pool_waiters",
                    "Callers blocked waiting for a connection. Sustained non-zero means the pool "
                    "is the bottleneck", ps.waiters);
            // The saturation signal a load balancer or autoscaler wants: 0..1.
            m.gauge("fileengine_db_pool_utilization",
                    "in_use / size, as a ratio. The saturation signal for load shedding",
                    ps.size > 0 ? static_cast<double>(ps.in_use) / ps.size : 0.0);
            m.counter("fileengine_db_pool_acquired_total", "Connections handed out since boot",
                      static_cast<double>(ps.acquired_total));
            m.counter("fileengine_db_pool_released_total", "Connections returned since boot",
                      static_cast<double>(ps.released_total));
            m.counter("fileengine_db_pool_wait_events_total",
                      "Acquires that had to block because the pool was empty",
                      static_cast<double>(ps.wait_events));
            m.counter("fileengine_db_pool_wait_timeouts_total",
                      "Acquires that gave up waiting. Any value above zero is a leak or an overload",
                      static_cast<double>(ps.wait_timeouts));
            m.counter("fileengine_db_pool_replaced_total",
                      "Dead connections rebuilt on release",
                      static_cast<double>(ps.replaced_total));
            m.gauge("fileengine_db_pool_longest_wait_seconds",
                    "Longest single wait for a connection since boot",
                    static_cast<double>(ps.longest_wait_ms) / 1000.0);
            // acquired - released is the count currently checked out. It should
            // equal in_use; a lasting gap is the signature of a leaked connection.
            m.gauge("fileengine_db_pool_outstanding",
                    "acquired_total - released_total. Should track in_use; a lasting gap means a "
                    "connection was acquired and never returned",
                    static_cast<double>(ps.acquired_total) - static_cast<double>(ps.released_total));
        }

        // --- outage-recovery watchdog -------------------------------------
        if (auto concrete = std::dynamic_pointer_cast<Database>(db_)) {
            const auto w = concrete->watchdog_stats();
            m.gauge("fileengine_db_watchdog_running", "1 when the recovery watchdog thread is active",
                    w.running ? 1 : 0);
            m.gauge("fileengine_db_watchdog_interval_seconds", "Configured probe interval",
                    w.interval_seconds);
            m.counter("fileengine_db_watchdog_probes_total", "Probes the watchdog has completed",
                      static_cast<double>(w.probes_total));
            m.counter("fileengine_db_watchdog_transitions_total",
                      "Up/down transitions the watchdog has acted on",
                      static_cast<double>(w.transitions_total));
            m.gauge("fileengine_db_watchdog_probe_in_flight", "1 while a probe is running",
                    w.probe_in_flight ? 1 : 0);
            // The alert to build on: a watchdog that has not completed a probe in
            // well over its interval is stuck, and a stuck watchdog means an
            // outage would never be noticed or recovered from.
            m.gauge("fileengine_db_watchdog_last_probe_age_seconds",
                    "Seconds since the watchdog last completed a probe; -1 before the first",
                    static_cast<double>(w.last_probe_age_seconds));
            m.gauge("fileengine_db_watchdog_last_probe_duration_seconds",
                    "How long the watchdog's last probe took",
                    static_cast<double>(w.last_probe_duration_ms) / 1000.0);
            m.gauge("fileengine_db_primary_available", "1 when the primary database is reachable",
                    w.primary_available ? 1 : 0);
            m.gauge("fileengine_db_using_secondary", "1 while reads are served from the standby",
                    w.using_secondary ? 1 : 0);
        }
        m.gauge("fileengine_db_connected", "1 when the database is connected",
                (db_ && db_->is_connected()) ? 1 : 0);
        m.gauge("fileengine_db_readonly_mode", "1 while the server is in read-only fallback",
                ConnectionPoolManager::get_instance().is_server_in_readonly_mode() ? 1 : 0);

        res.set_content(m.str(), "text/plain; version=0.0.4; charset=utf-8");
    });

    // ---------------------------------------------------------------------
    // /poolz — the same picture as JSON, for a human troubleshooting a live
    // server. /metrics is for machines; this is for the person reading it,
    // and it carries the per-RPC detail that would be unbounded as labels.
    // ---------------------------------------------------------------------
    http_->Get("/poolz", [this](const httplib::Request&, httplib::Response& res) {
        json j;

        const auto th = read_thread_stats();
        json t;
        t["available"] = th.available;
        if (th.available) {
            t["total"]           = th.total;
            t["running"]         = th.running;
            t["sleeping"]        = th.sleeping;
            t["uninterruptible"] = th.uninterruptible;
            t["stopped"]         = th.stopped;
            t["zombie"]          = th.zombie;
            t["other"]           = th.other;
            t["not_waiting"]     = th.not_waiting();
            t["note"] = "A large total is expected: the gRPC sync server parks a fixed pool of "
                        "pollers. Watch not_waiting and uninterruptible instead.";
        }
        j["threads"] = std::move(t);

        const auto rpc = RpcTracker::instance().stats();
        json r;
        r["in_flight"]           = rpc.in_flight;
        r["started_total"]       = rpc.started_total;
        r["completed_total"]     = rpc.completed_total;
        r["oldest_age_ms"]       = rpc.oldest_age_ms;
        r["oldest_method"]       = rpc.oldest_method;
        r["by_method"]           = rpc.by_method;
        json detail = json::array();
        for (const auto& e : rpc.in_flight_detail) {
            detail.push_back({{"method", e.method}, {"age_ms", e.age_ms}});
        }
        r["in_flight_detail"] = std::move(detail);
        j["rpc"] = std::move(r);

        // The pool actually serving traffic belongs to the Database; the manager
        // singleton is never initialized (see Database::pool_stats).
        if (auto concrete_db = std::dynamic_pointer_cast<Database>(db_)) {
            const auto ps = concrete_db->pool_stats();
            json p;
            p["size"]            = ps.size;
            p["available"]       = ps.available;
            p["in_use"]          = ps.in_use;
            p["waiters"]         = ps.waiters;
            p["acquired_total"]  = ps.acquired_total;
            p["released_total"]  = ps.released_total;
            p["outstanding"]     = static_cast<long long>(ps.acquired_total) -
                                   static_cast<long long>(ps.released_total);
            p["wait_events"]     = ps.wait_events;
            p["wait_timeouts"]   = ps.wait_timeouts;
            p["replaced_total"]  = ps.replaced_total;
            p["longest_wait_ms"] = ps.longest_wait_ms;
            p["shutting_down"]   = ps.shutting_down;
            j["db_pool"] = std::move(p);
        }

        if (auto concrete = std::dynamic_pointer_cast<Database>(db_)) {
            const auto w = concrete->watchdog_stats();
            json wd;
            wd["running"]                = w.running;
            wd["probe_in_flight"]        = w.probe_in_flight;
            wd["interval_seconds"]       = w.interval_seconds;
            wd["probes_total"]           = w.probes_total;
            wd["transitions_total"]      = w.transitions_total;
            wd["last_probe_epoch"]       = w.last_probe_epoch;
            wd["last_probe_age_seconds"] = w.last_probe_age_seconds;
            wd["last_probe_duration_ms"] = w.last_probe_duration_ms;
            wd["primary_available"]      = w.primary_available;
            wd["using_secondary"]        = w.using_secondary;
            j["watchdog"] = std::move(wd);
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
