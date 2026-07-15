# Proposal: Two-Factor Authentication + WebDAV IP-Binding

**Status:** Draft / research — for review
**Branch:** `research/2fa-webdav-ip-binding` (file_engine_core)
**Author:** security hardening follow-up (2026-07)
**Scope (cross-repo):** `http_bridge`, `ldap_manager`, `webdav_bridge`, `frontend`, `docker_unified` (nginx), `audit_service` (event types)

> This is a design proposal, not an implementation. It grounds every decision in
> the current code and calls out the decisions that need your sign-off (see
> **§11 Open decisions**).

> **Amendment 2026-07-15 (§14).** Feature B (§5) is amended by **§14**, which
> reframes the WebDAV gate as a single **origin-aware** policy — *LAN trust
> boundary* vs *Internet* — and upgrades the external branch from IP-**pinning**
> (a 12 h TTL that outlives logout) to session-**liveness** (revoked the moment the
> Web-UI session ends). Read §5 for the mechanism and §14 for the amended
> semantics; where they differ, **§14 wins**.

> **Amendment 2026-07-15 (§15).** **§15** hardens the WebDAV *credential itself*:
> replace the reusable LDAP **directory password** on the WebDAV door with a
> **backend-generated, high-entropy `key:secret`** — individually named, revocable,
> shown once, WebDAV-scoped — and let a deployment **force** it (disable
> password-based WebDAV auth). This composes with, and is orthogonal to, the §14
> origin/session gate: §14 governs *origin & liveness*, §15 governs *the credential*.

---

## 1. Goals & threat model

Two related controls that together neutralize a leaked/weak **directory
password**:

1. **2FA on interactive (Web UI) login** — TOTP authenticator app, with an
   **email one-time-code fallback**. A stolen password alone can no longer mint a
   Web session.
2. **WebDAV IP-binding** — WebDAV clients speak HTTP Basic (username+password)
   and **cannot do interactive 2FA or bearer tokens**, so 2FA cannot protect the
   WebDAV door directly. Instead we require that the user has a **recent,
   authenticated (2FA'd) Web-UI session from the same client IP**, and only allow
   WebDAV requests from an IP that currently has such a session. A leaked WebDAV
   password is then useless unless the attacker also holds a live, 2FA'd browser
   session **from the same egress IP** — a strong, pragmatic second factor for a
   protocol that can't prompt.

**Hybrid availability goal.** These controls must **degrade gracefully**: in a
hybrid deployment, when the upstream/cloud link is lost, **LAN users must keep
READ access to WebDAV.** The hardening therefore must never make the *loss of the
cloud* the thing that blocks on-prem access — the LAN path stays entirely local
(§5.7). This is an explicit design constraint, not an afterthought.

**Threat model.** Adversary has a valid username + password (phished, reused,
brute-forced, or leaked) but does **not** control the user's authenticator device
**and** is not operating from the user's current network egress IP.

- Web UI: blocked by 2FA (needs the TOTP code / email code).
- WebDAV: blocked by IP-binding (needs a live 2FA'd Web session from the same IP).

**Non-goals.** Not replacing LDAP passwords; not adding 2FA *inside* the WebDAV
protocol (clients can't do it); not protecting against a fully-compromised client
device sharing the victim's egress IP (out of scope — that adversary already has
the browser session). Not FIDO2/WebAuthn in v1 — that is a **V2** extension (needs security-key /
platform-authenticator hardware to build & test); the full design + plan is in
**§13**.

---

## 2. Where the pieces live (current architecture)

| Concern | Component | Anchor |
|---|---|---|
| Interactive login (mint session JWT) | **http_bridge** (C++) | `issueToken`, `oauthCallback`, `refreshToken`, `clientIp()` in `src/http_server.cpp` |
| Identity service: users, SMTP, Redis token store, Postgres, rate-limiting | **ldap_manager** (Python) | `email.py` (Mailer), `tokens.py` (TokenStore + `rate_ok`), `ldap_client.py` (`update_profile`), `routers/me.py`, `routers/public_auth.py`, `audit.py` |
| WebDAV door (per-request Basic→LDAP) | **webdav_bridge** (C++) | `authenticateUser` — called by **all 8 handlers** in `src/webdav_server.cpp` |
| Web UI | **frontend** (Vue 3) | `LoginView.vue`, `stores/auth.ts` (`ldapLogin`, `completeOAuth`), `ProfileView.vue`, `TenantAdminView.vue` |
| Shared cross-server state | **Redis** | already the shared broker; http_bridge links hiredis (`HTTPBRIDGE_HAS_HIREDIS`) |
| Edge / client-IP identity | **nginx** | `docker_unified/images/nginx/snippets/*.conf` set `X-Real-IP $remote_addr` + `X-Forwarded-For $proxy_add_x_forwarded_for` on both the tenant (UI/API) and webdav routes |

**Design principle:** the **identity service (ldap_manager) owns the MFA secret,
enrollment, verification, and email delivery**; **http_bridge orchestrates the
login and remains the sole JWT minter**; **webdav_bridge is a thin enforcement
point** that consults a shared Redis binding. This keeps the sensitive TOTP
secret out of the C++ bridges and reuses ldap_manager's SMTP/Redis/rate-limiting.

---

## 3. Client-IP trust (a prerequisite for BOTH features) ⚠️

The IP-binding is only as trustworthy as the client IP we compute — and today it
is **not** trustworthy for a security control:

- `http_bridge::clientIp()` takes the **first** `X-Forwarded-For` hop. Behind
  nginx, `$proxy_add_x_forwarded_for` **appends** the real `$remote_addr` to any
  client-supplied XFF, so the **first** hop is the *client-supplied* (spoofable)
  value. A malicious client can set `X-Forwarded-For: <victim-ip>` and have it
  believed.

**Required change (both bridges):** derive the client IP from the
**proxy-authoritative** source — nginx's `X-Real-IP` (`$remote_addr`) — or, if XFF
is used, the **rightmost untrusted hop** after stripping the known proxy chain.
Never the leftmost hop. This must be shared, correct, and used for the binding,
the audit `source_addr`, and rate-limiting.

- Config: `TRUSTED_PROXY_HOPS` (int, default 1) or `TRUSTED_PROXY_CIDRS` — how
  many trailing XFF entries / which source addresses are our own proxies.
- webdav_bridge currently uses `request.clientAddress()` only for logging; it must
  adopt the same trusted-IP derivation.

This item is a **hard dependency** — the whole IP-binding control rests on it, and
it also improves audit/rate-limit accuracy. Treat it as Phase 0.

---

## 4. Feature A — 2FA (TOTP + email fallback)

### 4.1 Enrollment (self-service, ldap_manager)

New `/v1/me/2fa/*` endpoints (mirror the existing `me.py` self-service pattern,
bearer-authenticated, audited):

- `GET  /v1/me/2fa/status` → `{enabled, methods:[...], has_recovery_codes}`
- `POST /v1/me/2fa/setup` → generates a TOTP secret, returns
  `{secret, otpauth_uri, qr_svg}` (server renders the `otpauth://totp/...` URI;
  the secret is held **pending** until verified). Uses `pyotp` (or equivalent).
- `POST /v1/me/2fa/verify-setup` `{code}` → on a valid TOTP, marks 2FA enabled and
  returns one-time **recovery codes** (shown once).
- `POST /v1/me/2fa/disable` `{code | password}` → disables (requires a current
  factor). Audited.
- `POST /v1/me/2fa/recovery-codes` `{code}` → regenerate recovery codes.

### 4.2 Secret storage — **recommend Postgres (encrypted), not LDAP**

ldap_manager already runs Postgres (email templates, auto-created on startup). Add:

```sql
CREATE TABLE user_2fa (
  tenant        TEXT NOT NULL,
  uid           TEXT NOT NULL,
  totp_secret   BYTEA,            -- AES-GCM encrypted with a service key
  enabled       BOOLEAN NOT NULL DEFAULT false,
  recovery_codes JSONB,           -- array of {hash, used_at}
  updated_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (tenant, uid)
);
```

- **Why Postgres over an LDAP `totpSecret` attribute:** no LDAP schema change /
  ACL grant needed; keeps secret + enabled flag + recovery codes (hashed) +
  versioning together; encrypt-at-rest with a dedicated key (`TOTP_SECRET_KEY`,
  base64, like the core's `AT_REST_KEY`); a compromised LDAP read (e.g. the broad
  service bind) does not leak MFA secrets.
- **Alternative (documented):** custom LDAP attribute via `update_profile()` —
  simpler co-location with the account, but requires schema/permission work and
  spreads the secret into the directory read path. *Not recommended.*

### 4.3 Challenge/verify during login (the coordination) — **recommend P1**

**P1 — http_bridge orchestrates, ldap_manager verifies, http_bridge mints.**

1. `POST /v1/auth/token` (Basic) → http_bridge does the LDAP bind as today
   (**+ empty-password guard, C2**). On success it asks ldap_manager (internal,
   server-to-server) *"does this user require 2FA?"* — via a new
   `GET /internal/2fa/required?uid=&tenant=` (or a cached flag).
2. **If 2FA required:** http_bridge does **not** mint a session token. It returns
   **HTTP 200 with an MFA challenge**:
   ```json
   { "mfa_required": true,
     "mfa_token": "<short-lived signed pre-auth JWT>",
     "methods": ["totp","email"] }
   ```
   The `mfa_token` is an HS256 JWT signed by http_bridge: `amr:["pwd"]`,
   `mfa_pending:true`, `sub`, `tenant`, bound to the **trusted client IP**,
   `exp` ~5 min. It is **not** accepted by the resource gate (which requires a
   fully-authenticated token — see §4.4).
3. **TOTP:** client submits `POST /v1/auth/2fa {mfa_token, code}`. http_bridge
   verifies `mfa_token` (sig+exp+IP), then calls ldap_manager
   `POST /internal/2fa/verify {uid, tenant, code}` (server-to-server) → ok/deny.
4. **Email fallback:** client submits `POST /v1/auth/2fa/email {mfa_token}` →
   http_bridge asks ldap_manager to send an email code
   (`POST /internal/2fa/email-challenge`), which uses `TokenStore.issue("2fa_email",
   uid, 300)` + `Mailer.send()` with a new `2fa_email_code` template. Client then
   submits the emailed code via the same `POST /v1/auth/2fa`.
5. **On success:** http_bridge mints the full session JWT with
   `amr:["pwd","otp"]` (or `["pwd","email"]`), audits `login_success` (already
   fail-closed, L3), and **registers the WebDAV IP-binding** (§5).

**Why P1:** the TOTP secret and email delivery stay in the identity service; the
C++ bridge never sees the secret; http_bridge stays the single minter. Cost: one
internal call at login (login is infrequent; can be cached briefly). The
`FILEENGINE_JWT_SECRET` is already shared for verification, so the `mfa_token` and
its exchange verify cleanly.

**Alternative (documented, not recommended):** **P2** — http_bridge reads the
secret from a shared store and verifies TOTP (HMAC-SHA1) itself. Avoids the
server-to-server call but puts the secret in the C++ bridge and splits MFA between
bridge (TOTP) and ldap_manager (email). Messier and wider secret exposure.

### 4.4 Resource gate must require a fully-authenticated token

`http_bridge::authenticate()` and every service that verifies the JWT must treat
`mfa_pending:true` (or missing required `amr`) as **not authenticated** for
resource routes — so the pre-auth token can *only* be exchanged at `/v1/auth/2fa`,
never used to reach data. (Small addition to the existing verify path.)

### 4.5 OAuth logins

OAuth (IdP) logins already asserted a factor at the IdP. Policy decision (§11):
either (a) trust the IdP's MFA and skip our 2FA, or (b) still require our TOTP for
`totp_required` tenants. Recommend **(a)** when the IdP asserts MFA (`amr`/`acr`),
else **(b)**.

### 4.6 Enforcement policy

- Per-user opt-in (self-enroll) **and** per-tenant requirement:
  `totp_required_tenants` (ldap_manager Settings) toggled by a tenant admin
  (`POST /v1/admin/tenants/2fa-required`). When required, a user without 2FA is
  forced into enrollment on next login (a `mfa_enroll_required` challenge).
- Rate-limit challenge attempts with `TokenStore.rate_ok(f"2fa:ip:{ip}", 5, 300)`
  and `f"2fa:uid:{uid}"`; lock/backoff on repeated failure; audit every attempt.

### 4.7 New audit events (auth category)

`2fa_setup`, `2fa_verify_setup`, `2fa_disable`, `2fa_challenge` (detail: method),
`2fa_verify` (ok/denied, method, attempt#), `2fa_recovery_used`. All fail-closed
where they mint a session (consistent with L3).

### 4.8 Method-availability policy (configurable; disable weak email for critical tenants)

Which 2FA methods are *offered* is configurable, so weaker methods — notably the
**email fallback** — can be disabled for critical-security tenants and deployments.

- **Deployment cap** `MFA_ALLOWED_METHODS` (e.g. `totp,email` [,`webauthn` in V2]) —
  the hard maximum for the whole deployment. A regulated / air-gapped deployment
  sets `totp` (or `totp,webauthn`) to forbid email **everywhere**, regardless of
  tenant settings.
- **Per-tenant** `mfa_allowed_methods` (ldap_manager Settings / tenant config) — a
  subset a tenant admin may further restrict but **never exceed** the cap. A
  high-security tenant sets `totp` only even if the deployment permits email.
- **Effective set = deployment_cap ∩ tenant_allowed.** A method is available only if
  present in **both**.

Enforcement:
- The login challenge (§4.3) returns `methods` = the **effective set** only, so a
  critical tenant's users are never offered "use an email code".
- **Defense-in-depth:** `POST /v1/auth/2fa` and the email-challenge endpoint
  **reject** any method not in the effective set — never trust the client to honor
  the offered list.
- Enrollment (§4.1) hides/blocks disabled methods.

**Recovery trade-off (surface to admins).** Disabling email hardens against
mailbox compromise (email is the weakest factor — §9) but removes a *self-service*
recovery path: with email off, a user who loses their TOTP authenticator recovers
only via **one-time recovery codes** or an **admin-assisted reset** (tenant admin
re-issues enrollment / recovery codes, audited). Critical tenants should therefore
**mandate recovery-code generation at enrollment** and document the admin-reset
runbook.

---

## 5. Feature B — WebDAV IP-binding

### 5.1 Binding store (Redis — spans app servers)

On **every event that establishes a fresh, fully-authenticated Web session** for a
user — `issueToken` (post-2FA), `oauthCallback`, and `refreshToken` — http_bridge
records the user's **trusted client IP** into Redis:

```
SET  webdav:ipbind:{tenant}:{uid}:{ip}  "1"  EX {WEBDAV_IP_BIND_TTL_SECONDS}
```

- A **set of per-IP keys with independent TTLs** (not a single IP) so a user with
  several concurrent networks/devices is supported; each login/refresh refreshes
  the TTL for that IP. `refreshToken` (which the SPA calls at ~60% of token life)
  keeps the binding alive for as long as the browser session is active.
- Default `WEBDAV_IP_BIND_TTL_SECONDS` ≈ session lifetime (e.g. 12h) — long enough
  to not disrupt a working day, short enough that a stale IP expires.
- Keys namespaced by tenant+uid; value can carry metadata (login time, amr).

### 5.2 Enforcement (webdav_bridge)

In `authenticateUser` (the single choke point for all 8 handlers), after the LDAP
bind succeeds:

1. Derive the **trusted client IP** (§3).
2. **Allow if** the binding exists **or** the client IP is in the tenant's trusted
   internal CIDRs (§5.6): `EXISTS webdav:ipbind:{tenant}:{uid}:{ip}  OR  ip ∈
   WEBDAV_IP_BIND_TRUSTED_CIDRS`. Otherwise **403** with a clear body: *"WebDAV
   access requires a recent Web UI sign-in from this network (IP a.b.c.d). Sign in
   at <url>, then retry."*
3. Gate behind `WEBDAV_IP_BINDING_ENABLED` (default **off** for backward compat;
   per-tenant enablement via a Redis/`config` flag).

Result: a correct-but-leaked WebDAV password is refused unless the attacker also
has a live, 2FA'd Web session from the **same** IP within the TTL.

### 5.3 Redis-unavailable posture — fail-closed for EXTERNAL, unaffected for LAN

If Redis is unreachable, webdav_bridge cannot verify a binding — but this only
matters for **external** requests. **LAN/internal requests are resolved by the
local, cloud-independent trusted-CIDR path (§5.6) *before* any Redis lookup**, so a
Redis (or full upstream) outage never blocks them. This is precisely what makes
degraded-mode LAN read access work (§5.7) — the availability goal (§1) and the
security control do not conflict once the LAN path is local.

For **external** requests — whose security *depends on* the Redis binding —
**fail-closed** (deny when the binding can't be checked) is the safe default;
denying an unknown external client during an outage is the conservative choice.
Configurable via `WEBDAV_IP_BINDING_FAIL_OPEN` (default false), with Redis
reachability surfaced in webdav's `/readyz` (mirror L3 A-ii). Net: an outage
drains *external* readiness while LAN reads continue uninterrupted.

### 5.4 Self-service visibility (frontend + ldap_manager)

- `ProfileView.vue` "WebDAV access" section: list the caller's currently-bound IPs
  (from a read endpoint), a "this connection: a.b.c.d — bound ✓/✗", and a
  "revoke" per IP. Backed by ldap_manager read/unbind endpoints that operate on
  the Redis keys (`/v1/me/webdav-ips` GET/DELETE), audited (`webdav_ip_bind`,
  `webdav_ip_unbind`).
- Copy already primes users: ProfileView notes *"You need a directory password for
  WebDAV even if you sign in with SSO"* — extend it to explain the IP-binding.

### 5.5 Edge cases to document/handle

- **IPv4/IPv6 mismatch:** the browser may egress IPv6 while `davfs`/Finder uses
  IPv4 → different bound IP → WebDAV denied. Mitigations: (a) bind **both** stacks
  seen at login if available; (b) document a "use the same stack" requirement; (c)
  optional /64 prefix match for IPv6. **Decision needed (§11).**
- **NAT / shared egress:** browser + WebDAV client behind one NAT share the public
  IP → works transparently (this is the common home/office case).
- **Carrier-grade NAT / roaming mobile:** IP changes frequently → WebDAV breaks
  until the next Web sign-in. This is *by design* (the security goal), but note the
  UX cost; the SPA can surface "re-bind this connection."
- **Corporate proxy pools:** the egress IP may rotate across a pool → use the
  per-tenant trusted-CIDR exemption (§5.6) for the internal ranges.
- **XFF spoofing:** neutralized by §3 (trusted-IP derivation) — critical here, and
  doubly so for the LAN exemption (§5.6).

### 5.6 Same-LAN / internal-network exemption (hybrid deployments)

Hybrid deployments (some users on the corporate LAN / site VPN, some remote) can
exempt **internal** traffic from the binding while still enforcing it for external
requests, via a per-tenant trusted-CIDR set. The enforcement (§5.2) becomes an OR:

```
allow WebDAV if:
    EXISTS webdav:ipbind:{tenant}:{uid}:{ip}       # a recent 2FA'd Web session at this IP
    OR  client_ip ∈ WEBDAV_IP_BIND_TRUSTED_CIDRS   # on the LAN / VPN → skip the binding
```

**Hard requirement — the CIDR match MUST use the *authoritative* client IP, never
a client-supplied header.** This is §3 inverted and *more* dangerous: if the LAN
check reads a spoofable `X-Forwarded-For` hop, an external attacker sends
`X-Forwarded-For: 10.0.0.5` and exempts themselves *into* the trusted zone with
just the leaked password — strictly worse than having no exemption. Use the real
TCP peer (`request.clientAddress()`) when internal clients reach webdav over a
distinct trusted path, or a proxy header **only** if that proxy provably
strips/overrides any client-supplied value.

**Feasibility is topology-dependent.** Cleanest and safest when internal traffic
has a **separate, trusted ingress whose source IP is genuine** (internal
VLAN/ingress, not the public edge). A single public hairpin ingress — or a CDN/LB
in front of nginx — may not preserve a reliable LAN-vs-external distinction (NAT
can make LAN clients appear as the public IP); validate per deployment before
relying on it.

**Security trade-off (be deliberate).** The exemption *keeps* protection against
external leak-and-exfiltrate but *gives up* protection on the exempted network: a
leaked WebDAV password becomes exploitable by an insider, a compromised internal
host, lateral-movement malware, or any device that joins that LAN — the "trusted
internal network" assumption that zero-trust rejects. Reasonable when the primary
threat is external exfiltration and the LAN is trusted; keep the binding on
internally for regulated / BYOD tenants.

**Recommended shape:**
- `WEBDAV_IP_BIND_TRUSTED_CIDRS` — **per-tenant, default empty** (opt-in); nothing
  is exempted until a deployment declares its trusted ranges.
- Matched against the **authoritative** IP only (the Phase-0 §3 primitive).
- **Allow-but-audit:** a LAN request that bypasses the binding still emits a
  `webdav_access` event with `via:"trusted_cidr"` — never a silent allow, so the
  exemption is traceable.
- Per-tenant, so one tenant can relax it while a regulated tenant keeps the binding
  on even for LAN.
- **Evaluated *before and independently of* the Redis binding lookup, from static
  config only** — it needs no cloud connectivity, so it doubles as the
  degraded-mode / outage-survival path (§5.7).

### 5.7 Degraded-mode / offline LAN read access (hybrid resilience)

The hybrid availability goal (§1): **when the upstream/cloud link is lost, LAN
users keep READ access to WebDAV.** The hardening in this proposal must degrade
gracefully — LAN reads must not depend on the cloud services that are down. Three
local dependencies must be satisfied, and the platform already supports all three:

1. **Auth locally** — an **on-prem read-only LDAP replica**. Both bridges already
   have replica failover (`FILEENGINE_LDAP_ENDPOINT_REPLICA` + the `failover.h`
   circuit-breaker), so password binds continue against the local replica when the
   master is unreachable.
2. **Serving locally** — an **on-prem core engine + local storage/cache**. The core
   already keeps a local-FS store + `CacheManager` and does async S3 sync, and
   `ConnectionPoolManager` supports read-only DB failover — so cached/local files
   are served without the cloud.
3. **A gate that needs no cloud** — the §5.6 trusted-CIDR exemption is a purely
   local decision (static config + authoritative peer IP, no Redis), so LAN users
   are unaffected by a Redis/cloud outage.

**Read-only downgrade.** During a detected upstream degradation (core / object-store
sync unreachable), LAN WebDAV should downgrade to **read-only** — serve `GET` /
`PROPFIND` / `OPTIONS`, but refuse `PUT` / `DELETE` / `MKCOL` / `MOVE` / `COPY`
(`503`/`423` with a clear message) — so no un-syncable writes are created and there
is no split-brain to reconcile when the link returns. (Full offline read-write with
later reconciliation is a much larger, separate effort — **out of scope for v1**.)

- Config: `WEBDAV_OFFLINE_READONLY` (`auto` | `on` | `off`) + a degradation signal
  (core `/readyz` / S3-sync health) that flips the mode.
- Audit: `webdav_degraded_mode` transitions; and any write refused while degraded.

**Requirement summary:** outage-surviving LAN reads require the site to deploy the
on-prem replicas above; the IP-binding / 2FA additions are deliberately designed to
be **inert on the LAN path** so they never become the thing that breaks
availability. 2FA is unaffected — it is a Web-UI concern; LAN WebDAV takes the
trusted-CIDR path and never prompts.

---

## 6. End-to-end sequences (textual)

**Interactive login with TOTP + IP-binding**
```
Browser → nginx → http_bridge  POST /v1/auth/token (Basic)
  http_bridge: LDAP bind ok; ldap_manager says 2FA required
  → 200 { mfa_required, mfa_token(amr=pwd, ip=X, exp=5m), methods }
Browser → http_bridge  POST /v1/auth/2fa { mfa_token, code }
  http_bridge: verify mfa_token (sig/exp/ip==X)
             → ldap_manager POST /internal/2fa/verify {uid,tenant,code} → ok
  → mint session JWT (amr=[pwd,otp]); audit login_success (fail-closed)
  → Redis SET webdav:ipbind:{tenant}:{uid}:{X} EX ttl
  → 200 { token, expires_in }
```

**WebDAV request**
```
davfs → nginx (X-Real-IP=X) → webdav_bridge  PROPFIND /...
  authenticateUser: LDAP bind ok (empty-pw guard)
  → derive trusted IP = X
  → Redis EXISTS webdav:ipbind:{tenant}:{uid}:{X}?  yes → allow
  (attacker from IP Y with the leaked password → key absent → 403)
```

---

## 7. Data model & config summary

**Redis keys**
- `webdav:ipbind:{tenant}:{uid}:{ip}` = 1, TTL `WEBDAV_IP_BIND_TTL_SECONDS`
- `ldapmgr:2fa_email:{sha256(code)}` = uid, TTL 300 (reuses TokenStore)
- rate buckets: `2fa:ip:{ip}`, `2fa:uid:{uid}`

**Postgres (ldap_manager)** — `user_2fa` (§4.2)

**New config**
| Var | Where | Default |
|---|---|---|
| `TRUSTED_PROXY_HOPS` / `TRUSTED_PROXY_CIDRS` | both bridges | 1 / nginx CIDR |
| `MFA_ENABLED` | http_bridge + ldap_manager | false |
| `TOTP_SECRET_KEY` (base64, AES-GCM) | ldap_manager | (required if MFA on) |
| `MFA_TOKEN_TTL_SECONDS` | http_bridge | 300 |
| `MFA_ALLOWED_METHODS` (deployment cap; e.g. `totp,email`, §4.8) | http_bridge + ldap_manager | `totp,email` |
| `mfa_allowed_methods` (per-tenant subset; disable email for critical tenants) | ldap_manager | = deployment cap |
| `totp_required_tenants` | ldap_manager | ∅ |
| `WEBDAV_IP_BINDING_ENABLED` | webdav_bridge (+http_bridge writes) | false |
| `WEBDAV_IP_BIND_TTL_SECONDS` | http_bridge | 43200 (12h) |
| `WEBDAV_IP_BINDING_FAIL_OPEN` | webdav_bridge | false |
| `WEBDAV_IP_BIND_TRUSTED_CIDRS` (per-tenant; internal/LAN exemption, §5.6) | webdav_bridge | ∅ |
| `WEBDAV_OFFLINE_READONLY` (`auto`/`on`/`off`; degraded-mode LAN reads, §5.7) | webdav_bridge | auto |
| `FILEENGINE_LDAP_ENDPOINT_REPLICA` (on-prem read replica; already exists) | both bridges | ∅ |
| `FILEENGINE_REDIS_*` | webdav_bridge (NEW — add hiredis like http_bridge) | shared |

All default **off** → the change is inert until a deployment opts in; fully
backward compatible.

---

## 8. API additions

**http_bridge**
- `POST /v1/auth/2fa` `{mfa_token, code}` → `{token, expires_in}` | 401
- `POST /v1/auth/2fa/email` `{mfa_token}` → 202 (sends email code)
- (modified) `POST /v1/auth/token` may now return an `mfa_required` challenge
- internal: `GET /internal/2fa/required`, `POST /internal/2fa/verify`,
  `POST /internal/2fa/email-challenge` (server-to-server; not client-facing —
  restrict to loopback / shared secret)

**ldap_manager**
- `GET/POST /v1/me/2fa/*` (enrollment, §4.1)
- `GET/DELETE /v1/me/webdav-ips` (visibility, §5.4)
- `POST /v1/admin/tenants/2fa-required` (enforcement toggle)
- internal 2FA endpoints backing http_bridge

**frontend**
- `TotpChallengeView.vue` (TOTP + "use email code" tabs) — mirrors ResetPassword
- `TotpEnrollmentView.vue` (QR + verify + recovery codes) — mirrors SetPassword
- `ProfileView.vue`: 2FA section + WebDAV IP section
- `TenantAdminView.vue`: "Security" tab (require-2FA toggle, per-user 2FA status,
  allowed-methods policy incl. an "allow email recovery" toggle — §4.8)
- `stores/auth.ts` `ldapLogin`: branch on `mfa_required`; new `submit2fa()`

---

## 9. Security considerations

- **Secret at rest:** TOTP secrets AES-GCM-encrypted (`TOTP_SECRET_KEY`), never
  logged, never returned after enrollment; recovery codes stored **hashed**.
- **MFA-pending token** cannot reach data (§4.4); IP-bound + 5-min TTL limits
  replay; single-use exchange.
- **Email fallback** is the weakest factor (mailbox compromise) — rate-limit, short
  TTL, audit; and it is **disable-able per tenant and capped per deployment** via
  the method-availability policy (§4.8), so critical tenants/deployments can forbid
  it outright (falling back to recovery codes + admin reset).
- **IP source integrity** is the linchpin (§3) — without it, the binding is
  bypassable by header spoofing.
- **No new bypass of ACLs** — both features sit *in front of* the existing
  authn/authz; the core's trust model is unchanged.
- **Audit completeness** — all new flows emit auth events (builds on L3).
- **Availability trade-offs** stated explicitly (§5.3): fail-closed WebDAV +
  `/readyz` audit/binding health.

## 10. Testing plan

- **Unit:** TOTP verify (RFC 6238 test vectors) incl. skew window; trusted-IP
  derivation (spoofed XFF rejected, X-Real-IP honored); binding key TTL logic;
  recovery-code single-use; email-code TTL/rate-limit.
- **Bridge C++:** extend `test_security.cpp` (mfa_token verify, IP derivation) and
  add a webdav unit for the binding check (mock Redis).
- **E2E (live, patterns already in repo):** login→TOTP→token; login→email-code→
  token; WebDAV allowed from a bound IP, **denied from a different IP** (mirror
  `test_e2e_tenant_boundary.sh`); Redis-down → webdav fail-closed + `/readyz` 503;
  refresh keeps the binding alive.
- **Frontend:** vitest for the challenge/enrollment views + `mfa_required` branch.

## 11. Open decisions (need your call)

1. **Secret storage:** Postgres-encrypted (recommended) vs LDAP `totpSecret`.
2. **Coordination:** P1 (ldap_manager verifies, http_bridge mints — recommended)
   vs P2 (bridge verifies TOTP).
3. **WebDAV Redis-down posture:** fail-closed (recommended) vs fail-open.
4. **IP-binding granularity:** exact IP (recommended) vs IPv6 /64 prefix; how to
   handle IPv4/IPv6 dual-stack at login.
5. **Same-LAN / internal exemption (§5.6):** enable the per-tenant
   `WEBDAV_IP_BIND_TRUSTED_CIDRS` (skip the binding for internal/VPN ranges) — and
   accept the insider/lateral-movement trade-off it implies? If yes, confirm the
   authoritative-IP source per the deployment topology, and keep it allow-but-audit.
6. **Degraded-mode LAN reads (§5.7) — read-only is confirmed spec.** Remaining:
   confirm the on-prem replica topology (LDAP read replica + local core/cache) is
   deployed at hybrid sites, and the degradation signal that flips read-only mode.
   Offline read-**write** with reconciliation is explicitly **out of scope for v1**
   — the no-corruption / conflict-reconciliation guarantees make it a much larger,
   separate effort.
7. **OAuth + 2FA:** trust IdP MFA (recommended) vs always require our TOTP.
8. **Email fallback:** now specified as configurable — disable-able per tenant with
   a deployment-wide cap (§4.8). Remaining call: the **default** (email allowed by
   default vs off), and whether high-security *deployments* ship with it off.
9. **Binding TTL** (default 12h) and whether `refreshToken` should extend it.
   Under the §14 session modes this TTL becomes the crash/no-logout **backstop**
   rather than the working lifetime — see §11.13 (revised).
10. **TOTP library** for ldap_manager (`pyotp`) and QR rendering (server-side SVG).

*Items 11–13 are added by the §14 amendment (origin-aware gate + session-liveness);
they are stated in **§14.7** to keep the amendment self-contained.*

## 12. Phased implementation plan

- **Phase 0 — Trusted client IP (§3).** Shared, correct IP derivation in both
  bridges; use it for binding, audit, rate-limit. *Prerequisite; independently
  valuable.*
- **Phase 1 — 2FA backend.** ldap_manager: `user_2fa` table, enroll/verify
  endpoints, email template + code, internal verify API; http_bridge:
  `mfa_required` challenge + `/v1/auth/2fa`, mint with `amr`, gate on
  `mfa_pending`.
- **Phase 2 — 2FA frontend.** Enrollment + challenge views; ProfileView 2FA
  section; per-tenant enforcement toggle in TenantAdmin.
- **Phase 3 — WebDAV IP-binding.** http_bridge writes bindings on login/refresh;
  webdav_bridge adds hiredis + the enforcement check + `/readyz` health; ProfileView
  "WebDAV access" section.
- **Phase 4 — Hardening & rollout.** Recovery codes, rate-limit/lockout tuning,
  fail-closed posture, per-tenant enablement, docs (end-user docs live in the
  frontend repo per project convention), audit dashboards for the new events.

Each phase is behind a default-off flag and independently shippable.

---

## 13. Future extension (V2): FIDO2 / WebAuthn

Deferred to **V2** — needs security-key / platform-authenticator hardware to build
and test the ceremonies end-to-end. Captured here so the design is ready to pick up
when that hardware is on hand.

### 13.1 Background — what it is and why it's stronger

**FIDO2** = **WebAuthn** (the W3C browser API `navigator.credentials.create()` /
`.get()`, supported by every modern browser) + **CTAP2** (how the browser talks to
the authenticator device). Instead of a *shared secret* (a password, a TOTP seed),
the authenticator generates a **public/private key pair per website**; the
**private key never leaves the device**, and the server stores only the **public
key**. Login is a challenge-response: the server sends a random challenge, the
authenticator signs it, the server verifies the signature against the stored public
key.

Two properties that beat passwords **and TOTP**:

1. **Phishing-resistant (the big one).** The signature is bound to the **origin**
   (the "RP ID" = the domain). A credential for `app.example.com` cannot sign for a
   look-alike phishing domain — the browser enforces it. A phished TOTP code can be
   relayed in real time within its 30-second window; a WebAuthn assertion cannot be
   replayed to another site at all.
2. **No server-side secret.** A database breach yields only public keys — useless.
   (Contrast TOTP, where a leaked seed store forges valid codes forever.)

Plus a genuine possession factor (you must hold the device) and, optionally, a
biometric/PIN ("user verification") for inherence.

**Vocabulary.**
- *Authenticator*: **platform** (Touch ID / Face ID / Windows Hello / Android
  biometrics) or **roaming** (a YubiKey / security key, or a phone acting as a key
  over "hybrid" Bluetooth transport).
- *Passkey*: the consumer name for a WebAuthn credential — usually a **discoverable,
  synced** one (iCloud Keychain / Google Password Manager). Can be a 2nd factor or
  replace the password entirely (**passwordless**). Device-bound passkeys (on a
  security key) don't sync.
- *RP (Relying Party)*: your service, identified by an **RP ID** (a domain);
  credentials are scoped to it (see §13.3).
- *Ceremonies*: **registration/attestation** (`create()` → store {public key,
  credential ID}) and **authentication/assertion** (`get()` → sign challenge →
  verify).
- *Attestation* (optional): cryptographic proof of the authenticator's make/model —
  `"none"` for consumer/privacy; **require** it for an enterprise "issued-keys-only"
  policy.
- *Sign counter* (optional): monotonic counter to detect cloned authenticators.

### 13.2 How it maps onto the stack (same roles as the V1 TOTP design)

| Role | Component |
|---|---|
| **Relying-Party server** (challenge gen, assertion/attestation verify) | **ldap_manager** (Python `py_webauthn`) — same "P1" split as TOTP; the C++ bridge does **not** parse CBOR/COSE |
| **JWT minting / orchestration** | **http_bridge** — starts the ceremony, mints on success (`amr` includes `webauthn`) |
| **WebAuthn client** | **frontend** — `navigator.credentials.create()/get()` via `@simplewebauthn/browser` |
| **Credential store** | **ldap_manager Postgres** — `user_webauthn_credentials(tenant, uid, credential_id, public_key, sign_count, transports, aaguid, nickname, created_at, last_used)`; a user may register several authenticators |
| **Challenge store** | **Redis** — single-use, short-TTL challenges (reuse the `TokenStore` pattern from V1) |

### 13.3 RP-ID vs per-tenant subdomains (the one architectural decision)

WebAuthn credentials bind to an **RP ID** (a registrable domain), but the
deployment uses per-tenant subdomains (`acme.example.com`):
- **RP ID = the registrable parent** (`example.com`) → one passkey spans every
  tenant subdomain + the apex (cleanest for one org's users; the login origin must
  be under it).
- **RP ID = the tenant subdomain** → credentials isolated per tenant; a multi-tenant
  user enrolls per tenant. More isolation, more friction.

Recommendation: RP ID = the domain the SPA is served from; document the subdomain
implications per deployment.

### 13.4 Two integration modes

- **(A) WebAuthn as a 2FA method — drop-in.** Reuses the V1 `mfa_token` →
  `POST /v1/auth/2fa {method, ...}` exchange, adding `method:"webauthn"` alongside
  `totp`/`email`. Password first, then an assertion; mint with
  `amr:["pwd","webauthn"]`. Minimal flow change.
- **(B) Passwordless / passkey first-factor.** The user logs in with a passkey, no
  password — username-less or username-first, then the assertion mints the session.
  Best UX, but reworks the login screen, enrollment, and recovery.

### 13.5 WebDAV relationship

WebAuthn is a **browser** mechanism — **WebDAV clients cannot do WebAuthn** (Basic
auth only), exactly like TOTP. So WebAuthn does **not** protect WebDAV directly; the
**IP-binding (§5) remains the bridge** that carries the Web-UI assurance to WebDAV.
Synergy: a phishing-resistant, hardware-backed Web login makes the session that
authorizes an IP-binding much stronger, so WebDAV inherits that strength.

### 13.6 Trade-offs & recovery

- Needs HTTPS + a stable RP-ID / origin (already in place via nginx TLS).
- **Recovery is the hard part:** losing your only authenticator = lockout. Mitigate
  by requiring **≥2 registered authenticators** and keeping the V1 **email /
  recovery-code fallback** — which then becomes the weakest link, so gate it
  carefully.
- Enterprise device policy via **attestation** (optional).
- Libraries: `py_webauthn` (server), `@simplewebauthn/browser` (client) — **no
  custom crypto**. The LDAP password (used by WebDAV) stays.

### 13.7 V2 plan

- **Prerequisite:** security key(s) (e.g. YubiKey) and/or a platform authenticator
  (Touch ID / Windows Hello / Android) to develop and verify the ceremonies.
- **V2 Phase A — WebAuthn as an additional 2FA method.** `user_webauthn_credentials`
  table; enroll/verify endpoints in ldap_manager; `method:"webauthn"` in the
  http_bridge challenge exchange; enrollment + "manage security keys" in
  `ProfileView`; per-tenant "allow / require WebAuthn". Low flow risk — reuses the
  V1 MFA plumbing.
- **V2 Phase B — passwordless passkey login.** First-factor passkeys; reworked
  login / enrollment / recovery UX. Do **after** Phase A proves the plumbing.

*Status: parked pending hardware; revisit to begin V2 Phase A.*

---

## 15. Amendment (2026-07-15): backend-generated WebDAV credentials (`key:secret`)

**Status:** Draft / for review — further hardens Feature B. **Composes with** §5/§14
(does not replace them). 2FA (§4) and V2 (§13) are untouched.

### 15.1 Motivation — harden the credential, not just the gate

§5/§14 gate WebDAV by *origin* and *session liveness*, but the credential presented
is still the user's **LDAP directory password** — the exact thing that is weak,
reused, phishable, and (because it is the account password) high-blast-radius. §14
revokes access on logout; it does not make the credential itself strong, nor let a
user revoke *one* WebDAV client without changing their account password.

This amendment replaces the directory password on the WebDAV door with a
**backend-generated `key:secret`** — an app-password / access-key pattern:

- **Strong by construction** — a 256-bit random secret; the user never chooses it,
  so it can't be weak or reused.
- **WebDAV-scoped** — it authenticates *only* the WebDAV door, never the Web UI, API,
  or LDAP itself. A leaked WebDAV key can't sign into anything else.
- **Individually revocable** — one per client/device, named, revoked independently
  and instantly, without touching the account password or other clients.
- **Basic-auth compatible** — the client still sends HTTP Basic (`username=key_id`,
  `password=secret`), so Finder / Windows Explorer / `davfs` need no changes.

This is the most direct answer to the §1 premise "*WebDAV can't do 2FA*": a 256-bit,
non-phishable, instantly-revocable credential is a far stronger stand-in for a second
factor than a user-chosen password, and it addresses the **root cause** (password
reuse/weakness) that motivated the whole IP-binding design.

### 15.2 Credential model

- **`key_id`** — public identifier, carried in the Basic **username**. Prefixed for
  secret-scanning / leak detection, e.g. `wdav_<random>`. Globally unique; resolves
  to `(tenant, uid)`.
- **`secret`** — 256-bit CSPRNG value, base32/base64url-encoded, carried in the Basic
  **password**. **Shown to the user exactly once** at creation, never retrievable.
- **At rest — hash-only, shown once.** Store only
  `HMAC-SHA256(secret, WEBDAV_CRED_HASH_PEPPER)` (a fast MAC is sufficient because the
  secret is full-entropy — a slow KDF protects *low*-entropy inputs and isn't needed
  here; the server pepper defends a bare DB read). Verify with a constant-time compare.
  **The plaintext secret is displayed exactly once, at creation, and is never stored in
  a recoverable form and never re-displayed.** If a secret is lost, the user
  **regenerates** on demand (§15.3) — there is no reveal path, so a DB read never yields
  a usable secret.

**The credential is a pure relational-DB record — it never touches LDAP.** No
directory schema change, no `totpSecret`-style attribute, no directory ACL grant.
LDAP stays the source of truth for **identity and roles (authorization) only**; the
`key:secret` (authentication material) lives entirely in Postgres. This mirrors the
§4.2 decision to keep the TOTP secret out of the directory, and for the same reasons:
a compromised LDAP read (e.g. the broad service bind) leaks no WebDAV credentials, and
the store carries the secret hash + label + usage/expiry/revocation together.

**Postgres (ldap_manager, mirrors the §4.2 `user_2fa` pattern):**

```sql
CREATE TABLE webdav_credential (
  key_id        TEXT PRIMARY KEY,        -- 'wdav_...'; the Basic username
  tenant        TEXT NOT NULL,
  uid           TEXT NOT NULL,
  secret_hash   BYTEA NOT NULL,          -- HMAC-SHA256(secret, pepper)
  label         TEXT,                    -- user-facing, e.g. 'MacBook Finder'
  created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
  last_used_at  TIMESTAMPTZ,
  expires_at    TIMESTAMPTZ,             -- optional (WEBDAV_CRED_EXPIRY_DAYS)
  revoked_at    TIMESTAMPTZ
);
CREATE INDEX ON webdav_credential (tenant, uid);
```

### 15.3 Lifecycle (self-service; supersedes the "directory password" for WebDAV)

Managed from `ProfileView.vue` (extends the §5.4 "WebDAV access" section, and
**replaces** the current *"set a directory password for WebDAV"* copy):

- **Create** — "Add WebDAV credential" `{label}` → returns `{key_id, secret}`,
  displayed **once** with copy-to-clipboard and a mount-string hint. The secret is
  never shown again. Cap per user via `WEBDAV_CRED_MAX_PER_USER`.
- **List** — name, created, last-used, expiry, status. **Metadata only — never the
  secret.**
- **Regenerate / force-recreate on demand (anytime)** — rotate a credential: mint a new
  `secret` for the same `key_id` (so an existing mount only needs its password updated)
  **or** issue a new `key_id` and revoke the old — a per-call choice. The new secret is,
  again, shown once; the old secret stops working immediately (bounded by the verify
  cache TTL, §15.7). This is both the routine rotation path and the recovery path when a
  secret is lost — since secrets are never re-displayable, "I lost it" is always
  resolved by regenerating.
- **Revoke** — delete a `key_id` (immediate, bounded by the verify cache TTL, §15.7).
- Audit: `webdav_cred_create`, `webdav_cred_rotate`, `webdav_cred_revoke`,
  `webdav_cred_use` (first use), `webdav_cred_auth_fail`.

### 15.4 Auth mode — "force strong credentials"

Per-tenant / per-deployment `WEBDAV_AUTH_MODE`:

| Mode | Directory password on WebDAV? | `key:secret`? |
|---|---|---|
| `ldap_password` | ✅ (today's behavior) | ✗ |
| `both` (**default**, migration) | ✅ | ✅ |
| `key_secret` (**hardened**) | **✗ rejected** | ✅ only |

`key_secret` is the "force very strong credentials" posture: the directory password
no longer authenticates WebDAV at all — a user **must** generate a `key:secret`. Ship
`both` for backward compatibility; a tenant/deployment opts into `key_secret` once its
users have migrated. A deployment cap can forbid `ldap_password` fleet-wide (mirrors
the §4.8 method-cap pattern).

### 15.5 Identity & authorization (no user password-bind needed)

In `authenticateUser` (the single choke point for all 8 handlers):

1. If the Basic username matches the key prefix (`wdav_`) **and** the mode allows
   `key_secret` → **key path**: verify `key:secret` (§15.7), resolve `(tenant, uid)`,
   and **cross-check** the key's tenant against the host-derived tenant (reject a
   mismatch — a key is bound to its tenant).
2. Else if the mode allows `ldap_password` → today's LDAP user-bind path. Under
   `key_secret`, this path is refused with a clear body pointing at
   ProfileView → "Add WebDAV credential".
3. **Roles** come from LDAP exactly as today, but via the **existing service-bind
   search** — `LDAPAuthenticator::searchUser` + `extractRolesFromGroups` already run
   on the service connection (`bind_dn_`/`bind_password_`), so the key path fetches
   roles **without** the user's password. Authorization (the core's ACL/RBAC) is
   unchanged.

The §14 origin/session gate still applies **after** this on the external branch (see
§15.6). `webdav_bridge` already links **libpq/pqxx** (the path→UUID resolver), so the
credential store is directly reachable if the direct-PG verify option is chosen.

### 15.6 How it composes with §14 (origin gate) and §4 (2FA)

`WEBDAV_AUTH_MODE` (§15, the *credential*) and `WEBDAV_EXTERNAL_GATE` (§14, *origin &
liveness*) are **independent, multiplying** layers. A deployment picks a posture:

| Credential (§15) | Origin/session gate (§14) | Result |
|---|---|---|
| `ldap_password` | `session_ip` | today + logout-revocation (the §14 baseline) |
| `key_secret` | off (`ip_ttl` disabled / no gate) | strong revocable credential, any origin — **kills the reuse/weak-password root cause**, but a key works with no live Web session (by design) |
| `key_secret` | `session_ip` or LAN-exempt | **strongest**: strong credential **AND** (LAN origin **or** live 2FA'd session) |

Key point: they solve *different* halves of §1. `key_secret` makes the credential
strong and scoped (so a leak is low-value and instantly revocable); §14 ties WebDAV to
Web-UI **liveness** (revoke on logout) and **origin**. `key_secret` alone does *not*
give logout-coupling — a generated key is meant to keep working while mounted,
independent of any browser session — so a deployment that specifically wants
"WebDAV dies when I log out of the browser" still needs §14. Most deployments should
adopt **`key_secret` first** (root-cause fix, minimal UX cost) and layer §14 where the
liveness coupling is required.

### 15.7 Verification path & caching (chatty-safe)

WebDAV is method-heavy (PROPFIND storms), so per-request verification must not hammer
a store. Two options (open decision §11.15):

- **(A, recommended — §2 principle) internal ldap_manager endpoint.** `POST
  /internal/webdav/verify {key_id, secret, tenant}` → `{uid}` | 401 (server-to-server,
  loopback / shared secret). Keeps the secret hash + pepper in the identity service;
  the C++ bridge never holds the pepper. `webdav_bridge` caches a **successful**
  result keyed by `(key_id, sha256(secret))` → `{uid, roles}` for
  `WEBDAV_CRED_VERIFY_CACHE_TTL_SECONDS` (default 60), so a PROPFIND burst is one
  verify. Never cache the plaintext secret; cache a salted hash → result.
- **(B) direct Postgres.** `webdav_bridge` reads `webdav_credential` itself (it already
  links libpq). Lowest latency, no internal hop, but spreads the pepper + HMAC into the
  C++ bridge.

**Revocation staleness.** Revoke is immediate at the store; the bridge honors it within
the cache TTL. For instant revocation, push-invalidate the cache via the existing Redis
event stream (a `webdav_cred_revoke` event), mirroring the core's cache-invalidation
pattern. Note the contrast with §14, whose logout revocation is immediate by design;
document the ≤`cache_ttl` window for `key_secret`.

### 15.8 Config delta (amends §7)

| Var | Where | Default | Note |
|---|---|---|---|
| `WEBDAV_AUTH_MODE` (`ldap_password`/`both`/`key_secret`) | webdav_bridge (+ldap_manager issues) | `both` | `key_secret` forces generated creds (§15.4) |
| `WEBDAV_CRED_VERIFY` (`ldap_manager`/`direct_pg`) | webdav_bridge | `ldap_manager` | verify split (§15.7) |
| `WEBDAV_CRED_HASH_PEPPER` (base64) | wherever verify runs | (required if enabled) | server pepper for the HMAC (hash-only, shown once) |
| `WEBDAV_CRED_VERIFY_CACHE_TTL_SECONDS` | webdav_bridge | 60 | bounds revocation staleness |
| `WEBDAV_CRED_MAX_PER_USER` | ldap_manager | 10 | per-user credential cap |
| `WEBDAV_CRED_EXPIRY_DAYS` | ldap_manager | ∅ (no expiry) | optional forced rotation |

All default to preserving today's behavior (`both`, no expiry), so the amendment is
inert until a deployment opts into `key_secret`.

### 15.9 API additions (amends §8)

**ldap_manager**
- `GET    /v1/me/webdav-credentials` → list metadata (key_id, label, timestamps, status);
  **never the secret**
- `POST   /v1/me/webdav-credentials {label}` → `201 {key_id, secret}` (secret shown once)
- `POST   /v1/me/webdav-credentials/{key_id}/rotate {new_key_id?: bool}` →
  `{key_id, secret}` — regenerate on demand (§15.3), old secret invalidated, new one
  shown once
- `DELETE /v1/me/webdav-credentials/{key_id}` → revoke (audited)
- internal: `POST /internal/webdav/verify` (§15.7 option A; loopback / shared secret)

There is intentionally **no endpoint to read back a secret** — lost secrets are
resolved by `rotate`, not retrieval.

**frontend**
- `ProfileView.vue`: "WebDAV credentials" panel — create (secret shown once), list
  (metadata), **regenerate/force-recreate**, revoke; replaces the "set a directory
  password" copy; keep the §5.4 bound-IP view when §14 is also on. Optional
  **mount-setup script generator** per §15.13.

### 15.10 Security considerations (extends §9)

- **Blast radius** — the WebDAV credential no longer *is* the account password; a leak
  is WebDAV-only and revocable without an account-wide password reset.
- **Entropy** — 256-bit CSPRNG; the user cannot weaken it. HMAC-with-pepper at rest; a
  bare DB read yields neither usable secrets nor an offline-crackable low-entropy hash.
- **Prefix for detection** — the `wdav_` prefix lets secret scanners and leaked-cred
  monitors flag exposure.
- **Still fronts the same ACLs** — like §4/§5, this sits in front of the core's
  unchanged authn→authz; roles/ACLs are untouched.
- **Interaction with §3** — the trusted-IP derivation and §14 gate still apply on the
  external branch; `key_secret` does not bypass them when both are enabled.

### 15.11 Open decisions (extends §11)

- **§11.14 — Auth-mode default & rollout:** ship `both` (recommended) and let
  tenants/deployments move to `key_secret`; confirm whether any deployment ships
  `key_secret` from day one.
- **§11.15 — Verify split:** internal ldap_manager endpoint (recommended, §2
  principle) vs direct-Postgres in webdav_bridge (lower latency; bridge already links
  libpq).
- **§11.16 — At-rest scheme: DECIDED — hash-only, shown once.** Store
  `HMAC-SHA256(secret, pepper)`; the secret is displayed only at creation and is never
  recoverable. Lost secrets are resolved by **regenerate/force-recreate on demand**
  (§15.3), not by a reveal path. (Remaining sub-choice, minor: HMAC-SHA256 + pepper —
  recommended for full-entropy secrets — vs a slow KDF as belt-and-suspenders.)
- **§11.17 — Revocation immediacy:** accept the ≤`cache_ttl` staleness (simplest) vs
  push-invalidate via the Redis event stream for instant revocation.
- **§11.18 — Deprecate the WebDAV directory password?** once `key_secret` is adopted,
  retire the directory-password concept (ProfileView copy, §5.4) — timeline TBD.

### 15.12 Testing & phasing (amends §10, §12)

- **Unit:** secret generation entropy/format; HMAC verify + constant-time compare;
  key→(tenant,uid) resolution incl. tenant-mismatch reject; cache TTL + event
  invalidation; `WEBDAV_AUTH_MODE` gating (password refused under `key_secret`).
- **E2E:** mount with a generated key → allowed; **revoke → denied within cache TTL
  (and immediately with push-invalidation)**; directory password **rejected** under
  `key_secret`, accepted under `both`; roles resolved via service-search match the
  LDAP-bind path.
- **Phasing (new Phase 5, after §12 Phase 3):** ldap_manager `webdav_credential` table
  + `/v1/me/webdav-credentials` + internal verify; webdav_bridge key path + verify
  cache + `WEBDAV_AUTH_MODE`; ProfileView panel. Default `both`, independently
  shippable, and orthogonal to the §14 gate work.

### 15.13 QOL: OS mount-setup script generator (optional)

A usability nicety on the §15 credentials panel. Mounting a WebDAV share by hand
(exact URL, `https`/`@SSL`, per-OS driver quirks) is the fiddly part of onboarding,
and the panel already knows the precise URL and `key_id` — so it can emit a
ready-to-run helper. **Explicitly out of the core security scope; purely convenience.**

**Behavior.**
- After create/rotate (or from any listed credential), a **"Download setup script"**
  control offers a **Bash** (Linux/macOS) and a **PowerShell** (Windows) variant.
- Generated **entirely client-side in the SPA** — no server round-trip — so the
  one-time secret is never re-transmitted anywhere to build it.
- The script embeds only **non-secret** parameters: the HTTPS WebDAV URL (tenant host +
  path), a suggested mount point / drive letter, and the `key_id` as the pre-filled
  **username** (a public identifier, §15.2).

**The secret is never written into the script — the user supplies it at run time via
the OS's own prompt/dialog:**
- **macOS:** `open 'https://host/path'` raises Finder's native "Connect to Server"
  dialog (prompts for name/password); or `mount_webdav`, which prompts.
- **Windows (PowerShell):** `net use <drive> \\host@SSL\path /user:<key_id>` prompts for
  the password at the console; or `Get-Credential` → `New-PSDrive` (native credential
  dialog). Nothing persisted to disk.
- **Linux:** `sudo mount -t davfs https://host/path <mountpoint>` prompts interactively
  for the credentials; or a `gio mount` one-liner. The script deliberately **does not**
  write `/etc/davfs2/secrets`.

**Security.**
- Secret never in the file, never to disk, never over the wire to build the script;
  only the public URL + `key_id` are embedded.
- **HTTPS only.** If the deployment uses a private CA, include a *commented* trust note;
  never default to `-o insecure` / `-SkipCertificateCheck`.
- **Persistent auto-mount across reboot is out of scope** — that needs a stored secret;
  the helper is for interactive setup, and the copy should point users at their OS
  keychain if they want persistence (their choice, not baked in).

**Placement.** Part of the Phase 5 ProfileView panel (§15.12); additive and
independently shippable. No blocking open decision.

---

## 14. Amendment (2026-07-15): origin-aware gate + session-liveness

**Status:** Draft / for review — amends **§5** (Feature B). 2FA (§4) and V2 (§13)
are untouched.

**Why this amendment.** §5 as written binds WebDAV to an **IP** for a fixed TTL
(`SET webdav:ipbind:{tenant}:{uid}:{ip} EX 12h`, §5.1). That is *IP-pinning*: the
key survives an explicit Web-UI **logout** and only lapses when the TTL expires — so
a user who signs out of the browser still has up to 12 h of WebDAV access from that
IP. The requirement this amendment captures is stronger and simpler to reason about:

> WebDAV access from the Internet is allowed **only while the user has a live
> Web-UI session**, and is cut the instant they log out. Access from inside the
> **LAN trust boundary** is allowed independently (and keeps working when the cloud
> link is down).

This is the same two-control structure as §5, re-expressed as **one origin-aware
predicate with two branches**, plus a semantic upgrade of the external branch from
*TTL-expiry* to *logout-revoked liveness*.

### 14.1 The unified policy

Evaluated per request in `authenticateUser` (the single choke point for all 8
handlers, `webdav_server.cpp`), **after** the LDAP bind succeeds:

```
allow WebDAV iff  authenticated (Basic → LDAP)  AND
  ┌─ authoritative_ip ∈ WEBDAV_IP_BIND_TRUSTED_CIDRS      → ALLOW   (LAN branch)
  │     • static config, NO Redis, evaluated FIRST
  │     • audited via:"trusted_cidr"  (§5.6)
  │     • survives cloud/Redis outage; read-only when degraded (§5.7)
  └─ else (Internet branch)                                → ALLOW only if
        a LIVE Web-UI session exists for this user          (§14.2)
        • cloud-authoritative; created on login, destroyed on logout
        • Redis-unavailable → fail-closed by default (WEBDAV_IP_BINDING_FAIL_OPEN)
     else → 403
```

The **LAN branch is the §5.6 trusted-CIDR exemption, unchanged** — it is still
evaluated first, from static config, needs no Redis, and is therefore still the
degraded-mode / outage-survival path (§5.7). The amendment only changes what the
**Internet branch** checks.

### 14.2 External branch: session-*liveness*, not IP-*pinning*

Replace the flat, TTL-expiring per-IP key (§5.1) with a **session-presence
registry** that is written on login and **deleted on logout**. The JWT already
carries the two identifiers we need — `sub` (uid) and a unique `jti` per session
(`http_bridge/src/http_server.cpp:1346`), plus `tenant`.

**Redis structure** — one sorted set per user, members = live sessions:

```
key    webdav:session:{tenant}:{uid}
member {jti}|{ip}            # ip = the trusted client IP at login (§3)
score  {session_exp_epoch}   # = the JWT's exp
```

- **Login / refresh** (`issueToken` post-2FA, `oauthCallback`, `refreshToken`):
  `ZADD webdav:session:{tenant}:{uid} {exp} {jti}|{ip}`. Refresh re-scores the
  member (or adds the new `jti` and `ZREM`s the old), keeping the session live for
  as long as the browser is active — same "refresh keeps it alive" property as §5.1.
- **Logout** (`revokeToken`, `http_bridge/src/http_server.cpp:1769` — today it only
  emits an audit event and returns 204): additionally
  `ZREM webdav:session:{tenant}:{uid} {jti}|{ip}`. **This is the new behavior** that
  makes logout actually cut WebDAV. It already decodes+verifies the bearer and has
  `jti`/`sub`/`tenant` in hand, so the data is present.
- **Enforcement** (webdav_bridge, per request): derive the trusted IP (§3), then
  ```
  ZREMRANGEBYSCORE webdav:session:{tenant}:{uid} 0 {now}   # purge expired sessions
  ```
  then, depending on granularity mode (§14.3), test for a live member. The
  `ZREMRANGEBYSCORE` step makes the **TTL a crash/backstop only** — the primary
  lifetime is now "until logout," not "until 12 h elapses." (Sessions whose browser
  was closed without an explicit logout still age out at `exp`, so nothing leaks
  indefinitely if `revokeToken` is never called.)

**Why a sorted set instead of the §5.1 flat keys.** Logout must revoke **one
session** without collateral damage: a user with two browser sessions from the same
IP who logs out of one must keep WebDAV while the other is live. Per-IP flat keys
can't express that (deleting the IP key kills both); per-session members with an
independent expiry score can. It also gives multi-device support and per-session
expiry for free — the same goals §5.1 listed, now correct under revocation.

### 14.3 Granularity: `session` vs `session_ip` (new open decision)

The Internet branch can require, for the authenticated uid:

- **`session`** — *any* live Web-UI session exists (`ZCARD > 0` after the purge).
  Friendlier to roaming/mobile and to IPv4/IPv6 dual-stack (sidesteps the §5.5
  mismatch), because the WebDAV client's IP need not match the browser's.
- **`session_ip`** — a live session exists **whose member IP equals the request
  IP** (`session` **and** the §5-style IP pin). Strictest: a leaked password from a
  new IP is refused even during an active session — this is §5's original threat
  model **plus** logout-revocation.

Recommendation: **`session_ip`** for parity with §5's threat model, with `session`
available for deployments where dual-stack / roaming friction outweighs the
same-IP assurance. Selected via `WEBDAV_EXTERNAL_GATE` (§14.5).

### 14.4 Hybrid / disconnected operation is unchanged — and this is why the gate must NOT replicate to the edge

The origin split makes the disconnected story fall out exactly as §5.7 already
specifies, with one clarification worth stating explicitly (it was the question
that motivated this amendment):

- **The LAN branch already carries disconnected operation.** It is local
  (static CIDR + authoritative peer IP), evaluated before any Redis call, so a
  severed cloud link does not affect it. LAN users keep read access, downgraded to
  read-only per §5.7. **Unchanged.**
- **The session-liveness (Internet) branch is inherently cloud-authoritative and
  must stay cloud-only.** Unlike LDAP (`FILEENGINE_LDAP_ENDPOINT_REPLICA`) and
  Postgres (`FILEENGINE_PG_REPLICA_*`), **Redis has no edge-replica story in this
  platform, and this gate should not create one.** A logout is a cloud-side event;
  a disconnected edge cannot learn of it, so a stale edge replica of
  `webdav:session:*` would **fail toward allow** — silently defeating the exact
  revocation this amendment adds. Async replication would buy availability the LAN
  branch already provides, at the cost of the gate's correctness. **Decision:** do
  **not** replicate the session registry to the edge; a partitioned site serves LAN
  users via the trusted-CIDR branch (read-only), and the Internet branch is simply
  unavailable while the site is cut off — which is the correct posture, since
  "Internet access without a verifiable live session" is precisely what we mean to
  deny.

### 14.5 Config delta (amends §7)

| Var | Where | Default | Note |
|---|---|---|---|
| `WEBDAV_EXTERNAL_GATE` | webdav_bridge (+http_bridge writes) | `ip_ttl` | `ip_ttl` = legacy §5 IP-pin; `session` = liveness, any IP (§14.3); `session_ip` = liveness + IP pin (**recommended**) |

- **Backward compatibility:** default `ip_ttl` preserves §5 exactly, so this
  amendment is inert until a deployment opts into `session`/`session_ip`. All other
  §7 vars are unchanged; `WEBDAV_IP_BIND_TTL_SECONDS` becomes the **backstop** TTL
  (crash / no-explicit-logout) under the session modes rather than the primary
  lifetime.
- **No new secret, no new service.** Reuses the webdav_bridge hiredis addition
  already required by §5 and the shared Redis. http_bridge already links hiredis.

### 14.6 Sequences (amend §6)

**Logout now cuts WebDAV**
```
Browser → http_bridge  POST /v1/auth/logout (Bearer)
  revokeToken: verify JWT → audit logout (fail-closed as today)
             → Redis ZREM webdav:session:{tenant}:{uid}  {jti}|{ip}   # NEW
  → 204
Next WebDAV request from that IP with the still-valid password:
  webdav_bridge: not in a trusted CIDR → Internet branch
    → ZREMRANGEBYSCORE purge; no live member for uid[/ip] → 403
```

**Internet WebDAV while logged in (session_ip mode)**
```
davfs → nginx (X-Real-IP=X) → webdav_bridge  PROPFIND /...
  authenticateUser: LDAP bind ok (empty-pw guard)
    → derive trusted IP = X; X ∉ WEBDAV_IP_BIND_TRUSTED_CIDRS → Internet branch
    → ZREMRANGEBYSCORE webdav:session:{t}:{uid} 0 now
    → any live member with ip==X?  yes → allow   (attacker from Y → none → 403)
```

**LAN WebDAV (any cloud state, incl. disconnected)** — unchanged from §5.6/§5.7:
```
davfs → internal ingress (authoritative src ∈ trusted CIDR) → webdav_bridge
  authenticateUser: LDAP bind ok (local replica if master down)
    → LAN branch: allow, audit via:"trusted_cidr"; read-only if degraded
    → no Redis consulted
```

### 14.7 Open decisions this amendment adds (extends §11)

- **§11.11 — External-branch semantics:** `session_ip` (recommended) vs `session`
  vs keep `ip_ttl` (legacy §5). Governs `WEBDAV_EXTERNAL_GATE` default.
- **§11.12 — Logout revocation is immediate:** confirm `revokeToken` should `ZREM`
  the session member (recommended — it is the point of the amendment). Note the
  minor coupling: logout now performs a best-effort Redis write; keep it
  **best-effort / non-blocking** (a failed `ZREM` must not fail the logout — the
  member still ages out at `exp`).
- **§11.13 (revises §11.9) — TTL role:** under session modes,
  `WEBDAV_IP_BIND_TTL_SECONDS` is the crash/no-logout **backstop**, not the working
  lifetime; confirm the value (12 h is fine as a backstop).

### 14.8 Testing delta (amends §10)

- **Unit (webdav):** sorted-set presence check — expired members purged; `session`
  vs `session_ip` matching; multi-session same-IP (logout of one keeps the other).
- **E2E:** login → WebDAV allowed → **logout → same-IP WebDAV now 403** (the core
  new assertion); second concurrent session survives the first's logout; browser
  closed without logout → access lapses at backstop TTL; LAN branch unaffected by
  Redis-down and by logout.

### 14.9 Implementation touch-points (amends §12, Phase 3)

- **http_bridge:** on login/refresh, `ZADD` the session member (replaces/augments
  the §5.1 `SET`); in `revokeToken` (`http_server.cpp:1769`), add the best-effort
  `ZREM`. Carry `jti`/`ip` through (already available).
- **webdav_bridge:** the §5.2 enforcement becomes the §14.2 purge-then-presence
  check under `session`/`session_ip`; the LAN branch and `/readyz` health are
  unchanged.
- Still behind `WEBDAV_IP_BINDING_ENABLED` and default-off via
  `WEBDAV_EXTERNAL_GATE=ip_ttl`; independently shippable.
