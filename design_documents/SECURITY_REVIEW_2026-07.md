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
| H1 | High | Destructive ops gated on `WRITE`, not their dedicated bits | core | **Open** (needs decision) |
| H2 | High | Any `cn=administrators` group maps to `system_admin` | both bridges | **Open** |
| M1 | Medium | "DENY always wins" only holds within a tier | core | **Open** (documented) |
| M2 | Medium | `GROUP`-type ACL matches every principal | core | **Open** (latent) |
| M3 | Medium | `X-Tenant`/Host tenant trusted with no membership check | bridges | **Open** |
| M4 | Medium | webdav COPY/MOVE ignore `Destination` authority + `Overwrite` | webdav_bridge | **Open** |
| M5 | Medium | Unescaped path/name reflected into PROPFIND/PROPPATCH XML | webdav_bridge | **Open** |
| L1 | Low | `validate_user_permissions` fails **open** when ACL mgr is null | core | **Open** |
| L2 | Low | Core REST monitor defaults to `0.0.0.0:8081` | core | **Open** |
| L3 | Low | Audit fail-**open** when disabled; OAuth/refresh unaudited | http_bridge | **Open** |
| L4 | Low | Dead-code LDAP injection in unused digest/getUserInfo | http_bridge | **Open** |

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

## High findings (open — need a decision)

### H1 — Destructive ops gated on `WRITE`, not their dedicated bits
`RemoveFile`/`RemoveDirectory` check `WRITE` not `DELETE`
(`grpc_service.cpp:553, 277`); `UndeleteFile` not `UNDELETE` (589);
`RestoreToVersion` not `RESTORE_TO_VERSION` (1065); `ListVersions`/`GetVersion`
use `READ` not `VIEW_VERSIONS`/`RETRIEVE_BACK_VERSION`. The DELETE / UNDELETE /
VIEW_VERSIONS / RESTORE bits are grantable/deniable but **never enforced** — a
"WRITE-only" collaborator silently gets delete + history rollback, and a targeted
`DENY DELETE` is a no-op.
- **Recommendation:** switch the handlers to the dedicated bits. Because a
  WRITE-only user *can* currently delete, this is a semantic change — gate it
  behind `FILEENGINE_STRICT_PERMISSION_BITS` (default off → on) for a safe
  rollout. The core unit test already proves the bits are independent, so the
  change is localized to the handlers.

### H2 — Any `cn=administrators` group maps to `system_admin`
`http_bridge/src/ldap_authenticator.cpp:1148–1152`,
`webdav_bridge/src/ldap_authenticator.cpp:794–798`. The Basic-auth role
resolution searches domain-wide bases, case-insensitively; membership in *any*
group named `administrators` (not scoped to the request tenant) yields the global
`system_admin` bypass. The JWT path is properly tenant-bucketed — the two paths
disagree.
- **Recommendation:** scope the `administrators → system_admin` mapping to the
  request tenant's admin group DN; drop the domain-wide fallback search.

---

## Medium findings (open)

- **M1 — cross-tier DENY:** `core/src/acl_manager.cpp:394–399`. A higher-tier
  (USER) ALLOW settles a bit before a lower-tier (ROLE) DENY is evaluated
  (`undecided &= ~(allow[t]|deny[t])`), so a USER ALLOW overrides a ROLE DENY.
  The header (`acl_manager.h:76`) advertises "a matching DENY always wins" — true
  only within a tier. Since the creator gets a USER-level ALLOW by default,
  role-level DENYs are easily defeated. **Decide:** make DENY win across tiers
  (aggregate all denies first) *or* correct the documented contract. The core
  test documents current behavior and flips on decision.
- **M2 — GROUP matches everyone:** `acl_manager.cpp:375` returns a match for any
  principal. Latent (no gRPC path creates GROUP rows today) but a foot-gun the
  moment GROUP is wired.
- **M3 — tenant not membership-checked:** `http_server.cpp:1176` /
  webdav Host resolution take the tenant verbatim. Bearer path is bounded (empty
  roles in a foreign tenant), but the Basic path stamps real roles into an
  arbitrary tenant; cross-tenant exposure then rides on core read-by-default ACLs.
  **Recommendation:** reject an `X-Tenant` not in the caller's known tenants.
- **M4 — webdav COPY/MOVE:** `webdav_server.cpp:861, 1025` ignore the
  `Destination` authority and the `Overwrite` header (silent clobber, contract
  violation).
- **M5 — XML injection:** `webdav_server.cpp:662, 716, 745, 835` reflect
  path/name into PROPFIND/PROPPATCH XML without escaping.

---

## Low findings (open)

- **L1 — fail-open gate:** `grpc_service.h:303` (`FileSystem::validate_user_permissions`,
  `filesystem.cpp:2050`) returns `true` when `acl_manager_` is null. Wrong default
  for the central gate even as a test fallback.
- **L2 — monitor bind:** core REST monitor defaults to `0.0.0.0:8081`, violating
  the loopback-only monitoring convention the bridges follow (they bind
  `127.0.0.1`). Bind loopback by default.
- **L3 — audit fail-open:** `http_bridge/src/audit_publisher.cpp:52` returns
  success when auditing is disabled/built without hiredis, so logins succeed with
  no trail; OAuth (`http_server.cpp:1150`) and refresh logins emit no audit event.
- **L4 — dead-code injection:** `http_bridge/src/ldap_authenticator.cpp:228`
  (unused `authenticateDigest`/`getUserInfo`) builds an unescaped filter. Delete
  or escape before it is ever wired up.

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
