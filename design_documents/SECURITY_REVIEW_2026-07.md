# FileEngine Security Review — 2026-07

**Scope:** focused review of the authorization core (`file_engine_core`) and the
authentication path (`http_bridge`, `webdav_bridge`), plus the SDK trust
boundary. Feature services (`convert_search_ai`, `discussion`, `audit_service`)
were surveyed and given regression tests but not deep-reviewed in this pass.

**Method:** parallel evidence-based code review (read the actual source, not the
design docs), with each headline finding independently re-verified against the
code before inclusion. Every finding cites `file:line` and a concrete exploit.

**Trust model (context, not a bug):** the gRPC core trusts the
`AuthenticationContext{user, roles, tenant, claims}` stamped by upstream bridges
and does not authenticate. The `system_admin` role bypasses all ACL checks by
design. Authentication and admin-gating are the **bridges'** responsibility, and
gRPC `:50051` must never be network-reachable. Findings below are places where
the system fails to enforce *its own* intended model.

---

## Status at a glance

| ID | Sev | Title | Location | Status |
|----|-----|-------|----------|--------|
| C1 | Critical | Role endpoints lack an admin gate → self-escalation to `system_admin` | http_bridge (+ core by design) | **Fixed** (http_bridge `41bde9f`) |
| C2 | Critical | Empty-password LDAP bind = auth bypass | both bridges | **Fixed** (`41bde9f`, `165e814`) |
| C3 | Critical | LDAP filter injection on webdav live auth path | webdav_bridge | **Fixed** (`165e814`) |
| C4 | Critical | Plaintext credentials logged at default level | webdav_bridge | **Fixed** (`165e814`) |
| H1 | High | Destructive ops gated on `WRITE`, not their dedicated bits | core | **Fixed** (core `security/hardening`) |
| H2 | High | Any `cn=administrators` group maps to `system_admin` | both bridges | **Proposed** (tenant-scoped superuser — design below) |
| M1 | Medium | ALLOW/DENY precedence — hierarchical resolution | core | **Fixed** (core `security/hardening`) |
| M2 | Medium | `GROUP`-type ACL matches every principal | core | **Open** (latent) |
| M3 | Medium | `X-Tenant`/Host tenant trusted with no membership check | bridges | **Fixed** (http_bridge `security/hardening`) |
| M4 | Medium | webdav COPY/MOVE ignore `Destination` authority + `Overwrite` | webdav_bridge | **Fixed** (webdav `security/hardening`) |
| M5 | Medium | Unescaped path/name reflected into PROPFIND/PROPPATCH XML | webdav_bridge | **Fixed** (webdav `security/hardening`) |
| L1 | Low | `validate_user_permissions` fails **open** when ACL mgr is null | core | **Fixed** (core `security/hardening`) |
| L2 | Low | Monitor bind address + optional IP allowlist | all services | **Fixed** (C++ tier); Python binds loopback, allowlist follow-up |
| L3 | Low | Audit fail-**open** when disabled; OAuth/refresh unaudited | http_bridge | **Open** |
| L4 | Low | Dead-code LDAP injection in unused digest/getUserInfo | both bridges | **Fixed** (bridges `security/hardening`) |

Fixes live on the `security/hardening` branch in each affected repo (unmerged).

---

## Critical findings (fixed)

### C1 — Role-management endpoints have no authorization
Any authenticated user could assign themselves `system_admin` and bypass every
ACL in the tenant.

- **Core:** `core/src/grpc_service.cpp:1951–2075` (`CreateRole`, `DeleteRole`,
  `AssignUserToRole`, `RemoveUserFromRole`) read `user`/`roles` but never check
  them — line 1962 literally: *"we'll allow any authenticated user to create
  roles."* This is **by design** per the trust model (the core trusts callers).
- **Bridge:** `http_bridge/src/http_server.cpp:912–959` — the handlers just
  `fillAuth(...)` with the caller's identity and forward. **Neither layer gated
  role administration**, so the model's required admin-check was simply absent.
- **Exploit:** as a non-admin with a valid token,
  `PUT /v1/roles/system_admin/users/<self>` → next request carries `system_admin`.
- **Fix:** `requireTenantAdmin()` added in the bridge; called first in all four
  role handlers. Non-admins get `403`. (This is the correct layer per the trust
  model — the core stays trusting.) Consider a core-side defense-in-depth flag as
  a follow-up.

### C2 — Empty-password LDAP bind = authentication bypass
- **`http_bridge/src/ldap_authenticator.cpp:147–160`**,
  **`webdav_bridge/src/ldap_authenticator.cpp:99–111`** — the client password
  is passed straight to `ldap_sasl_bind_s` with no non-empty check
  (`cred.bv_len = password.length()`).
- **Exploit:** per RFC 4513, a simple bind with a valid DN and a zero-length
  password is an *unauthenticated bind* that OpenLDAP answers `LDAP_SUCCESS`.
  `Authorization: Basic base64("victim:")` mints a full session as the victim
  with their roles.
- **Fix:** reject empty passwords before binding in both bridges.

### C3 — LDAP filter injection on webdav's live auth path
- **`webdav_bridge/src/ldap_authenticator.cpp:50`** — `"(uid=" + username + ")"`
  with the raw username, no RFC 4515 escaping (http_bridge's live path already
  escaped).
- **Exploit:** a crafted username manipulates the search filter for enumeration
  or DN selection; combined with C2, targeted account takeover.
- **Fix:** added `escapeLdapFilterValue()` (RFC 4515) and applied it to the
  `(uid=…)` and `(member=…)` filters.

### C4 — Plaintext credentials logged at the default level
- **`webdav_bridge/src/webdav_server.cpp:1245`** logged the decoded
  `username:password` (`:1237` the base64), and the shipped `.env-default` set
  `LOG_LEVEL=debug`.
- **Exploit:** every request's LDAP password lands in stdout/journal/container
  logs — reusable well beyond WebDAV.
- **Fix:** removed the credential log lines (username-only at debug); flipped the
  shipped default to `info` in both bridges.

---

## High findings

### H1 — Destructive ops gated on `WRITE`, not their dedicated bits — **FIXED**
`RemoveFile`/`RemoveDirectory` checked `WRITE` not `DELETE`
(`grpc_service.cpp:553, 277`); `UndeleteFile` not `UNDELETE` (589);
`RestoreToVersion` not `RESTORE_TO_VERSION` (1065); `ListVersions`/`GetVersion`
used `READ` not `VIEW_VERSIONS`/`RETRIEVE_BACK_VERSION`. The DELETE / UNDELETE /
VIEW_VERSIONS / RESTORE bits were grantable/deniable but **never enforced** — a
"WRITE-only" collaborator silently got delete + history rollback, and a targeted
`DENY DELETE` was a no-op.

- **Fix (implemented):** both enforcement layers now check the dedicated bits —
  the gRPC handlers (`core/src/grpc_service.cpp`) and the `FileSystem` methods
  (`core/src/filesystem.cpp`): DELETE for remove file/dir, UNDELETE for undelete,
  RESTORE_TO_VERSION for restore, VIEW_VERSIONS for ListVersions,
  RETRIEVE_BACK_VERSION for GetVersion.
- **Why it is backward-safe (no flag needed):** `apply_default_acls`
  (`acl_manager.cpp`) already grants a resource's creator the **full** bit set
  (DELETE, LIST_DELETED, UNDELETE, VIEW_VERSIONS, RETRIEVE_BACK_VERSION,
  RESTORE_TO_VERSION, MANAGE_ACL). Owners and `system_admin` are unaffected; only
  a collaborator *explicitly* granted WRITE-only loses implicit delete/rollback —
  which is the intended effect. `test_security_acl.cpp` proves the bits are
  independent at the ACL layer.

### H2 — Any `cn=administrators` group maps to `system_admin` — **PROPOSAL**
`http_bridge/src/ldap_authenticator.cpp:1148–1152`,
`webdav_bridge/src/ldap_authenticator.cpp:794–798`. The Basic-auth role
resolution searches domain-wide bases, case-insensitively; membership in *any*
group named `administrators` (not scoped to the request tenant) yields the global
`system_admin` bypass. The JWT path is properly tenant-bucketed — the two paths
disagree. Root cause: the core's `system_admin` is a **global** bypass, so any
"local admin" role that maps to it inherently crosses tenant boundaries.

#### Proposed fix: a tenant-scoped superuser

Introduce a distinct **tenant-scoped superuser** that grants full-control *within
one tenant only*, and reserve the existing global `system_admin` for genuine
deployment operators. A tenant's "administrators" group maps to the scoped role,
never to the global one.

**1. Core — new reserved role `tenant_admin`, scoped bypass.**
`AclManager::check_permission` already receives the request `tenant`. Add a
second reserved role alongside `kSystemAdminRole`:
```cpp
static constexpr const char* kSystemAdminRole = "system_admin";  // global (all tenants)
static constexpr const char* kTenantAdminRole = "tenant_admin";  // this tenant only
```
`check_permission`/`get_effective_permissions` bypass ACLs when the caller holds
`system_admin` **OR** holds `tenant_admin` *and the resource's tenant equals the
auth-context tenant*. Because every resource lookup is already tenant-scoped
(`get_schema_prefix(tenant)`), a `tenant_admin` in tenant A resolving a tenant-B
resource simply operates in A's schema and cannot see B — the scoped bypass adds
no cross-tenant reach. `is_system_admin()` gains a sibling `is_tenant_admin()`;
role administration (C1 gate) accepts either.

**2. Bridges — map the tenant admin group to the scoped role.**
Replace `if (r == "administrators") add "system_admin"` with a mapping to
`tenant_admin`, and — critically — only when the `administrators` group is found
**under the request tenant's OU** (`ou=<tenant>,…`), not via the domain-wide
subtree fallback. `system_admin` is granted only from an explicit, separately
named operators group (e.g. `cn=platform-operators,ou=system`), never from a
per-tenant `administrators` group.

**3. Result.** A tenant admin gets full control of their own tenant and **zero**
reach into others, even if a same-named `administrators` group exists elsewhere
in the directory. The blast radius of a mis-placed or attacker-created
`administrators` group is contained to a single tenant. `system_admin` becomes a
deliberately-provisioned platform role.

**Migration:** existing deployments that rely on `administrators → system_admin`
keep working if the operators group is seeded; otherwise tenant admins transparently
downgrade to tenant-scoped (a *tightening*, not a break). Add tests: a
`tenant_admin` bypasses within-tenant but is denied cross-tenant; the bridge maps
`administrators` only from the tenant OU.

This is a design proposal for review — **not yet implemented.**

---

## Medium findings (open)

- **M1 — ALLOW/DENY precedence — FIXED (hierarchical resolution):**
  `core/src/acl_manager.cpp` `calculate_effective_permissions`. Resolved as a
  deliberate identity hierarchy rather than a bug: the most specific tier that
  touches a bit settles it, and DENY is absolute only *within* a tier. The
  previously-lumped role/claim tier is now split so claims outrank groups:
  **USER > CLAIM > ROLE/GROUP > OTHER(everyone) > read-only default.** Header
  comments corrected (no more "a matching DENY always wins"). `test_security_acl`
  gained `test_hierarchical_precedence` covering USER>ROLE, CLAIM>ROLE, USER-DENY
  over CLAIM-ALLOW, and ROLE-ALLOW over everyone-DENY.
- **M2 — GROUP matches everyone:** `acl_manager.cpp:375` returns a match for any
  principal. Latent (no gRPC path creates GROUP rows today) but a foot-gun the
  moment GROUP is wired.
- **M3 — tenant not membership-checked — FIXED (http_bridge):** `authenticate()`
  now rejects a selected tenant the caller is not a member of. Bearer: the active
  tenant must be the token's issued tenant or a key in its `{tenant:[roles]}`
  map, else **403**. Basic: when the client explicitly sets `X-Tenant` (the
  attacker-controlled vector), LDAP membership (`getTenantsForUser`) is verified,
  else **403**. Host/subdomain routing (set by the trusted nginx proxy) is left
  intact. **webdav:** its tenant is host-derived only (`extractTenantFromHostname`)
  with no client `X-Tenant` override, so the header-injection vector does not
  exist there; Host is set by the proxy. No change made in webdav.
- **M4 — webdav COPY/MOVE — FIXED:** a `prepareDestination()` guard now runs
  before both handlers: the `Destination` authority must match the request host
  (else **502**), and RFC 4918 `Overwrite` is honored — an existing target with
  `Overwrite: F` yields **412**, and with `Overwrite: T` the target is deleted
  first (the core permission-checks the delete, so an unauthorized overwrite is
  **403**). Resolution uses the caller's auth, so destination permissions are
  enforced with appropriate status codes.
- **M5 — XML injection — FIXED:** added `xmlEscape()` and applied it to every
  path/name reflected into PROPFIND/PROPPATCH `<D:href>`/`<D:displayname>`
  (9 sites), escaping `& < > " '`.

---

## Low findings (open)

- **L1 — fail-open gate — FIXED:** `grpc_service.h` and
  `FileSystem::validate_user_permissions` (`filesystem.cpp`) now return **deny**
  when `acl_manager_` is null (the root-READ carve-out is preserved). Test
  harnesses that construct these without an ACL manager must now inject one.
- **L2 — monitor bind + IP allowlist — FIXED (C++ tier):** the core REST monitor
  now defaults to **`127.0.0.1`** (was `0.0.0.0`) and gained an optional
  exact-match client-IP allowlist (`FILEENGINE_HTTP_METRICS_ALLOW_IPS`) enforced
  before routing. Both bridges (already loopback) gained the same allowlist
  (`HTTP_MONITORING_ALLOW_IPS`, `WEBDAV_MONITORING_ALLOW_IPS`). **Python tier:**
  all Python services already bind their monitoring/health to `127.0.0.1` by
  default (config `monitoring_host` / `api_host` / `http_host`), so the L2 bind
  requirement is met deployment-wide. Adding an IP allowlist there is a scoped
  follow-up: `ldap_manager` has a dedicated monitor app (clean middleware), while
  `audit-api` / `csai` / `discussion` expose `/healthz` on their main
  authenticated app, so the allowlist must be **route-scoped** to the monitoring
  paths (not a blanket middleware) to avoid gating real API traffic.
- **L3 — audit fail-open:** `http_bridge/src/audit_publisher.cpp:52` returns
  success when auditing is disabled/built without hiredis, so logins succeed with
  no trail; OAuth (`http_server.cpp:1150`) and refresh logins emit no audit event.
- **L4 — dead-code injection — FIXED:** removed the unused `authenticateDigest`
  and `getUserInfo` from **both** bridges (each built raw, unescaped LDAP filters
  and had no callers). The live paths (`getUserInfoByEmail`, `authenticateUser`)
  are retained and already escape.

## SDK trust boundary (by design)
`python_interface` / `javascript_interface` pass `user`/`roles`/`tenant`
verbatim to the core. This is intended, but means gRPC `:50051` must never be
network-reachable and the SDKs must stay server-side. Not a bug; noted so the
deployment invariant is explicit.

---

## Confirmed correct (regression-worthy — now covered by tests)
- JWT HS256 alg-pinning + constant-time compare + `exp` + fail-closed secret.
- `GrantPermission`/`RevokePermission` correctly require `MANAGE_ACL`
  (no write → ACL self-escalation).
- `PurgeOldVersions` requires the dedicated `CULL_VERSIONS` bit; it is never in
  the default/creator ACLs.
- Parent-traversal + soft-deleted-ancestor reachability fails closed.
- AES-256-GCM uses a fresh random IV per encryption; tag verified.
- OAuth state = one-shot 256-bit MAC with PKCE + return-URL allowlist.
- No SQL injection: parameterized queries + `validate_schema_name`.
- http_bridge live LDAP paths escape filters; monitoring binds loopback.

---

## Tests added (branch `security/hardening` in each repo)

| Area | File | Checks |
|------|------|--------|
| Bridge auth (JWT) | `http_bridge/tests/test_security.cpp` (CTest `security_tests`) | 9 |
| Bridge auth E2E | `http_bridge/tests/test_e2e_security.sh` | live; C1/C2 tagged EXPECT-FAIL-UNTIL-FIX |
| Core ACL semantics | `file_engine_core/tests/test_security_acl.cpp` | 18 |
| Feature-service gating | `convert_search_ai/tests/test_permission_gate_security.py` | 9 |
| Audit integrity | `audit_service/src/tests/test_chain_tamper.py` | 5 |

All unit suites pass; both bridges build clean with the C1–C4 fixes. The E2E
script requires the live stack (core + bridge); its `[C1]`/`[C2]` cases turn
green against the patched binaries.

## Fix commits
- http_bridge `security/hardening`: tests `cfe0b1f`, fixes `41bde9f`
- webdav_bridge `security/hardening`: fixes `165e814`
- file_engine_core `security/hardening`: ACL tests `6d0d392`
- convert_search_ai `security/hardening`: `d1dcfcb`
- audit_service `security/hardening`: `acf0110`
