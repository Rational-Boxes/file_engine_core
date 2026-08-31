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
#include "IStorage.h"
#include "IObjectStore.h"
#include "IDatabase.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>

namespace fileengine {

struct SyncConfig {
    bool enabled;
    int retry_seconds;           // Number of seconds to wait before retrying failed operations
    bool sync_on_startup;        // Whether to sync on startup
    bool sync_on_demand;         // Whether to support on-demand sync
    std::string sync_pattern;    // Pattern of files to sync (e.g., all, recent, etc.)
    bool bidirectional;          // Whether sync is bidirectional
};

class ObjectStoreSync {
public:
    ObjectStoreSync(std::shared_ptr<IDatabase> db, IStorage* storage, IObjectStore* object_store);
    
    // Configure sync parameters
    void configure(const SyncConfig& config);
    
    // Start the sync service (including background thread for recovery)
    Result<void> start_sync_service();
    
    // Stop the sync service
    void stop_sync_service();
    
    // Perform a one-time sync operation
    Result<void> perform_sync(std::function<void(const std::string&, int, int)> progress_callback = nullptr);
    
    // Perform startup sync to synchronize existing files
    Result<void> perform_startup_sync();
    
    // Perform tenant-specific sync
    Result<void> perform_tenant_sync(const std::string& tenant);
    
    // Check connection health
    bool is_connection_healthy() const;
    
    // Attempt connection recovery
    Result<void> attempt_recovery();
    
    // Get sync statistics
    size_t get_synced_file_count() const;
    size_t get_failed_sync_count() const;
    
    // Check if the sync service is running
    bool is_sync_running() const;

    // --- Observability -------------------------------------------------------
    //
    // All three are CHEAP: they read atomics the sync pass itself writes. That is
    // deliberate, not an optimisation. The honest answer to "how many versions
    // are not in the object store" costs one existence check per version — 585
    // network round trips on a modest deployment — so computing it on a /metrics
    // scrape would turn a 15-second scrape interval into sustained load against
    // the bucket, and a slow bucket into a scrape timeout that looks like the
    // core is down. The sync loop already pays that cost once a minute because
    // it has to; these publish what it found.
    //
    // Staleness is therefore part of the reading, which is why the age is
    // exposed alongside the count. A pending count that has stopped changing
    // looks identical to a healthy one until you can see nothing has scanned in
    // an hour.

    // Versions found needing sync at the last completed scan. The backlog.
    size_t get_pending_count() const;

    // Seconds since that scan finished; -1 if none has completed. Rising without
    // bound means the sync loop itself has stopped, which the count alone cannot
    // distinguish from "nothing to do".
    int64_t get_last_scan_age_seconds() const;

    // The last observed result of is_connection_healthy(), which performs a
    // bucket_exists() call and so must never be invoked from a scrape.
    bool get_connection_healthy() const;

    // Perform comprehensive sync of all local files (for startup)
    Result<void> perform_comprehensive_local_sync(const std::string& tenant = "");

    ~ObjectStoreSync();

private:
    std::shared_ptr<IDatabase> db_;
    IStorage* storage_;
    IObjectStore* object_store_;
    SyncConfig config_;

    std::thread sync_thread_;
    std::thread recovery_thread_;   // declared and joined, but never started
    // The startup backlog sync. A member so it can be JOINED: it used to be
    // detached, which meant it kept calling methods on `this` — and using the
    // AWS SDK — while shutdown destroyed both underneath it.
    std::thread startup_sync_thread_;
    std::atomic<bool> running_;
    std::atomic<bool> sync_in_progress_;
    mutable std::mutex sync_mutex_;
    // Interruptible retry sleep in monitoring_loop: stop_sync_service() flips
    // running_ and wakes this CV so the worker exits at once instead of blocking
    // join() for the remainder of a retry_seconds interval.
    std::mutex sync_wait_mutex_;
    std::condition_variable sync_wait_cv_;

    std::atomic<size_t> synced_file_count_;
    std::atomic<size_t> failed_sync_count_;

    // Published by the sync pass, read by the metrics endpoint. See the note on
    // the getters above for why these are cached rather than computed on demand.
    std::atomic<size_t> pending_count_{0};
    std::atomic<size_t> pending_scratch_{0};
    std::atomic<long long> last_scan_epoch_s_{-1};
    std::atomic<bool> connection_healthy_{false};

    // Bracket a full pass so the published count is a whole-fleet number rather
    // than whichever tenant happened to be scanned last.
    void begin_scan();
    void end_scan();

    // Background thread for monitoring and recovery
    void monitoring_loop();

    // Internal sync method
    Result<void> sync_files(const std::string& tenant = "");

    // Sync a specific file
    Result<void> sync_file(const std::string& uid, const std::string& version_timestamp,
                          const std::string& tenant = "");

    // Get list of files to sync
    Result<std::vector<std::pair<std::string, std::string>>> get_files_to_sync(const std::string& tenant = "");

    // Check if file needs sync (compare local and remote versions)
    Result<bool> needs_sync(const std::string& uid, const std::string& version_timestamp,
                           const std::string& tenant = "");

    // Get tenant list for multi-tenant sync
    Result<std::vector<std::string>> get_tenant_list();

    // Verify sync completion
    Result<void> verify_sync_completion();
};

} // namespace fileengine