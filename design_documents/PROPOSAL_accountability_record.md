# Proposal: a guaranteed accountability record in the core

**Status:** Draft / research — for review
**Branch:** `design/accountability-record` (file_engine_core)
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

### 2.1 Destructive operations outside culling

The §1.1 guarantee in the metadata proposal — committed data goes away only
through explicit, permissioned culling — is currently false in five places:

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
    ts           TIMESTAMPTZ NOT NULL DEFAULT now(),
    actor        VARCHAR(255) NOT NULL,      -- never empty; "system" is explicit, not a default
    actor_roles  TEXT[],                     -- roles as presented at the time
    source_iface VARCHAR(32),                -- grpc | rest | webdav | cmis | …
    source_addr  VARCHAR(64),                -- client IP forwarded by the bridge
    category     VARCHAR(32) NOT NULL,       -- authorization | destruction | identity
    action       VARCHAR(64) NOT NULL,       -- acl.grant | acl.revoke | role.assign | cull.versions | tenant.delete | …
    target_uid   VARCHAR(64),                -- resource, where one applies
    target_type  VARCHAR(32),
    principal    VARCHAR(255),               -- for authorization changes: whose access changed
    detail       JSONB NOT NULL DEFAULT '{}',-- permission mask, effect, keep_count, cut timestamp, …
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
| Tenant create / delete | listings, stats | no accountability content |
| Hard deletes, if any are ever added | | |

**The organising rule: if a subsystem holds its own attributed, immutable
history, it does not also belong here.** That is why the metadata proposal and
this one are complementary rather than overlapping — they answer different
questions:

- *State reconstruction* — "what was the value at time T" — belongs to the
  versioned store (content versions, the metadata log).
- *Accountability* — "who did what, when, and were they allowed to" — belongs
  here.

A versioned store cannot answer the second (it records values, not the act of
changing them, and it disappears when the row is destroyed). An accountability
log cannot answer the first without becoming the store. Both are needed.

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

Each consumer keeps a per-tenant cursor over `seq`. Because `seq` is assigned
under the chain lock (§5.3.2) rather than by a sequence generator, it is
**gap-free and commit-ordered**: a cursor cannot skip a record, cannot observe
one that was rolled back, and cannot be overtaken by a lower-numbered record
committing later.

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

Instead the core exposes a pull endpoint — `ListAccountabilityRecords(tenant,
after_seq, limit)` over the existing gRPC surface, or an `/internal` REST route
alongside the monitoring listener — returning records in `seq` order with a
`has_more` flag. The consumer advances its cursor only after its own durable
write, which makes redelivery-on-crash at-least-once; `(tenant, seq)` is the
idempotency key, and `audit_service` already de-duplicates on a comparable key.

#### 4.3.2 What the consumer verifies on every read

Whether triggered by the schedule or by a queue hint, the consumer performs the
same check — the hint only changes *when*, never *what*. On each batch, in `seq`
order:

| Check | Break means |
|---|---|
| `seq` is contiguous from the cursor | A record is missing. Under §5.3.2 gaps cannot occur naturally, so this is an integrity alarm, not a retry |
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

So the core's gap-free, commit-ordered `seq` (§5.3.2) becomes the **anchor** the
rest of the platform is sequenced against. Other subsystems have no equivalent
guarantee; interleaving them against a source that does is what keeps the
combined log meaningful.

Two consequences worth being explicit about:

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

Consequence worth noting: polling is per tenant, so a deployment with many
tenants makes many small queries per interval. A cheap `max(seq)` probe per
tenant, or a single global "tenants with new records" query, keeps that from
scaling badly. Not a v1 concern at alpha, but it should not be designed out of
reach.

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

If retention over this table is ever needed, it must itself be an explicitly
permissioned operation that records its own execution, and should require export
before deletion. **Not proposed here** — the right default at alpha is that it
never deletes.

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
    last_hash  BYTEA
);
```

Each accountability write, inside the operation's transaction:

1. `SELECT ... FOR UPDATE` the tenant's head row — serializing appends per tenant.
2. `seq = last_seq + 1`; `hash = H(last_hash ‖ canonical(row))`.
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

---

## 6. Non-goals

- Replacing `audit_service` or its cross-service chain (§4.3).
- Recording reads. Volume makes it a different problem, already answered by the
  sampled access audit.
- Fixing §2.1's destructive operations. This proposal makes them *recorded*; it
  does not make them *non-destructive*. Whether ACL revoke and role removal
  should also become append-only with tombstones — as metadata is becoming — is a
  real question and a separate one (§7).
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
2. **Should the authorization layer also become append-only?** §2 shows ACLs and
   roles are current-state tables with in-place destruction. Recording the change
   (this proposal) and versioning the state (like metadata) are different fixes.
   Recording is the higher value per unit of work and should come first, but the
   question deserves its own answer.
3. **Tenant deletion.** `DROP SCHEMA CASCADE` destroys the tenant's
   `accountability_record` along with everything else — the one operation whose
   record cannot live in the tenant schema. Options: a global (non-tenant) table
   for `identity`-category records, or requiring export before tenant deletion.
   **Recommend a global table for tenant lifecycle**, which also fixes the
   missing tenant event noted in §2.1.
4. **Does the bridge-supplied identity need strengthening?** The core trusts
   `AuthenticationContext` entirely, so `actor` is only as good as the bridge that
   set it. That is the existing trust model and this proposal does not change it —
   but a guaranteed record of an unverifiable actor is worth being explicit about.
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
     record with Redis down throughout, polling by cursor from `after_seq`.
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
   - Commit order matches `seq` order — no record with a lower `seq` becomes
     visible after a higher one.
   - The chain links cleanly end to end, with no fork.
   - A cursor reader polling throughout observes every record exactly once.
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
8. A load check confirming the synchronous write is not on a hot path — a
   content read/write benchmark is unchanged, because neither is in scope (§4.1).
