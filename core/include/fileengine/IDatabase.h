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
#include "accountability.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>

namespace fileengine {

struct FileInfo;
enum class FileType;

class IDatabase {
public:
    virtual ~IDatabase() = default;

    // Connection management
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;

    // Schema management
    virtual Result<void> create_schema() = 0;
    virtual Result<void> drop_schema() = 0;

    // File metadata operations (using UUIDs instead of paths/ids)
    virtual Result<std::string> insert_file(const std::string& uid, const std::string& name,
                                            const std::string& path, const std::string& parent_uid,
                                            FileType type, const std::string& owner,
                                            int permissions, const std::string& tenant = "") = 0;
    virtual Result<void> update_file_modified(const std::string& uid, const std::string& tenant = "") = 0;
    virtual Result<void> update_file_current_version(const std::string& uid, const std::string& version_timestamp, const std::string& tenant = "") = 0;
    // Update the stored byte size of a file's current content. Non-pure so
    // existing mocks need no change; the concrete Database overrides it.
    virtual Result<void> update_file_size(const std::string& /*uid*/, int64_t /*size*/, const std::string& /*tenant*/ = "") {
        return Result<void>::ok();
    }
    virtual Result<bool> delete_file(const std::string& uid, const std::string& tenant = "") = 0;
    virtual Result<bool> undelete_file(const std::string& uid, const std::string& tenant = "") = 0;
    virtual Result<std::optional<FileInfo>> get_file_by_uid(const std::string& uid, const std::string& tenant = "") = 0;
    virtual Result<std::optional<FileInfo>> get_file_by_path(const std::string& path, const std::string& tenant = "") = 0;  // Keep for backward compatibility
    virtual Result<void> update_file_name(const std::string& uid, const std::string& new_name, const std::string& tenant = "") = 0;
    virtual Result<std::vector<FileInfo>> list_files_in_directory(const std::string& parent_uid, const std::string& tenant = "") = 0;
    virtual Result<std::vector<FileInfo>> list_files_in_directory_with_deleted(const std::string& parent_uid, const std::string& tenant = "") = 0;
    virtual Result<std::vector<FileInfo>> list_all_files(const std::string& tenant = "") = 0;
    virtual Result<std::optional<FileInfo>> get_file_by_name_and_parent(const std::string& name, const std::string& parent_uid, const std::string& tenant = "") = 0;
    virtual Result<std::optional<FileInfo>> get_file_by_name_and_parent_include_deleted(const std::string& name, const std::string& parent_uid, const std::string& tenant = "") = 0;
    virtual Result<int64_t> get_file_size(const std::string& file_uid, const std::string& tenant = "") = 0;
    virtual Result<int64_t> get_directory_size(const std::string& dir_uid, const std::string& tenant = "") = 0;
    virtual Result<std::optional<FileInfo>> get_file_by_uid_include_deleted(const std::string& uid, const std::string& tenant = "") = 0;
    virtual Result<void> update_file_parent(const std::string& uid, const std::string& new_parent_uid, const std::string& tenant = "") = 0;

    // Path-to-UUID mapping (for backward compatibility)
    virtual Result<std::string> path_to_uid(const std::string& path, const std::string& tenant = "") = 0;
    virtual Result<std::vector<std::string>> uid_to_path(const std::string& uid, const std::string& tenant = "") = 0;

    // Version operations (using UUIDs and timestamp strings)
    virtual Result<int64_t> insert_version(const std::string& file_uid, const std::string& version_timestamp,
                                            int64_t size, const std::string& storage_path,
                                            const std::string& revised_by = "", const std::string& tenant = "") = 0;
    virtual Result<std::optional<std::string>> get_version_storage_path(const std::string& file_uid, const std::string& version_timestamp, const std::string& tenant = "") = 0;
    virtual Result<std::vector<std::string>> list_versions(const std::string& file_uid, const std::string& tenant = "") = 0;
    // Timestamp AND uploader. list_versions() above is the timestamp-only form,
    // kept so existing callers and mocks are unaffected.
    virtual Result<std::vector<VersionInfo>> list_versions_detailed(const std::string& file_uid, const std::string& tenant = "") = 0;
    // Remove a single version row for a file. Non-pure so existing mocks need
    // no update; the concrete Database overrides it.
    virtual Result<bool> delete_version(const std::string& /*file_uid*/, const std::string& /*version_timestamp*/, const std::string& /*tenant*/ = "") {
        return Result<bool>::err("delete_version not implemented");
    }

    // Version restoration operations
    virtual Result<bool> restore_to_version(const std::string& file_uid, const std::string& version_timestamp, const std::string& user, const std::string& tenant = "") = 0;

    // Metadata operations (versioned by timestamp)
    virtual Result<void> set_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& value, const std::string& tenant = "") = 0;
    virtual Result<std::optional<std::string>> get_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& tenant = "") = 0;
    virtual Result<std::map<std::string, std::string>> get_all_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& tenant = "") = 0;
    virtual Result<void> delete_metadata(const std::string& file_uid, const std::string& version_timestamp, const std::string& key, const std::string& tenant = "") = 0;

    // Direct SQL execution for testing
    virtual Result<void> execute(const std::string& sql, const std::string& tenant = "") = 0;
    virtual Result<std::vector<std::vector<std::string>>> query(const std::string& sql, const std::string& tenant = "") = 0;

    // Cache tracking operations
    virtual Result<void> update_file_access_stats(const std::string& uid, const std::string& user, const std::string& tenant = "") = 0;
    virtual Result<std::vector<std::string>> get_least_accessed_files(int limit = 10, const std::string& tenant = "") = 0;
    virtual Result<std::vector<std::string>> get_infrequently_accessed_files(int days_threshold = 30, const std::string& tenant = "") = 0;
    virtual Result<int64_t> get_storage_usage(const std::string& tenant = "") = 0;
    virtual Result<int64_t> get_storage_capacity(const std::string& tenant = "") = 0;

    // Tenant management operations
    // `ctx` attributes the tenant's creation in the GLOBAL accountability record
    // (§7.3). Only a schema that did not already exist writes one — this is
    // called lazily on nearly every tenant context lookup, so recording
    // unconditionally would fill the global chain with duplicate "created"
    // rows for tenants that have existed for months.
    virtual Result<void> create_tenant_schema(const std::string& tenant,
                                              const AccountabilityContext& ctx) = 0;
    virtual Result<bool> tenant_schema_exists(const std::string& tenant) = 0;
    // DROP SCHEMA CASCADE — the platform's most destructive operation. The
    // tenant's own accountability history goes with it, by design (§7.3): a
    // tenant's history is tenant data. The record that the deletion HAPPENED is
    // written to the global table first, in the same transaction, because a
    // record inside the schema being dropped would delete itself.
    virtual Result<void> cleanup_tenant_data(const std::string& tenant,
                                             const AccountabilityContext& ctx) = 0;
    virtual Result<std::vector<std::string>> list_tenants() = 0;

    // ACL operations
    struct AclEntry {
        std::string resource_uid;
        std::string principal;
        int type;
        int permissions;
        int effect = 0; // 0 = ALLOW, 1 = DENY (matches AclEffect)
    };

    // A pending ACL grant tied to a not-yet-created resource. Used by
    // create_file_with_acls to wrap the file row + all default/inherited
    // ACLs in a single transaction.
    struct AclGrant {
        std::string principal;
        int type;
        int permissions;
        std::string performed_by;
        int effect = 0; // 0 = ALLOW, 1 = DENY
    };

    // Atomic resource creation: insert the file row and apply all ACL grants
    // in a single Postgres transaction. Either everything commits or nothing
    // does — a crash mid-creation can never leave a file without ACLs.
    // See plan §6.2.
    virtual Result<std::string> create_file_with_acls(const std::string& uid,
                                                       const std::string& name,
                                                       const std::string& path,
                                                       const std::string& parent_uid,
                                                       FileType type,
                                                       const std::string& owner,
                                                       int permissions,
                                                       const std::vector<AclGrant>& acl_grants,
                                                       const std::string& tenant = "") = 0;

    // `ctx` names who is making the change and from where. It replaces the old
    // free-standing performed_by: ctx.actor lands in granted_by and the
    // acl_audit table exactly as before, AND drives the accountability record
    // written in the same transaction as the ACL change. ctx.actor must not be
    // empty — an unattributed authorization change is refused rather than
    // recorded as anonymous (§5.1).
    //
    // ctx.mode selects whether this write is its own accountability event. See
    // AccountabilityMode: every deliberate grant/revoke uses Record; only
    // creation-time default and inherited ACLs use PartOfCreation.
    //
    // effect (default 0 = ALLOW) selects which logical row for the
    // (resource, principal, type) tuple is updated — ALLOW and DENY are stored
    // separately so they can coexist for the same principal.
    virtual Result<void> add_acl(const std::string& resource_uid, const std::string& principal,
                                 int type, int permissions,
                                 const std::string& tenant,
                                 const AccountabilityContext& ctx,
                                 int effect = 0) = 0;
    // Clear the bits in `permissions` from the matching ACL row. If the
    // resulting permission bitmask is zero the row is deleted. Pass -1 (all
    // bits set) to fully revoke the principal's row in one call. effect
    // (default 0 = ALLOW) selects which logical row to revoke from.
    virtual Result<void> remove_acl(const std::string& resource_uid, const std::string& principal,
                                    int type, int permissions,
                                    const std::string& tenant,
                                    const AccountabilityContext& ctx,
                                    int effect = 0) = 0;
    virtual Result<std::vector<AclEntry>> get_acls_for_resource(const std::string& resource_uid,
                                                                 const std::string& tenant = "") = 0;
    virtual Result<std::vector<AclEntry>> get_user_acls(const std::string& resource_uid,
                                                        const std::string& principal,
                                                        int type,
                                                        const std::string& tenant = "") = 0;
    // Catalog the distinct CLAIM-type ACL principals ("key=value") defined across
    // the tenant, for an ACL editor's claim type-ahead. `prefix` is a
    // case-insensitive filter ("" = all); `limit` caps results (<= 0 = no cap).
    virtual Result<std::vector<std::string>> list_claims(const std::string& prefix,
                                                         int limit,
                                                         const std::string& tenant = "") = 0;

    // Role management operations. Each takes the acting identity for the same
    // reason the ACL calls do: the accountability record is written in the
    // operation's own transaction, so the layer that owns the transaction has
    // to know who is acting. Note the coverage limit worth being honest about
    // (§7.2): core `user_roles` is nearly always empty on this platform because
    // roles are request-borne from LDAP groups, so these records cover local
    // role management only — real role accountability comes from ldap_manager
    // recording group-membership changes.
    virtual Result<void> create_role(const std::string& role, const std::string& tenant,
                                     const AccountabilityContext& ctx) = 0;
    virtual Result<void> delete_role(const std::string& role, const std::string& tenant,
                                     const AccountabilityContext& ctx) = 0;
    virtual Result<void> assign_user_to_role(const std::string& user, const std::string& role,
                                             const std::string& tenant,
                                             const AccountabilityContext& ctx) = 0;
    virtual Result<void> remove_user_from_role(const std::string& user, const std::string& role,
                                               const std::string& tenant,
                                               const AccountabilityContext& ctx) = 0;
    virtual Result<std::vector<std::string>> get_roles_for_user(const std::string& user,
                                                                const std::string& tenant = "") = 0;
    virtual Result<std::vector<std::string>> get_users_for_role(const std::string& role,
                                                                const std::string& tenant = "") = 0;
    virtual Result<std::vector<std::string>> get_all_roles(const std::string& tenant = "") = 0;

    // Cull a file's old versions and record the cull, in ONE transaction.
    //
    // Batched deliberately: this is one logical act, and §5.3.2 requires bulk
    // changes to be a single chained record rather than N — both because the
    // chain serializes per tenant and because "the cull removed 40 versions" is
    // the true statement, not forty separate destructions.
    //
    // `cut_version_timestamp` is the oldest version RETAINED, so the record
    // states where the history was cut without listing what was destroyed.
    // Non-pure so ACL/RBAC mocks need no change.
    virtual Result<int> purge_versions(const std::string& /*file_uid*/,
                                       const std::vector<std::string>& /*version_timestamps*/,
                                       const std::string& /*cut_version_timestamp*/,
                                       int /*keep_count*/,
                                       const std::string& /*tenant*/,
                                       const AccountabilityContext& /*ctx*/) {
        return Result<int>::err("version culling is not available from this database implementation");
    }

    // ── The accountability pull surface (§4.3) ──────────────────────────────
    // Read forward by cursor. `newer_than_micros` is the consumer's
    // `recorded_until` watermark in epoch microseconds; because ts is assigned
    // under the chain lock and forced strictly monotonic per tenant (§5.3.3),
    // a strictly-greater-than fetch is exact — it cannot skip a record, return
    // one twice, observe one that was rolled back, or be overtaken by an
    // earlier-stamped record committing later.
    //
    // Pass kGlobalChainKey as `tenant` to read the global tenant-lifecycle
    // chain. Non-pure so the ACL/RBAC mocks need no change; the concrete
    // Database is the only implementation that can honour it.
    virtual Result<std::vector<StoredAccountabilityRecord>> list_accountability_records(
            const std::string& /*tenant*/, std::int64_t /*newer_than_micros*/,
            int /*limit*/, bool& /*has_more*/) {
        return Result<std::vector<StoredAccountabilityRecord>>::err(
            "accountability records are not available from this database implementation");
    }
};

} // namespace fileengine