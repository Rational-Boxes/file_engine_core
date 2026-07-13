// Security semantics regression tests for AclManager — the single choke point
// every permission-gated operation funnels through. These drive AclManager with
// an in-memory mock DB (no live database) and pin the authorization guarantees
// the rest of the system relies on:
//
//   * permission BITS are independent — WRITE does not confer DELETE / UNDELETE /
//     RESTORE_TO_VERSION / VIEW_VERSIONS / MANAGE_ACL / CULL_VERSIONS
//   * MANAGE_ACL is not implied by WRITE (no write->ACL self-escalation)
//   * CULL_VERSIONS (destroy history) is never granted by default
//   * within a tier, a DENY beats an ALLOW ("deny wins in-tier")
//   * system_admin is a role-based bypass (revoking the role revokes access)
//   * CLAIM (ABAC) rules match only the exact key=value presented
//   * strict mode (default_read=false) denies principals with no matching rule
//
// It also pins the hierarchical resolution order (finding M1, resolved as an
// intended identity hierarchy): the most specific tier that touches a bit
// settles it — USER > CLAIM > ROLE/GROUP > OTHER — then read-only default. DENY
// is absolute only within a tier, so a more specific ALLOW overrides a less
// specific DENY.
//
// Hermetic: links fileengine_core + mock DB; run via `ctest -R test_security_acl`.

#include <cassert>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fileengine/IDatabase.h"
#include "fileengine/acl_manager.h"
#include "fileengine/types.h"

using namespace fileengine;

// ---------------------------------------------------------------------------
// Minimal in-memory IDatabase. Only the ACL/file-lookup methods carry behavior;
// everything else is an unused no-op (mirrors tests/test_deleted_reachability).
// ---------------------------------------------------------------------------
class MockDatabase : public IDatabase {
public:
    struct Node { std::string parent; bool is_dir; bool deleted; };
    std::map<std::string, Node> tree_;
    std::map<std::string, std::vector<AclEntry>> acls_;

    void add_node(const std::string& uid, const std::string& parent, bool is_dir) {
        tree_[uid] = Node{parent, is_dir, false};
    }

    FileInfo make_info(const std::string& uid, const Node& n) {
        FileInfo info;
        info.uid = uid;
        info.name = uid;
        info.parent_uid = n.parent;
        info.type = n.is_dir ? FileType::DIRECTORY : FileType::REGULAR_FILE;
        info.deleted = n.deleted;
        return info;
    }

    Result<std::optional<FileInfo>> get_file_by_uid(const std::string& uid, const std::string& = "") override {
        auto it = tree_.find(uid);
        if (it == tree_.end() || it->second.deleted)
            return Result<std::optional<FileInfo>>::ok(std::nullopt);
        return Result<std::optional<FileInfo>>::ok(make_info(uid, it->second));
    }
    Result<std::optional<FileInfo>> get_file_by_uid_include_deleted(const std::string& uid, const std::string& = "") override {
        auto it = tree_.find(uid);
        if (it == tree_.end())
            return Result<std::optional<FileInfo>>::ok(std::nullopt);
        return Result<std::optional<FileInfo>>::ok(make_info(uid, it->second));
    }
    Result<std::vector<AclEntry>> get_acls_for_resource(const std::string& resource_uid, const std::string& = "") override {
        auto it = acls_.find(resource_uid);
        if (it != acls_.end()) return Result<std::vector<AclEntry>>::ok(it->second);
        return Result<std::vector<AclEntry>>::ok(std::vector<AclEntry>{});
    }
    Result<std::vector<std::string>> get_roles_for_user(const std::string&, const std::string& = "") override {
        return Result<std::vector<std::string>>::ok(std::vector<std::string>{});
    }

    // ---- everything below is an unused no-op for these tests ----
    bool connect() override { return true; }
    void disconnect() override {}
    bool is_connected() const override { return true; }
    Result<void> create_schema() override { return Result<void>::ok(); }
    Result<void> drop_schema() override { return Result<void>::ok(); }
    Result<std::string> insert_file(const std::string& uid, const std::string&, const std::string&, const std::string&, FileType, const std::string&, int, const std::string& = "") override { return Result<std::string>::ok(uid); }
    Result<std::string> create_file_with_acls(const std::string& uid, const std::string&, const std::string&, const std::string&, FileType, const std::string&, int, const std::vector<AclGrant>&, const std::string& = "") override { return Result<std::string>::ok(uid); }
    Result<void> update_file_modified(const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<void> update_file_current_version(const std::string&, const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<bool> delete_file(const std::string&, const std::string& = "") override { return Result<bool>::ok(true); }
    Result<bool> undelete_file(const std::string&, const std::string& = "") override { return Result<bool>::ok(true); }
    Result<std::optional<FileInfo>> get_file_by_path(const std::string&, const std::string& = "") override { return Result<std::optional<FileInfo>>::ok(std::nullopt); }
    Result<void> update_file_name(const std::string&, const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<std::vector<FileInfo>> list_files_in_directory(const std::string&, const std::string& = "") override { return Result<std::vector<FileInfo>>::ok({}); }
    Result<std::vector<FileInfo>> list_files_in_directory_with_deleted(const std::string&, const std::string& = "") override { return Result<std::vector<FileInfo>>::ok({}); }
    Result<std::vector<FileInfo>> list_all_files(const std::string& = "") override { return Result<std::vector<FileInfo>>::ok({}); }
    Result<std::optional<FileInfo>> get_file_by_name_and_parent(const std::string&, const std::string&, const std::string& = "") override { return Result<std::optional<FileInfo>>::ok(std::nullopt); }
    Result<std::optional<FileInfo>> get_file_by_name_and_parent_include_deleted(const std::string&, const std::string&, const std::string& = "") override { return Result<std::optional<FileInfo>>::ok(std::nullopt); }
    Result<int64_t> get_file_size(const std::string&, const std::string& = "") override { return Result<int64_t>::ok(0); }
    Result<int64_t> get_directory_size(const std::string&, const std::string& = "") override { return Result<int64_t>::ok(0); }
    Result<void> update_file_parent(const std::string&, const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<std::string> path_to_uid(const std::string&, const std::string& = "") override { return Result<std::string>::ok(""); }
    Result<std::vector<std::string>> uid_to_path(const std::string&, const std::string& = "") override { return Result<std::vector<std::string>>::ok({}); }
    Result<int64_t> insert_version(const std::string&, const std::string&, int64_t, const std::string&, const std::string& = "", const std::string& = "") override { return Result<int64_t>::ok(0); }
    Result<std::optional<std::string>> get_version_storage_path(const std::string&, const std::string&, const std::string& = "") override { return Result<std::optional<std::string>>::ok(std::nullopt); }
    Result<std::vector<std::string>> list_versions(const std::string&, const std::string& = "") override { return Result<std::vector<std::string>>::ok({}); }
    Result<bool> restore_to_version(const std::string&, const std::string&, const std::string&, const std::string& = "") override { return Result<bool>::ok(true); }
    Result<void> set_metadata(const std::string&, const std::string&, const std::string&, const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<std::optional<std::string>> get_metadata(const std::string&, const std::string&, const std::string&, const std::string& = "") override { return Result<std::optional<std::string>>::ok(std::nullopt); }
    Result<std::map<std::string, std::string>> get_all_metadata(const std::string&, const std::string&, const std::string& = "") override { return Result<std::map<std::string, std::string>>::ok({}); }
    Result<void> delete_metadata(const std::string&, const std::string&, const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<void> execute(const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<std::vector<std::vector<std::string>>> query(const std::string&, const std::string& = "") override { return Result<std::vector<std::vector<std::string>>>::ok({}); }
    Result<void> update_file_access_stats(const std::string&, const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<std::vector<std::string>> get_least_accessed_files(int = 10, const std::string& = "") override { return Result<std::vector<std::string>>::ok({}); }
    Result<std::vector<std::string>> get_infrequently_accessed_files(int = 30, const std::string& = "") override { return Result<std::vector<std::string>>::ok({}); }
    Result<int64_t> get_storage_usage(const std::string& = "") override { return Result<int64_t>::ok(0); }
    Result<int64_t> get_storage_capacity(const std::string& = "") override { return Result<int64_t>::ok(0); }
    Result<void> create_tenant_schema(const std::string&) override { return Result<void>::ok(); }
    Result<bool> tenant_schema_exists(const std::string&) override { return Result<bool>::ok(true); }
    Result<void> cleanup_tenant_data(const std::string&) override { return Result<void>::ok(); }
    Result<std::vector<std::string>> list_tenants() override { return Result<std::vector<std::string>>::ok({}); }
    Result<void> add_acl(const std::string& r, const std::string& p, int t, int perm, const std::string& = "", const std::string& = "", int eff = 0) override { AclEntry e; e.resource_uid = r; e.principal = p; e.type = t; e.permissions = perm; e.effect = eff; acls_[r].push_back(e); return Result<void>::ok(); }
    Result<void> remove_acl(const std::string&, const std::string&, int, int, const std::string& = "", const std::string& = "", int = 0) override { return Result<void>::ok(); }
    Result<std::vector<AclEntry>> get_user_acls(const std::string& r, const std::string& p, int t, const std::string& = "") override { std::vector<AclEntry> out; auto it = acls_.find(r); if (it != acls_.end()) for (auto& e : it->second) if (e.principal == p && e.type == t) out.push_back(e); return Result<std::vector<AclEntry>>::ok(out); }
    Result<std::vector<std::string>> list_claims(const std::string&, int, const std::string& = "") override { return Result<std::vector<std::string>>::ok({}); }
    Result<void> create_role(const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<void> delete_role(const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<void> assign_user_to_role(const std::string&, const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<void> remove_user_from_role(const std::string&, const std::string&, const std::string& = "") override { return Result<void>::ok(); }
    Result<std::vector<std::string>> get_users_for_role(const std::string&, const std::string& = "") override { return Result<std::vector<std::string>>::ok({}); }
    Result<std::vector<std::string>> get_all_roles(const std::string& = "") override { return Result<std::vector<std::string>>::ok({}); }
};

static int g_checks = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            std::cerr << "  ✗ FAILED: " << (msg) << " (line " << __LINE__ << ")\n"; \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

namespace {
const int P_READ    = static_cast<int>(Permission::READ);
const int P_WRITE   = static_cast<int>(Permission::WRITE);
const int P_DELETE  = static_cast<int>(Permission::DELETE);
const int P_UNDEL   = static_cast<int>(Permission::UNDELETE);
const int P_VIEWV   = static_cast<int>(Permission::VIEW_VERSIONS);
const int P_RESTORE = static_cast<int>(Permission::RESTORE_TO_VERSION);
const int P_MANAGE  = static_cast<int>(Permission::MANAGE_ACL);
const int P_CULL    = static_cast<int>(Permission::CULL_VERSIONS);

bool can(AclManager& acl, const std::string& uid, const std::string& user,
         const std::vector<std::string>& roles, int perm,
         const std::map<std::string, std::string>& claims = {}) {
    return acl.check_permission(uid, user, roles, perm, "", claims).value;
}
}  // namespace

// A WRITE-only grant must NOT confer any of the dedicated destructive/version
// permissions. This is the ACL-layer invariant behind finding H1. The gRPC
// handlers + FileSystem layer now gate RemoveFile/RemoveDirectory on DELETE,
// UndeleteFile on UNDELETE, RestoreToVersion on RESTORE_TO_VERSION, and
// ListVersions/GetVersion on VIEW_VERSIONS/RETRIEVE_BACK_VERSION (H1 fixed), so
// these bits are real gates — this test proves they are genuinely independent
// of WRITE. Resource creators keep full control (apply_default_acls grants them
// the whole set); only narrowly-granted WRITE-only collaborators are affected.
void test_write_does_not_imply_destructive() {
    std::cout << "test_write_does_not_imply_destructive\n";
    auto db = std::make_shared<MockDatabase>();
    db->add_node("F", "", false);
    AclManager acl(db);
    acl.set_default_read(false);  // strict: only explicit grants count
    acl.grant_permission("F", "alice", PrincipalType::USER, P_WRITE);

    CHECK(can(acl, "F", "alice", {}, P_WRITE),  "WRITE grant confers WRITE");
    CHECK(!can(acl, "F", "alice", {}, P_DELETE),  "WRITE must NOT confer DELETE");
    CHECK(!can(acl, "F", "alice", {}, P_UNDEL),   "WRITE must NOT confer UNDELETE");
    CHECK(!can(acl, "F", "alice", {}, P_RESTORE), "WRITE must NOT confer RESTORE_TO_VERSION");
    CHECK(!can(acl, "F", "alice", {}, P_VIEWV),   "WRITE must NOT confer VIEW_VERSIONS");
    CHECK(!can(acl, "F", "alice", {}, P_READ),    "WRITE must NOT confer READ in strict mode");
}

// MANAGE_ACL is the guard against write->ACL self-escalation: a user who can
// write must not be able to rewrite the resource's ACLs.
void test_write_does_not_imply_manage_acl() {
    std::cout << "test_write_does_not_imply_manage_acl\n";
    auto db = std::make_shared<MockDatabase>();
    db->add_node("F", "", false);
    AclManager acl(db);
    acl.set_default_read(false);
    acl.grant_permission("F", "alice", PrincipalType::USER, P_WRITE | P_READ);
    CHECK(!can(acl, "F", "alice", {}, P_MANAGE), "WRITE+READ must NOT confer MANAGE_ACL");
}

// The one irreversibly destructive permission (destroy version history) must
// never be handed out by the default-ACL path granted to a resource creator.
void test_cull_never_default() {
    std::cout << "test_cull_never_default\n";
    auto db = std::make_shared<MockDatabase>();
    db->add_node("F", "", false);
    AclManager acl(db);
    acl.apply_default_acls("F", "creator");
    CHECK(!can(acl, "F", "creator", {}, P_CULL),
          "creator default ACLs must NOT include CULL_VERSIONS");
    CHECK(!can(acl, "F", "outsider", {}, P_CULL),
          "a non-creator must NOT have CULL_VERSIONS");
}

// Within one tier, a DENY overrides an ALLOW for the same bit ("deny wins
// in-tier"). This is the property the ACL model leans on for lockdowns.
void test_deny_wins_within_tier() {
    std::cout << "test_deny_wins_within_tier\n";
    auto db = std::make_shared<MockDatabase>();
    db->add_node("F", "", false);
    AclManager acl(db);
    acl.grant_permission("F", "alice", PrincipalType::USER, P_WRITE, "", "", AclEffect::ALLOW);
    acl.grant_permission("F", "alice", PrincipalType::USER, P_WRITE, "", "", AclEffect::DENY);
    CHECK(!can(acl, "F", "alice", {}, P_WRITE),
          "same-tier DENY must override same-tier ALLOW");
}

// system_admin is a role-based bypass: access follows the role. Dropping the
// role must drop the bypass (so a stale grant can't linger).
void test_system_admin_is_role_scoped() {
    std::cout << "test_system_admin_is_role_scoped\n";
    auto db = std::make_shared<MockDatabase>();
    db->add_node("F", "", false);
    AclManager acl(db);
    acl.set_default_read(false);  // no ambient access
    CHECK(can(acl, "F", "root", {"system_admin"}, P_CULL),
          "system_admin role bypasses ACL checks");
    CHECK(!can(acl, "F", "root", {}, P_CULL),
          "same user WITHOUT the role has no bypass");
}

// CLAIM (ABAC) rules match only the exact key=value presented in the request.
void test_claim_abac_exact_match() {
    std::cout << "test_claim_abac_exact_match\n";
    auto db = std::make_shared<MockDatabase>();
    db->add_node("F", "", false);
    AclManager acl(db);
    acl.set_default_read(false);
    acl.grant_permission("F", "department=eng", PrincipalType::CLAIM, P_READ);
    CHECK(can(acl,  "F", "bob", {}, P_READ, {{"department", "eng"}}),
          "matching claim grants access");
    CHECK(!can(acl, "F", "bob", {}, P_READ, {{"department", "sales"}}),
          "wrong claim value denies");
    CHECK(!can(acl, "F", "bob", {}, P_READ, {}),
          "absent claim denies");
}

// Strict mode (default_read=false) denies a principal with no matching rule,
// while the default read-by-default mode grants READ on undecided bits.
void test_default_read_toggle() {
    std::cout << "test_default_read_toggle\n";
    auto db = std::make_shared<MockDatabase>();
    db->add_node("F", "", false);

    AclManager strict(db);
    strict.set_default_read(false);
    CHECK(!can(strict, "F", "nobody", {}, P_READ),
          "strict mode denies READ with no matching rule");

    AclManager lenient(db);  // default_read defaults to true
    CHECK(can(lenient, "F", "nobody", {}, P_READ),
          "read-by-default grants READ on undecided bits");
}

// Hierarchical precedence (finding M1, resolved as an intended identity
// hierarchy): the most specific tier that touches a bit settles it —
// USER > CLAIM > ROLE/GROUP > OTHER — then read-only default. DENY is absolute
// only within a tier, so a more specific ALLOW overrides a less specific DENY.
// Each sub-case uses a distinct resource uid so grants don't accumulate.
void test_hierarchical_precedence() {
    std::cout << "test_hierarchical_precedence\n";
    auto db = std::make_shared<MockDatabase>();
    AclManager acl(db);
    acl.set_default_read(false);

    // USER ALLOW overrides ROLE DENY (USER tier 0 settles before ROLE tier 2).
    db->add_node("u_over_r", "", false);
    acl.grant_permission("u_over_r", "alice", PrincipalType::USER, P_WRITE, "", "", AclEffect::ALLOW);
    acl.grant_permission("u_over_r", "contractors", PrincipalType::ROLE, P_WRITE, "", "", AclEffect::DENY);
    CHECK(can(acl, "u_over_r", "alice", {"contractors"}, P_WRITE),
          "USER ALLOW overrides ROLE DENY");

    // CLAIM ALLOW overrides ROLE DENY (CLAIM tier 1 more specific than ROLE tier 2).
    db->add_node("c_over_r", "", false);
    acl.grant_permission("c_over_r", "dept=eng", PrincipalType::CLAIM, P_WRITE, "", "", AclEffect::ALLOW);
    acl.grant_permission("c_over_r", "contractors", PrincipalType::ROLE, P_WRITE, "", "", AclEffect::DENY);
    CHECK(can(acl, "c_over_r", "bob", {"contractors"}, P_WRITE, {{"dept", "eng"}}),
          "CLAIM ALLOW overrides ROLE DENY");

    // USER DENY overrides CLAIM ALLOW (USER is the most specific tier).
    db->add_node("u_deny", "", false);
    acl.grant_permission("u_deny", "carol", PrincipalType::USER, P_WRITE, "", "", AclEffect::DENY);
    acl.grant_permission("u_deny", "dept=eng", PrincipalType::CLAIM, P_WRITE, "", "", AclEffect::ALLOW);
    CHECK(!can(acl, "u_deny", "carol", {}, P_WRITE, {{"dept", "eng"}}),
          "USER DENY overrides CLAIM ALLOW");

    // ROLE ALLOW overrides everyone(OTHER) DENY; a user without the role is still denied.
    db->add_node("r_over_o", "", false);
    acl.grant_permission("r_over_o", "staff", PrincipalType::ROLE, P_READ, "", "", AclEffect::ALLOW);
    acl.grant_permission("r_over_o", "everyone", PrincipalType::OTHER, P_READ, "", "", AclEffect::DENY);
    CHECK(can(acl, "r_over_o", "dave", {"staff"}, P_READ),
          "ROLE ALLOW overrides everyone(OTHER) DENY");
    CHECK(!can(acl, "r_over_o", "eve", {}, P_READ),
          "a user without the role is denied by everyone DENY");
}

// H2 — tenant-scoped superuser boundary. tenant_admin grants full control within
// the caller's OWN tenant but is NOT the global system_admin operator, so a
// per-tenant admin can never inherit global-operator capabilities.
void test_tenant_admin_is_scoped_not_global() {
    std::cout << "test_tenant_admin_is_scoped_not_global\n";
    auto db = std::make_shared<MockDatabase>();
    db->add_node("F", "", false);
    AclManager acl(db);

    // A tenant_admin is a tenant admin + an admin — but NOT a global system_admin.
    CHECK(acl.is_tenant_admin("carol", {"tenant_admin"}, "acme"), "tenant_admin -> is_tenant_admin");
    CHECK(acl.is_admin("carol", {"tenant_admin"}, "acme"),        "tenant_admin -> is_admin");
    CHECK(!acl.is_system_admin("carol", {"tenant_admin"}, "acme"),
          "tenant_admin must NOT be a global system_admin (H2 boundary)");

    // A system_admin is a global operator + admin, but not the tenant role.
    CHECK(acl.is_system_admin("root", {"system_admin"}, "acme"),  "system_admin -> is_system_admin");
    CHECK(acl.is_admin("root", {"system_admin"}, "acme"),         "system_admin -> is_admin");
    CHECK(!acl.is_tenant_admin("root", {"system_admin"}, "acme"), "system_admin is not tenant_admin");

    // A plain user is neither.
    CHECK(!acl.is_admin("bob", {"users"}, "acme"), "plain user is not an admin");
}

// Both admin roles fully bypass ACLs within the tenant; a plain user does not.
void test_admin_roles_bypass_within_tenant() {
    std::cout << "test_admin_roles_bypass_within_tenant\n";
    auto db = std::make_shared<MockDatabase>();
    db->add_node("F", "", false);   // no grants at all
    AclManager acl(db);
    acl.set_default_read(false);    // strict: nothing allowed without a grant

    // tenant_admin and system_admin both get full control, incl. the destructive
    // CULL_VERSIONS bit that is never granted by default.
    CHECK(can(acl, "F", "carol", {"tenant_admin"}, P_CULL), "tenant_admin bypasses to CULL within tenant");
    CHECK(can(acl, "F", "root",  {"system_admin"}, P_CULL), "system_admin bypasses to CULL");
    int eff = acl.get_effective_permissions("F", "carol", {"tenant_admin"}, "acme").value;
    CHECK((eff & P_CULL) && (eff & P_MANAGE) && (eff & P_DELETE),
          "tenant_admin effective permission set is all bits");
    // a plain user with no grant is denied in strict mode
    CHECK(!can(acl, "F", "bob", {"users"}, P_READ), "plain user denied without a grant");
}

// The boundary is STRUCTURAL: each tenant has its own AclManager + data store, so
// the same tenant_admin role, evaluated by a different tenant's manager, only ever
// sees that tenant's data. Two isolated mocks stand in for two tenant schemas.
void test_tenant_admin_boundary_is_per_manager() {
    std::cout << "test_tenant_admin_boundary_is_per_manager\n";
    auto tenantA = std::make_shared<MockDatabase>();
    auto tenantB = std::make_shared<MockDatabase>();
    tenantA->add_node("docA", "", false);
    tenantB->add_node("docB", "", false);
    AclManager aclA(tenantA), aclB(tenantB);
    aclA.set_default_read(false);
    aclB.set_default_read(false);
    aclB.grant_permission("docB", "secret=1", PrincipalType::CLAIM, P_READ);  // a grant only in tenant B

    // A tenant_admin has full control of the tenant whose manager handles the
    // request (here, tenant A's own node):
    CHECK(can(aclA, "docA", "carol", {"tenant_admin"}, P_DELETE),
          "tenant_admin controls its own tenant's node");
    // ...and tenant A's manager holds NONE of tenant B's ACL data — the schemas do
    // not bleed. (In production, M3 also ensures a tenant_admin request is only
    // ever routed to their own tenant's manager.)
    CHECK(aclA.get_acls_for_resource("docB").value.empty(),
          "tenant A's manager has no ACL data for tenant B's node (boundary)");
    CHECK(!aclB.get_acls_for_resource("docB").value.empty(),
          "tenant B's manager does hold its own node's ACL");
}

// GROUP is a reserved/unused principal type — roles are the group mechanism, so
// no code path creates a GROUP row. Should a stray one appear (e.g. a manual DB
// insert), it must match NOBODY (fail-closed), never everyone. (M2 + cleanup.)
void test_group_type_matches_nobody() {
    std::cout << "test_group_type_matches_nobody\n";
    auto db = std::make_shared<MockDatabase>();
    db->add_node("F", "", false);
    AclManager acl(db);
    acl.set_default_read(false);
    const int GROUP = static_cast<int>(PrincipalType::GROUP);
    const int USER  = static_cast<int>(PrincipalType::USER);

    // A GROUP ALLOW READ injected directly must grant no one.
    db->add_acl("F", "anygroup", GROUP, P_READ, "", "", 0 /*ALLOW*/);
    CHECK(!can(acl, "F", "alice", {}, P_READ), "GROUP ALLOW grants nobody (fail-closed)");
    CHECK(!can(acl, "F", "bob", {"anygroup"}, P_READ),
          "GROUP ALLOW grants nobody even when a role of the same name is held");

    // A GROUP DENY must affect no one — a legitimately granted user still passes.
    db->add_acl("F", "carol", USER,  P_READ, "", "", 0 /*ALLOW*/);
    db->add_acl("F", "anygrp", GROUP, P_READ, "", "", 1 /*DENY*/);
    CHECK(can(acl, "F", "carol", {}, P_READ), "GROUP DENY affects nobody; USER ALLOW stands");
}

int main() {
    std::cout << "=== AclManager security semantics ===\n";
    test_write_does_not_imply_destructive();
    test_write_does_not_imply_manage_acl();
    test_cull_never_default();
    test_deny_wins_within_tier();
    test_system_admin_is_role_scoped();
    test_claim_abac_exact_match();
    test_default_read_toggle();
    test_hierarchical_precedence();
    test_tenant_admin_is_scoped_not_global();
    test_admin_roles_bypass_within_tenant();
    test_tenant_admin_boundary_is_per_manager();
    test_group_type_matches_nobody();
    std::cout << "\nAll " << g_checks << " checks passed.\n";
    return 0;
}
