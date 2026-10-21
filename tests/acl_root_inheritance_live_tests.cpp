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

// Creation-time ACL inheritance from the tenant ROOT, through a real FileSystem
// and Postgres.
//
// Production, 2026-10: the `file_services` grant the background workers rely
// on is provisioned at each tenant root (resource '') with ACL_INHERIT, and
// creation skipped inheritance whenever the parent was the root. Everything
// that existed when the grant was applied had it; every top-level folder made
// afterwards — and everything put inside one — did not, so difference_service
// was refused VIEW_VERSIONS on a file in one of them every fifteen minutes.
//
// The unit suite pins AclManager::inheritable_rules_for_child; this pins that
// the creation path the gRPC service actually runs (mkdir / touch →
// compute_initial_acl_grants → create_file_with_acls) uses it at the root.
//
// Skips (77) when no Postgres is reachable, like the other live suites.

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "fileengine/accountability.h"
#include "fileengine/database.h"
#include "fileengine/acl_manager.h"
#include "fileengine/filesystem.h"
#include "fileengine/tenant_manager.h"

using namespace fileengine;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::cerr << "  FAIL: " << what << std::endl;
    }
}

static std::string env_or(const char* key, const std::string& dflt) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : dflt;
}

// Creating directly under the root is admin-only; tenant_admin is only how the
// fixture gets to create there.
static const std::string USER = "root-inherit-tester";
static const std::vector<std::string> ROLES = {"tenant_admin"};
static const int FS_MASK = 0x1730;   // ACL_INHERIT | READ | WRITE | DELETE | VIEW_VERSIONS | RETRIEVE

struct Fixture {
    std::shared_ptr<Database> db;
    std::shared_ptr<TenantManager> tm;
    std::shared_ptr<AclManager> acl;
    std::unique_ptr<FileSystem> fs;
    std::string tenant;
    std::string root;
};

static bool make_fixture(Fixture& f) {
    TenantConfig cfg;
    cfg.db_host = env_or("FILEENGINE_PG_HOST", env_or("FE_TEST_PG_HOST", "localhost"));
    cfg.db_port = std::stoi(env_or("FILEENGINE_PG_PORT", env_or("FE_TEST_PG_PORT", "5434")));
    cfg.db_name = env_or("FILEENGINE_PG_DATABASE", env_or("FE_TEST_PG_DB", "fileengine"));
    cfg.db_user = env_or("FILEENGINE_PG_USER", env_or("FE_TEST_PG_USER", "postgres"));
    cfg.db_password = env_or("FILEENGINE_PG_PASSWORD", env_or("FE_TEST_PG_PASSWORD", "postgres"));
    f.root = "/tmp/fe_rootinh_" + std::to_string(::getpid());
    cfg.storage_base_path = f.root;
    cfg.s3_endpoint = "";
    cfg.s3_region = "";
    cfg.s3_bucket = "";
    cfg.s3_access_key = "";
    cfg.s3_secret_key = "";
    cfg.s3_path_style = true;
    cfg.encrypt_data = false;
    cfg.compress_data = false;

    f.tenant = "rinh_" + std::to_string(::getpid());
    f.db = std::make_shared<Database>(cfg.db_host, cfg.db_port, cfg.db_name,
                                      cfg.db_user, cfg.db_password, /*pool_size=*/4);
    if (!f.db->connect()) return false;
    if (!f.db->create_schema().success) return false;
    f.tm = std::make_shared<TenantManager>(cfg, f.db);
    if (!f.tm->initialize_tenant(f.tenant)) return false;
    f.fs = std::make_unique<FileSystem>(f.tm);
    f.acl = std::make_shared<AclManager>(f.db);
    f.fs->set_acl_manager(f.acl);
    return true;
}

static void destroy_fixture(Fixture& f) {
    f.fs.reset();
    f.tm.reset();
    if (f.db && !f.tenant.empty()) {
        f.db->cleanup_tenant_data(f.tenant, AccountabilityContext::system());
    }
    f.db.reset();
    std::error_code ec;
    std::filesystem::remove_all(f.root, ec);
}

// The file_services rule stored on `uid`, as the database holds it (0 = none).
static int file_services_mask(Fixture& f, const std::string& uid) {
    auto acls = f.db->get_acls_for_resource(uid, f.tenant);
    if (!acls.success) return -1;
    for (const auto& a : acls.value) {
        if (a.principal == "file_services" && a.type == static_cast<int>(PrincipalType::ROLE)) {
            return a.permissions;
        }
    }
    return 0;
}

static bool worker_may_view_versions(Fixture& f, const std::string& uid) {
    auto r = f.acl->check_permission(uid, "svc-difference", {"file_services"},
                                     static_cast<int>(Permission::VIEW_VERSIONS), f.tenant);
    return r.success && r.value;
}

int main() {
    std::cout << "=== acl_root_inheritance_live_tests ===\n";
    Fixture f;
    if (!make_fixture(f)) {
        std::cout << "SKIP: could not initialise a tenant (no Postgres?)\n";
        destroy_fixture(f);
        return 77;
    }

    // What provisioning does: the workers' role, inheritable, at the root.
    check(f.acl->grant_permission("", "file_services", PrincipalType::ROLE, FS_MASK, f.tenant,
                                  AccountabilityContext::system()).success,
          "grant file_services at the tenant root");

    // A top-level folder, and the file put in it — the production case.
    auto top = f.fs->mkdir("", "TopLevel", USER, ROLES, 0755, f.tenant);
    check(top.success, "mkdir at the root");
    if (top.success) {
        check(file_services_mask(f, top.value) == FS_MASK,
              "a top-level folder inherits the root's file_services rule");
        check(worker_may_view_versions(f, top.value),
              "a worker can list versions on a new top-level folder");

        auto inner = f.fs->touch(top.value, "inside.mp4", USER, ROLES, f.tenant);
        check(inner.success, "touch inside the top-level folder");
        if (inner.success) {
            check(file_services_mask(f, inner.value) == FS_MASK,
                  "the rule cascades to what is created inside it");
            check(worker_may_view_versions(f, inner.value),
                  "a worker can list versions on a file in a new top-level folder");
        }
    }

    // A file created directly at the root takes it too.
    auto rootfile = f.fs->touch("", "at-root.txt", USER, ROLES, f.tenant);
    check(rootfile.success, "touch at the root");
    if (rootfile.success) {
        check(file_services_mask(f, rootfile.value) == FS_MASK,
              "a file created at the root inherits the root's file_services rule");
    }

    destroy_fixture(f);
    std::cout << "\n=== " << (g_checks - g_failures) << "/" << g_checks << " checks passed ===\n";
    return g_failures == 0 ? 0 : 1;
}
