# Proposal: Two-Factor Authentication + WebDAV IP-Binding

**Status:** Draft / research — for review
**Branch:** `research/2fa-webdav-ip-binding` (file_engine_core)
**Author:** security hardening follow-up (2026-07)
**Scope (cross-repo):** `http_bridge`, `ldap_manager`, `webdav_bridge`, `frontend`, `docker_unified` (nginx), `audit_service` (event types)

> This is a design proposal, not an implementation. It grounds every decision in
> the current code and calls out the decisions that need your sign-off (see
> **§11 Open decisions**).

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
   4b. **Same-LAN / internal exemption (§5.6):** enable the per-tenant
   `WEBDAV_IP_BIND_TRUSTED_CIDRS` (skip the binding for internal/VPN ranges) — and
   accept the insider/lateral-movement trade-off it implies? If yes, confirm the
   authoritative-IP source per the deployment topology, and keep it allow-but-audit.
9. **Degraded-mode LAN reads (§5.7) — read-only is confirmed spec.** Remaining:
   confirm the on-prem replica topology (LDAP read replica + local core/cache) is
   deployed at hybrid sites, and the degradation signal that flips read-only mode.
   Offline read-**write** with reconciliation is explicitly **out of scope for v1**
   — the no-corruption / conflict-reconciliation guarantees make it a much larger,
   separate effort.
5. **OAuth + 2FA:** trust IdP MFA (recommended) vs always require our TOTP.
6. **Email fallback:** now specified as configurable — disable-able per tenant with
   a deployment-wide cap (§4.8). Remaining call: the **default** (email allowed by
   default vs off), and whether high-security *deployments* ship with it off.
7. **Binding TTL** (default 12h) and whether `refreshToken` should extend it.
8. **TOTP library** for ldap_manager (`pyotp`) and QR rendering (server-side SVG).

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
