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

#ifndef FILEENGINE_DATABASE_H
#define FILEENGINE_DATABASE_H

#include "types.h"
#include "accountability.h"
#include "service_credential.h"
#include "IDatabase.h"
#include "connection_pool.h"
#include "connection_router.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>
#include <libpq-fe.h>
#include <regex>
#include <thread>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <functional>

namespace fileengine {

class Database : public IDatabase {
public:
    Database(const std::string& host, int port, const std::string& dbname,
             const std::string& user, const std::string& password, int pool_size = 10);
    ~Database();

    // Connection management
    bool connect() override;
    void disconnect() override;
    bool is_connected() const override;

    // Schema management
    Result<void> create_schema() override;
    Result<void> create_global_schema();
    Result<void> create_tenant_schema(const std::string& tenant,
                                      const AccountabilityContext& ctx) override;
    Result<bool> tenant_schema_exists(const std::string& tenant) override;
    Result<void> drop_schema() override;

    // File metadata operations (using UUIDs instead of paths/ids) - now tenant-specific
    Result<std::string> insert_file(const std::string& uid, const std::string& name,
                                    const std::string& path, const std::string& parent_uid,
                                    FileType type, const std::string& owner,
                                    int permissions, const std::string& tenant) override;

    Result<std::string> create_file_with_acls(const std::string& uid,
                                               const std::string& name,
                                               const std::string& path,
                                               const std::string& parent_uid,
                                               FileType type,
                                               const std::string& owner,
                                               int permissions,
                                               const std::vector<AclGrant>& acl_grants,
                                               const std::string& tenant = "") override;
    Result<void> update_file_modified(const std::string& uid, const std::string& tenant) override;
    Result<void> update_file_current_version(const std::string& uid, const std::string& version_timestamp, const std::string& tenant) override;
    Result<void> update_file_size(const std::string& uid, int64_t size, const std::string& tenant = "") override;
    Result<bool> delete_file(const std::string& uid, const std::string& tenant) override;
    Result<bool> undelete_file(const std::string& uid, const std::string& tenant) override;
    Result<std::optional<FileInfo>> get_file_by_uid(const std::string& uid, const std::string& tenant) override;
    Result<std::optional<FileInfo>> get_file_by_path(const std::string& path, const std::string& tenant) override;  // Keep for backward compatibility
    Result<void> update_file_name(const std::string& uid, const std::string& new_name, const std::string& tenant) override;
    Result<std::vector<FileInfo>> list_files_in_directory(const std::string& parent_uid, const std::string& tenant) override;
    Result<std::vector<FileInfo>> list_files_in_directory_with_deleted(const std::string& parent_uid, const std::string& tenant) override;
    Result<std::vector<FileInfo>> list_all_files(const std::string& tenant) override;
    Result<std::optional<FileInfo>> get_file_by_name_and_parent(const std::string& name, const std::string& parent_uid, const std::string& tenant) override;
    Result<std::optional<FileInfo>> get_file_by_name_and_parent_include_deleted(const std::string& name, const std::string& parent_uid, const std::string& tenant) override;
    Result<int64_t> get_file_size(const std::string& file_uid, const std::string& tenant) override;
    Result<int64_t> get_directory_size(const std::string& dir_uid, const std::string& tenant) override;
    Result<std::optional<FileInfo>> get_file_by_uid_include_deleted(const std::string& uid, const std::string& tenant) override;
    Result<void> update_file_parent(const std::string& uid, const std::string& new_parent_uid, const std::string& tenant) override;

    // Path-to-UUID mapping (for backward compatibility)
    Result<std::string> path_to_uid(const std::string& path, const std::string& tenant) override;
    Result<std::vector<std::string>> uid_to_path(const std::string& uid, const std::string& tenant) override;

    // Version operations (using UUIDs and timestamp strings)
    Result<int64_t> insert_version(const std::string& file_uid, const std::string& version_timestamp,
                                    int64_t size, const std::string& storage_path,
                                    const std::string& revised_by, const std::string& tenant) override;
    Result<std::optional<std::string>> get_version_storage_path(const std::string& file_uid, const std::string& version_timestamp, const std::string& tenant) override;
    Result<std::vector<std::string>> list_versions(const std::string& file_uid, const std::string& tenant) override;
    Result<std::vector<VersionInfo>> list_versions_detailed(const std::string& file_uid, const std::string& tenant) override;
    Result<bool> delete_version(const std::string& file_uid, const std::string& version_timestamp, const std::string& tenant = "") override;

    // Version restoration operations
    Result<bool> restore_to_version(const std::string& file_uid, const std::string& version_timestamp, const std::string& user, const std::string& tenant = "") override;

    // Metadata operations (versioned by timestamp)
    Result<void> set_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& value, const std::string& tenant) override;
    Result<std::optional<std::string>> get_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& tenant) override;
    Result<std::map<std::string, std::string>> get_all_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& tenant) override;
    Result<void> delete_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& tenant) override;

    // Direct SQL execution for testing
    Result<void> execute(const std::string& sql, const std::string& tenant) override;
    Result<std::vector<std::vector<std::string>>> query(const std::string& sql, const std::string& tenant) override;

    // Cache tracking operations (global schema)
    Result<void> update_file_access_stats(const std::string& uid, const std::string& user, const std::string& tenant = "") override;
    Result<std::vector<std::string>> get_least_accessed_files(int limit, const std::string& tenant = "") override;
    Result<std::vector<std::string>> get_infrequently_accessed_files(int days_threshold, const std::string& tenant = "") override;
    Result<int64_t> get_storage_usage(const std::string& tenant = "") override;
    Result<int64_t> get_storage_capacity(const std::string& tenant = "") override;

    // Tenant management operations
    Result<void> cleanup_tenant_data(const std::string& tenant,
                                     const AccountabilityContext& ctx) override;
    Result<std::vector<std::string>> list_tenants() override;

    // ACL operations
    Result<void> add_acl(const std::string& resource_uid, const std::string& principal,
                         int type, int permissions,
                         const std::string& tenant,
                         const AccountabilityContext& ctx,
                         int effect = 0) override;
    Result<void> remove_acl(const std::string& resource_uid, const std::string& principal,
                            int type, int permissions,
                            const std::string& tenant,
                            const AccountabilityContext& ctx,
                            int effect = 0) override;
    Result<std::vector<AclEntry>> get_acls_for_resource(const std::string& resource_uid,
                                                        const std::string& tenant = "") override;
    Result<std::vector<AclEntry>> get_user_acls(const std::string& resource_uid,
                                                const std::string& principal,
                                                int type,
                                                const std::string& tenant = "") override;
    Result<std::vector<std::string>> list_claims(const std::string& prefix,
                                                 int limit,
                                                 const std::string& tenant = "") override;

    // Role management operations
    Result<void> create_role(const std::string& role, const std::string& tenant,
                             const AccountabilityContext& ctx) override;
    Result<void> delete_role(const std::string& role, const std::string& tenant,
                             const AccountabilityContext& ctx) override;
    Result<void> assign_user_to_role(const std::string& user, const std::string& role,
                                     const std::string& tenant,
                                     const AccountabilityContext& ctx) override;
    Result<void> remove_user_from_role(const std::string& user, const std::string& role,
                                       const std::string& tenant,
                                       const AccountabilityContext& ctx) override;
    Result<std::vector<std::string>> get_roles_for_user(const std::string& user,
                                                        const std::string& tenant = "") override;
    Result<std::vector<std::string>> get_users_for_role(const std::string& role,
                                                        const std::string& tenant = "") override;
    Result<std::vector<std::string>> get_all_roles(const std::string& tenant = "") override;

    Result<int> purge_versions(const std::string& file_uid,
                               const std::vector<std::string>& version_timestamps,
                               const std::string& cut_version_timestamp,
                               int keep_count,
                               const std::string& tenant,
                               const AccountabilityContext& ctx) override;

    // The pull surface (§4.3.1). Ordered by ts, which under the chain lock is
    // also seq order; `has_more` tells the consumer to come straight back
    // rather than wait out its poll interval.
    Result<std::vector<StoredAccountabilityRecord>> list_accountability_records(
            const std::string& tenant, std::int64_t newer_than_micros,
            int limit, bool& has_more) override;

    // The freshness-hint callback (§4.3). Set by the server once the event sink
    // exists; unset means no hints, which costs latency only — the pull path is
    // the guarantee and does not depend on it.
    //
    // Fired AFTER the commit, never before, so a hint can only under-assert:
    // a consumer that acts on one always finds the record it names.
    using AccountabilityHint = std::function<void(const std::string& tenant, std::int64_t seq)>;
    void set_accountability_hint(AccountabilityHint hint) { accountability_hint_ = std::move(hint); }

    // ── The service map (PROPOSAL_service_authentication.md §3.5) ───────────
    //
    // Resolve a presented token to a caller. Returns an empty service_id when
    // the token names nobody or the secret does not verify — the two are
    // deliberately indistinguishable to the caller, and cost the same, so a
    // wrong id cannot be told from a wrong secret by timing.
    //
    // `pepper` is the current one; `previous_pepper` may be set during a
    // rotation, in which case a row that verifies under it is rehashed under
    // the current one, best-effort, at most once per row (§3.5).
    Result<ServiceIdentity> resolve_service_token(const std::string& token,
                                                  const std::string& pepper,
                                                  const std::string& previous_pepper,
                                                  int current_pepper_version);

    // Issue a credential and record the act in the same transaction. Returns
    // the plaintext token, which exists nowhere else and is never stored.
    Result<std::string> issue_service_credential(const std::string& service_id,
                                                 const std::string& pepper,
                                                 int pepper_version,
                                                 const AccountabilityContext& ctx,
                                                 bool rotate);

    // Drop credentials. `prune` removes every credential except the newest
    // (completing a rotation); revoke removes all of them.
    Result<int> prune_service_credentials(const std::string& service_id,
                                          const AccountabilityContext& ctx);
    Result<int> revoke_service_credentials(const std::string& service_id,
                                           const AccountabilityContext& ctx);

    struct ServiceCredentialInfo {
        std::string service_id;
        int         pepper_version = 0;
        std::string created_at;
        std::string last_used_at;
        std::vector<std::string> capabilities;
    };
    Result<std::vector<ServiceCredentialInfo>> list_service_credentials();

    // Capability assignment. `grant` refuses a high-risk capability unless the
    // caller confirms, and never rides along with an issue.
    Result<void> grant_service_capability(const std::string& service_id,
                                          Capability capability,
                                          const AccountabilityContext& ctx);
    Result<void> revoke_service_capability(const std::string& service_id,
                                           Capability capability,
                                           const AccountabilityContext& ctx);
    Result<std::vector<Capability>> service_capabilities(const std::string& service_id);

    // Bootstrap: one unauthenticated enrolment, permitted only while the map is
    // empty AND the marker is absent, and closed irreversibly on use.
    Result<bool> service_bootstrap_complete();
    Result<std::string> enrol_bootstrap_credential(const std::string& service_id,
                                                   const std::string& pepper,
                                                   int pepper_version,
                                                   const AccountabilityContext& ctx);
    // Break-glass: reopen enrolment. A deliberate, recorded administrative act
    // rather than a config flag, because a restore will eventually lose the
    // credential files and the alternative is hand-written SQL under pressure.
    Result<void> reopen_service_bootstrap(const AccountabilityContext& ctx);

    // The committed position of the last accountability record for a tenant.
    // The queue hint carries this so a consumer can tell "no new records" apart
    // from "I am reading a replica that has not caught up" (§4.3).
    Result<std::int64_t> accountability_head_seq(const std::string& tenant);

    // Connection info access for administrative operations
    std::string get_connection_info() const;

    // Methods for configuring and managing primary-secondary database connections
    void configure_secondary_connection(const std::string& host, int port, const std::string& database_name,
                                        const std::string& user, const std::string& password);

    // Connection monitoring methods
    bool is_primary_available() const { return primary_available_.load(); }
    bool is_using_secondary() const { return using_secondary_.load(); }
    void start_connection_monitoring();
    void stop_connection_monitoring();

    // Watchdog state for the monitoring listener. `last_probe_age_seconds` is the
    // one to alert on: a watchdog that has not completed a probe in well over its
    // interval is stuck, and a stuck watchdog means an outage would never be
    // noticed or recovered from.
    struct WatchdogStats {
        bool          running                 = false;
        bool          probe_in_flight         = false;
        bool          primary_available       = true;
        bool          using_secondary         = false;
        int           interval_seconds        = 0;
        std::uint64_t probes_total            = 0;
        std::uint64_t transitions_total       = 0;
        std::int64_t  last_probe_epoch        = 0;
        std::int64_t  last_probe_duration_ms  = 0;
        std::int64_t  last_probe_age_seconds  = -1;
    };
    WatchdogStats watchdog_stats() const;

    // Live pool telemetry. Read from the pool this Database owns rather than from
    // ConnectionPoolManager: despite its name and comment, the manager's pool is
    // never initialized anywhere in the tree — every Database constructs its own
    // ConnectionPool directly — so the manager is only a holder for the read-only
    // flag. Going to the owner is the only way to see the pool that is actually
    // serving traffic.
    ConnectionPoolStats pool_stats() const;
    ConnectionPoolStats secondary_pool_stats() const;
    bool has_secondary_pool() const { return secondary_pool_ != nullptr; }

private:
    // Primary connection pool (main database)
    std::shared_ptr<ConnectionPool> connection_pool_;
    std::string hostname_;

    // Secondary/local database (read-only standby) for failover. Reads route here
    // while failed over; writes always use the primary (REPLICATION_FAILOVER.md).
    std::shared_ptr<DatabaseConnection> secondary_connection_;
    std::shared_ptr<ConnectionPool> secondary_pool_;
    std::string secondary_conn_info_;
    std::atomic<bool> using_secondary_{false};
    int pool_size_{10};                  // reused when building the secondary pool

    // Acquire a connection for the given operation kind: writes -> primary; reads
    // -> the replica while failed over, else the primary.
    std::shared_ptr<DatabaseConnection> acquire(DbOp op);

    // Connection health monitoring
    std::atomic<bool> primary_available_{true};
    std::atomic<bool> monitoring_active_{false};
    std::thread connection_monitor_thread_;
    std::mutex connection_mutex_;
    int retry_interval_seconds_{30}; // Default retry interval

    // The watchdog polls, so it spends nearly all its life asleep. That sleep has
    // to be interruptible: a plain sleep_for() means stop_connection_monitoring()
    // joins a thread that will not look at the stop flag until its full interval
    // has elapsed, which held shutdown for up to retry_interval_seconds_.
    std::mutex              monitor_wait_mutex_;
    std::condition_variable monitor_wait_cv_;

    // Watchdog telemetry, so "is the recovery thread alive and doing its job?" is
    // answerable from the monitoring endpoint instead of by inference.
    std::atomic<std::uint64_t> monitor_probes_{0};        // completed probes
    std::atomic<std::uint64_t> monitor_transitions_{0};   // up<->down changes acted on
    std::atomic<std::int64_t>  monitor_last_probe_epoch_{0};
    std::atomic<std::int64_t>  monitor_last_probe_ms_{0}; // how long the last probe took
    std::atomic<bool>          monitor_probe_in_flight_{false};

    // Append one accountability record INSIDE the caller's already-open
    // transaction on `conn`. This is the whole guarantee: the record and the
    // operation commit together or not at all. A failure here must be treated
    // as fatal by the caller — roll back and fail the operation rather than
    // proceed unrecorded (§4.2 guarantee 2).
    //
    // Locks the tenant's chain-head row, so appends serialize per tenant. That
    // is affordable only because the scope is rare operations (§4.1).
    struct AccountabilityCommit {
        std::int64_t seq = 0;
        std::int64_t ts_micros = 0;
    };
    Result<AccountabilityCommit> append_accountability(PGconn* conn,
                                                       const std::string& tenant,
                                                       const AccountabilityRecord& rec);

    // Fire the post-commit hint, swallowing anything it throws. seq == 0 means
    // "nothing was recorded" (a no-op operation), and emits nothing.
    void fire_accountability_hint(const std::string& tenant, std::int64_t seq) noexcept;

    AccountabilityHint accountability_hint_;

    Result<void> check_connection() const;
    std::string escape_string(const std::string& str, PGconn* conn) const;
    std::string validate_schema_name(const std::string& schema_name) const;
    std::string get_hostname() const;
    std::string get_schema_prefix(const std::string& tenant) const;
    Result<PGresult*> execute_query_with_params(PGconn* conn, const std::string& sql_template,
                                                const std::vector<std::string>& params) const;
};

} // namespace fileengine

#endif // FILEENGINE_DATABASE_H