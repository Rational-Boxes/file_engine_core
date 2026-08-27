# Proposal: a guaranteed accountability record in the core

**Status:** Draft / research — for review
**Branch:** `feat/accountability-record` (file_engine_core)
**Author:** follows the subsystem parity sweep prompted by `PROPOSAL_metadata_change_events.md` (2026-08-26)
**Scope:** `file_engine_core` (schema + service layer); `audit_service` (consumer, unchanged contract)

> Two parts. **§2** reports a sweep of every core subsystem against the six
> architectural properties the platform claims, prompted by finding that metadata
> met none of them. **§4–§6** propose the fix for what the sweep found: a single
> core-local table that guarantees an accountability record, committed with the
> operation it describes, and therefore immune to queue failure, node loss and
> configuration.
>
> The platform is dev/alpha — no production data, no compatibility obligations.
> This is the window in which findings like these cost a schema change.

---

## 1. Summary

The metadata proposal found one subsystem that had never been held to the
platform's architecture. The obvious question was whether it was alone. **It is
not.**

The sweep (§2) found that *"culling is the only permissible destructive
operation"* — a guarantee the platform states and its permission model is built
around — is violated in **five places**, and that three subsystems keep no
history at all. The audit subsystem that would otherwise carry the record is
better-built than expected (§3) but is structurally **incapable of guaranteeing
one** (§3.2): it is best-effort by design for mutations, silently absent when
unconfigured, node-local, non-transactional, and not queryable from the core.

The result is that for the operations that matter most to accountability — who
was granted access, who revoked it, who destroyed what — **the platform cannot
promise that a record exists.**

§4 proposes closing that with an `accountability_record` table in the core,
written in the same transaction as the operation, fail-closed, never sampled and
never culled — and **delivered by pull rather than push**: `audit_service` reads
forward from a cursor over the table instead of depending on a queue (§4.3). The
table is a transactional outbox, so no broker outage, outbox overflow or lost
node can lose a record, and a consumer can replay core history from zero.

### The document has a second half

§5.4 addresses a capability the platform lacks entirely: **true delete**. EU
data-protection law grants a right to erasure, and a system that cannot
technically remove a specific piece of data cannot satisfy it — the inability is
itself the violation. Contracts add their own purge obligations on top.

That belongs here rather than in a separate document because it **changes the
rule this proposal depends on.** §1.1 states that culling is the only permissible
destructive operation; erasure makes it two, and the accountability record is
what makes the second one defensible — an irreversible, legally-motivated
destruction that leaves no attributable trace would be worse than not offering it.

The two halves also size differently. The accountability record improves a
guarantee the platform already partly makes; erasure supplies one it does not
have at all, and its cross-service half (§5.4.2) is larger than its core half.
**If only one is built, build erasure.**

---

## 2. The sweep

Each subsystem against the six properties. ✓ = holds, ✗ = does not, — = not
applicable.

| Subsystem | Versioned | Immutable | Soft delete | Attributed | Cull-only destruction | Observable |
|---|---|---|---|---|---|---|
| **Content / files** | ✓ version series | ✓ | ✓ `UndeleteFile` | ✓ `revised_by` | ✓ `PurgeOldVersions` | ✓ `file.*` |
| **Directories** | — | — | ✓ | partial | ✓ | ✓ `dir.*` |
| **Renditions** | ✓ inherits file | ✓ | ✓ | ✓ | ✓ | ✓ `is_rendition` |
| **Metadata** | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| **ACLs** | ✗ | ✗ | ✗ | ✓ `granted_by` | ✗ | ✓ `acl.changed` |
| **Roles / memberships** | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ `role.*` |
| **Tenants** | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |

Content is the reference implementation and holds on every axis. Everything
attached *to* content — the authorization and identity layer — does not.

> **Where the matrix is heading.** The metadata row is closed by
> `PROPOSAL_metadata_change_events.md`; the ACL and role rows by the §7.2
> decision, on the same append-only pattern. The tenant row is closed differently
> (§7.3): a tenant's history is tenant data and is destroyed with it, so the row
> stays ✗ **by design** — what changes is that the *act* of creating or destroying
> a tenant becomes recorded, globally and permanently, which today it is not.

### 2.1 Destructive operations outside culling

The §1.1 guarantee in the metadata proposal — committed data goes away only
through explicit, permissioned destruction (culling, and now erasure, §5.4) — is
currently false in five places:

| Site | Operation | Destroys |
|---|---|---|
| `database.cpp:1852` | `delete_metadata` | the metadata row, no history |
| `database.cpp:3105` | ACL revoke | the ACL row when the remaining bitmask is zero |
| `database.cpp:3383` | `delete_role` | every membership of that role |
| `role_manager.cpp:46` | `remove_user_from_role` | the membership row |
| `database.cpp:2562`, `:2575` | tenant deletion | `DROP SCHEMA ... CASCADE` + the registry row |

Only `database.cpp:1614` (version purge) is sanctioned — that *is* the cull.

Each of these is reachable by an ordinary permissioned caller, not by
`CULL_VERSIONS`. Tenant deletion is the most severe: a single `DROP SCHEMA
CASCADE` destroys every file, version, ACL, role and metadata row for a tenant,
and emits **no event at all** — there is no tenant event type in the vocabulary.
That the destruction is intended (§7.3) does not make its silence acceptable:
the platform currently cannot say that a tenant ever existed, let alone who
removed it.

### 2.2 What the sweep implies

Metadata is the worst case but not a special case. The pattern is that
**the object graph was built to the architecture and the authorization graph was
not.** Files got versioning, immutability, soft deletion, attribution and events;
the ACL, role and tenant layers got current-state tables and a best-effort audit
emit.

That matters more than it would for ordinary data, because the authorization
layer *is* the accountability story. "Who could read this in March" is not
answerable from a current-state table.

---

## 3. What exists today

The core is not naive here, and the proposal should not pretend otherwise.

### 3.1 The audit subsystem is well built

- Two **separate** streams, correctly distinguished: `fileengine:events`
  (`RedisEventSink`, fail-open, trimmed, drop-oldest — for consumers and
  reconciliation) and `fileengine:audit` (`RedisAuditSink` — for accountability).
- `RedisAuditSink` is **WAL-backed**: `publish()` appends to an append-only,
  fsync'd local WAL — *that* is the durability point — then a worker `XADD`s to
  the stream, retrying on failure so nothing is dropped. The WAL is truncated
  only when the backlog drains, and replayed on `start()`, with the consumer's
  `(event_id, ts)` key absorbing duplicates.
- Audit entries are richly structured (`scope`, `tenant`, `category`, `action`,
  `outcome`, `actor`, `actor_roles`, `target_uid`, `target_type`, `source_iface`,
  `source_addr`, `detail`).
- Permission audits (`emit_permission_audit`) **return a bool and can block the
  operation** — the fail-closed path already exists for that category.
- Denied accesses are always recorded in full and are never sampled away.

Nothing below is a criticism of that design. The gap is narrower and structural.

> **One thing does need changing, though, and it is not a gap but a leak.**
> `emit_mutate_audit` and `emit_access_audit` resolve the target's display name
> and store it in `AuditEntry::target_name` (`grpc_service.cpp:127`, `:189`).
> Filenames are party data, so the audit log accumulates content-derived data in
> a structure designed never to release it — see §5.4.7, which sets the rule that
> the chain captures identifiers and structure but never payload, and shows how
> to keep the logs readable without storing the name.

### 3.2 Why it cannot *guarantee* a record

| # | Gap | Evidence |
|---|---|---|
| 1 | **Mutations are best-effort.** The publish result is discarded and the mutation proceeds regardless | `grpc_service.cpp:132` — *"best-effort; the mutation proceeds regardless"* |
| 2 | **No sink means no record, silently.** Auditing unconfigured or disabled returns success and the operation proceeds | `grpc_service.cpp:75` *"auditing disabled -> never blocks the op"*; `NullAuditSink` *"pretends every entry is durable"* |
| 3 | **Not transactional.** The DB commit and the WAL append are separate; a crash between them loses the record for a committed operation | `RedisAuditSink::publish` is outside the operation's transaction |
| 4 | **Node-local.** The WAL guarantees delivery only if the node survives to drain it. A destroyed node takes undrained entries with it | `wal_path` is a local file — closed by §4.3's pull model, which puts the record in PostgreSQL |
| 5 | **Not queryable from the core.** The record is a delivery buffer en route elsewhere; the core cannot answer an accountability question about itself | no read path over the WAL |
| 6 | **Access records are sampled** by design, so read history is deliberately partial | `access_mode_`, `access_interval_` |

Points 1–3 are the substantive ones. **There is no moment at which "the operation
happened" and "a record exists" are atomic**, and the paths that would make it so
are explicitly best-effort.

Combined with §2 — the operations in question destroy their own evidence in place
— the failure mode is: an ACL is revoked, the row disappears, the audit entry is
dropped or never emitted, and **nothing anywhere records that access ever
existed**.

---

## 4. Proposal — `accountability_record`

A single append-only table in each tenant schema, written **in the same
transaction as the operation it describes**.

```sql
CREATE TABLE accountability_record (
    seq          BIGINT PRIMARY KEY,         -- gap-free, commit-ordered; assigned under the chain lock (§5.3.2) — NOT a BIGSERIAL
    ts           TIMESTAMPTZ NOT NULL,       -- microsecond; clock_timestamp() under the chain lock, strictly monotonic per tenant (§5.3.3). NOT now()
    actor        VARCHAR(255) NOT NULL,      -- never empty; "system" is explicit, not a default
    actor_roles  TEXT[],                     -- roles as presented at the time
    source_iface VARCHAR(32),                -- grpc | rest | webdav | cmis | …
    source_addr  VARCHAR(64),                -- client IP forwarded by the bridge
    category     VARCHAR(32) NOT NULL,       -- authorization | destruction | identity
    action       VARCHAR(64) NOT NULL,       -- acl.grant | acl.revoke | role.assign | cull.versions | tenant.delete | …
    target_uid   VARCHAR(64),                -- resource, where one applies. The uid ONLY — names are resolved at read time (§5.4.7), never stored
    target_type  VARCHAR(32),
    principal    VARCHAR(255),               -- for authorization changes: whose access changed
    detail       JSONB NOT NULL DEFAULT '{}',-- SCHEMA-CONSTRAINED per action (§5.4.7): permission mask, effect, keep_count, cut timestamp. Never content, filenames or metadata values
    prev_hash    BYTEA,                      -- §5.3
    hash         BYTEA
);
```

### 4.1 Scope — deliberately small

Recording everything rebuilds the event stream inside Postgres and puts a
synchronous write on every hot path. The table covers **only what has no
versioned home of its own**:

| Recorded | Not recorded | Because |
|---|---|---|
| ACL grant / revoke | content writes | the version series already records who and when |
| Role create / delete / assign / remove | metadata writes | the metadata log will (its own proposal) |
| Version culls, metadata culls | reads | volume; the audit sink samples these by design |
| **Erasures** — file, field and tenant (§5.4) | listings, stats | no accountability content |
| Tenant create / delete | | |
| **Service-credential lifecycle** — issue, rotate, prune, revoke, pepper rotation, capability grant/revoke | | |

The last row comes from `PROPOSAL_service_authentication.md` §3.6, which makes
each of those a record written in the same transaction as the change. They fit
this scope rule exactly: security-relevant, needing a guaranteed record, with no
versioned home of their own. Two constraints ride with them — the record must
never contain the credential itself (§5.4.7's payload rule applies with unusual
force there), and it names the **operator**, not the `cli` identity that carried
the call.

**The organising rule: an operation belongs here if it is security-relevant and
needs a guaranteed, chained, never-culled record — not merely because it lacks a
history elsewhere.** Content and metadata writes are excluded because their
versioned stores answer the question asked of them; authorization changes are
included even though §7.2 gives them their own append-only history, because they
are the canonical security event and the two records serve different purposes:

- *State reconstruction* — "what was the value at time T" — belongs to the
  versioned store (content versions, the metadata log).
- *Accountability* — "who did what, when, and were they allowed to" — belongs
  here.

A versioned store cannot answer the second (it records values, not the act of
changing them, and it is culled with the resource it describes). An
accountability log cannot answer the first without becoming the store. Both are
needed, and for authorization changes both are kept.

### 4.2 Guarantees

1. **Transactional.** The record is written in the operation's transaction. The
   operation and its record commit together or not at all — closing §3.2's gap 3.
2. **Fail-closed.** If the record cannot be written, the transaction rolls back
   and the operation fails. This deliberately inverts the best-effort posture of
   §3.2's gap 1, and is affordable *only* because §4.1 keeps the scope to rare
   operations.
3. **Not optional.** There is no configuration that disables it — closing gap 2.
   Auditing to `audit_service` remains configurable; the local record does not.
4. **Never sampled.** Unlike access auditing, every in-scope operation is
   recorded — closing gap 6 for the categories that matter.
5. **Queryable in the core**, over a tenant-scoped index on `(ts)`, `(actor, ts)`
   and `(target_uid, ts)` — closing gap 5.
6. **Delivered by pull, not push** — consumers read forward by cursor (§4.3), so
   no queue failure, outbox overflow or node loss can lose a record. Closes
   gap 4.
7. **Survives culling** (§5.2).

### 4.3 Relationship to the existing audit path — pull, don't push

This does not replace `audit_service`, which does cross-service correlation,
rules and a platform-wide tamper-evident chain that a single core table cannot.
It changes **how core records reach it**.

**`audit_service` polls the accountability table by cursor rather than relying on
the queue.** The table is a *transactional outbox*: records commit with the
operation, and a consumer reads forward from its last position.

```
operation ──┬─▶ commit (state + accountability_record, one transaction)
            │
            ├─▶ audit_service polls  seq > cursor      ← the guarantee
            └─▶ Redis XADD (optional)                  ← latency hint only
```

Each consumer keeps a per-tenant **`recorded_until` timestamp** — the `ts` of the
last record it appended to the audit chain — and fetches everything
**newer than** it. Because `ts` is assigned under the chain lock and forced
strictly monotonic per tenant (§5.3.3), a `ts > recorded_until` fetch is exact:
it cannot skip a record, return one twice, observe one that was rolled back, or
be overtaken by an earlier-stamped record committing later. `seq` travels with
each record so the consumer can verify contiguity and chain linkage (§4.3.2),
but the cursor itself is the timestamp.

What this buys, beyond the guarantee itself:

- **Node loss stops mattering** — §3.2's gap 4. The record is in PostgreSQL,
  which is replicated and backed up, not in a local WAL file that dies with its
  host. This closes the one gap the current design could not.
- **Delivery needs no WAL, no retry worker, no replay-on-start.** Those exist to
  make a push path reliable; a pull path is reliable by construction. For this
  category of record they can be retired.
- **`audit_service` can be rebuilt from zero** by resetting its cursor and
  replaying core history — impossible when the record's only home was a stream
  that trims.
- **Backlog is visible and bounded by disk**, not by an outbox capacity that
  drops oldest under pressure.

The queue is kept as a **trigger and a freshness assertion**, never a data
source. A best-effort `XADD` still fires, carrying the committed `seq` and
nothing the consumer acts on directly. On receipt the consumer does not process
the payload — it **reads the table immediately**, out of schedule.

That gives three things at once:

- **Latency.** The rules engine reacts to a suspicious authorization change
  without waiting out a poll interval.
- **No dependence.** A lost hint costs latency only; the scheduled poll collects
  the record regardless. Nothing is *only* on the queue.
- **A freshness check.** The hint asserts *"at least `seq` N exists"*. If the
  table read does not show N, the consumer is reading stale or lagging state —
  a replica behind the primary, say — and must retry rather than advance its
  cursor. Without the assertion that condition is invisible: the read simply
  looks like "no new records".

This is the same correction the metadata proposal makes one level down: *the
durable log is not the queue.* A stream that is trimmed, sampled or fail-open is
a fine transport and an unacceptable system of record.

#### 4.3.1 How the consumer reads it

`audit_service` should **not** read the core's tables directly. Reaching into
another service's schema would couple it to the core's internals, bypass the
core's own access control, and make any future schema change a cross-repo
release.

Instead the core exposes a pull endpoint over the **existing gRPC surface** —
`ListAccountabilityRecords(tenant, newer_than_ts, limit)`, returning records in
`ts` order (which is also `seq` order) with a `has_more` flag.

gRPC is the right channel and needs no new one: the logging service runs inside
the system network like the bridges, is within the trust boundary (§5.4.9), and
can already reach the core. So the `/internal` REST route an earlier draft
offered as an alternative is unnecessary — it would be a second door onto the
same data, with its own auth surface to get right.

**The endpoint still needs its own authorization.** Everything inside the
boundary is trusted, but the accountability table is the most sensitive dataset
in the system — reading it across tenants reconstructs who did what to whom
platform-wide. Least privilege applies even among trusted callers: gate the read
on a dedicated service role, so a compromised or misbehaving service that
legitimately holds gRPC access does not thereby hold the security log. The core
should also scope each call to the requested tenant rather than allowing an
unbounded "everything" read. The consumer advances its cursor only after its own durable
write, which makes redelivery-on-crash at-least-once; `(tenant, seq)` is the
idempotency key, and `audit_service` already de-duplicates on a comparable key.

#### 4.3.2 What the consumer verifies on every read

Whether triggered by the schedule or by a queue hint, the consumer performs the
same check — the hint only changes *when*, never *what*. On each batch, in `seq`
order:

| Check | Break means |
|---|---|
| `seq` is contiguous from the last seen | A record is missing. Under §5.3.2 gaps cannot occur naturally, so this is an integrity alarm, not a retry |
| `ts` is strictly increasing across the batch | The monotonic guard (§5.3.3) failed, or rows were reordered in transit — the cursor's exactness no longer holds |
| `prev_hash[n] == hash[n-1]` | The chain is broken or forked — tampering, or a write that bypassed the chain lock |
| `hash[n]` recomputes from the row | The row was altered after commit |
| The hint's asserted `seq` is present | Stale or lagging read (§4.3); retry, do **not** advance the cursor |

The first three are **security events in their own right** and must be raised as
such — not logged and stepped over. A consumer that skips a gap to keep draining
converts an integrity failure into silent data loss, which is the failure this
whole proposal exists to prevent. The correct behaviour is to stop advancing that
tenant's cursor, alarm, and require operator acknowledgement.

This is also why the chain is verified **on the consumer side** and not only at
rest in the core: it checks the record *and* its delivery in one operation, so a
transport that reorders, duplicates or drops is caught by the same mechanism that
catches a tampered row.

#### 4.3.3 Precedence — the database is the first authority

The rule generalises past core events: **on any incoming queue event, from any
subsystem, the core table is consulted and drained before that event is
recorded.**

```
queue event (csai / share / discussion / bridge / …)
        │
        ├─▶ 1. consult the core accountability table
        ├─▶ 2. drain and append every pending authoritative record, in seq order
        └─▶ 3. only then append the incoming subsystem event
```

The reason is the audit log's own chain. `audit_service` maintains a
tamper-evident hash chain across *all* sources, and a chain records the order in
which it was written. Appending a subsystem event while authoritative core
records that happened **earlier** are still unread would place them out of
temporal order permanently — the chain cannot be re-sorted after the fact without
rewriting it, which is precisely what a tamper-evident structure exists to
prevent.

So the core becomes the **anchor** the rest of the platform is sequenced against.
Other subsystems have no equivalent guarantee; interleaving them against a source
that does is what keeps the combined log meaningful.

This is also why the consumer's cursor is a **timestamp** rather than a sequence
number (§4.3.1). The precedence question is *"are there core records older than
this incoming subsystem event?"* — a comparison against the event's own time. A
`seq` cursor answers "have I read everything the core has written", which is a
weaker and differently-shaped question: it cannot be compared against an event
from a subsystem that has no `seq`. Time is the only axis the sources share, so
time is what the interleave runs on, and `recorded_until` is the watermark that
makes it decidable.

#### 4.3.4 What the ordering guarantee actually is

The operational rule is simple: **drain the table to current, then process the
queue event.** Never the reverse, and never partially.

What that buys is worth stating exactly, because the chain will contain
arrangements that look like anomalies to anyone expecting more:

- **The chain is ordered by *recording* time, not by *happening* time.** Entries
  are appended in the order the sink committed them.
- **Core records are exactly ordered among themselves** — strictly monotonic
  `ts` under the chain lock (§5.3.3), with no slack at all.
- **Cross-source ordering is accurate to within the queue latency.** A subsystem
  event is delivered some milliseconds after it occurred; a core record
  committing inside that window is drained first and therefore recorded first,
  even though the subsystem event happened marginally earlier.

That residual slack is accepted deliberately. Removing it would mean holding
every subsystem event in a buffer longer than the worst-case queue latency before
recording it — paying a delay on everything to correct an ordering nobody can
act on.

**Causal order survives regardless**, which is the property that actually
matters. If event B depends on event A — an access permitted by a grant, a
deletion following a permission change — then B could only occur after A was
*visible*, which is strictly later than A being recorded. Causally related events
are therefore separated by far more than the latency window, and the chain orders
them correctly. What may be transposed is only genuinely concurrent activity,
where no security conclusion turns on which came first.

**Consequence for the entry format:** each audit entry should carry both the time
it was *recorded* into the chain and the `occurred_at` reported by its source.
The chain is ordered by the former; the latter can legitimately run slightly
backwards between adjacent entries from different sources. Recording both makes
that visible and explicable, rather than looking like clock corruption to a later
reader — or worse, prompting someone to "fix" it by reordering a tamper-evident
structure.

Two further consequences worth being explicit about:

- **What this does and does not give.** Every subsystem event is correctly
  ordered *relative to core records*. Ordering **between** two different
  non-core subsystems still rests on their timestamps, and so remains subject to
  clock skew. This proposal anchors the chain; it does not claim a perfect global
  total order across services, and the audit log should not be read as offering
  one.
- **Behaviour when the core is unreachable.** Recording a subsystem event without
  first draining would break the ordering guarantee, so the security sink must
  **not** proceed. This costs nothing in practice: a core outage already stops
  the platform — feature services fail closed without the core, and the bridges
  have nothing to serve — so there is no work being lost that the sink would
  otherwise have recorded. **A core outage is intended to take the system down**
  (§7.8), which makes blocking here consistent rather than costly.

#### 4.3.5 Tenancy — independent histories over one transport

**Each tenant has an independent history.** The chain, the `seq`, the `ts`
monotonicity, the `recorded_until` cursor and the drain-before-process rule are
all *per tenant*. There is no global chain and no cross-tenant ordering
guarantee, because there is no question that needs one: tenants are isolated
by design, down to separate PostgreSQL schemas.

The queue is shared, and that is a **transport decision, not a coupling**. Items
are ownership-tagged — `AuditEntry` already carries `scope = Tenant` and the
tenant id — and demultiplexed by the sink into per-tenant processing. One queue
is chosen for performance and simplicity; it does not merge the histories any
more than a shared network link does.

This resolves the precedence rule (§4.3.3) to something much cheaper than it
first sounds: an arriving event for tenant X requires draining **tenant X's**
table, not everyone's. A busy tenant never gates a quiet one, and the sink's work
per event is bounded by that tenant's backlog alone.

It also means per-tenant polling is the **correct shape rather than an
inefficiency to engineer around**. An earlier draft treated "many small queries
per interval" as a scaling problem needing a global probe; it is simply what
independent histories look like. Should the query count ever matter at scale, the
fix is a cheap "tenants with records newer than their cursor" probe to skip idle
tenants — an optimisation of *when* to poll, never a merging of the histories
themselves.

Two properties fall out of per-tenant chains that a global chain could not offer:

- **A tenant's history can be verified and exported on its own**, without access
  to any other tenant's records — which a single interleaved chain would make
  impossible, since verification would require the entire chain.
- **A tenant's history can be removed with the tenant**, cleanly — which is
  exactly what §7.3 decides should happen. A global chain would have made tenant
  deletion either impossible to honour or destructive to every other tenant's
  verifiability.

The shared queue's usual hazard — head-of-line blocking, one tenant's burst
delaying another — is also defused by the pull design: the queue is a hint
(§4.3), so a delayed or dropped item costs latency only, and the scheduled poll
collects the records regardless.

---

## 5. Design details

### 5.1 Write path

The record is produced where the operation's transaction lives (the `Database`
layer), not at the gRPC boundary where auditing sits today. That is what makes it
transactional; it also means bridges cannot bypass it by calling a lower layer.

`actor` is never defaulted. An operation that cannot name its actor is a bug, and
should fail rather than record `""` — an unattributed accountability record is
worse than none, because it looks like coverage.

### 5.2 Retention — it outlives culling

The accountability record is **not** culled with content. Culling erases history
by design (that is the §1.1 bargain), and a record that vanished with it would
erase the evidence *of* the cull.

So the table survives `PurgeOldVersions`, and records it: a cull writes a
`destruction` row naming the actor, the target, the keep-count and the resulting
cut timestamp. Growth is acceptable because §4.1's scope is rare operations, not
traffic.

**Erasure is the deliberate exception, and only at the right scope.** A *file*
erasure (§5.4) destroys the content but keeps the accountability trail — the
record of the erasure is the point of it. A *tenant* erasure destroys the tenant's
records with everything else (§7.3), leaving only the global lifecycle entry.
So the rule is: the record outlives every destruction of the thing it describes,
except the destruction of the tenant that owns it.

If retention over this table is ever needed beyond that, it must itself be an
explicitly permissioned operation that records its own execution. **Not proposed
here** — the right default at alpha is that it never deletes.

### 5.3 The chain — tamper evidence *and* ordering

`prev_hash` / `hash` give a per-tenant hash chain:
`hash = H(prev_hash ‖ canonical(row))`. It makes tampering locally detectable
without depending on `audit_service` being reachable, mirroring the chain that
service already maintains.

But a chain is inherently **order-dependent**, and that turns out to solve a
second problem rather than merely coexisting with it.

#### 5.3.1 The hazard a naive sequence has

An earlier draft of §4 gave `seq` as `BIGSERIAL`. That is wrong for a chained,
cursor-read table, in two compounding ways:

- **Values are assigned at INSERT and become visible at COMMIT.** Two concurrent
  writers can take 9 and 10, and 10 can commit first. A reader polling
  `seq > cursor` sees 10, advances to 10, and **record 9 is never read** — lost
  permanently, silently, and precisely for the record type that must not be lost.
- **Rolled-back transactions burn values.** Gaps are therefore *normal*, so a
  consumer cannot treat a missing number as an anomaly — which makes gap
  detection useless exactly where it is most wanted.

Chaining on top of that is worse: two transactions both read the same chain head
and both write `prev_hash = H8`, forking the chain.

#### 5.3.2 The chain *is* the sequence

Derive both from one locked head row per tenant:

```sql
CREATE TABLE accountability_chain_head (
    tenant     VARCHAR(255) PRIMARY KEY,
    last_seq   BIGINT NOT NULL DEFAULT 0,
    last_ts    TIMESTAMPTZ,
    last_hash  BYTEA
);
```

Each accountability write, inside the operation's transaction:

1. `SELECT ... FOR UPDATE` the tenant's head row — serializing appends per tenant.
2. `seq = last_seq + 1`; `ts = monotonic clock` (§5.3.3);
   `hash = H(last_hash ‖ canonical(row))`.
3. Insert the record; update the head.
4. The lock releases at commit.

This aligns three orderings that were previously independent: **assignment order
== commit order == chain order.** The consequences are what make the design work:

- **No gaps, ever.** A rolled-back transaction releases the lock without
  advancing `last_seq`, so numbers are never burned. A gap is therefore an
  unambiguous integrity alarm rather than routine noise.
- **No out-of-order visibility.** A record with a lower `seq` cannot commit after
  a higher one, so a cursor reader can advance safely with no snapshot watermark,
  no lag heuristic, and no risk of skipping.
- **The chain verifies the transport.** A consumer checking contiguity and
  linkage is checking delivery integrity at the same time.

The cost is real and should be stated: accountability writes **serialize per
tenant**. That is affordable only because §4.1 keeps the scope to rare
operations — it would be unacceptable on a content path. One caveat follows from
it: bulk authorization changes (setting a tenant's initial ACLs, say) must be
written as **one record describing the batch**, not N chained rows — for
contention, and because they are one logical act.

Given that the chain now carries the ordering guarantee and not merely tamper
evidence, treating it as optional (§7.1) is much weaker than it first appeared.

#### 5.3.3 The timestamp — high resolution, and strictly monotonic

Every record carries a high-resolution timestamp, and **the same lock that orders
the chain also orders the clock**. Three details make it correct rather than
approximately correct:

- **Use `clock_timestamp()`, not `now()`.** `now()` / `CURRENT_TIMESTAMP` /
  `transaction_timestamp()` all return *transaction start* time, so every record
  written in one transaction would share a timestamp taken before the lock was
  even acquired — reintroducing exactly the ordering ambiguity §5.3.2 removes.
  `clock_timestamp()` reads the wall clock at the moment of the call, which,
  taken inside the lock, falls in lock order.
- **Resolution is native.** PostgreSQL `timestamptz` stores microseconds, so no
  special type is needed; microsecond granularity is what "high resolution" means
  here, and it is fine enough that distinct records under a serializing lock do
  not realistically collide.
- **Enforce strict monotonicity anyway.** A clock can step backwards — NTP
  correction, VM migration, a leap-second smear — and a non-monotonic timestamp
  silently breaks any newer-than cursor. The head row therefore carries
  `last_ts`, and the append takes
  `ts = max(clock_timestamp(), last_ts + 1µs)`. Cheap, and it removes a whole
  class of problem: within a tenant, `ts` is **strictly increasing**, no
  duplicates, never backwards, regardless of what the system clock does.

Strict monotonicity is what makes the consumer's cursor exact. Because no two
records in a tenant share a `ts`, a `ts > recorded_until` fetch needs no
composite key and no tiebreak, and can neither skip a record nor return one
twice.

`seq` and `ts` therefore have distinct, non-overlapping jobs, and both are kept:

| | Job |
|---|---|
| `seq` | Chain linkage and **gap detection** — contiguity is what proves nothing was removed |
| `ts` | The **consumer cursor**, and the axis on which core records interleave with other subsystems' events (§4.3.3) |

---

### 5.4 True delete (erasure) — the second destructive operation

**This is a compliance capability, not a convenience.** EU data-protection law
grants a right to erasure, and a system that *cannot technically remove a
specific piece of data* cannot satisfy it — the inability is itself the
violation, independent of whether anyone ever asks. Contracts add their own
purge obligations on top, but the legal floor is what makes erasure
non-optional: a document platform without it is unshippable into the EU
regardless of what its customers ask for.

Culling cannot serve this. It compacts *history* while preserving current
state — the opposite of what an erasure obligation asks for.

> This document describes the engineering shape that makes the obligation
> satisfiable. Whether a given deployment complies is a legal determination
> involving retention justifications and exemptions, not an engineering one, and
> nothing here should be read as making that determination.

So the platform needs a second destructive operation, with a different shape:

| | Cull (`PurgeOldVersions`) | **Erasure ("true delete")** |
|---|---|---|
| Destroys | old versions and pre-cut history | **all content, every version, everywhere** |
| Preserves | current state | **the record that the file existed** |
| Driven by | storage/retention housekeeping | contractual or legal obligation |
| Permission | `CULL_VERSIONS` | a distinct, stronger grant (below) |

> **Amendment required.** `PROPOSAL_metadata_change_events.md` §1.1 states that
> *culling is the only permissible destructive operation*. That must become
> **culling and erasure are the only two**, with erasure defined here. The rule's
> intent is unchanged — destruction happens only through named, permissioned,
> recorded operations — but it is no longer a single one. The metadata proposal
> is on a separate branch; this amendment should land when the two merge.

#### 5.4.1 What erasure destroys, and what survives

The principle mirrors §7.3's tenant split one level down: **the payload is
destroyed; the fact is retained.**

Destroyed: every version's content bytes, in local storage, in the object store,
and in cache. **The entire value history of the file's metadata** (below). And
critically, **everything derived from the content** — see §5.4.2.

Retained: a skeletal existence record — uid, parent, creation and erasure
timestamps, the erasing actor — plus the accountability record of the erasure
itself. Enough to answer *"a file existed here and was erased, by whom, when"*
without retaining anything of what it contained.

**Open question the obligation itself raises:** does the *filename* survive?
`Acme_Corp_Contract_J_Smith.pdf` is itself external-party data, and a contract
requiring complete purge may not tolerate it. Recommend the name is redacted by
default and retention of it made an explicit option, since the safe default for
an erasure feature is to erase.

##### Metadata history, not just current metadata

Metadata values routinely carry personal data — a party name in a custom
property, a reference number, a free-text note. Erasing the *current* value is
not enough: under `PROPOSAL_metadata_change_events.md` metadata becomes an
**append-only log**, so every superseded value remains readable in history, and a
purge that clears only the live value leaves the PII intact one query away.

So erasure destroys the **value history**, across every entry in the log and any
base state left by an earlier cull. What remains is a **metadata trace**: the
entries keep their `seq`, `ts`, `actor`, `key` and operation, with values
replaced by redaction tombstones.

That preserves exactly the right thing. The metadata proposal's temporal queries
still work *structurally* — `ListMetadataVersions` still returns the sequence, and
`GetAllMetadataAt` still reports which keys were set at an instant — so the record
of *activity* survives while the data does not. One can still establish that a
field was set four times, last by a named actor on a given date, and then
deleted, with no way to recover what it ever said.

Note this is the same immutable-structure conflict as §5.4.7: an append-only log
must support value redaction, or erasure and the log's own guarantees are
irreconcilable. The metadata log has no hash chain, so redaction there is simpler
than in the accountability record — but it must be designed in, not bolted on.

**Key names are the metadata equivalent of the filename question above.** A key
like `patient_name` or `claimant_ssn` reveals by existing. Keys are usually
schema-shaped and safe to retain as the trace — and retaining them is what makes
the trace useful — but the same default should apply where they are not:
redactable, with retention the explicit choice.

##### Granularity — erasing a field without erasing the file

The obligation does not always target a whole document. A perfectly legitimate
file may carry one property containing a person's data, and the lawful response is
to remove that property's history while leaving the document intact.

Erasure therefore needs two scopes:

| Operation | Destroys |
|---|---|
| **File erasure** | content, all versions, the whole metadata value history, all derived data |
| **Field erasure** | one metadata key's value history for one file; content and other fields untouched |

Both record accountability, both propagate to services holding derived copies
(§5.4.5) — a field-level erasure still has to reach csai, whose index will hold
the property value once shape-catalog properties become searchable — and both are
covered by the reconciliation sweep (§5.4.6).

> **Cross-document amendment.** `PROPOSAL_metadata_change_events.md` must gain
> value redaction as an operation on the log, alongside the cull it already
> defines. Its §1.1 amendment (culling *and erasure*) already anticipates a second
> destructive operation; this specifies what erasure means for that subsystem —
> values destroyed, trace retained, structure intact.

#### 5.4.2 Derived data is the hard part

An erasure that clears only core storage is a **false guarantee**, because the
content exists in derived form across the platform:

| Holder | What it retains of the content |
|---|---|
| **csai** | extracted text, chunk contents, vector embeddings, search index entries — **and metadata property values**, once shape-catalog properties become searchable |
| **cmis adapter** | its own property store keyed `(tenant, uid, version)`; checkin comments are free text and can carry party data |
| **Renditions / markup** | rendered page images, thumbnails, previews — hidden children under the file uid |
| **convert / ONLYOFFICE output** | converted document copies |
| **difference_service** | comparison manifests |
| **discussion** | comment text quoting document content |
| **Backups / replicas** | everything, at rest — §5.4.4 |

Extracted text and embeddings are the sharpest of these: an embedding is a lossy
but real derivative, and a full-text index holds the words verbatim. Erasing the
file while leaving the search index intact does not meet any purge obligation
worth signing.

Erasure is therefore a **platform-wide operation, not a core-local one**. The
core owns the authoritative act and the record; each service must honour an
erasure for a given uid or tenant, driven by the event and pull mechanism in
§5.4.5.

#### 5.4.3 Attestation — you must be able to prove it happened

The contractual value of erasure is the ability to **demonstrate compliance**, so
fire-and-forget propagation is insufficient. Erasure is a tracked job:

1. The core destroys its own content and records the erasure as *initiated*.
2. Each participating service is asked to erase and **acknowledges** completion
   for that uid/tenant.
3. The erasure is recorded *complete* only when every participant has
   acknowledged; partial completion stays visibly incomplete rather than
   silently passing.
4. The completion record — participants, timestamps, outcome — is what an
   auditor is shown.

A service that is unreachable delays completion; it must never be skipped, and an
erasure stuck incomplete is an operational alarm, because it represents an unmet
contractual obligation rather than a transient error. §5.4.5 covers how the ask
reaches each service and how a service that missed it converges.

#### 5.4.4 Storage reality, and where it stops

Two findings from the code, one of which contradicts the documentation:

- **Object-store deletion is available.** `S3Storage::delete_file` exists.
  `CLAUDE.md` states *"S3 objects are immutable by design — deletion is not
  supported in the object store"*, which is either stale or describes a policy
  stance rather than a capability. Worth correcting either way, because an
  erasure feature cannot be designed against a constraint that is not real — nor
  shipped against one that is.
- **Encryption is deployment-wide, not per-file.** `Storage` takes a
  `bool encrypt_data` flag; the key is not per object. So **crypto-shredding —
  destroying a per-file key to render its ciphertext unrecoverable — is not
  available today**, although the primitives support it (`EncryptStream` already
  takes a key per stream).

That second point matters because of what deletion cannot reach: **backups,
snapshots, replicas, and any storage configured to be immutable.**

Two cases, with genuinely different answers:

**Backups and snapshots — solvable, and already solved here.** Deleting live
objects does not erase a file from last night's dump. The generally accepted
regulatory position is that backups need not be rewritten on demand, provided the
data is put beyond use and **cannot return to production** — which is exactly what
§5.4.6's full-from-zero reconciliation sweep provides: restore, then re-purge
before the data becomes usable. That is not a limitation to disclose apologetically;
it is the recognised mitigation, and the design already implements it.

**Immutable media — not solvable by deletion, at all.** Object-lock or
compliance-mode buckets, WORM archives and write-once tape cannot be rewritten
until their retention expires, by design. No delete call helps. The only
mechanism that erases from media you cannot rewrite is **crypto-shredding**:
encrypt each file under its own key and destroy the key, leaving ciphertext that
is not data about anyone.

That elevates per-file keys from a nice-to-have to **the prerequisite for
compliance-grade erasure on immutable storage**. The primitives already exist —
`EncryptStream` takes a key per stream — but the storage layer wires in a
deployment-wide `bool encrypt_data` instead, so the capability is absent today.

**Recommendation:** v1 implements direct deletion, which covers live storage and,
with the sweep, backups. **Deployments on immutable or WORM storage must not be
told erasure works for them until per-file keys land** — that is a false
compliance claim, and the worst possible thing to be wrong about here.

#### 5.4.5 The erasure event, and how services honour it

Services need to be *told*, and the natural mechanism is the one they already
consume. Two new types join the vocabulary:

| Event | Meaning to a consumer |
|---|---|
| `file.erased` | Destroy everything you derived from this uid, permanently, then acknowledge |
| `tenant.erased` | The same for every uid in the tenant, plus your tenant-scoped state |

**These must be distinct from `file.deleted`.** A consumer treats `file.deleted`
as a *soft* delete — recoverable, since `UndeleteFile` exists — so csai may
reasonably keep an index entry marked deleted rather than destroying the vectors.
Reusing that type for erasure would leave the extracted text and embeddings
exactly where they were, which is the failure §5.4.2 exists to prevent. The
semantics are different, so the type must be too.

**The event triggers; it does not guarantee.** `fileengine:events` is fail-open,
trimmed and drop-oldest by design (EVENT_CONTRACT.md §6.4) — appropriate for a
notification, unacceptable for a contractual obligation. A dropped erasure event
would leave a service holding data the platform has certified destroyed, and
would do so silently.

So erasure uses the same push/pull split as §4.3, which by now is the platform's
consistent answer to this shape of problem:

- **Push** — the event, for latency. A service purges within milliseconds.
- **Pull** — each service periodically asks the core for **erasures it has not
  acknowledged**, and works through them. This is the guarantee path, and it is
  what §5.4.3's attestation counts.
- **Acknowledge** — completion is reported back, closing the loop.

A service that missed the event, was down, or is newly reconnected converges via
the pull. The reconcile-sweep obligation in EVENT_CONTRACT.md §7 already
establishes this pattern for consumers; erasure makes it mandatory rather than
advisory.

**The late-write race.** An erasure can arrive while a service is mid-flight on
the same uid — a conversion running, an embedding being written. Purging and then
letting the in-flight job complete puts the derived data straight back, after the
erasure was recorded complete. This is how purges silently fail, so honouring an
erasure has two parts:

1. Destroy what exists.
2. **Record a local tombstone for the uid and refuse to write derived data for it
   thereafter.** Cancelling in-flight work is best-effort; refusing the write is
   what actually closes the race.

**Cross-repo work.** `EVENT_CONTRACT.md` §3 gains both event types with their
consumer obligations spelled out — that consumers must *destroy* rather than mark
deleted, must acknowledge, and must tombstone. That doc is the contract every
consumer is written against, so an erasure semantics that lives only here will
not be implemented uniformly.

> **Restore resurrects derivatives.** Restoring a service from a backup taken
> before an erasure reinstates the derived data the erasure destroyed, with no
> event to re-trigger the purge. This is resolved by the reconciliation sweep
> (§5.4.6) rather than by the pull: because erasure records are never culled and
> the history is small, a restored service re-reads it **from zero** and re-purges
> what it should not hold — no retention window to get wrong, and no dependence on
> any instruction being redelivered.

#### 5.4.6 Reconciliation sweeps — the backstop

The pull in §5.4.5 answers *"did I process every erasure instruction?"*. A
reconciliation sweep answers a different and stronger question: *"does the
derived data I hold correspond to content that still exists?"* — and it is the
only mechanism that does not depend on the erasure instruction reaching the
service at all.

That difference matters, because the ack-pull cannot catch:

| Failure | Why the pull misses it |
|---|---|
| A purge that was acknowledged but silently failed — a partial delete, a swallowed exception | The instruction *was* processed; the ack is a lie |
| Derived data restored from a backup predating the erasure (§5.4.5) | The service already acked; nothing re-issues the instruction |
| Orphans — derived data for a uid the service was never told about | There is no erasure record to pull |
| A tombstone lost with the service's own state | Same |

So erasure needs both, and they are complementary rather than redundant:
**instruction-driven purge for promptness, state-driven reconciliation for
eventual correctness.**

**Sweep against the accountability table, not against current state.** The
erasure history in that table is the guaranteed artefact — committed with the
operation, gap-free and commit-ordered (§5.3.2), never culled (§5.2). Everything
else a sweep could consult is weaker: the queue can drop, and a state query can
mislead.

That last point is worth being explicit about, because it is the intuitive design
and it is wrong. Erasure retains a skeletal existence record (§5.4.1), so an
erased uid still *exists* as an entity — a sweep asking "does this uid exist?"
gets `true` and keeps the derived data, silently defeating the purge. Sweeping
the erasure history sidesteps that entirely: it asks what was erased rather than
what exists, and gets an authoritative answer.

**This makes the sweep cheap, which is what makes it viable.** Sweeping a corpus
is O(documents) — a million-document tenant means a million checks, so it gets
run rarely, incrementally, or not at all. Sweeping the erasure history is
**O(erasures)**, and erasures are rare by nature: a handful per tenant per year,
not per day. The entire erasure history for a tenant is small enough to re-read
**from zero**, regularly, without incremental bookkeeping.

Practically, a service reads the tenant's erasure records — from its watermark
for routine passes, from zero for a periodic full pass — intersects the uids with
what it holds, and purges the overlap. It reuses the same pull endpoint and
cursor mechanism as §4.3.1; there is no new query surface.

The full-from-zero pass is what makes the backup-restore case (§5.4.5) resolve
cleanly: a service restored from an old snapshot re-reads the whole erasure
history and re-purges everything it should not have, with no dependence on
retention windows or on any instruction being redelivered.

**What this sweep does not cover.** It reconciles against *erasures*, so derived
data for a uid that was never erased — an orphan from a bug, or an indexing race
— is outside its scope. That is the forward direction of the existing obligation
in EVENT_CONTRACT.md §7, which requires consumers to reconcile against FileEngine
for content that exists and should be indexed. The two directions together are
what make the derived corpus correct; erasure adds the second, and neither
subsumes the other.

**The sweep must itself be observable.** A reconciliation that silently stops
running produces exactly the failure it exists to prevent, and looks healthy while
doing it — the same shape as a drain that exits 0 and appears to have shut down
cleanly. Each service should expose the timestamp of its last successful sweep,
and a stale value should alarm. An unmonitored backstop is not a backstop.

#### 5.4.7 The chain must never capture content in the first place

The accountability record is deliberately **immutable, hash-chained and never
culled**. Anything it captures is therefore something the platform has committed
to keeping — which makes *what goes into it* a compliance decision, not a
convenience one.

**The governing rule: the chain records identifiers and structure, never
payload.** It is a history trace of what was done, and it must be constructed so
that data subject to a removal obligation cannot enter it. Prevention, not
remediation: the cleanest way to survive an erasure request is to have nothing to
erase.

| Never captured | Necessarily captured |
|---|---|
| File content, or anything extracted from it | `target_uid` — an opaque identifier |
| **Filenames** | `actor`, `actor_roles`, `source_addr` |
| Metadata **values** | `principal` whose access changed |
| Checkin comments, discussion text, any free-text user input | action, outcome, timestamps |
| Anything the platform stores on a customer's behalf | permission masks, keep-counts, cut timestamps |

**This is already violated.** `emit_mutate_audit` and `emit_access_audit` resolve
the target's name and store it in `AuditEntry::target_name`
(`grpc_service.cpp:127`, `:189`). Filenames are party data — §5.4.1 flags
`Acme_Corp_Contract_J_Smith.pdf` as needing redaction on erasure — so the audit
log currently accumulates exactly the class of data this rule excludes, in the
one structure designed never to release it.

**Store references, resolve for display.** The fix keeps audit logs readable
without capturing anything: store the `uid`, and let a viewer join to the current
name at read time. Before erasure the log reads exactly as it does today; after
erasure the join finds nothing and the log automatically stops disclosing the
name. Compliance becomes a property of the architecture rather than an operation
someone must remember to run.

**Make `detail` structurally incapable of holding content.** A free-form JSONB
blob is where content leaks in — one well-meaning `detail["new_value"] = value`
and the log is holding metadata payload forever. `detail` should be a
**schema-constrained, per-action structure** with enumerated fields, not an open
map: `acl.grant → {principal, mask, effect}`, `cull → {keep_count, cut_ts}`. If
there is no field to put content in, content cannot arrive by accident.

##### The residue — identity, and why redaction is still needed

The rule above removes content from the problem entirely. What it cannot remove
is the record's *purpose*: `actor`, `source_addr` and `principal` are personal
data, and an accountability log that omitted them would record nothing worth
recording.

That residue is far more defensible than content would be — audit records
commonly carry a retention justification, whether a legal obligation to keep them
or the establishment and defence of legal claims — and it is the category
regulators most readily accept retaining. But **"the chain makes it technically
impossible" is not a justification**, it is the failure mode. The system must be
*able* to redact an identity; whether it should in a given case is a separate
question with a separate answer.

So redaction remains, now as a rarely-exercised capability for identity rather
than the primary defence for content:

- Each row's stored `hash` is retained as the chain link, **and the link is what
  the next row chains from** — so replacing a payload never breaks continuity.
- The payload is overwritten with a redaction tombstone naming what was removed
  and under what authority.
- Verification then reports the row as *redacted*: the chain is intact and the
  row's own content is no longer verifiable — which is the honest and correct
  result, not a failure.
- The redaction is itself an accountability record, so removing evidence is
  recorded evidence.

The alternative — pseudonymising actors up front, storing an opaque subject id
resolvable through a separate mapping that can be destroyed — is cleaner
cryptographically but degrades every routine audit query into a join, and moves
the erasure problem into the mapping rather than removing it.

**Recommend building redaction-tolerant chaining from the start.** Retrofitting
it means either rebuilding every chain or accepting a permanent break, and the
choice determines whether the log can ever satisfy an erasure request at all.

#### 5.4.8 Tenant-scale erasure

The same operation at tenant scope is what makes §7.3's decision genuinely
executable: `DROP SCHEMA CASCADE` destroys the core's copy, but a tenant erasure
must also propagate to every service holding derived data for that tenant, under
the same attestation (§5.4.3), leaving only the global lifecycle record.

Tenant erasure is the platform's most destructive operation and should require a
distinct grant — not `CULL_VERSIONS`, which exists for routine housekeeping and
would be far too widely held. Recommend a separate `ERASE` permission, restricted
to administrators, and recorded in the **global** table (§7.3), since a
tenant-scoped record would destroy itself.

#### 5.4.9 Gating erasure — a bespoke permission, and why a new bit is not enough

Erasure is irreversible, compliance-driven and the most destructive operation on
a file. It must be gated by its own permission, held by very few people:
**`Permission::ERASE`** (bit 12), joining the enum after `CULL_VERSIONS`.

Adding the bit is the easy part. **On its own it would gate nothing**, for a
reason that is worth stating precisely because it also affects an existing
guarantee.

##### The superuser has everything; the tenant admin is the problem

The platform superuser — **`system_admin`** — holds all permissions including
`ERASE`, naturally and by design. That is the break-glass identity and nothing
below changes it.

The issue is that the bypass does not stop there.
`AclManager::get_effective_permissions` returns `kAllPermissions` for
**`system_admin` *or* `tenant_admin`**, resolved before any ACL or parent lookup
(`acl_manager.cpp:218-221`), and `kAllPermissions` is deliberately exhaustive:

> *"Control bits (ACL_INHERIT) are included so that
> `(kAllPermissions & required) == required` holds for every possible
> `required`."* — `acl_manager.h:63`

`tenant_admin` maps from a tenant's `administrators` LDAP group — a normal
operational population, not a break-glass one. So a new `ERASE` bit checked
through the normal path is satisfied automatically for **every tenant
administrator**, and the bespoke permission would exist on paper while gating
nothing against exactly the population it is meant to exclude.

##### The same already applies to `CULL_VERSIONS`

`kAllPermissions` explicitly includes `CULL_VERSIONS` (`acl_manager.h:65`), while
the proto documents that permission as a *"destroy-data op; must be granted
explicitly"*. For the superuser that is unremarkable. For `tenant_admin` the two
statements cannot both hold: every tenant administrator already has version-purge
authority without it ever having been granted.

A live finding independent of this proposal, and the reason the pattern needs
establishing rather than following.

##### What actually gates it

**Split the bypass** rather than removing it:

| Role | Effective permissions |
|---|---|
| `system_admin` | `kAllPermissions` — everything, unchanged |
| `tenant_admin` | everything **except the destroy-data bits** (`ERASE`, `CULL_VERSIONS`), which require an explicit grant |

A tenant administrator remains fully able to run their tenant and must be
*granted* the ability to irreversibly destroy data within it — which is what
"bespoke permission, very privileged users only" asks for, since it makes the
grant a deliberate, recorded act rather than a side effect of an LDAP group.

The invariant in `acl_manager.h:63` then holds for `system_admin` but not for
`tenant_admin`, so its comment must be qualified accordingly. That is the
substantive change: *"admin passes every check"* stops being uniformly true, and
the header should say which admin.

##### The trust boundary, and what the permission is actually worth

The world-facing services — the bridges — are the trust boundary, and they are
assumed trustworthy. They authenticate the caller and present the resolved
identity to the core, which does not re-authenticate; gRPC is never
network-exposed.

So the `ERASE` check is a **real gate**, not a formality: every call that reaches
the core arrives with an identity a trusted service established, and the split
bypass above genuinely determines who can erase. Role assertion by an untrusted
direct gRPC caller would require either network exposure or a compromised
bridge — both of which are failures of the boundary itself, outside this
operation's threat model and not something an in-core permission could fix
anyway.

That being settled, the surface restriction below is **not** a hedge against
forged identity. It rests on a plainer argument: blast radius and ergonomics.

##### The invariant this rests on, and how well it is enforced

**The gRPC interface must never be reachable by an untrusted client, and must be
blocked from outside connections.** Every authorization decision in the core —
including the erasure gate above — assumes it. If the invariant fails, the
permission model fails with it, because the core does not authenticate.

How well it currently holds depends entirely on how the platform is deployed:

| Deployment | Enforcement | Assessment |
|---|---|---|
| `docker_unified` | The `core` service publishes **no ports**; peers reach it as `FILEENGINE_GRPC_HOST: core` over the compose network | **Holds.** Verified — there is no `50051:` host publish anywhere in the stack |
| Bare metal / systemd (the `.deb`, `.rpm`, `PKGBUILD` and Ansible paths) | Defaulted to **`0.0.0.0`** in both `config_loader.h` and the shipped `core.conf`, with `InsecureServerCredentials()` | **Was a host-firewall dependency; now fixed** — see below |

As found, the invariant was enforced by **deployment topology, not by the
application**: adequate in containers, fragile everywhere else.

> The monitoring listener was checked expecting the same problem, and does not
> have it. `http_metrics_addr` has defaulted to `127.0.0.1` since the L2 security
> review, `core.conf` does not set it, no compose file publishes `8081`, and the
> Ansible role leaves the default. `CLAUDE.md` claimed it defaulted to `0.0.0.0`
> and was "a known exposure to verify per deployment" — stale, and misleading in
> the dangerous direction, since it invited someone to widen a correct bind to
> match the documentation. Corrected on `security/grpc-loopback-default`.

**Done — the default is now loopback.** Implemented on
`security/grpc-loopback-default` (core) and `security/grpc-explicit-bind`
(`docker_unified`):

- `server_address` defaults to `127.0.0.1`, requiring explicit opt-in to bind
  wider. This inverts the failure mode: previously, forgetting the firewall
  exposed the port silently; now, forgetting to widen the bind breaks
  connectivity immediately and visibly.
- **`core.conf` had to change too**, and this was the part that mattered. It
  shipped `FILEENGINE_GRPC_HOST=0.0.0.0` and is what the packaged systemd path
  installs to `/etc/fileengine/core.conf` — so changing the code default alone
  would have left the bare-metal deployment, the one actually at risk,
  completely unaffected.
- The compose stack sets `FILEENGINE_GRPC_HOST: "0.0.0.0"` explicitly on the
  `core` service, where the network supplies the isolation instead. Verified
  across all three compose files: the base declares no `ports` for core, the test
  overlay adds none, and the external-data overlay overrides only `depends_on`.
- The Ansible `fileengine_core` role **already** set `0.0.0.0` explicitly, so
  that path needed no change and is unaffected.

Still worth doing: **firewall the port explicitly in the Ansible deploy**, rather
than relying on whatever the host happens to have. The loopback default makes
that belt-and-braces rather than load-bearing.

> **Worth considering, not required: mTLS on gRPC.** The channel currently uses
> `InsecureServerCredentials()`, so "the caller is a trusted bridge" is a claim
> the topology makes, never one the core verifies. Client certificates would make
> the trust boundary cryptographic instead of positional, so that a
> misconfiguration or a network mistake is no longer immediately a total
> compromise. The cost is certificate management across every service. Given the
> platform explicitly assumes trustworthy world-facing services, this is
> defence-in-depth rather than a correction — but it is the difference between an
> invariant that is *assumed* and one that is *checked*.

- The erasure grant should be **resource-explicit and non-inheriting**. Granting
  it on a folder must not confer it on every descendant; blast radius is the
  whole point.
- **Restrict the surface, not just the permission.** Erasure should not be
  reachable through the file-protocol bridges at all. WebDAV and CMIS are mounted
  drives and document clients where an accidental or scripted invocation is
  plausible, and neither protocol has a natural verb for it. Expose it on the
  admin surface only — the REST admin API and the CLI — so the operation is
  deliberate by construction. `cmis/SPECIFICATION.md` §12.2 already excludes
  `CULL_VERSIONS` from `cmis:all`; `ERASE` must be excluded there too, and
  additionally not mapped to any CMIS operation.

##### Open

- **Does field-level erasure (§5.4.1) share `ERASE`?** A data-protection officer
  removing a PII property may need that routinely while never needing to destroy
  a document. A separate, lesser grant serves that — but a lesser grant tends to
  become widely held, and it still destroys history irreversibly. Recommend one
  permission until there is evidence of the operational need.
- **Two-person control?** For an irreversible, legally-motivated operation, a
  single call from a single credential is thin, and second-approver or
  out-of-band confirmation is the conventional control. Worth deciding
  explicitly rather than defaulting to single-call because it is easier.

---

## 6. Non-goals

- Replacing `audit_service` or its cross-service chain (§4.3).
- Recording reads. Volume makes it a different problem, already answered by the
  sampled access audit.
- Fixing §2.1's destructive operations. This proposal makes them *recorded*; it
  does not make them *non-destructive*. §7.2 decides that ACLs and roles do
  become append-only, but that is its own piece of work.
- **Implementing erasure.** §5.4 defines what true delete must mean and what it
  must reach, because it changes the destruction rule this proposal depends on.
  Building it — the core operation, the per-service purge and sweep, the
  attestation job — is a separate effort, and the cross-service half is larger
  than the core half. Given §5.4's regulatory framing it is also the highest
  priority of the work this document implies: the accountability record improves
  a guarantee the platform already partly makes, whereas erasure supplies a
  capability it currently lacks entirely.
- Retention/export tooling over the table (§5.2).

---

## 7. Open decisions

1. ~~**Hash chain in v1?**~~ **Effectively settled by §5.3.2.** It was posed as an
   optional tamper-evidence feature, but the chain's locked head row is also what
   makes `seq` gap-free and commit-ordered — without it a cursor reader can
   permanently skip a record that commits late. The chain is load-bearing for
   *delivery correctness*, not only for tamper evidence, so it ships. The genuine
   remaining question is narrower: whether the head row is locked with
   `SELECT ... FOR UPDATE` or a Postgres advisory lock, which is an
   implementation choice about lock granularity and deadlock behaviour under
   concurrent tenant activity.
2. **Should the authorization layer also become append-only?**
   **Decided: yes.** ACLs and role memberships become append-only on the same
   pattern as metadata, closing the remaining ✗ column in §2's matrix.

   That makes it a **platform pattern rather than three separate fixes** — the
   metadata proposal's design transfers directly: an append-only log keyed by the
   subsystem's natural identity, current state as the latest entry per key,
   revocation as a tombstone rather than a row removal, an actor on every entry,
   and destruction only through culling.

   | | Log key | Value | Ops |
   |---|---|---|---|
   | Metadata | `key` | value | set / delete |
   | ACLs | `(resource_uid, principal, principal_type, effect)` | permission bitmask | grant / revoke |
   | Role membership | `(user_name, role_name)` | — | assign / remove |

   Each entry records the **resulting** bitmask for its tuple rather than a
   delta, so reconstruction is a lookup rather than a fold; a revoke to zero
   writes a tombstone. ACL history culls with the resource it is attached to, at
   the same cut as that resource's version history — the same derivation the
   metadata log uses. Role history has no payload to mirror and should simply not
   be culled: role changes are rare, the volume is negligible, and it is precisely
   the data an accountability question needs.

   **Two things to weigh before implementing, neither of which changes the
   decision:**

   *The ACL read is genuinely hot, unlike metadata.* `validate_user_permissions`
   has 33 call sites and runs on essentially every operation, and evaluation
   walks ancestor containers (`acl_manager.cpp:230`) — so a permission check is
   already multi-row and recursive. Turning current-state reads into
   latest-entry-per-key scans multiplies that by ancestors × entries × a log
   seek each. The metadata pattern's `DISTINCT ON` with an index on
   `(resource_uid, principal, principal_type, effect, seq DESC)` should hold up,
   since resources carry few ACL entries, and the feature services already cache
   permission decisions behind a TTL. But this **must be benchmarked before it
   ships**, and it should not be assumed to transfer for free from metadata,
   where the equivalent read is cold by comparison.

   *Core role membership is nearly empty.* `user_roles` is effectively unused on
   this platform — roles are request-borne from LDAP groups, so the core table
   rarely holds anything. Making it append-only is therefore correct but
   low-value on its own: **the role changes that actually matter happen in LDAP**,
   via `ldap_manager`. Real coverage for role accountability comes from
   `ldap_manager` writing accountability records for group membership changes, not
   from versioning a core table nobody populates. Worth confirming that is
   understood, since the core-side work alone would give a misleading sense of
   coverage.

   **Interaction with §4.1's scope rule.** That rule says a subsystem holding its
   own attributed, immutable history does not also belong in
   `accountability_record` — which, read literally, would now exclude ACL and role
   changes. It should not, and the rule needs sharpening: the discriminator is
   **security relevance and guarantee, not merely the existence of a history.**
   The ACL log reconstructs *state* ("what could this principal do at time T") and
   is culled with its resource; the accountability record is the chained,
   never-culled, guaranteed record of the *act* and its actor. Authorization
   changes are the canonical security event and stay in both, deliberately.
   Content and metadata writes remain out, as before.
3. **Tenant deletion.** **Decided: the history is tenant-scoped data and is
   destroyed with the tenant.** `DROP SCHEMA CASCADE` takes the tenant's
   accountability records along with its files, versions, ACLs, roles and
   metadata. No mandatory export, no retention of tenant contents past the
   tenant.

   One thing cannot live there, for a mechanical reason: **the record of the
   deletion itself**. A record inside the schema being dropped deletes itself, so
   the act needs a home outside the tenant. That is not an exception to the rule
   — the *tenant's* history is tenant data, but *"this tenant existed and was
   destroyed, by whom, when"* is platform-level accountability about an operator
   action, not content belonging to the tenant.

   So a small **global** table carries tenant lifecycle only — create and delete,
   with actor, source and timestamp. Nothing about the tenant's contents. This
   also closes the gap §2.1 identified independently: tenant deletion is
   currently the most destructive operation in the system and emits **no event at
   all**, because no tenant event type exists.

   **The tension this resolves, deliberately.** If deleting a tenant erased every
   trace of it, tenant deletion would become the cleanest way to destroy
   evidence: whoever can delete a tenant could erase the entire record of what
   happened inside it, including the record of their own actions. Splitting it as
   above defuses that — the contents are genuinely gone, but the fact that they
   existed and who removed them is permanent and lives where the deleter cannot
   reach.

   That split is also the shape data-protection regimes ask for: erase the
   subject's data, retain the record that erasure occurred.

   **Consequence for `audit_service`.** Its own store is separate, so the rule
   has to reach it: on seeing a tenant-deletion record it stops polling that
   tenant, drops its `recorded_until` cursor, and purges its retained records for
   that tenant — keeping only the lifecycle entry. The global record is what makes
   this drivable; without it the service would poll a vanished schema forever and
   silently retain history the platform believes it destroyed.

   **`audit_service` is one participant among several.** Tenant destruction must
   be genuinely complete to satisfy the contractual purpose (§5.4), which means
   propagating to every service holding derived data for that tenant — csai's
   index and embeddings above all — under the attestation in §5.4.3. A
   `DROP SCHEMA CASCADE` alone destroys the core's copy and leaves the tenant's
   documents legible in the search index.
4. ~~**Does the bridge-supplied identity need strengthening?**~~ **Settled: no.**
   The world-facing services are the trust boundary and are assumed trustworthy;
   they authenticate and present the resolved identity, and gRPC is never
   network-exposed. `actor` is therefore exactly as trustworthy as the boundary
   the platform already relies on for every other authorization decision, and a
   record of it is as good as the platform's identity model is — no better and no
   worse. Nothing about guaranteeing the *record* changes what the record
   attests. Recorded here only so the question is visibly answered rather than
   left hanging over a document about accountability.
5. **Backfill.** None is possible; history that was never written cannot be
   recovered. The table starts at deployment. Worth stating so nobody expects
   otherwise.
6. **Pull interval, and whether the queue hint is worth keeping at all.** §4.3
   keeps a best-effort `XADD` purely so the rules engine reacts faster than a
   poll. If the interval is short enough for the rules that exist, the hint is
   dead weight and the audit sink's push path can be retired for this category
   outright — fewer moving parts, one delivery mechanism. Recommend deciding this
   against the actual alerting requirements rather than keeping both by default.
7. **Does the WAL survive for other categories?** `RedisAuditSink`'s WAL still
   serves *access* audits, which are sampled and stay push-only (they are not in
   this table's scope, §4.1). So the WAL is not removed, only relieved of the
   accountability categories. Confirm that is the intended split rather than a
   staging post toward pulling everything.
8. ~~**The dependency chain §4.3.3 creates.**~~ **Decided: a core outage should
   take the platform down.** Every service depends on the core; there is no
   useful work to preserve while it is unavailable.

   The concern as originally written here overstated the cost. It described the
   precedence rule as *turning* a core outage into a platform-wide write freeze —
   but the platform is already frozen in that situation, by design and
   independently of anything proposed here. Feature services fail closed on an
   unreachable core (csai's permission cache is explicitly fail-closed; the same
   posture holds across the feature services), the bridges are pass-throughs with
   nothing to serve, and no service can authorize a write without the core. The
   audit dependency **aligns with an existing failure mode rather than adding
   one**, so the alternatives previously sketched — buffer-and-replay, or a
   quarantine region of the log — would buy availability that does not exist and
   pay for it in ordering guarantees that do.

   Three corollaries worth acting on, since this makes the posture explicit
   rather than emergent:

   - **Core availability *is* platform availability.** It is therefore the single
     highest-leverage place to invest in resilience — which is what
     `REPLICATION_FAILOVER.md` and the connection-router failover work already
     address. Nothing else needs its own survive-the-core story.
   - **Fail fast and legibly.** Dependent services should surface "core
     unavailable" directly rather than degrading into scattered timeouts, and
     their `/readyz` should reflect core reachability so load balancers drain
     them instead of serving confusing partial failures.
   - **Do not build cleverness against it.** A service that tried to keep
     accepting work during a core outage would be manufacturing exactly the
     unordered, unverifiable records §4.3.3 exists to prevent. This should be
     stated in the architecture notes so the temptation is closed off rather than
     re-litigated per service.

---

## 8. Acceptance

1. An ACL grant, an ACL revoke, a role assignment and a version cull each write
   exactly one `accountability_record` row with a non-empty `actor`.
2. **Atomicity:** if the record insert fails, the operation fails and no ACL/role
   change is visible. If the operation fails, no record is written.
3. **Not bypassable by configuration:** with the audit sink disabled entirely,
   every operation in (1) still produces a row.
4. **Not bypassable by queue state:** with Redis unreachable for the whole test,
   every operation in (1) still produces a row and still succeeds. Further:
   - **Pull delivers what push could not** — `audit_service` receives every
     record with Redis down throughout, polling `ts > recorded_until`.
   - **Replay** — a consumer restarted with a reset cursor reproduces the full
     core history in `seq` order.
   - **Node loss** — records committed before a hard kill of the core process are
     delivered after restart, with no local WAL involved.
5. **Survives culling:** after `PurgeOldVersions`, the rows describing earlier
   operations on that object remain, *and* a new row describes the cull itself.
6. **Queryable:** "every authorization change affecting principal P" and
   "everything actor A did between T1 and T2" are answerable from the core alone,
   with no `audit_service` and no Redis.
7. **Chain and ordering hold under concurrency.** With N writers issuing
   accountability operations against one tenant simultaneously:
   - `seq` is contiguous with no gaps and no duplicates.
   - `ts` is **strictly increasing** with no duplicates, and matches `seq` order.
   - Commit order matches both — no record with a lower `seq`/`ts` becomes
     visible after a higher one.
   - The chain links cleanly end to end, with no fork.
   - A consumer polling `ts > recorded_until` throughout observes every record
     exactly once.
   - **Clock hostility:** with the system clock stepped backwards mid-run, `ts`
     still increases strictly and no record is skipped or duplicated — proving
     the monotonic guard (§5.3.3), not merely the absence of NTP events during
     the test.
8. **Integrity breaks are raised, not absorbed.** Modifying a row, deleting one,
   or inserting one out of band is detected on the next consumer read; the
   consumer stops advancing that tenant's cursor and alarms rather than skipping
   ahead.
9. **A queue hint naming a `seq` the read cannot see** causes a retry, not a
   cursor advance.
10. **Precedence holds (§4.3.3).** Given a core accountability record committed
    at T1 and a subsystem queue event arriving at T2 > T1 but delivered before
    the scheduled poll, the audit log contains the core record **ahead of** the
    subsystem event. Verified by inspecting the resulting chain order, not just
    the timestamps.
11. **The table is drained fully before a queue event is recorded (§4.3.4).**
    With a backlog of N pending core records and a subsystem event arriving, the
    chain contains all N ahead of that event — not a partial drain, and not the
    event first. Scoped per tenant (§4.3.5):
    - A backlog in tenant A does not delay recording for tenant B.
    - An event for tenant X drains only X's table.
    - Tenant A's chain verifies end to end with tenant B's records absent
      entirely — the property a single interleaved chain could not provide.
12. **Causal order is preserved end to end.** A grant, then an access permitted
    by that grant, appear in the chain in that order even when they originate
    from different sources — the property the §4.3.4 slack is argued not to
    disturb, so it is tested rather than assumed.
13. **Both timestamps are recorded** on every entry, and `occurred_at` running
    marginally backwards between adjacent entries from different sources is
    accepted by the chain verifier rather than flagged as corruption.
14. **Erasure destroys payload and derived data, not the fact (§5.4).** After
    erasing a file:
    - No version's content is retrievable from local storage, the object store,
      or cache.
    - **csai returns no extracted text, no chunk content and no embedding** for
      it, and its renditions, previews and conversions are gone — the check that
      distinguishes a real purge from a core-local one.
    - A skeletal record remains showing that a file existed and was erased, with
      actor and time, and the accountability record of the erasure survives.
    - The erasure is marked **complete only after every participating service
      acknowledges**; with one service unreachable it stays visibly incomplete
      rather than reporting success.
    - A service that **never receives the event** — stopped for the whole test —
      still purges after restart, via the unacknowledged-erasure pull (§5.4.5),
      proving the event is not the guarantee path.
    - **`file.erased` is not treated as `file.deleted`:** csai destroys the
      vectors rather than marking the entry deleted.
    - **Late-write race:** an erasure issued while a conversion is in flight
      leaves no derived data behind once that job finishes — the local tombstone
      refuses the write.
    - **Reconciliation catches what the instruction path cannot (§5.4.6):** a
      service whose purge silently half-failed, and one restored from a backup
      predating the erasure, both converge to purged after a sweep — the restored
      case without any instruction being redelivered.
    - **The sweep is observable:** each service reports its last successful sweep
      time, and a stopped sweep alarms rather than appearing healthy.
    - **Redaction preserves the chain (§5.4.7):** redacting a personal identifier
      from an accountability row leaves the chain verifying end to end, reports
      that row as redacted rather than corrupt, and writes its own accountability
      record.
    - **Metadata history is purged, not just current values (§5.4.1):** after
      erasing a file whose property was set, changed and deleted over time, no
      query — including `GetAllMetadataAt` at any instant and
      `ListMetadataVersions` — returns any of those values, while the trace
      (`seq`, `ts`, `actor`, `key`, operation) remains.
    - **Field erasure is scoped:** erasing one property's history leaves the
      file's content and its other properties intact, and still reaches csai's
      index.
    - **The chain holds no content (§5.4.7):** after erasing a file, a full scan
      of the accountability records and the audit chain yields no filename, no
      metadata value and no extract — only uids, identities, actions and
      timestamps. Asserted by scanning the chain for the erased file's known
      name and property values and finding neither.
    - **`detail` cannot carry content:** an attempt to record an unenumerated
      field for an action is rejected by the schema rather than stored.
15. **Erasure is gated (§5.4.9).**
    - A `system_admin` can erase — the superuser holds every permission.
    - A **`tenant_admin` cannot**, until `ERASE` is explicitly granted; the same
      holds for `CULL_VERSIONS`, closing the gap between the proto's
      "must be granted explicitly" and the bypass.
    - A user granted `ERASE` on a folder does **not** thereby hold it on the
      folder's descendants.
    - Erasure is unreachable over WebDAV and CMIS, and `ERASE` appears in no
      CMIS permission mapping.
    - **The gRPC port is not reachable from outside the trust boundary** — no
      host publish in the container stack, and a bare-metal install binds
      loopback unless explicitly configured otherwise. Checked as a deployment
      test, since this is the invariant every permission decision rests on.
16. **Tenant destruction is total but not silent (§7.3).** After deleting a
    tenant:
    - Its accountability records are gone with its schema, and every other
      tenant's chain still verifies end to end.
    - A **global** record names the deletion, its actor and its time, and
      survives.
    - `audit_service` stops polling the tenant, drops its cursor, and purges its
      retained records for it — keeping the lifecycle entry.
    - Creating a tenant with the same name afterwards starts a fresh chain and
      does not resurrect or splice onto the old one.
17. A load check confirming the synchronous write is not on a hot path — a
   content read/write benchmark is unchanged, because neither is in scope (§4.1).
