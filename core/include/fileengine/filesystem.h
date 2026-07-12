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
    virtual Result<void> get_stream(const std::string& file_uid,
                                    const std::function<bool(const uint8_t*, size_t)>& on_chunk,
                                    const std::string& user,
                                    const std::vector<std::string>& roles = {},
                                    const std::string& tenant = "");

    // Metadata operations
    virtual Result<FileInfo> stat(const std::string& file_uid, const std::string& user,
                                  const std::vector<std::string>& roles = {},
                                  const std::string& tenant = "");
    virtual Result<bool> exists(const std::string& file_uid, const std::string& tenant = "");

    // True if `uid` is a hidden child / sidecar — a file whose parent is itself a
    // file (a rendition, per file_renditions.md). Best-effort: false on any lookup
    // failure. Used by the audit path to suppress conversion-service noise.
    bool is_hidden_child(const std::string& uid, const std::string& tenant);

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
    virtual Result<void> purge_old_versions(const std::string& file_uid, int keep_count,
                                            const std::string& tenant = "");

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
    // AclManager directly (it supports GROUP/ROLE principals and DENY effects
    // that FileSystem::grant_permission doesn't model), so it uses this hook to
    // publish the event. No-op when events are disabled.
    void publish_acl_change(const std::string& tenant, const std::string& resource_uid,
                            const std::string& principal, int permissions,
                            const std::string& user) noexcept {
        emit_acl_event(tenant, resource_uid, principal, permissions, user);
    }

    // Emit a role-membership event (role.assigned / role.member_removed /
    // role.deleted) from the gRPC role RPCs, which call RoleManager directly.
    // `member` is empty for role.deleted. No-op when events are disabled.
    void publish_role_change(const std::string& tenant, FileEventType type,
                             const std::string& role, const std::string& member,
                             const std::string& user) noexcept {
        emit_role_event(tenant, type, role, member, user);
    }

private:
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