# Outside share links — verified-recipient download & drop links

> **Terminology.** The *door* is unauthenticated — no account, no session, no
> bearer token. The *person* is not anonymous: every redemption is gated on a
> one-time code sent to an address the creator named at creation (§6.9). Where
> this document says "unauthenticated" it means the route; it never means
> "unidentified".

**Status:** Design proposal — for review
**Scope (cross-repo):** `file_engine_core` (owner of the record + enforcement),
`http_bridge` (public + owner-side REST routes, orchestration), `ldap_manager`
(recipient OTP: delivery, verification, rate limits), `frontend` (drawer tab +
public landing view + help), `audit_service` (new action codes),
`docker_unified` (rate-limit zone). No change to `webdav_bridge`.

> Expanded from the original one-paragraph sketch (kept verbatim as §1). Every
> decision is grounded in current code; what has been settled and what is still
> open are collected in **§13**.

---

## 1. The original sketch

> Support to generate a URL to a file for download, with a limited number of
> downloads, and a specific timeframe. For folders an upload link that allows a
> max number of files to drop, and also a validity period. This allows
> unauthenticated external users to receive the 'magic' link to download/upload
> with no additional visibility or access.
>
> These features will be integrated as new sharing tabs on the file/folder drawer.

Everything below is the mechanism for that, plus the parts the sketch leaves
implicit: where the state lives, what a link is allowed to do when the creator's
own access has changed since, and how an unauthenticated door onto the tenant
origin avoids becoming the weakest thing in the deployment.

---

## 2. Goals & non-goals

**Goals**

1. **Download link** on a *file*: an opaque URL an outside recipient — someone
   with no account here — can fetch the bytes from, bounded by *N* uses **and** a
   validity window.
2. **Folder-download link**: the same, for a *folder*, served as a single
   streamed zip of a **snapshot** of its members taken at creation (§6.5).
3. **Upload link** ("drop box") on a *folder*: an opaque URL an outside sender
   can drop files into, bounded by a file count, a byte budget, and a validity
   window.
4. **No additional visibility.** A redeemed link exposes exactly the resource
   set it was minted for — never a sibling, never a parent, never the tenant's
   user directory, never the fact that other links exist. (A folder-download
   link's *members* are exposed by construction: they are the payload.)
5. **Delegation, not escalation.** A link can never convey more than its creator
   held at redemption time (§6.3) — this is the property that makes the feature
   safe to hand to ordinary users rather than admins only.
6. **Verified recipients.** Using a link requires proving control of an email
   address the creator listed at creation — enter the address, receive a
   one-time code, then proceed (§6.9). The URL alone is inert.
7. **Fully accountable.** Creation, revocation, every redemption, and every
   denied attempt are audited; the roadmap already names *share link* as a
   first-class audit object and "every object a departed employee could still
   reach via lingering share links" as a required reverse query
   (`FILEENGINE_ROADMAP.md` §1.5).

**Non-goals (v1)**

- Not a replacement for real sharing. Sharing *with a FileEngine user* stays the
  ACL path (`frontend/src/help/content/sharing.md`); this is for people who will
  never have an account.
- No "sign in to view" upgrade path, and no account is ever created for a
  recipient. Recipients *are* identified — by a verified email address, not by a
  directory entry (§6.9).
- Not a way to publish. Every link is addressed to a closed set of people named
  at creation; there is no "anyone with the link" mode (§6.9, §13-R4).
- No public *browsing*. A folder-download link yields one archive, not a
  navigable listing; an upload link never reveals what is already in the folder
  (§6.6).
- Not exposed to the MCP door (§11) — an LLM agent minting outside share links
  is not a default anyone should get by accident.

**Threat model.** The link URL is assumed to leak: forwarded email, a chat
scrollback, a proxy log, a screenshot. Every control is designed around *the
token is public knowledge eventually* — hence the use cap, the window, the
pinned version, the single-resource scope, immediate revocation, and above all
the recipient OTP (§6.9), which makes a leaked URL inert in the hands of anyone
the creator did not name. The adversary is also assumed to try enumerating link
ids, guessing codes, and probing the recipient list from many IPs (§8.4).

---

## 3. Decisions (locked unless §13 reopens them)

| Topic | Decision |
|---|---|
| Where the record lives | **The core**, in a per-tenant `share_links` table. Not the bridge, not a stateless JWT — see §4. |
| Token shape | `{link_uid}.{secret}` — 128-bit uid + 256-bit CSPRNG secret, base64url. The DB stores **only** `sha256(secret)`; the plaintext is shown once at creation. |
| Authority | The link carries the **creator's** authority, re-evaluated on **every** redemption, with their roles resolved **live from LDAP by the bridge** and admin roles stripped. Creator loses READ/WRITE → the link is dead (§6.3). |
| Creation gate | New ACL bit **`SHARE_EXTERNAL = 0x4000`**, never granted by default — mirrors `CULL_VERSIONS`. Plus READ (download links) or WRITE (upload links) on the target. |
| Use accounting | A **redemption session** consumes one use, not an HTTP request — so Range requests, resumes, and retries do not burn the budget (§6.4). |
| Version served | A file download link is **pinned** to the version current at creation. `follow_latest` is an explicit opt-in (§6.2). |
| Folder downloads | **In v1.** A folder link serves a **member snapshot** taken at creation, as a **store-only zip64** streamed by the bridge with a precomputed `Content-Length` (§6.5). Every member's authority is re-checked at redemption. |
| Passphrase | **None.** The OTP is the second factor; a passphrase would be a third secret to distribute for no additional property (§13-R1). |
| Upload versioning | An upload link **never** creates a new version of an existing file. Name collisions get a de-duplicating suffix (§6.7). |
| Upload ownership | Dropped files are **owned by the link creator**; the outside origin — including the verified sender address — is recorded in metadata and audit (§6.8). |
| Recipient verification | **Mandatory.** Email + one-time code before any session opens, for downloads and drops alike. Recipients are an **allowlist fixed at creation**; there is no open-email mode (§6.9). |
| Failure disclosure | Unknown / expired / revoked / exhausted / unlisted-address / wrong-code all return the **same** generic response to the outside caller. The real reason goes to audit only (§8.5). |
| Bridge routes | New `/v1/public/shares/*` prefix (unauthenticated, explicitly allowlisted) + owner-side `/v1/nodes/{uid}/shares` and `/v1/shares/*` (§7). |
| Frontend | A **Share** tab in `FileDetailsDrawer.vue`, plus a `requiresAuth: false` route `/s/:token` for the recipient (§10). |

---

## 4. Why the core owns this (and not the bridge)

The tempting design is the one already in the tree for SSO hand-off
(`http_bridge/include/sso_handoff.h`): a stateless signed JWT, no storage, a
replay guard for single use. That is right for hand-off and **wrong** here, for
five reasons:

1. **Read-by-default makes a synthetic principal catastrophic.**
   `AclManager` ships with `default_read_ = true`
   (`core/include/fileengine/acl_manager.h:277`): a principal with no matching
   rule can read every resource whose parent chain is also readable. A
   bridge-minted `AuthenticationContext{user: "share:abc"}` would therefore be a
   *tenant-wide reader*, and the only thing keeping it scoped to one file would
   be the bridge remembering to pass the right uid. Scope has to be enforced
   where the resource is resolved — in the core, which is the sole ACL enforcer
   by design (`CLAUDE.md`, trust model).
2. **A use counter is durable, atomic state.** "5 downloads" across bridge
   replicas is a single-statement `UPDATE ... WHERE uses_consumed < max_uses`
   in Postgres (§5.3). A JWT cannot count, and the existing `ReplayGuard` is
   explicitly per-process ("a multi-bridge deployment would back this with a
   shared store").
3. **Revocation must be instant.** Revoking a stateless token needs a denylist —
   i.e. durable state anyway, but the worse half of it.
4. **The authority re-check needs the ACL evaluator.** §6.3 requires
   re-evaluating the *creator's* permission on the *current* resource at
   redemption. Only the core can do that. The bridge's one contribution is the
   creator's live LDAP role list (§6.3) — identity resolution, which is exactly
   the job it does on every other request, and no part of the decision.
5. **Audit and forensics need a queryable object.** "Which links can still reach
   this file", "what did this departed user leave open", "who downloaded it and
   from where" are table queries, not token introspection.

The bridge stays what it is everywhere else: a protocol shim with no enforcement
logic. It parses the URL, splits `{uid}.{secret}`, and hands both to the core.

---

## 5. Data model (core, per-tenant schema)

Created alongside `files` / `versions` / `acls` in
`Database::create_tenant_schema` (`core/src/database.cpp:2033`), with the same
`CREATE TABLE IF NOT EXISTS` + idempotent-migration idiom used there.

### 5.1 `share_links`

```sql
CREATE TABLE IF NOT EXISTS "<tenant>".share_links (
    link_uid        UUID PRIMARY KEY,
    kind            SMALLINT     NOT NULL,   -- 0 = file download, 1 = upload,
                                             -- 2 = folder download (zip)
    resource_uid    UUID         NOT NULL,   -- file (0) or directory (1, 2)
    secret_hash     BYTEA        NOT NULL,   -- sha256(secret); never the secret
    created_by      TEXT         NOT NULL,   -- the delegating principal
    created_at      TIMESTAMPTZ  NOT NULL DEFAULT now(),
    expires_at      TIMESTAMPTZ  NOT NULL,   -- hard requirement; no perpetual links
    revoked_at      TIMESTAMPTZ,
    revoked_by      TEXT,
    -- budgets (0 = unlimited, subject to the deployment cap in §9)
    max_uses        INTEGER      NOT NULL DEFAULT 0,
    uses_consumed   INTEGER      NOT NULL DEFAULT 0,
    max_uses_per_recipient INTEGER NOT NULL DEFAULT 0,  -- 0 = only the shared pool
    max_bytes       BIGINT       NOT NULL DEFAULT 0,
    bytes_consumed  BIGINT       NOT NULL DEFAULT 0,
    max_file_bytes  BIGINT       NOT NULL DEFAULT 0,   -- upload: per-file cap
    -- file-download options
    pinned_version  TEXT,                    -- version name; NULL = follow latest
    -- folder-download options
    follow_folder   BOOLEAN      NOT NULL DEFAULT false,  -- true = live folder, no snapshot
    include_subdirs BOOLEAN      NOT NULL DEFAULT true,
    archive_bytes   BIGINT,                  -- precomputed zip size at creation (§6.5)
    -- upload options
    landing_prefix  TEXT,                    -- optional subfolder name, created lazily
    ext_allowlist   TEXT[],                  -- NULL = any
    -- abuse
    failed_attempts INTEGER      NOT NULL DEFAULT 0,
    locked_until    TIMESTAMPTZ,
    note            TEXT                     -- creator's own label, shown in the UI
);
CREATE INDEX IF NOT EXISTS share_links_resource ON "<tenant>".share_links (resource_uid);
CREATE INDEX IF NOT EXISTS share_links_creator  ON "<tenant>".share_links (created_by);
CREATE INDEX IF NOT EXISTS share_links_live     ON "<tenant>".share_links (expires_at)
    WHERE revoked_at IS NULL;
```

### 5.2 `share_link_members` (folder-download snapshot)

Written once at creation for `kind = 2` when `follow_folder = false` (the
default). It is the authoritative member set: a redemption serves *this* list,
never a fresh directory walk, so files added to the folder after the link was
minted are never included.

```sql
CREATE TABLE IF NOT EXISTS "<tenant>".share_link_members (
    link_uid     UUID   NOT NULL REFERENCES "<tenant>".share_links(link_uid) ON DELETE CASCADE,
    member_uid   UUID   NOT NULL,
    archive_path TEXT   NOT NULL,   -- path inside the zip, relative to the folder
    version_name TEXT   NOT NULL,   -- pinned, same reasoning as §6.2
    size_bytes   BIGINT NOT NULL,
    PRIMARY KEY (link_uid, member_uid)
);
```

`archive_path` is normalized and validated at creation — no `..`, no absolute
paths, no leading `/` — so a hostile file name cannot produce a zip-slip archive
on the recipient's machine.

### 5.3 `share_redemptions`

One row per consumed use — simultaneously the counter's ledger and the forensic
record the roadmap's reverse queries read.

```sql
CREATE TABLE IF NOT EXISTS "<tenant>".share_redemptions (
    redemption_uid UUID PRIMARY KEY,
    link_uid       UUID        NOT NULL REFERENCES "<tenant>".share_links(link_uid),
    opened_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at     TIMESTAMPTZ NOT NULL,   -- session TTL (§6.4), default 1 h
    verified_email TEXT        NOT NULL,   -- the recipient who passed the OTP (§6.9);
                                           -- NOT NULL is the schema-level statement
                                           -- that no session opens unverified
    source_addr    TEXT,
    user_agent     TEXT,
    bytes_moved    BIGINT      NOT NULL DEFAULT 0,
    result_uid     UUID,                   -- upload: the file the drop created
    -- folder download: the member set frozen at session open, and the exact
    -- archive length derived from it (§7.3). Members the creator could no
    -- longer read are already excluded here.
    frozen_members UUID[],
    archive_bytes  BIGINT,
    completed_at   TIMESTAMPTZ
);
```

**The atomic consume** — one statement, no races, no serializable retries:

```sql
UPDATE "<tenant>".share_links
   SET uses_consumed = uses_consumed + 1
 WHERE link_uid = $1
   AND revoked_at IS NULL
   AND expires_at > now()
   AND (locked_until IS NULL OR locked_until < now())
   AND (max_uses = 0 OR uses_consumed < max_uses)
RETURNING kind, resource_uid, created_by, pinned_version, max_bytes, bytes_consumed, ...;
```

Zero rows returned ⇒ generic 404 (§8.5). The secret and the recipient's OTP are
verified *before* this statement, so nothing an unverified caller does can burn a
use.

### 5.4 `share_link_recipients`

The closed destination set (§6.9). Written at creation and editable afterwards
**only by an authenticated user with rights on the link** (§10.2) — an outside
caller can never add an address, and a redemption never mutates the list.
Removal is a soft delete (`removed_at`) so a partial revoke keeps its history.

```sql
CREATE TABLE IF NOT EXISTS "<tenant>".share_link_recipients (
    link_uid          UUID        NOT NULL REFERENCES "<tenant>".share_links(link_uid) ON DELETE CASCADE,
    email             TEXT        NOT NULL,   -- normalized (lowercased, trimmed)
    invited_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    invited_by        TEXT        NOT NULL,   -- creator, or whoever added them later
    -- the status ladder the Share tab renders (§10.2) — every rung is a column so
    -- the roster is one query, not a reconstruction from the audit log
    invite_sent_at    TIMESTAMPTZ,            -- the system mailed the link (§6.9)
    invite_error      TEXT,                   -- SMTP rejection / bounce, surfaced as "invite failed"
    last_code_sent_at TIMESTAMPTZ,            -- "Opened" — they reached the landing page
    first_verified_at TIMESTAMPTZ,            -- "Verified"
    last_used_at      TIMESTAMPTZ,            -- "Downloaded" / "Dropped"
    uses_consumed     INTEGER     NOT NULL DEFAULT 0,
    failed_codes      INTEGER     NOT NULL DEFAULT 0,
    removed_at        TIMESTAMPTZ,            -- partial revoke; row kept for history
    removed_by        TEXT,
    PRIMARY KEY (link_uid, email)
);
```

Stored in plaintext because it is a delivery address — hashing it would make the
mail undeliverable. It is therefore **PII in a tenant schema**, and the §5.5
retention window applies to it as much as to the ledger.

`max_uses` remains a pool shared across recipients; the optional
`max_uses_per_recipient` on the link bounds any single recipient
(`uses_consumed` here is what it is checked against), which is how "each of the
three of them may download twice" is expressed.

### 5.5 Retention

Expired and revoked rows are **kept** (they are audit evidence) and purged by the
existing background-worker pattern (`FileCuller`) after
`share.retention_days` (default 365), emitting `share_link_expired` on the way
out. Nothing about correctness depends on the sweeper — every check is
evaluated at redemption time.

---

## 6. Behaviour

### 6.1 Creating a link

`CreateShareLink` requires, on the target resource:

- `SHARE_EXTERNAL` (§8.1) **and**
- `READ` for `kind = 0 | 2` (download), `WRITE` for `kind = 1` (upload), **and**
- the target's type matches the kind — file for `0`, directory for `1` and `2`
  (mismatch ⇒ `INVALID_ARGUMENT`), **and**
- at least one recipient address, capped at `share.max_recipients` (§6.9), **and**
- `expires_at` within the deployment's `share.max_ttl_days` cap (§9), **and**
- for `kind = 2`: the snapshot walk succeeds within
  `share.zip_max_members` / `share.zip_max_bytes` (§9). Creation is where an
  oversized folder is refused — with a clear count/size in the error — so the
  recipient never discovers the limit mid-download.

The response carries the **only** copy of the secret the system will ever emit.
Re-reading a link later returns its uid, budgets, and usage — never the secret.

### 6.2 Version pinning (file download)

By default `pinned_version` is set to the version current at creation. The
reason is a leak the obvious design has: an unpinned link keeps serving *future*
edits of the file, so a document shared for review in March silently exposes
whatever it becomes in September. `follow_latest: true` is available and is a
deliberate choice the UI labels as such ("always send the newest version").

If the pinned version is later culled (`CULL_VERSIONS`) the link is dead —
generic 404, audited as `share_link_denied / version_gone`.

### 6.3 The authority re-check (the defining rule)

On every redemption the core re-evaluates, as `created_by`:

- `check_permission(created_by, resource_uid, READ|WRITE)` — the *current* ACL
  state, including parent traversal, and
- that the resource still exists and is not deleted.

Any failure ⇒ the link is dead. Consequences, all of them intended:

- Revoking a user's access to a folder retroactively kills every link they minted
  under it. No separate cleanup step, no lingering grants.
- A departed employee's links die the moment their access does — this is what
  makes the roadmap's "lingering share links" query actionable rather than
  merely informative.
- A link can never outrank its creator. That is what allows `SHARE_EXTERNAL` to
  be an ordinary user permission instead of an admin-only one.

#### Resolving the creator's roles when nobody is authenticated

This is the subtle part, and the naive reading of the code gets it wrong.
`AclManager::resolve_effective_roles` (`core/src/acl_manager.cpp:242`) unions the
*request-supplied* roles with `db_->get_roles_for_user`. A share redemption
carries no session, so it supplies none — and the obvious implementation
therefore evaluates the creator as
**role-less**.

That is not a corner case here. Role membership in this platform lives in
**LDAP**, not in the core: `ldap_manager` administers `groupOfNames` entries
(the SPA's role admin calls `/v1/admin/roles/{role}/members`,
`frontend/src/services/ldapAdminService.ts`), and the bridge attaches the
resolved groups to every request in `fillAuth` / `addRolesAliased`
(`http_bridge/src/http_server.cpp:479–493`). The core's `user_roles` table is
written only by its own `AssignUserToRole` RPC, which is not on that path — so
in a normal deployment `get_roles_for_user` returns **empty** and the DB union
contributes nothing.

Read-by-default (`default_read_ = true`) hides this most of the time: a
role-less principal still holds baseline READ. It stops hiding it the moment a
folder carries `DENY READ → everyone` plus `ALLOW READ → some role` — which is
exactly the **"👥 Gated section (role)"** one-click template the product ships
(`frontend/src/help/content/sharing.md`). Links minted inside a gated section
would be born dead. Claims have the same shape: CLAIM-tier rules match on
JWT-borne claims, and a redemption carries no JWT.

**Therefore, two rules.**

1. **The bridge re-resolves the creator's roles live, at redemption.** It already
   owns identity resolution, and `LDAPAuthenticator::getRolesByTenant(username)`
   (`http_bridge/src/ldap_authenticator.cpp:587`) does exactly this for an
   arbitrary username over the service bind — no credentials needed. The bridge
   passes the result into `RedeemShareLink`, which forwards it as the
   `request_roles` argument of the re-check. Roles are therefore **current, not
   snapshotted**: removing a departed employee from the LDAP group kills their
   links on the next redemption, with nothing stored anywhere to go stale. LDAP
   being unreachable denies the redemption (fail-closed, consistent with every
   other door).
2. **Admin roles are stripped before the re-check** — `system_admin`,
   `tenant_admin`, and the `administrators` alias that `addRolesAliased` maps to
   it. The ACL bypass must never reach an unauthenticated route: a link minted by
   an admin has to stand on ordinary ACL rules or not stand at all. An admin who
   can reach a file *only* via the bypass therefore cannot mint a working link to
   it — the safe failure, caught at creation by the pre-flight below.

**Pre-flight at creation.** `CreateShareLink` runs the *exact* check a redemption
will run — creator, LDAP-resolved roles, no claims, no admin bypass — and refuses
to mint a link that would be born dead, naming the reason. This turns every
variant of the problem above (claim-only access, bypass-only access, an LDAP
lookup that resolves differently than the caller's live JWT) into a clear error
at creation rather than a mystery 404 for the recipient.

### 6.4 Redemption sessions (why a use ≠ a request)

Naively decrementing per HTTP request breaks on the first real browser: a Range
request, a resumed download, or a retry after a dropped connection would each
burn a use, and a 5-use link would be exhausted by one recipient.

Instead, `RedeemShareLink` opens a **redemption session** (a `share_redemptions`
row, TTL `share.session_ttl_seconds`, default 1 h) and *that* consumes the use. Every
byte transfer within the session — including `Range` continuations and retries —
rides `redemption_uid` and consumes nothing further. `bytes_moved` accumulates
on the row; `bytes_consumed` on the link is updated at session close.

The SPA landing page therefore calls **peek** (metadata only: file name, size,
expiry, uses remaining), which consumes nothing.
Peek is rate-limited like everything else on the public prefix (§8.4).

Everything before the session is likewise free: the whole recipient-verification
exchange in §6.9 — entering an address, receiving a code, entering it — consumes
nothing. The use is spent at session open, once a verified recipient has actually
asked for the payload.

### 6.5 Folder downloads (zip)

**Snapshot, not a live walk.** At creation the core walks the folder (respecting
`include_subdirs`) as the creator and writes `share_link_members` (§5.2): each
member's uid, its **pinned version**, its size, and its path inside the archive.
Redemption serves that list. This is the folder-scale form of §6.2's argument —
a live link keeps exposing whatever anyone drops into the folder next month,
which for a shared project directory is a much bigger leak than a single file's
future edits. `follow_folder: true` opts into live semantics and the UI labels it
plainly ("include anything added later").

**Store-only zip64, with a known length.** The archive is assembled with **no
compression** (`method 0`), which makes its byte length exactly computable at
creation from the member sizes plus fixed header/central-directory overhead —
so `archive_bytes` is stored on the link and served as a real `Content-Length`.
The recipient gets a progress bar instead of an indefinite chunked stream, and
the deployment gets an exact number to budget against. Zip64 unconditionally, so
>4 GiB archives and >65 535 members are not a special case. Most of what this
system holds (PDF, IFC, images, office documents) is already compressed, so the
forfeited ratio is small; `share.zip_deflate = true` is available for
text-heavy corpora and trades `Content-Length` for compression.

**Per-member authority re-check.** §6.3 runs on the folder *and* on each member
at redemption. A member the creator can no longer read is **omitted**, not
fatal — the archive is still served, and the redemption's audit `detail` carries
`members_served` / `members_omitted`. Failing the whole download because one file
gained a DENY would make the feature unusable in exactly the deployments that
manage ACLs carefully.

**No resume.** A zip stream is not Range-servable, so `/content` returns
`Accept-Ranges: none` for `kind = 2`. This is precisely why uses are counted per
*session* and not per request (§6.4): a recipient whose connection drops retries
inside the same session and burns nothing.

**Path safety.** `archive_path` is normalized and validated at creation (§5.2).
Empty folders are emitted as zip directory entries so the structure survives.

### 6.6 What a redemption exposes

The resource set the link was minted for, and nothing else. Concretely, the core
resolves every operation against `resource_uid` (and, for `kind = 2`, against
`share_link_members`) and ignores any uid the caller supplies. A share credential
grants **no** `ListDirectory`, no `Stat` of a parent, no ACL read, no role or
principal lookup, no metadata read beyond display name / size / content-type,
and no version listing.

- **File download:** one file, one version.
- **Folder download:** the snapshot's members. Peek returns the member count and
  total size; the full member list is available at `/manifest` — hiding it would
  be theatre, since the archive contains it.
- **Upload:** the sender never sees the folder's existing contents — only what
  *they* dropped in the current session.

Renditions (`file_renditions.md`) are **not** served in v1: a download link
serves the parent file only, and a folder snapshot contains no rendition
children.

### 6.7 Upload semantics

- **Never versions.** A drop whose name collides with an existing entry is stored
  as `name (1).ext`, `name (2).ext`, … It must not be possible for an outside
  sender to inject a new version into an existing document's history — that is
  both a data-integrity problem and a plausible attack (poison the "latest" of a
  file someone else trusts).
- **Budgets** are enforced twice: streaming in the bridge (fail fast, don't buffer
  a 40 GB body) and authoritatively in the core (`max_file_bytes`,
  `max_bytes` remaining, `max_uses` as the file count).
- **Extension allowlist** is optional and matched on the claimed name only —
  it is a convenience for the sender, never a security control. Content type is
  never trusted; nothing is executed.
- **Landing prefix**: an optional subfolder (created lazily on first drop, owned
  by the creator) for deployments that want outside drops quarantined from the
  folder proper. Default is none — drops land in the target folder and flow
  through the normal `file.created` event, so `folder_actions` and CSAI ingest
  them like any other upload.

### 6.8 Drop provenance

Dropped files are **owned by `created_by`** — so inherited ACLs behave exactly as
if the creator had uploaded them, and no orphan principal appears in the ACL
tables. The outside origin is recorded in full, in file metadata:

| Key | Value |
|---|---|
| `share.link_uid` | the link |
| `share.redemption_uid` | the specific drop |
| `share.source_addr` | client IP as forwarded by the bridge |
| `share.verified_email` | the recipient address that passed the OTP challenge (§6.9) — **verified**, not claimed |
| `share.claimed_name` | free text the sender optionally typed; **untrusted**, and the UI must render it as such |

Audit events for the drop use `actor = "share:<link_uid>|<verified_email>"`,
`source_iface = "rest"`, with `created_by` carried in `detail` — so the ledger
shows the door, the verified human who walked through it, and the person
accountable for opening it.

### 6.9 Recipient verification (email + OTP) — required

**Every redemption is gated on a verified recipient.** Before any byte moves, the
visitor enters an email address, receives a one-time code at that address, and
enters it. Only then does a session open (and only then is a use consumed). This
applies to **all three kinds** — a folder drop is gated exactly like a download,
which is what turns an anonymous drop into an attributable one (§6.8).

#### The recipient allowlist — and why there is no open-email mode

Recipient addresses are **fixed at creation** (`share_link_recipients`, §5.4).
The OTP is only ever mailed to an address on that list. A visitor who enters
anything else gets the same response as one who entered a listed address —
*"if that address is authorized, we've sent a code"* — so the endpoint never
discloses who the intended recipients are (§8.5's rule, applied here).

The alternative — accept any address the visitor types, mail a code to it, and
record it for the audit trail — was **rejected outright**. It makes an
unauthenticated internet caller the chooser of a destination address for
tenant-branded mail: an open relay with the deployment's own sending reputation
behind it. No rate limit makes that acceptable; the allowlist removes the
capability entirely, because the destination set is closed at creation by an
authenticated user with `SHARE_EXTERNAL`.

Two consequences, both intended:

- **The link is genuinely non-forwardable.** Forwarding the URL conveys nothing:
  the forwardee cannot receive a code. This is the property the plain
  use-cap/expiry design could never provide.
- **The creator must know the address up front.** "Post the link in a channel and
  let whoever needs it grab a copy" is no longer a supported shape. That is the
  direct cost of the requirement, and it is the right trade for documents worth
  gating.

#### Flow

1. `GET …/{link_uid}` — **peek**. Consumes nothing, reveals nothing about
   recipients, states that a code will be required.
2. `POST …/{link_uid}/identify` `{email}` — if the address is on the allowlist,
   mint a 6-digit code and mail it. Uniform response either way.
3. `POST …/{link_uid}/verify` `{email, code}` — on success, issue a
   **recipient token** (256-bit, hashed at rest, TTL
   `share.recipient_ttl_seconds`, bound to link + email). Consumes nothing.
4. `POST …/{link_uid}/session` — requires the recipient token. **This is where
   the use is consumed** (§6.4) and where `verified_email` is written onto the
   redemption row.
5. Transfer, as §6.4/§6.5 describe.

A failed or abandoned challenge therefore never costs a use, and a recipient who
already holds a live recipient token can start a second session (a re-download
inside the window) without a fresh code — subject to the same use budget.

#### Where each piece lives

The split follows the platform's existing 2FA seam, documented in
`ldap_manager/src/ldap_manager/routers/twofa.py`: *"the identity service owns the
secret + verification; http_bridge orchestrates."*

| Piece | Owner | Reuses |
|---|---|---|
| Recipient allowlist, budgets, `verified_email` on the ledger | **core** | durable authorization state, per §4 |
| Code generation, delivery, storage, single-use verify, send/attempt rate limits | **ldap_manager** | `TokenStore.issue_code` / `consume_code` (hashed, TTL, constant-time, single-use) and `rate_ok` — `tokens.py:106–137` |
| Orchestration, recipient-token minting | **http_bridge** | the `require_internal` server-to-server pattern already used for `/internal/2fa/*` (`http_server.cpp:1710–1731`) |

**New endpoints on ldap_manager:** `POST /internal/share/email-challenge` and
`POST /internal/share/email-verify`, guarded by the same shared internal secret.
They are *siblings* of the 2FA pair, not reuses of it: `/internal/2fa/email-challenge`
refuses when a tenant's 2FA policy excludes the `email` method
(`routers/twofa.py:218`) and renders the 2FA template — neither of which should
govern whether an outside recipient can open a share link. New `kind` for the
token store (`share_otp`), keyed by `link_uid|email`; new mail template
`SHARE_OTP_EMAIL` in `templates.py`.

**The core trusts the bridge's assertion** that an address was verified, exactly
as it trusts `AuthenticationContext` everywhere else. The verification itself
happens in `ldap_manager`; the bridge relays it; the core records it and enforces
that the address is on the link's allowlist — so a bridge bug can misattribute a
redemption but cannot admit an unlisted recipient.

#### Sending the link

Because the addresses are known and a mailer is already in hand, creation offers
**"email the link to these recipients for me"** (default on, template
`SHARE_INVITE_EMAIL`). It is a convenience, not a security boundary — the URL is
useless without a code — but it closes the loop that otherwise has the creator
copying a URL into a mail client by hand.

#### New failure modes to plan for

- **Redis down ⇒ no redemptions.** `issue_code` no-ops and `consume_code`
  returns `False` when Redis is off (`tokens.py:106,113`), so the gate fails
  closed. Correct, and a hard new dependency: share links now require Redis and
  SMTP to be healthy, alongside the core and LDAP.
- **SMTP misconfigured ⇒ every link is unusable**, and the current 2FA handler
  swallows send failures into `sent = False` (`routers/twofa.py:231`). The share
  variant must keep the *recipient's* response uniform while surfacing the
  failure loudly — audit event, bridge log, and a notification to the link's
  creator. A silent mail failure here looks identical to a wrong address, which
  is the worst possible support experience.
- **Mail-flooding a listed recipient** by a party who holds the URL: per
  `(link, email)` and per-link send caps via `rate_ok` (§8.4).
- **OTP brute force:** `consume_code` deliberately does *not* delete on a wrong
  code — its docstring puts rate-limiting on the caller. That counter must live
  in **`ldap_manager`'s Redis** (a `rate_ok` bucket keyed `share_otp:{link}:{email}`),
  **not** in bridge process memory: the bridge is horizontally scaled, and a
  per-process counter would give an attacker one fresh allowance per replica —
  the same trap `ReplayGuard`'s own header calls out ("a multi-bridge deployment
  would back this with a shared store"). Five failures burn the challenge (not
  the link); the failures also feed the link's `failed_attempts` (§8.4).

---

## 7. HTTP Bridge routes

Two clearly separated families. The split matters: the bridge's auth gate today
is a prefix decision (`handleV1`, `src/http_server.cpp:301`) with individual
unauthenticated paths allowlisted inline (`/v1/auth/token`,
`/v1/auth/sso/redeem`, lines 334–357). Rather than scatter more exceptions
through that block, **all** unauthenticated surface goes under one greppable
prefix that nginx, CORS, and the rate limiter can target as a unit.

### 7.1 Owner-side (existing bearer auth, unchanged gate)

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/v1/nodes/{uid}/shares` | Create a link. Body: `kind`, **`recipients[]` (required, ≥1)**, `send_invite?`, `expires_at`\|`ttl`, `max_uses`, `max_uses_per_recipient?`, `max_bytes`, `max_file_bytes`, `follow_latest?`, `follow_folder?`, `include_subdirs?`, `landing_prefix?`, `ext_allowlist?`, `note?`. **201** returns the full URL once, plus `archive_bytes` / member count for `kind = 2`. |
| `GET` | `/v1/nodes/{uid}/shares` | Links on this node (requires `MANAGE_ACL` or being the creator). |
| `GET` | `/v1/shares` | The caller's own links. `?all=true` (**`tenant_admin` only**) returns the tenant-wide set backing the admin console (§10.3), with `creator`, `recipient`, `subtree`, and `status` filters and `live=true` by default. |
| `DELETE` | `/v1/shares/{link_uid}` | Revoke. Idempotent. |
| `GET` | `/v1/shares/{link_uid}` | One link's **live status** (§10.2): computed state, budgets, and the result of the §6.1 pre-flight re-run — this is what surfaces "not working: you no longer have access". |
| `GET` | `/v1/shares/{link_uid}/recipients` | The roster: per-address status ladder, invite/verify/download timestamps, uses consumed, failure counts. |
| `POST` | `/v1/shares/{link_uid}/recipients` | Add an address to the allowlist after creation (creator only; optionally mails the invite). |
| `DELETE` | `/v1/shares/{link_uid}/recipients/{email}` | Partial revoke — that address loses access, the link keeps working for the rest. |
| `POST` | `/v1/shares/{link_uid}/recipients/{email}/resend` | Re-send the invite (or a fresh code), subject to the §9 send limits; returns the remaining wait when throttled. |
| `GET` | `/v1/shares/{link_uid}/redemptions` | Usage ledger for one link (who, when, from where, how much; members served/omitted; `result_uid` for drops). |

### 7.2 Public (unauthenticated — new `/v1/public/` prefix)

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/v1/public/shares/{link_uid}` | **Peek.** Consumes nothing. Returns `kind`, display name, `expires_at`, `uses_remaining`, and either size + content-type (`kind = 0`) or member count + `archive_bytes` (`kind = 2`) or the remaining file/byte budget (`kind = 1`). |
| `POST` | `/v1/public/shares/{link_uid}/identify` | `{email}` → mails a 6-digit code **iff** the address is on the link's allowlist. Uniform response either way (§6.9). Consumes nothing. |
| `POST` | `/v1/public/shares/{link_uid}/verify` | `{email, code}` → the **recipient token** (TTL `share.recipient_ttl_seconds`). Consumes nothing; 5 failures burn the challenge. |
| `POST` | `/v1/public/shares/{link_uid}/session` | Open a redemption session (**this is the use-consuming call**). Requires the recipient token. Returns `redemption_uid` + TTL, and writes `verified_email` onto the ledger row. |
| `GET` | `/v1/public/shares/{link_uid}/content` | Stream the payload. `kind = 0`: the file, Range-capable, reusing the existing `streamFileDownload` path (`src/http_server.cpp:562`). `kind = 2`: the zip, `Content-Length: archive_bytes`, `Accept-Ranges: none`, assembled member-by-member (§7.3). Requires an open session. |
| `POST` | `/v1/public/shares/{link_uid}/files` | Drop a file (raw body + `X-File-Name`, streamed through `StreamFileUpload`). Requires an open session. |
| `GET` | `/v1/public/shares/{link_uid}/manifest` | `kind = 2`: the snapshot's member list (name, path, size). `kind = 1`: what *this session* dropped — never the folder's contents. |

The secret travels in the `X-Share-Secret` header (SPA) or `?k=` (direct-link
fallback for `curl`/email clients); §13-Q3 decides whether the plain-URL form
ships at all. Notes that apply to the whole public family:

- **No CORS wildcard.** Same-origin only; the SPA and the public routes are the
  same host under `docker_unified/images/nginx/snippets/tenant.conf`.
- **No bearer token is read, and none is issued.** A logged-in browser hitting a
  public route is still session-less there — the routes never fall back to session
  auth, so the audit record can never misattribute a redemption to a passing
  authenticated user.
- **Response headers, set once for the whole prefix** — not per route:
  `Content-Disposition: attachment`, `X-Content-Type-Options: nosniff`,
  `Cache-Control: no-store`, `X-Robots-Tag: noindex, nofollow`,
  `Content-Security-Policy: sandbox`. This is the anti-XSS boundary; §8.3 says
  why it is a handler-level concern and what has to be tested to keep it true.
- **Logging:** the bridge logs `link_uid` and never the secret; the nginx
  `access_log` for `location /api/v1/public/` must strip the query string
  (`log_format` without `$query_string`) or the fallback `?k=` form defeats
  itself.

### 7.3 Where the zip is assembled

**In the bridge, not the core.** The core stays a byte source: the bridge opens
the session, reads the member list, and for each member issues
`StreamFileDownload` carrying the *share credential* plus that `member_uid` —
the core validates membership against `share_link_members` and runs the §6.5
per-member re-check, so the bridge never decides who may read what. The bridge
frames the stream into zip entries and streams them straight out; nothing is
buffered to disk or to memory beyond one chunk, matching the
`proxy_request_buffering off` / `proxy_buffering off` posture already set for
`/api/` in `docker_unified/images/nginx/snippets/tenant.conf`.

**The per-member re-check runs at session open, not mid-stream.** `RedeemShareLink`
for `kind = 2` re-evaluates §6.3 across the whole snapshot, freezes the surviving
member list onto the `share_redemptions` row, and returns its exact archive
length — which is what `/content` then serves as `Content-Length`. Doing it any
later means discovering an omitted member after the header is on the wire, and a
zip whose declared length no longer matches its body is a corrupt download. The
residual window (an ACL change between session open and stream end) is bounded
by the transfer itself and is accepted; the link's *next* session sees the new
state.

The zip writer itself is ~200 lines of local-header / central-directory framing
— store-only removes the compressor entirely — with CRC-32 computed per member
as its bytes pass through.

---

## 8. Security

### 8.1 The `SHARE_EXTERNAL` permission bit

```c++
enum class Permission {
    SHARE_EXTERNAL = 0x4000, // Mint an outside share link to this resource —
                             // redeemable without a session, by a recipient
                             // verified only by a one-time code to an address
                             // named at creation. Never granted by default: the
                             // one permission that reaches outside the tenant's
                             // identity boundary. Cf. CULL_VERSIONS.
    CULL_VERSIONS  = 0x2000,
    ...
};
```

It must be added to `kAllPermissions` (`acl_manager.h:64`) or the `system_admin`
bypass invariant `(kAllPermissions & required) == required` breaks for it.
Downstream: the proto permission enum, the REST permission-name map, and
`AclEditor.vue`'s checkbox list (labelled **"Create outside share links"**, with
the same "this reaches outside" treatment `CULL_VERSIONS` gets).

Piggy-backing on `READ` was rejected: read-by-default means nearly every user
holds `READ` nearly everywhere, so link creation would be effectively ungated.
Piggy-backing on `MANAGE_ACL` was rejected as too coarse in the other direction —
"can share a file with an outside client" and "can rewrite this folder's
permissions" are not the same job.

### 8.2 Token strength & storage

- Secret: 32 bytes from the CSPRNG, base64url (43 chars) ⇒ 256 bits. The
  practical enumeration bound is the rate limiter, not the entropy, but the
  entropy makes offline reasoning trivial.
- Stored as `sha256(secret)`; compared in constant time. A dump of
  `share_links` yields **no live links**.
- `link_uid` is public (it is in the URL and in logs); the secret is the only
  bearer credential.
- **No passphrase.** The second factor is the OTP to a listed address (§6.9),
  which is stronger than a shared passphrase (it cannot be forwarded with the
  link, it expires, and it identifies who used it) and asks the creator to
  distribute one secret fewer. See §13-R1.

### 8.3 Serving share bytes from the tenant origin

This is the sharpest new risk and it is not about share links at all — it is
about *any* unauthenticated GET that returns user-controlled bytes on the SPA's
origin. A shared `.html` (or `.svg`, or anything sniffable) fetched at
`https://acme.example.com/api/v1/public/...` runs script in the SPA's origin and
can read `localStorage` — where the frontend keeps its bearer token
(`frontend/src/stores/auth.ts`).

**Decision: the header triple, not an origin split.** `Content-Disposition:
attachment` + `X-Content-Type-Options: nosniff` + `Content-Security-Policy:
sandbox` (§7.2) closes this for every current browser, with no new vhost, TLS
name, or cross-origin dance between the landing page and the bytes. The cost is
that the property is *maintained* rather than *structural*: a future public
route that forgets one header reopens it. Two guards follow from that, and they
are not optional:

1. The headers are set **once**, in the public-prefix handler, before dispatch —
   never per route. A new public route inherits them or does not ship.
2. A test asserts all three on every response from `/v1/public/shares/*`,
   including error responses.

A separate `dl.<base>` origin remains the structural answer if the deployment
ever serves untrusted HTML/SVG at volume; **URLs are built from
`SHARE_PUBLIC_BASE_URL`** (§9) precisely so that move does not invalidate links
already in the wild.

### 8.4 Abuse controls

| Vector | Control |
|---|---|
| Guessing secrets for a known `link_uid` | `failed_attempts` on the row; after 10 within the window, `locked_until = now() + 15 min`, and the link's creator is notified. Distributed guessing hits the same per-link counter, so IP rotation does not help. |
| Enumerating `link_uid`s | 128-bit uid + a per-IP nginx `limit_req` zone on `location /api/v1/public/` + identical 404s (§8.5). |
| Upload flood / storage exhaustion | Per-link file count, per-file bytes, total bytes; per-IP request rate; the bridge's existing `max_body_bytes` (`src/http_server.cpp:294`) as the outer bound. |
| Bandwidth drain on a download link | `max_uses` is the primary bound; sessions are also capped in count per link per hour. |
| Zip amplification (a folder link as a bandwidth cannon) | `share.zip_max_bytes` / `share.zip_max_members` refused **at creation** (§6.1); `archive_bytes × max_uses` is the exact worst-case egress of a link and the UI shows it; concurrent zip streams capped per link and per bridge instance. |
| Zip-slip against the recipient | `archive_path` normalized and validated at creation (§5.2) — no `..`, no absolute paths, no leading separator. |
| Replaying a session or recipient token | Both are random 256-bit values, hashed at rest, bound to `link_uid` (and the recipient token also to the verified address); neither survives its TTL. |
| **Using the OTP endpoint as a mail relay** | Structurally impossible: the destination set is closed at creation (§6.9). There is no mode in which an outside caller chooses an address. |
| **Mail-flooding a listed recipient** | `rate_ok` buckets per `(link, email)` (3 / 15 min) and per link (20 / day) — `tokens.py:128`. |
| **OTP brute force** | 6 digits, 5 attempts per challenge, challenge burned on exhaustion, failures also feed the link's `failed_attempts`. `consume_code` does not delete on a wrong code, so the counter is an explicit `rate_ok` bucket in `ldap_manager`'s Redis — **shared across bridge replicas**, never per-process (§6.9). |
| **Enumerating the recipient list** | `/identify` returns the identical response for listed and unlisted addresses (§6.9, §8.5). |
| Confused deputy via MCP | The share RPCs are simply not exposed on the MCP door (§11). |

### 8.5 Uniform failure

Unknown uid, bad secret, expired, revoked, exhausted, locked, resource deleted,
creator lost access, pinned version culled — **all** return
`404 {"error":"not_found"}` with no timing-distinguishable path. The real reason
goes into the audit `detail` only. Anything else turns the public endpoint into
an oracle: "this link exists but is exhausted" tells an attacker they hold a
valid uid, and a distinct response for an unlisted address would enumerate the
recipient list (§6.9).

---

## 9. Configuration (core `core.conf` / env, bridge env)

> Note the two `share.*` namespaces: the **configuration** keys below, and the
> per-file **metadata** keys in §6.8 (`share.link_uid`, `share.verified_email`,
> `share.source_addr`, `share.claimed_name`). They never appear in the same
> place, but the shared prefix is worth knowing before grepping.

| Key | Default | Meaning |
|---|---|---|
| `share.enabled` | `false` | Deployment kill switch. Off ⇒ `CreateShareLink` refuses and the public prefix 404s wholesale. **Off by default** — an unauthenticated door is opt-in. |
| `share.max_ttl_days` | `30` | Cap on `expires_at`; the UI cannot offer longer. |
| `share.max_uses_cap` | `100` | Cap on `max_uses`; `0` (unlimited) is only accepted if this is `0`. |
| `share.default_ttl_days` | `7` | Pre-filled in the UI. |
| `share.session_ttl_seconds` | `3600` | Redemption-session lifetime (§6.4). |
| `share.max_sessions_per_hour` | `20` | Sessions one link may open per hour, independent of the use cap (§8.4). |
| `share.max_recipients` | `20` | Addresses one link may be minted for. |
| `share.otp_ttl_seconds` | `900` | Code lifetime (§6.9). |
| `share.otp_max_attempts` | `5` | Wrong codes before the challenge is burned. |
| `share.otp_send_limit` | `3 / 15 min` per `(link, email)`, `20 / day` per link | `rate_ok` buckets. |
| `share.recipient_ttl_seconds` | `86400` | Recipient-token lifetime — how long a verified recipient can open further sessions without a new code. |
| `share.send_invite_default` | `true` | Pre-tick "email the link to these recipients". |
| `share.upload_max_bytes` | `1 GiB` | Default byte budget for a new upload link. |
| `share.upload_max_file_bytes` | `256 MiB` | Default per-file cap. |
| `share.zip_max_bytes` | `2 GiB` | Largest folder snapshot a link may be minted over; refused at creation (§6.1). |
| `share.zip_max_members` | `5000` | Same, by file count. |
| `share.zip_deflate` | `false` | Compress instead of store — forfeits `Content-Length` and resume-free progress (§6.5). |
| `share.zip_max_concurrent` | `4` | Simultaneous zip streams per bridge instance. |
| `share.retention_days` | `365` | How long dead rows — including recipient addresses (PII, §5.4) — are kept for audit (§5.5). |
| `SHARE_PUBLIC_BASE_URL` (bridge) | derived from `Host` | The origin URLs are built from at creation. Set explicitly to move share traffic to a separate download host later without invalidating links already sent (§8.3). |

---

## 10. Frontend

### 10.1 The drawer tab

`FileDetailsDrawer.vue` gains **`Share`** in `visibleTabs` (`:245–253`), shown
only when the user holds `SHARE_EXTERNAL` on the item and `share.enabled` is
advertised by the bridge. A file gets the download-link form; a **folder gets
both**, as a two-way choice at the top of the tab — *"Let someone download this
folder"* vs *"Let someone send you files"* — since a folder can carry links of
either kind at once. The tab is deliberately **separate from `Access`** — Access
is "which of our people", Share is "someone outside" — and the empty state links
to the Access tab for the common case where the user actually wanted ACLs.

Create form: **recipients** (an email-chip field, at least one, capped at
`share.max_recipients`) with a pre-ticked *"email the link to them"*; expiry
(presets + date picker, clamped to `share.max_ttl_days`); max downloads / max
files and an optional per-recipient cap; optional note; plus per kind:

- **File download** — "always send the newest version" toggle (off = pinned,
  with the current version name shown).
- **Folder download** — "include subfolders", and "include anything added later"
  (off = snapshot, §6.5). The form shows the resolved **member count and archive
  size** before the user commits, and refuses with a clear number when the
  folder exceeds `share.zip_max_*`. Worst-case egress (`archive size × max
  downloads`) is shown beside the use cap — it is the number that surprises
  people.
- **Upload** — per-file cap, total byte budget, optional landing subfolder,
  optional extension allowlist.

On success: the URL exactly once, in a copy-to-clipboard field, with the existing
`QrCode.vue` beside it (a QR of a one-time drop link is genuinely useful on a
job site) and an unmistakable *"this is the only time this link is shown"*
notice.

### 10.2 Status and history — did they actually get it?

The question a sharing user asks five minutes after sending a link is *"did it
arrive, and did they open it?"* — and with no account on the far side, the Share
tab is the only place that can answer. This is not a nice-to-have panel; it is
what makes an unauthenticated hand-off usable by someone who is accountable for
the document.

#### Link-level status

Each link in the list shows one computed state, not a pile of raw fields:

| Badge | Meaning |
|---|---|
| **Active** | Redeemable right now. Shows the expiry countdown and `3 / 5 used`. |
| **Exhausted** | Use budget spent. Distinct from expired — the fix is a new link, not a longer one. |
| **Expired** | Past `expires_at`. |
| **Revoked** | Revoked by *(who, when)*. |
| **Blocked** | `locked_until` is live after repeated bad secrets or codes (§8.4) — someone is probing it. |
| **⚠ Not working — you no longer have access** | The §6.3 re-check now fails for the creator. |

That last state earns its prominence. Because a link carries its creator's
*current* authority, a link can stop working with nothing about the link having
changed — someone edited an ACL three folders up, or the pinned version was
culled. Without this badge the creator's experience is "my recipient says the
link is broken and everything looks fine to me", which is the single most
expensive support conversation this feature could generate. The list therefore
runs the same pre-flight as creation (§6.1) when the tab opens, and names the
reason in a tooltip.

#### Per-recipient status

Because the recipient set is closed (§6.9), the useful view is a **roster**, not
an event stream — "who has picked this up and who hasn't" is the actual
question. One row per address, with a status ladder:

| Status | Reached when |
|---|---|
| **Invited** | On the list. Shows whether the system mailed the link, and when. |
| **⚠ Invite failed** | SMTP rejected or bounced (§6.9) — the creator finds out here, not from silence. |
| **Opened** | A code was requested — proof the person reached the landing page. |
| **Verified** | Code accepted; they proved control of the address. |
| **Downloaded** *(or **Dropped 3 files**)* | A session completed with bytes moved. Shows count, timestamp, and size. |
| **⚠ Code attempts failing** | Repeated wrong codes against this address. |

The roster answers "Priya has it, Marcus never opened the mail" at a glance,
which is what a person chasing a deliverable needs — and it doubles as the
nudge list.

#### History

Expanding a recipient (or the link's **History** view) shows the ledger from
`GET /v1/shares/{link_uid}/redemptions` (§7.1), newest first: verified address,
timestamp, source IP, user agent, bytes moved, and — for a folder download — how
many members were served and how many were omitted by the §6.5 per-member
re-check. For an upload link each row links to the file the drop created
(`result_uid`), so "what did they send us" is one click, not a hunt through the
folder.

Failed attempts are summarised from the link's counters rather than the audit
log, so an ordinary user needs no `AUDIT_READ` scope to see that their link is
being probed. The full forensic trail stays in `audit_service` for anyone who
does (§12).

#### Actions on the roster

- **Resend invite** / **Resend code** — subject to the §9 send limits, with the
  remaining wait shown rather than a silent no-op.
- **Add recipient** — extends the allowlist after the fact (the common case:
  "loop in their site manager"). An **authenticated creator** may extend the
  list; an outside caller never can (§6.9). Audited as a permission change.
- **Remove recipient** — a partial revoke: kills that address's access while the
  link keeps working for everyone else. Cheaper and less disruptive than revoking
  and re-issuing, which would invalidate the URL for people who already have it.
- **Revoke** — the whole link, immediately.

### 10.3 The admin console — everything currently reachable from outside

A `tenant_admin` sees the Share tab's status model applied to the whole tenant at
`/admin/shares` (`meta: { requiresAuth: true, requiresAdmin: true }`, alongside
the existing admin routes in `src/router/index.ts:55–67`), backed by
`GET /v1/shares?all=true` (§7.1).

**The question it answers** is not "list some rows" — it is *what is currently
reachable from outside this tenant, by whom, and who opened that door*. So the
default view is **live links only**, sorted by risk rather than by date: links
whose resource sits highest in the tree first, then by recipient count, then by
remaining budget. A hundred expired links are noise; one live link on a project
root shared with eleven outside addresses is the finding.

Columns: resource (deep-linked), kind, **creator**, recipient count, status badge
(§10.2), expiry countdown, uses `n / m`, and last activity. Filters that matter
in practice:

- **By creator** — the departed-employee query. Selecting a person shows every
  door they left open; **Revoke all** ends the review in one action, which is the
  whole point of having the console rather than a report.
- **By recipient address / domain** — "what did we ever send to
  `@contractor.example`", the question that arrives when a relationship ends or a
  counterparty is breached.
- **By resource subtree** — everything shared out of a given folder.
- **Status** — live / expired / revoked / **not working**, that last one being a
  useful queue in its own right: links a creator will complain about tomorrow.

**Admin powers stop at revocation.** The console can revoke a link or a
recipient, and read the ledger. It cannot *mint* a link on someone else's behalf,
cannot re-send a URL (the plaintext secret does not exist server-side, §8.2), and
cannot see file contents it would not otherwise be entitled to — the resource
column deep-links into the normal browser, where the admin's own ACLs apply. An
oversight surface should shrink the blast radius, not become a new way to
enlarge it.

Every action here is audited with the acting admin as `actor`, and the console
itself carries the honest caveat that a revoked link's *already-downloaded* bytes
are gone — revocation stops future redemptions, it does not un-send anything.

### 10.4 The recipient's view

New route `{ path: '/s/:token', name: 'ShareLanding', component: ShareLandingView,
meta: { requiresAuth: false } }` in `src/router/index.ts` — the same shape
`SsoLandingView` already uses (`:41`). The SPA fallback in
`docker_unified/images/nginx/snippets/tenant.conf` (`try_files … /index.html`)
means **no nginx change is required** for the route itself.

The view peeks (§6.4), then runs the **verification step first, before any of the
kind-specific UI**: an email field, then a 6-digit code field, then the payload
screen. The copy has to carry the uniform-response rule without sounding evasive
— *"If that address is on this link, a code is on its way"* — and must not hint
at how many recipients exist or who they are. Resend is available once the send
rate limit allows, with the remaining wait shown.

After verification the view shows one of:

- **File download:** file name, size, "expires in 6 days", "2 downloads left", a
  Download button. Nothing else — no nav bar, no tenant branding beyond the logo,
  no login affordance.
- **Folder download:** folder name, "38 files, 412 MB", a collapsible member
  list (from `/manifest`), and one Download button producing the zip. The list
  is read-only text — no per-file fetch, no preview, no navigation.
- **Drop:** a drop zone reusing `UploadTray.vue`, the remaining budget
  ("4 of 5 files, 780 MB left"), an optional free-text name (`share.claimed_name`
  — the address is already verified, so there is no email field here), and a list
  of what *this* session dropped. On completion, a plain confirmation.

All three render a session-less shell: `AppNav` is not mounted, and no call on
this page may carry a bearer token even if one is in `localStorage`.

### 10.5 Help

New `frontend/src/help/content/share-links.md` (category *Permissions*,
`related: [sharing, acl-basics]`), and a paragraph in the existing
`sharing.md` pointing at it — "sharing with someone who has no account". Per
project convention end-user docs live in the frontend repo, and this doc is the
internal counterpart, not a substitute.

---

## 11. Other doors

- **WebDAV** — no change. Share links are an HTTP/browser concept (the OTP
  exchange needs a browser); the WebDAV door has no session-less mode and gains
  none.
- **MCP** — deliberately **not** exposed in v1. Minting a link that reaches
  outside the tenant is exactly the action an agent should not be able to take
  from a prompt-injected document (the roadmap's own confused-deputy concern,
  §9.2). If it is ever wanted, it wants the interactive-consent path, not a tool
  call.
- **CSAI / discussion / folder_actions** — no change. Outside drops arrive as
  ordinary `file.created` events and are indexed, converted, and processed
  normally.
- **CLI** — `fileengine_cli share create|list|revoke` falls out of the new RPCs
  for free; useful for scripted distribution. Note it can only *mint* links —
  redemption needs the browser flow — so it is a provisioning tool, not a way to
  fetch shared bytes. Low priority.

---

## 12. Audit events (`audit_service`)

New actions, added to `codes.py` and `audit_entry.h`'s string map in lockstep
(append, never renumber — the header says so at `:25`):

| Action | Category | Fail-closed? | Notes |
|---|---|---|---|
| `share_link_create` | `permission` | **yes** | It is a grant of access; `is_fail_closed(Permission)` is already `true` (`audit_entry.cpp:62`). Creation must fail if it cannot be recorded. |
| `share_link_revoke` | `permission` | **yes** | Same reasoning. |
| `share_link_recipient_add` | `permission` | **yes** | Extending the allowlist widens who can reach the resource — the same class of change as the grant itself. |
| `share_link_recipient_remove` | `permission` | **yes** | The partial revoke (§10.2). |
| `share_link_redeem` | `access` (download) / `mutate` (upload) | no | Follows the existing access/mutate policy. `actor = "share:<link_uid>\|<verified_email>"` — the same actor string §6.8 writes onto dropped files, so the door and the human are one identifier. |
| `share_link_denied` | `access` | no | Outcome `denied`; `detail.reason` carries the real cause the caller never sees (§8.5). This is the event a rules-engine alert should watch — a burst of `denied` on one uid is a guessing attempt. |
| `share_link_challenge_sent` | `auth` | **yes** | An OTP was mailed (§6.9). `is_fail_closed(Auth)` is already `true` — consistent with the platform rule that auth events block the operation if they cannot be recorded. `detail` carries the address; `outcome = error` when SMTP failed, which is the event support will look for. |
| `share_link_challenge_verified` | `auth` | **yes** | The recipient proved control of the address. This is the event that makes a redemption attributable to a human. |
| `share_link_challenge_failed` | `auth` | **yes** | Wrong code, burned challenge, or an unlisted address — `detail.reason` separates them, the recipient's response does not. |
| `share_link_expired` | `admin` | no | Emitted by the retention sweeper. |

`detail` carries `link_uid`, `resource_uid`, `created_by`, `verified_email`,
budgets, and `bytes_moved`; `target_uid` / `target_type` are the shared resource, so the
existing "everything that touched this file" query picks share traffic up with
no new query path.

For a folder download the redemption event additionally carries
`members_served` / `members_omitted` (§6.5) and the member uid list, so
"everything that left the building in that archive" is answerable per file
rather than only per folder — otherwise a zip is a hole in the very reverse
query this feature exists to keep answerable.

---

## 13. Decisions taken, and what is still open

Identifiers are stable: a question keeps its number when it is answered, so
**R1–R6** below are the resolved ones (R1 and R2 were originally posed as Q1 and
Q2) and **Q3–Q6** are what remains.

### Resolved (2026-08-17)

**R1 — Passphrase: dropped.** *(Reversed once R4 made recipient verification
mandatory.)* An optional passphrase was proposed as the "email the link, text the
code" second factor. R4 supplies a strictly better one: the OTP goes to an
address fixed at creation, expires, is single-use, and **names the person who
used it** — none of which a shared passphrase does. Keeping both would mean two
secrets to distribute out-of-band for one property, and a passphrase is the half
that gets written in the same email as the link. Removes
`passphrase_salt`/`passphrase_hash` from `share_links`, the PBKDF2 work from the
core, and the `/unlock` route from the public surface.

**R2 — Folder download: in v1.** Store-only zip64 over a creation-time member
snapshot, assembled in the bridge, with the member set and exact archive length
frozen at session open (§6.5, §7.3). This is the largest single piece of new
work in the proposal — it adds a zip writer to the bridge, a members table, a
per-member authority pass, and a genuine egress-amplification surface on an
unauthenticated route (§8.4). Two sub-decisions were taken with it and are worth
revisiting if the first real corpus argues otherwise:

- **Snapshot by default, not a live walk.** Symmetric with §6.2's version
  pinning, and the bigger of the two leaks — a live folder link keeps exposing
  whatever lands in the folder next month.
- **Store-only, not deflate.** Buys an exact `Content-Length` (progress bars,
  precise egress budgeting) at a ratio cost that is near zero for PDF/IFC/image
  corpora. `share.zip_deflate` flips it for text-heavy folders.

**R3 — XSS boundary: header triple, no origin split** (§8.3), with
`SHARE_PUBLIC_BASE_URL` in place from day one so the split stays available
later without invalidating live links. The two guards in §8.3 are the price of
choosing the maintained property over the structural one.

**R4 — Recipient email gating: in v1, and mandatory.** *(Reversed 2026-08-17;
the earlier draft deferred this to v2.)* Every redemption — download **and**
drop — requires the recipient to enter an email address, receive a one-time code
there, and enter it before a session opens (§6.9). Two things follow that are
worth stating as decisions in their own right:

- **Recipients are an allowlist fixed at creation, and there is no open-email
  mode.** Accepting any address the visitor types would make an unauthenticated
  caller the chooser of a destination for tenant-branded mail — an open relay
  backed by the deployment's sending reputation. The closed list removes the
  capability rather than rate-limiting it, and as a side effect makes the link
  genuinely non-forwardable. The cost is real and accepted: "post the link and
  let whoever needs it grab it" is no longer supported.
- **`ldap_manager` joins the cross-repo scope**, owning code generation,
  delivery, single-use verification, and the send/attempt rate limits — reusing
  `TokenStore.issue_code` / `consume_code` / `rate_ok` and the
  `require_internal` server-to-server seam the 2FA flow already established.
  Share links now depend on **Redis and SMTP** being healthy; both fail closed.

**R5 — Creator role resolution at redemption: live from LDAP, via the bridge**
(§6.3, verified against the code). The earlier draft assumed the core could
re-resolve the creator's roles from `user_roles` and proposed refusing links
whose access came from a request-attached role. Both halves were wrong on the
facts: `user_roles` is effectively always **empty** in this platform (roles live
in LDAP `groupOfNames`, administered through `ldap_manager`; the core's
`AssignUserToRole` is off that path), so "refuse when the DB has no role" would
refuse links inside any **"Gated section (role)"** folder — one of the two
one-click templates the product ships. The resolved design instead has the
bridge call the existing `getRolesByTenant(created_by)` at redemption (current,
never snapshotted), **strips `system_admin` / `tenant_admin` / `administrators`**
so the ACL bypass can never reach a session-less route, and runs the identical
check as a **pre-flight at creation** so a link that would be born dead is
refused with a reason instead of 404-ing the recipient.

**R6 — Tenant-wide admin console: yes.** A `tenant_admin` gets one view of
**every live link in the tenant** — the deployment's answer to "what is currently
reachable from outside, by whom, and who opened that door" (§10.3). It is the
concrete form of the roadmap's *"every object a departed employee could still
reach via lingering share links"* reverse query, and it is nearly free: the
tables and the per-link status computation already exist for the Share tab, so
the console is a different query over the same data plus one route.

### Still open

**Q3 — Token in the URL path, or in the fragment?**
`/s/{uid}.{secret}` works everywhere (email, curl, QR) but lands in every proxy
and access log between here and the recipient. `/s/{uid}#{secret}` never leaves
the browser — but breaks `curl`, breaks link-preview bots benignly, and confuses
anyone who copies "the part before the #". Recommend the **path form**, with
nginx query/path scrubbing on the public location and short TTLs as the
compensating control.

**R4 largely defuses this question.** A leaked URL — in a proxy log, a
scrollback, a forwarded mail — is now inert on its own: redemption additionally
requires a code delivered to an address the creator listed at creation. The token
went from *the* credential to *one of two*, and the weaker one. Worth deciding,
no longer worth much.

**Q4 — Multi-file selection → one link?** R2 covers "share this folder". The
adjacent ask is "share *these six* drawings from across two folders" — an
arbitrary uid set rather than a folder subtree. `share_link_members` already
models it (the members table has no folder dependency); what is missing is a
selection UI in the browser and a creation call that takes a uid list. Cheap
follow-on, but not in the v1 stages below unless you want it.

**Q5 — Do folder links need a member cap the *creator* can see coming?**
§6.1 refuses at creation over `share.zip_max_*`, which is correct but blunt for
a 6 000-file project folder. Options: raise the cap, offer "include only files
modified since…", or let the UI mint per-subfolder links. Wants a real folder to
decide against.

**Q6 — Notifications to the creator.** Two are already mandatory under R4: a
**failed OTP send** (§6.9 — otherwise a mail misconfiguration is indistinguishable
from a mistyped address, and nobody finds out) and a **drop arriving** (a drop box
nobody watches is useless). Still open is the optional set — first redemption,
budget exhausted, expiry approaching. Recommend **first redemption = yes** now
that the notification carries a verified name (*"alice@contractor.example
downloaded Drawing-A.pdf"*), which is materially more useful than the anonymous
version would have been. The mailer and templates are in scope regardless.

*(Q7 — the tenant-wide admin console — is resolved; see **R6**.)*

---

## 14. Implementation stages

1. **M0 — Core: model & enforcement.** `SHARE_EXTERNAL` bit (+ `kAllPermissions`,
   proto enum, REST name map); `share_links` / `share_link_members` /
   `share_redemptions` in `create_tenant_schema`; `CreateShareLink` /
   `ListShareLinks` / `RevokeShareLink` / `RedeemShareLink` /
   `CloseShareSession` RPCs; the share-credential path on `StreamFileDownload` /
   `StreamFileUpload`; the §6.3 re-check (taking the creator's roles as an RPC
   argument, admin roles stripped core-side as a second line of defence);
   the creation pre-flight; the recipient allowlist and its enforcement at
   session open; audit emission.
   **Tests:** the authority re-check (creator loses READ mid-life), a link inside
   a `DENY everyone + ALLOW role` gated section redeeming correctly with supplied
   roles and failing without them, `system_admin`/`tenant_admin`/`administrators`
   never granting anything on the share path, atomic use consumption under
   concurrency, scope containment (a share credential cannot reach a sibling, a
   parent, or an ACL), uniform failure, a session refusing to open for an address
   not on the allowlist, and a wrong secret consuming an attempt but not a use.
2. **M1 — Core: folder snapshots.** The creation-time walk and member capture,
   `archive_bytes` computation, the per-member re-check at session open, the
   frozen member list on the redemption row. Kept separate from M0 because it is
   the one piece with real algorithmic content (§6.5, §7.3) and it should be
   testable against a fixture tree before any bridge code exists. **Tests:**
   snapshot excludes later additions; a member gaining DENY is omitted and
   audited, not fatal; archive-length arithmetic matches a real zip byte-for-byte
   (including zip64 and empty directories); `archive_path` rejects `..` and
   absolute forms.
3. **M2 — Bridge: owner-side routes.** `/v1/nodes/{uid}/shares`,
   `/v1/shares/*`; URL assembly from `SHARE_PUBLIC_BASE_URL`; audit passthrough.
   No public surface yet — the model is exercisable end-to-end by an
   authenticated caller before any public surface opens.
4. **M3 — `ldap_manager`: recipient OTP.** `POST /internal/share/email-challenge`
   and `/internal/share/email-verify` behind `require_internal`; `share_otp` token
   kind keyed by `link_uid|email`; `SHARE_OTP_EMAIL` + `SHARE_INVITE_EMAIL`
   templates; send/attempt rate-limit buckets; loud failure on SMTP error
   (unlike the 2FA handler's swallow). Independent of M4 and parallelizable with
   it. **Tests:** code single-use and constant-time (inherited from
   `consume_code`), send limits, SMTP failure surfaced rather than swallowed.
5. **M4 — Bridge: public routes + zip writer.** `/v1/public/shares/*` including
   `identify` / `verify` and the recipient token, the allowlist check delegated
   to the core, the uniform response for unlisted addresses, the per-challenge
   attempt counter; the explicit unauthenticated prefix; the creator-role
   resolution via `getRolesByTenant` on the redeem path (§6.3) with
   LDAP-unreachable failing closed; the store-only zip64 framer; response-header
   hardening (with the §8.3 all-routes test); per-IP rate-limit zone; concurrency
   cap; log scrubbing. **This is the review gate** — it warrants its own
   security-review pass before merge, and it is where an unauthenticated door
   first exists.
6. **M5 — Frontend: owner side.** Share tab — recipient chips + invite toggle,
   the file/folder-download/upload forms, member-count and egress preview, QR —
   plus the **status and history** surface (§10.2): computed link badges
   including *"not working: you no longer have access"*, the per-recipient
   roster, the redemption ledger, resend / add / remove-recipient / revoke.
   The status half is the part users will judge the feature by; it is not
   trimmable scope.
7. **M6 — Frontend: recipient side.** `/s/:token` landing, the email → code
   verification step, file download, folder download + manifest, drop flow.
8. **M7 — Frontend + bridge: admin console.** `/admin/shares` (§10.3) over
   `GET /v1/shares?all=true` with the creator / recipient / subtree / status
   filters, bulk **Revoke all by creator**, and the read-only ledger. Gated on
   `tenant_admin`; reuses the status computation written in M5 rather than
   re-deriving it.
9. **M8 — Ops & docs.** `audit_service` codes + a rules-engine alert on
   `share_link_denied` bursts; retention sweeper; `share-links.md` help page;
   `core.conf` / compose defaults (`share.enabled = false`).

M0–M4 are the security-bearing work; M5–M8 are surface. M2 is deliberately
shippable on its own so the model can be exercised before a public door exists
anywhere; M1 is separable so the zip arithmetic is proven against fixtures
rather than debugged through an HTTP stream; and M3 sits in a different repo and
language, so it can run in parallel with M2 once the two `/internal/share/*`
request shapes are agreed.

**Ops preconditions before M4 ships** — the unauthenticated door now depends on
more than the core: **Redis** (OTP storage) and **SMTP** (delivery) must be
healthy or no link can be redeemed, and the sending domain needs working
SPF/DKIM/DMARC or codes land in spam and every link looks broken. Verify these in
the target deployment before turning `share.enabled` on, not after the first
recipient complains.
