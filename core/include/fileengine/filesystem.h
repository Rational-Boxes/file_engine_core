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
#include "IDatabase.h"
#include "IStorage.h"
#include "IObjectStore.h"
#include "acl_manager.h"
#include "cache_manager.h"
#include "tenant_manager.h"
#include "file_culler.h"
#include "event_sink.h"
#include <string>
#include <vector>
#include <memory>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <shared_mutex>
#include <functional>

namespace fileengine {

class FileSystem {
public:
    FileSystem(std::shared_ptr<TenantManager> tenant_manager);

    virtual ~FileSystem();

    // Directory operations - now using UUIDs
    virtual Result<std::string> mkdir(const std::string& parent_uid, const std::string& name,
                                      const std::string& user,
                                      const std::vector<std::string>& roles = {},
                                      int permissions = 0755,
                                      const std::string& tenant = "");
    virtual Result<void> rmdir(const std::string& dir_uid, const std::string& user,
                               const std::vector<std::string>& roles = {},
                               const std::string& tenant = "");
    virtual Result<std::vector<DirectoryEntry>> listdir(const std::string& dir_uid,
                                                        const std::string& user,
                                                        const std::vector<std::string>& roles = {},
                                                        const std::string& tenant = "");
    virtual Result<std::vector<DirectoryEntry>> listdir_with_deleted(const std::string& dir_uid,
                                                                     const std::string& user,
                                                                     const std::vector<std::string>& roles = {},
                                                                     const std::string& tenant = "");

    // File operations
    virtual Result<std::string> touch(const std::string& parent_uid, const std::string& name,
                                      const std::string& user,
                                      const std::vector<std::string>& roles = {},
                                      const std::string& tenant = "");
    virtual Result<void> remove(const std::string& file_uid, const std::string& user,
                                const std::vector<std::string>& roles = {},
                                const std::string& tenant = "");
    virtual Result<void> undelete(const std::string& file_uid, const std::string& user,
                                  const std::vector<std::string>& roles = {},
                                  const std::string& tenant = "");
    virtual Result<void> put(const std::string& file_uid, const std::vector<uint8_t>& data,
                             const std::string& user,
                             const std::vector<std::string>& roles = {},
                             const std::string& tenant = "");
    virtual Result<std::vector<uint8_t>> get(const std::string& file_uid,
                                             const std::string& user,
                                             const std::vector<std::string>& roles = {},
                                             const std::string& tenant = "");

    // Streaming write: pulls plaintext chunks via `next_chunk` (fills the vector,
    // returns false at end-of-input) and writes them compress->encrypt->disk
    // without ever holding the whole file in memory. Performs the same
    // version/size/backup/event bookkeeping as put(). On failure the partial
    // file is removed.
    virtual Result<void> put_stream(const std::string& file_uid,
                                    const std::function<bool(std::vector<uint8_t>&)>& next_chunk,
                                    const std::string& user,
                                    const std::vector<std::string>& roles = {},
                                    const std::string& tenant = "");
    // Streaming read: resolves the current version and emits plaintext chunks via
    // `on_chunk` (disk->decrypt->decompress), never buffering the whole file.
    // `on_chunk` returns false to abort early. Falls back to the whole-buffer
    // get() (incl. S3 restore) when the file is not present locally.
    // `version_timestamp` selects a specific version; empty means the current
    // one. Without it a caller wanting an older version has only the unary
    // GetVersion, which materialises the whole file on both sides and is capped
    // by the message limit -- so a link pinned to a past version would stop
    // being streamable the moment the file was edited.
    virtual Result<void> get_stream(const std::string& file_uid,
                                    const std::function<bool(const uint8_t*, size_t)>& on_chunk,
                                    const std::string& user,
                                    const std::vector<std::string>& roles = {},
                                    const std::string& tenant = "",
                                    const std::string& version_timestamp = "");

    // Metadata operations
    virtual Result<FileInfo> stat(const std::string& file_uid, const std::string& user,
                                  const std::vector<std::string>& roles = {},
                                  const std::string& tenant = "");
    virtual Result<bool> exists(const std::string& file_uid, const std::string& tenant = "");

    // Is this target a hidden child / sidecar (a rendition — its parent is a
    // file, per file_renditions.md)? Used to keep the conversion service's
    // thumbnail/preview churn out of the audit log.
    //
    // This used to also return the target's display NAME, which the audit
    // emitters stored in AuditEntry::target_name. That was a leak, not a
    // feature: filenames are party data ("Acme_Corp_Contract_J_Smith.pdf"), and
    // the audit log is a structure designed never to release what it holds, so
    // it was accumulating exactly the class of data an erasure obligation has to
    // be able to remove. The rule is that the log records identifiers and
    // structure, never payload — store the uid and resolve the name at read
    // time, so an erasure automatically stops the log disclosing it.
    // (PROPOSAL_accountability_record.md §5.4.7.)
    //
    // Best-effort: returns false on any lookup failure. At most two DB lookups.
    bool audit_target_is_hidden_child(const std::string& uid, const std::string& tenant);

    // Path operations
    virtual Result<void> move(const std::string& src_uid, const std::string& dst_uid,
                              const std::string& user,
                              const std::vector<std::string>& roles = {},
                              const std::string& tenant = "");
    virtual Result<void> copy(const std::string& src_uid, const std::string& dst_uid,
                              const std::string& user,
                              const std::vector<std::string>& roles = {},
                              const std::string& tenant = "");
    virtual Result<void> rename(const std::string& uid, const std::string& new_name,
                                const std::string& user,
                                const std::vector<std::string>& roles = {},
                                const std::string& tenant = "");

    // Version operations
    virtual Result<std::vector<std::string>> list_versions(const std::string& file_uid,
                                                           const std::string& user,
                                                           const std::vector<std::string>& roles = {},
                                                           const std::string& tenant = "");

    // Timestamp AND uploader, same ordering and the same VIEW_VERSIONS check.
    virtual Result<std::vector<VersionInfo>> list_versions_detailed(const std::string& file_uid,
                                                                    const std::string& user,
                                                                    const std::vector<std::string>& roles,
                                                                    const std::string& tenant = "");
    virtual Result<std::vector<uint8_t>> get_version(const std::string& file_uid,
                                                     const std::string& version_timestamp,
                                                     const std::string& user,
                                                     const std::vector<std::string>& roles = {},
                                                     const std::string& tenant = "");

    virtual Result<bool> restore_to_version(const std::string& file_uid,
                                           const std::string& version_timestamp,
                                           const std::string& user,
                                           const std::vector<std::string>& roles = {},
                                           const std::string& tenant = "");

    // S3 archival operations
    virtual Result<void> backup_to_object_store(const std::string& file_uid,
                                                const std::string& tenant = "");
    virtual Result<void> backup_to_object_store_with_version(const std::string& file_uid,
                                                             const std::string& tenant,
                                                             const std::string& version_timestamp);
    // Permanently destroys every version older than the newest `keep_count`.
    // This is the one genuine destroy-data operation, so it requires the
    // dedicated CULL_VERSIONS permission (validated here as defense-in-depth,
    // in addition to the gRPC boundary) — WRITE is deliberately insufficient.
    virtual Result<void> purge_old_versions(const std::string& file_uid, int keep_count,
                                            const std::string& user,
                                            const std::vector<std::string>& roles = {},
                                            const std::string& tenant = "");

    // Erasure — "true delete" (PROPOSAL_accountability_record.md §5.4).
    //
    // The second and stronger destroy-data operation. Culling compacts history
    // while preserving current state; erasure destroys all content, every
    // version, and everything derived from it, keeping only the record that the
    // file existed. Requires ERASE, which is never conferred by the
    // tenant_admin bypass and never inherited.
    //
    // Returns the erasure id. The erasure is INITIATED, not finished: the core's
    // own content is gone, but derived data lives in other services and each
    // must acknowledge before the obligation is met (§5.4.3). Callers must not
    // report this as a completed erasure — ask GetErasureStatus.
    struct EraseOutcome {
        std::string erasure_id;
        int versions_destroyed = 0;
        int metadata_values_destroyed = 0;
        int renditions_destroyed = 0;
        bool complete = false;          // true only where there are no participants
        std::vector<std::string> awaiting;
    };
    virtual Result<EraseOutcome> erase_file(const std::string& file_uid,
                                            const std::string& reason,
                                            bool retain_name,
                                            const std::string& user,
                                            const std::vector<std::string>& roles = {},
                                            const std::string& tenant = "");

    // The attestation surface (§5.4.3/§5.4.5). These are SERVICE-facing, not
    // user-facing: a consumer polls for what it has not acknowledged and reports
    // back. Authorization is the service-auth capability gate at the gRPC
    // boundary — there is no per-file ACL to apply, because the file's ACLs were
    // destroyed along with everything else about it.
    // `tenant` empty means EVERY tenant — see the proto's all_tenants. Each row
    // carries the tenant it belongs to, because the acknowledgement has to go
    // back to the same schema the erasure lives in.
    virtual Result<std::vector<PendingErasureRow>> list_pending_erasures(
            const std::string& participant, int limit, const std::string& tenant = "");
    virtual Result<ErasureStatusRow> acknowledge_erasure(const std::string& erasure_id,
                                                         const std::string& participant,
                                                         bool complied,
                                                         const std::string& detail,
                                                         const std::string& actor,
                                                         const std::vector<std::string>& roles = {},
                                                         const std::string& tenant = "");
    virtual Result<ErasureStatusRow> get_erasure_status(const std::string& erasure_id,
                                                        const std::string& tenant = "");

    // Which services must acknowledge an erasure before it is complete. Set from
    // deployment configuration, because the platform is deliberately à-la-carte:
    // requiring an ack from a service the deployment does not run would leave
    // every erasure permanently incomplete, which is indistinguishable from a
    // real unmet obligation and would make the alarm useless.
    void set_erasure_participants(const std::vector<std::string>& participants) {
        erasure_participants_ = participants;
    }
    const std::vector<std::string>& erasure_participants() const { return erasure_participants_; }

    // Given a file's version timestamps ordered newest-first (exactly as
    // Database::list_versions returns them — ORDER BY version_timestamp DESC),
    // return the current/newest version, or "" when there are none. Centralizes
    // the "newest == front()" contract so callers never reintroduce the
    // .back() (== oldest) selection bug.
    static std::string newest_version(const std::vector<std::string>& versions_newest_first);

    // Cache management operations
    virtual void update_cache_threshold(double threshold, const std::string& tenant = "");
    virtual Result<double> get_cache_usage_percentage(const std::string& tenant = "") const;

    // Metadata operations (versioned)
    virtual Result<void> set_metadata(const std::string& file_uid, const std::string& key,
                                      const std::string& value, const std::string& user,
                                      const std::vector<std::string>& roles = {},
                                      const std::string& tenant = "");
    virtual Result<std::string> get_metadata(const std::string& file_uid, const std::string& key,
                                             const std::string& user,
                                             const std::vector<std::string>& roles = {},
                                             const std::string& tenant = "");
    virtual Result<std::map<std::string, std::string>> get_all_metadata(const std::string& file_uid,
                                                                        const std::string& user,
                                                                        const std::vector<std::string>& roles = {},
                                                                        const std::string& tenant = "");
    virtual Result<void> delete_metadata(const std::string& file_uid, const std::string& key,
                                         const std::string& user,
                                         const std::vector<std::string>& roles = {},
                                         const std::string& tenant = "");
    virtual Result<std::string> get_metadata_for_version(const std::string& file_uid,
                                                         const std::string& version_timestamp,
                                                         const std::string& key,
                                                         const std::string& user,
                                                         const std::vector<std::string>& roles = {},
                                                         const std::string& tenant = "");
    virtual Result<std::map<std::string, std::string>> get_all_metadata_for_version(const std::string& file_uid,
                                                                                    const std::string& version_timestamp,
                                                                                    const std::string& user,
                                                                                    const std::vector<std::string>& roles = {},
                                                                                    const std::string& tenant = "");

    // ACL operations
    virtual Result<void> grant_permission(const std::string& resource_uid,
                                          const std::string& principal,
                                          int permissions,
                                          const std::string& user,
                                          const std::vector<std::string>& roles = {},
                                          const std::string& tenant = "");
    virtual Result<void> revoke_permission(const std::string& resource_uid,
                                           const std::string& principal,
                                           int permissions,
                                           const std::string& user,
                                           const std::vector<std::string>& roles = {},
                                           const std::string& tenant = "");
    virtual Result<bool> check_permission(const std::string& resource_uid, 
                                          const std::string& user, 
                                          const std::vector<std::string>& roles, 
                                          int required_permissions, 
                                          const std::string& tenant = "");

    // Cleanup
    virtual void shutdown();

    // Helper method to fetch from object store if missing locally
    virtual Result<std::vector<uint8_t>> fetch_from_object_store_if_missing(const std::string& uid,
                                                                           const std::string& version_timestamp,
                                                                           const std::string& tenant = "");

    // Setter for ACL manager
    virtual void set_acl_manager(std::shared_ptr<AclManager> acl_manager) {
        acl_manager_ = acl_manager;
    }

    // Setter for FileCuller
    virtual void set_file_culler(std::unique_ptr<FileCuller> file_culler) {
        file_culler_ = std::move(file_culler);
    }

    // Setter for the optional event sink. When unset (nullptr), no events are
    // emitted — the default. Injected by the server when event queueing is
    // enabled (see event_sink_factory.h).
    virtual void set_event_sink(std::shared_ptr<IEventSink> event_sink) {
        event_sink_ = std::move(event_sink);
    }

    // Emit an acl.changed event from an external ACL path. The gRPC layer calls
    // AclManager directly (it supports ROLE/CLAIM principals and DENY effects
    // that FileSystem::grant_permission doesn't model), so it uses this hook to
    // publish the event. No-op when events are disabled.
    void publish_acl_change(const std::string& tenant, const std::string& resource_uid,
                            const std::string& principal, int permissions,
                            const std::string& user) noexcept {
        emit_acl_event(tenant, resource_uid, principal, permissions, user);
    }

    // Emit the accountability freshness hint (§4.3). The Database layer calls
    // this after a record COMMITS — never before, so the hint can only ever
    // under-assert, and a consumer that acts on it always finds the record.
    // No-op when events are disabled; losing it costs latency, not data.
    void publish_accountability_hint(const std::string& tenant, int64_t seq) noexcept;

    // Emit a role-membership event (role.assigned / role.member_removed /
    // role.deleted) from the gRPC role RPCs, which call RoleManager directly.
    // `member` is empty for role.deleted. No-op when events are disabled.
    void publish_role_change(const std::string& tenant, FileEventType type,
                             const std::string& role, const std::string& member,
                             const std::string& user) noexcept {
        emit_role_event(tenant, type, role, member, user);
    }

private:
    std::vector<std::string> erasure_participants_;
    std::shared_ptr<TenantManager> tenant_manager_;
    std::shared_ptr<AclManager> acl_manager_;
    std::unique_ptr<CacheManager> cache_manager_;
    std::unique_ptr<FileCuller> file_culler_;
    std::shared_ptr<IEventSink> event_sink_;  // optional; nullptr = events disabled

    // Best-effort emission of a file-activity event after a successful mutation.
    // noexcept + fully guarded: never disturbs the calling operation. Enriches
    // the envelope (name/parent/size/version/is_folder/is_rendition) via a
    // best-effort DB read; a rendition is detected when the parent is a file.
    void emit_fs_event(const std::string& tenant, FileEventType type,
                       const std::string& uid, const std::string& user) noexcept;
    // ACL grant/revoke event for a resource + principal.
    void emit_acl_event(const std::string& tenant, const std::string& resource_uid,
                        const std::string& principal, int permissions,
                        const std::string& user) noexcept;
    // Role-membership event (no resource): role +/- member.
    void emit_role_event(const std::string& tenant, FileEventType type,
                         const std::string& role, const std::string& member,
                         const std::string& user) noexcept;
    
    // Helper to get tenant context for operations
    TenantContext* get_tenant_context(const std::string& tenant);
    
    // Helper to validate permissions
    Result<bool> validate_user_permissions(const std::string& resource_uid,
                                          const std::string& user,
                                          const std::vector<std::string>& roles,
                                          int required_permissions,
                                          const std::string& tenant);

    // For a newly created resource: copy ACL_INHERIT-marked rules from parent
    // if it has any, otherwise fall back to default ACLs. The creator always
    // also gets full USER bits via apply_default_acls so they can manage
    // what they just made — inheritance is additive.
    void apply_acls_for_new_resource(const std::string& parent_uid,
                                     const std::string& new_uid,
                                     const std::string& user,
                                     const std::string& tenant);

    // Build the list of ACL grants that should accompany resource creation
    // (creator's default USER bits + every inheritable rule on the parent +
    // an optional OTHER->READ rule when default_world_readable is on). Used
    // by mkdir/touch/copy to feed Database::create_file_with_acls so the
    // file row and its ACLs commit atomically (plan §6.2).
    std::vector<IDatabase::AclGrant> compute_initial_acl_grants(const std::string& parent_uid,
                                                                const std::string& creator,
                                                                const std::string& tenant);

private:
    // Async object store backup functionality
    struct BackupTask {
        std::string file_uid;
        std::string tenant;
        std::string version_timestamp;  // Added to ensure backup uses the correct version
    };

    std::queue<BackupTask> backup_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread backup_worker_thread_;
    std::atomic<bool> backup_worker_running_{false};

    // Method to start the async backup worker
    void start_async_backup_worker();

    // Method to stop the async backup worker
    void stop_async_backup_worker();

    // Method run by the backup worker thread
    void backup_worker_loop();
};

} // namespace fileengine