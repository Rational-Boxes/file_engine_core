# ACL / RBAC Layer — Review Findings and Development Plan

**Status:** Draft
**Branch reviewed:** `ACL-improvments` (4 commits ahead of `main`)
**Scope:** Permission enforcement across `AclManager`, `RoleManager`, `Database` ACL/role persistence, and the `FileSystem` + `GRPCFileService` enforcement layers.

The permission system is split across three layers: `AclManager` (in-memory rule evaluation), `Database` / `IDatabase` (persistence), and two enforcement layers (`FileSystem::validate_user_permissions` + `GRPCFileService::validate_user_permissions`). It is partially wired, contains several critical bugs that nullify enforcement, and large pieces are explicit stubs.

---

## Part 1 — Findings

### 1. Critical bugs

#### 1.1 gRPC enforcement uses the wrong bitmask (silent miscompare)

Every `validate_user_permissions(...)` call inside `core/src/grpc_service.cpp` passes POSIX-octal literals (`0400`, `0200`), while the `Permission` enum in `core/include/fileengine/acl_manager.h:11-21` is defined in **hex** (`READ = 0x400`, `WRITE = 0x200`, …):

- `0x400` = 1024 ≠ `0400` octal = 256
- `0x200` = 512 ≠ `0200` octal = 128
- Worse: `0200` (octal) = `0x80` = the `LIST_DELETED` bit; `0400` (octal) = `0x100` = the `DELETE` bit.

So the gRPC layer asking for "WRITE" is actually checking the `LIST_DELETED` bit, and "READ" is actually checking `DELETE`.

**Affected sites in `grpc_service.cpp`:** lines 65, 118, 149, 208, 295, 355, 387, 442, 496, 530, 624, 656, 663, 695, 702, 734, 769, 803, 839, 871, 903, 938, 971, 999, 1030, 1100, 1256, 1292, 1395.

With `apply_default_acls` (`acl_manager.cpp:96-111`) only granting `READ|WRITE|EXECUTE` (= 0x601), no creator has bit 0x100 set, so every gRPC-level "READ" check fails for non-root callers. Operations succeed today only because gRPC also relies on the root-bypass branch or because the `FileSystem` layer redoes the check with the right constants.

#### 1.2 `FileSystem` enforcement always drops roles

Every `validate_user_permissions(...)` call in `core/src/filesystem.cpp` passes an empty roles vector `{}`.

**Affected sites in `filesystem.cpp`:** lines 50, 115, 161, 198, 240, 275, 297, 319, 461, 633, 668, 674, 712, 718, 859, 881, 904, 982, 1094, 1112, 1135, 1153, 1174, 1197, 1216, 1236.

The plumbing for role-based decisions exists end-to-end (`AclManager::check_permission` accepts a roles vector, the gRPC helper passes the auth context's roles), but the `FileSystem` API never accepts or forwards them. Role-based ACLs are therefore unenforceable for any operation routed through `FileSystem`. The `roles` parameter on `FileSystem::check_permission` (filesystem.h:127) exists but is the only path that uses it.

#### 1.3 `revoke_permission` deletes the whole row, ignoring the bitmask

`Database::remove_acl` (`database.cpp:2152-2183`) issues `DELETE … WHERE resource_uid=$1 AND principal=$2 AND principal_type=$3` — it does not mask off only the bits the caller asked to revoke. `AclManager::revoke_permission` and `FileSystem::revoke_permission` accept a `permissions` bitmask and silently discard it: revoking `WRITE` from a principal that has `READ|WRITE` removes both.

#### 1.4 ACL table created lazily inside `add_acl`

`Database::add_acl` (`database.cpp:2102-2123`) issues `CREATE TABLE IF NOT EXISTS … .acls(…)` on every insert. Tenants that never had `add_acl` called have no table, so `get_acls_for_resource` and `get_user_acls` will return a Postgres error (`relation … does not exist`) rather than an empty list. Schema creation belongs in `create_tenant_schema`. The same applies to the missing `roles` / `user_roles` tables (referenced in CLAUDE.md and `database_architecture.md` but never created).

#### 1.5 `apply_default_acls` always grants world-readable

`acl_manager.cpp:96-111` unconditionally grants `OTHER → READ` on every new resource. Combined with the precedence rule in `calculate_effective_permissions` (see 2.1), the practical default for a freshly created file/directory is "any authenticated user can read it" — there's no path to create a private resource through the normal API.

#### 1.6 No parent → child inheritance

`AclManager::inherit_acls` exists (`acl_manager.cpp:113-133`) but is **never called**. `FileSystem::mkdir`/`put`/`move`/`copy` all call `apply_default_acls` instead. Granting `bob → WRITE` on a directory therefore confers nothing on files later created inside it.

### 2. Logic flaws in `calculate_effective_permissions`

`acl_manager.cpp:160-210`.

#### 2.1 Role grants suppressed by *any* user-level rule

The function sets `user_found = true` as soon as *any* USER-typed rule matches, then skips role lookup entirely (`if (!user_found)` at line 179). A per-user grant of `READ` on a resource silently strips that user of all role-provided permissions on the same resource. The precedence comment says "user > role > group > other", but real ACL systems union grants of equal-or-lower priority, not mask them. Group rules have the same gap (line 191).

#### 2.2 `other_found` is set but unused

`other_found` is assigned at line 205 and never read — dead state.

#### 2.3 No deny semantics, no group plumbing

`PrincipalType::GROUP` is recognized in `calculate_effective_permissions` but there is no DB or RPC path to (a) define a group, (b) record group memberships, or (c) populate the `roles`/groups list at request time. Effectively, only USER and OTHER rules are actionable.

### 3. The `RoleManager` is a stub

`core/src/role_manager.cpp` and the `Database::*role*` methods (`database.cpp:2269-2324`) are **all no-ops** that return success without touching the database. Yet the gRPC service exposes `CreateRole`, `DeleteRole`, `AssignUserToRole`, `RemoveUserFromRole`, `GetRolesForUser`, `GetUsersForRole`, `GetAllRoles`. All of these:

- Return success to clients.
- Do not persist anything.
- Have log lines admitting it (e.g. `grpc_service.cpp:1574: "GetRolesForUser returning empty vector"`).

The product contract is "trusted upstream authentication" (roles come on `AuthenticationContext`), which is internally consistent — **but** the RPCs to manage roles are still wired up and lie to callers. Either remove them from the proto, or actually back them with `tenant_*.roles` and `tenant_*.user_roles` tables (CLAUDE.md already claims these exist; they don't).

### 4. Authorization model gaps

#### 4.1 Hard-coded `"root"` string bypass

`grpc_service.h:225` and `filesystem.cpp:44`, `grpc_service.cpp:1030`, `1100`, `1395` all special-case the literal username `"root"` to bypass checks. There's no config flag, no role check (`admin` role would be more idiomatic given the rest of the design), and no audit hook. Anyone able to set `AuthenticationContext.user="root"` upstream wins everything.

#### 4.2 "Filesystem root" auto-read

Both enforcement layers grant READ on empty `resource_uid` unconditionally (`filesystem.cpp:1346-1351`, `grpc_service.h:232-234`). Probably intentional, but tenant isolation at "root" relies entirely on the gRPC tenant routing layer; ACLs are not a backstop.

#### 4.3 No self-management of grant rights

`grpc_service.cpp:1030`, `1100` allow anyone with `WRITE` (broken constant, see 1.1) on the resource to grant/revoke any permission to anyone. There is no separate `ADMIN`/`MANAGE_ACL` permission bit, so any user who can write a file can give other users any permission on it — including ones they themselves don't hold.

#### 4.4 No path for tenant admins

The only admin concept is the global `root` bypass. There's no "tenant-admin" role, even though the system is explicitly multi-tenant.

### 5. Permission enum design issues

`acl_manager.h:11-21`:

- Mixed hex values (`0x400`, `0x080`, `0x001`) read like octal at a glance — almost certainly the root cause of bug 1.1.
- `LIST_DELETED = 0x080` is written `0x080` instead of `0x80` (harmless, but the leading zero suggests confusion about octal vs hex).
- `EXECUTE = 0x001` is documented as "kept for compatibility" but never checked or used by any operation. The file-level `permissions` integer column (used by `insert_file`) is unrelated to ACL permissions and the relationship is undocumented — two parallel concepts both called "permissions".

### 6. Other defects

- **`AclManager::inherit_acls` swallows errors silently** (`acl_manager.cpp:127-129`) — partial inheritance with no caller signal.
- **No deny rules.** All grants are additive; you cannot model "everyone in role X except user Y".
- **No audit trail.** `created_at` / `updated_at` exist on the row but who granted/revoked is not recorded.
- **No transactional grouping.** Default-ACL application after `insert_file` is a separate uncoordinated write; a crash between them leaves files with no ACL.
- **No index on `(principal, principal_type)`** — only `(resource_uid)` and `(principal)` (line 2113-2114). Per-user ACL listings will scan.
- **`get_user_acls` does not filter by `principal_type`** — returns rows matching the principal name regardless of whether the row is USER, ROLE, GROUP, or OTHER (`database.cpp:2239`, `acl_manager.cpp:135`). A role named "alice" and a user named "alice" would be conflated.

### 7. Test coverage caveat

The four ACL test files in `tests/` (`test_acl_rbac_comprehensive.cpp`, `test_comprehensive_acl_roles.cpp`, `test_role_based_access_scenarios.cpp`, `test_acl_group_role_permissions.cpp`) appear to drive `AclManager` directly — they would not catch bugs 1.1 (gRPC bitmask), 1.2 (`FileSystem` drops roles), or 1.6 (no inheritance), because those live in the layers above `AclManager`. A few targeted gRPC-level integration tests (or `FileSystem::*` tests that supply roles) would surface every critical bug above.

---

## Part 2 — Recommended Development Plan

Phased so each step lands a working, testable improvement and de-risks the next.

### Phase 0 — Stop the bleeding (1 PR, ~half a day)

Goal: make existing ACL/role checks actually do what the comments say. No API or schema changes.

1. **Replace `0400`/`0200` octal literals in `grpc_service.cpp`** with `static_cast<int>(Permission::READ)` / `static_cast<int>(Permission::WRITE)` at every site listed in §1.1. Likely a `sed` followed by a build.
2. **Add a regression test** that drives a non-root user through a gRPC `mkdir → put → get → grant → revoke` flow and asserts the expected accept/deny pattern. This is the test that would have caught §1.1 originally.
3. **Add `principal_type` to `Database::get_user_acls`** WHERE clause (and to `AclManager::get_user_acls`) so principal-name collisions across user/role/group namespaces don't conflate.

**Exit criteria:** existing tests still pass, new gRPC-level test passes, manual smoke through `fileengine_cli` confirms a non-root user can READ a file they own.

### Phase 1 — Thread roles into the enforcement path (1 PR, 1–2 days)

Goal: make role-based ACLs actually enforced.

1. **Add a `roles` parameter to every `FileSystem::*` operation that currently takes `user`** (or, cleaner: introduce a small `RequestContext { user, roles, tenant }` and replace the three separate args). Update `filesystem.h` and all 25+ enforcement sites in `filesystem.cpp` to forward it into `validate_user_permissions`.
2. **Have `GRPCFileService` populate that context** from `AuthenticationContext` (already extracts `user`/`tenant`/`roles` via the existing helpers) at the top of each handler and pass it through.
3. **Tests:** extend the gRPC integration test from Phase 0 to grant a permission to a role, request as a user holding only that role, and assert allow.

**Exit criteria:** role-based ACLs work end-to-end through gRPC. The `roles` field on `AuthenticationContext` is no longer cosmetic.

### Phase 2 — Fix evaluation semantics (1 PR, ~1 day)

Goal: `calculate_effective_permissions` matches a documented policy.

1. **Choose and document a model.** Recommendation: **union of all matching grants across user, roles, groups, other** — simpler, matches POSIX-ACL/NFSv4-style additive grants, removes the surprising "user-rule masks role-rule" footgun.
2. **Rewrite `calculate_effective_permissions`** to that model. Delete the dead `user_found`/`role_found`/`other_found` bookkeeping.
3. **Decide on OTHER default.** Make `apply_default_acls` granting `OTHER → READ` opt-in via tenant config (e.g. `default_world_readable = false` in `core.conf`). Default to OFF.
4. **Tests:** add precedence-table tests covering "user-only", "role-only", "user+role", "group", "other", "none", with and without the default-world-readable knob.

**Exit criteria:** the precedence comments in the source match the code; a private-by-default tenant has no world-readable files.

### Phase 3 — Make persistence honest (1 PR, ~1 day)

1. **Move DDL into `create_tenant_schema`.** Create `acls`, `roles`, `user_roles` tables (and indexes) at tenant init time. Drop the `CREATE TABLE IF NOT EXISTS` from `add_acl`.
2. **Add a migration** for existing tenants — a one-shot startup pass that creates missing tables for each tenant in `list_tenants()`. This is the only schema-rolling step in the plan; everything else is additive.
3. **Fix `remove_acl` to mask bits, not delete rows.** SQL: `UPDATE acls SET permissions = permissions & ~$4 WHERE …; DELETE FROM acls WHERE permissions = 0 AND …`. Keep the row-delete fallback for callers that want a full revoke (separate API or sentinel mask `-1`).
4. **Add index** on `(principal, principal_type)` and a compound `(resource_uid, principal, principal_type)` (the existing `UNIQUE` already gives the compound — drop the duplicate single-column index if so).
5. **Tests:** revoke-partial test; tenant-init test that confirms tables exist before any `add_acl` call.

**Exit criteria:** a fresh tenant can be queried for ACLs without first writing one; bit-level revoke works.

### Phase 4 — Land real RBAC persistence (1–2 PRs, 2–3 days)

This is the biggest single piece of new work. Only worth doing if the product genuinely needs server-side role management (i.e., not all deployments will rely on upstream IdP).

1. **Implement `Database::create_role`, `delete_role`, `assign_user_to_role`, `remove_user_from_role`, `get_roles_for_user`, `get_users_for_role`, `get_all_roles`** against the `roles` and `user_roles` tables added in Phase 3.
2. **`RoleManager` becomes a thin wrapper** that delegates to the DB.
3. **Decide on conflict resolution** between roles in `AuthenticationContext` and DB-stored roles. Recommendation: union them (request-supplied roles for federated IdP cases + DB-stored roles for local management).
4. **Tests:** role CRUD, user-role membership round-trip, integration test that grants on a role then assigns a user and re-checks.

**Alternative:** if upstream-IdP is the only supported model, **remove the role-management RPCs from `proto/fileservice.proto`** and the stubs from `RoleManager`/`Database`. Half-wired is the worst state.

### Phase 5 — Authorization model improvements (separate PRs, prioritize per need)

These are independent and can land in any order.

1. **Inheritance.** Replace `apply_default_acls` calls on file creation with `inherit_acls(parent_uid, new_uid, tenant)` when the parent has ACLs; fall back to `apply_default_acls` only at the tenant root. Make `inherit_acls` fail-loud rather than swallowing per-rule errors. Add a `ACL_INHERIT` bit on parent rules to control which rules propagate (POSIX default-ACL style).
2. **Replace the `"root"` string bypass** with a `system_admin` role checked against config. Keep the bypass for emergencies but gate it on a server-side flag (e.g. `allow_root_user = false` by default).
3. **Add a `MANAGE_ACL` permission bit.** Require it (separately from `WRITE`) on `GrantPermission`/`RevokePermission`. Creators get it via `apply_default_acls`.
4. **Add a tenant-admin role concept** with implicit `MANAGE_ACL` on all resources in the tenant.
5. **Audit trail.** Add `granted_by` / `revoked_at` / `revoked_by` columns (or a separate `acl_audit` table) and populate from the request context.

### Phase 6 — Defense in depth (longer-term)

1. **Deny rules.** Extend `ACLRule` with an `effect` (allow/deny), evaluate deny first. Most operational mistakes are easier to express as "everyone in X except user Y".
2. **Transactional resource creation.** Wrap `insert_file` + default-ACL application in a single transaction so a crash between them can't leave an unprotected file.
3. **Negative caching of permission checks** at request scope — every operation today re-hits Postgres for the same `(resource, user, tenant)` tuple.

---

## Suggested PR ordering and rough sizing

| Phase | PR(s) | Effort | Risk | Unblocks |
|-------|-------|--------|------|----------|
| 0 | Fix bitmask + namespace filter | 0.5 day | Low | Everything below |
| 1 | Thread roles through `FileSystem::*` | 1–2 days | Low | Phase 2 |
| 2 | Evaluation semantics + default-OTHER knob | 1 day | Medium (behavior change) | Phase 3 |
| 3 | Persistence cleanup + partial revoke | 1 day | Low (migration is additive) | Phase 4 |
| 4 | Real role persistence *or* drop the RPCs | 2–3 days *or* 0.5 day | Medium | Phase 5 |
| 5 | Inheritance, root role, `MANAGE_ACL`, audit | 3–5 days, split | Medium | — |
| 6 | Deny + tx + caching | Open-ended | Higher | — |

## Quick wins for the current branch

If the intent is to ship `ACL-improvments` to `main` without taking on the full plan, the minimum to ship a coherent state is **Phase 0 + Phase 1**. Without those, the test suites added on this branch are exercising `AclManager` in isolation while the deployed enforcement path is bypassed.
