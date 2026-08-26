# Proposal: bring metadata up to the platform's versioning architecture

**Status:** Decided 2026-08-26 — all decisions recorded; ready for implementation planning
**Branch:** `design/metadata-change-events` (file_engine_core)
**Author:** gap surfaced 2026-08-26 while specifying the CMIS adapter (`cmis/SPECIFICATION.md`)
**Scope (cross-repo):** `file_engine_core` (metadata layer + publisher), `convert_search_ai` (contract doc + consumer), `folder_actions` (consumer), `cmis` (consumer, planned)

> **The metadata model was never brought up to the architecture the rest of the
> platform is built on.** FileEngine is pervasively versioned, immutable,
> soft-deleting, attributed, and observable through an event stream. Metadata —
> a first-class part of every object — is none of those things. It is a mutable,
> unattributed, silently-overwritten key/value side-table whose versioning was
> stubbed out and left with a comment saying so.
>
> This is a foundational oversight rather than a collection of bugs: the
> individual faults in §2 are not independent, they are what a subsystem looks
> like when it has not been held to the standard applied everywhere else. This
> plan closes that gap and brings metadata to parity.
>
> **Read §4.7 before sizing this.** Bringing a storage model to parity is
> substantially more than a patch, and §4.7 gives the real surface area. It also
> carries the reason to do it **now**: no deployment makes significant use of
> metadata yet, so this is a drop-and-replace with no migration — and it stops
> being one as soon as the CMIS shape catalog and csai's metadata indexing start
> putting data there that users care about.

---

## 1. Summary

FileEngine's architecture is stated plainly across the codebase and its docs:
content is **pervasively versioned**, versions are **immutable**, deletion is
**soft and reversible**, revisions are **attributed**, destruction happens **only
through explicit permissioned culling**, and every mutation is **observable** on
`fileengine:events` so downstream services can stay consistent.

Metadata meets none of them.

| Architectural standard | Content | Metadata (today) |
|---|---|---|
| Versioned | version series per file | **No** — versioning stubbed behind a `"current"` sentinel |
| Immutable | versions never rewritten | **No** — `set_metadata` overwrites in place |
| Soft-deleting | `RemoveFile` + `UndeleteFile` | **No** — `delete_metadata` destroys the row |
| Attributed | versions carry `revised_by` | **No** — no actor is recorded at all |
| Destruction only by culling | `PurgeOldVersions` + `CULL_VERSIONS` | **No** — any caller with WRITE destroys history |
| Observable | events on every mutation | **No** — metadata writes emit nothing |

These are not six separate bugs to trade off against each other. They are one
omission with six faces: **metadata was built as a mutable key/value side-table
and never revisited when the platform committed to a versioned, immutable,
event-sourced model.** The evidence that this was an unfinished intention rather
than a decision is in the source itself — four functions carry the sentinel
`version_timestamp = "current"` with the comment *"Would use actual current
version in practice"* (§2.1).

The cost is not theoretical. Three services are blocked on the missing events
(§2.6), two advertised RPCs return "not found" for every possible input (§2.2),
and the platform's own destruction guarantee — that committed data goes away only
through culling — is silently false for metadata.

**This plan brings metadata to parity**, on the same terms the rest of the core
already uses: its own version series, an append-only immutable log, tombstones
instead of deletes, attribution on every change, culling as the sole destructive
operation, and an event for every mutation.

One piece was already known: `convert_search_ai/design_documents/EVENT_CONTRACT.md`
§6 lists `metadata.changed` under *"Not yet in the publisher (tracked in the core
plan)"*. What that entry frames as a missing feature is better understood as one
visible symptom of the gap described here.

### 1.1 The standard being restored — destruction is culling, and nothing else

Of the six parity gaps above, one is worth stating as an explicit rule, because
it is a **system-wide guarantee that metadata currently breaks silently** (§2.3)
and because the §6 design is built to restore it:

> **Culling is the only permissible destructive operation.** Old payload versions
> are lost when history is culled; pre-cut metadata history is lost in exactly
> the same way, at exactly the same cut, under exactly the same permission. There
> is no other path in the core by which committed data goes away.
>
> Everything else is additive or reversible: content versions are immutable, file
> deletion is soft and reversed by `UndeleteFile`, and a metadata delete is a
> tombstone append rather than a row removal.

The loss is symmetric and deliberate, not a side effect. An operator who culls
history is already accepting that old file content is gone; this makes the
metadata that accompanied it go with it, on the same terms — rather than leaving
orphaned metadata history describing payloads that no longer exist.

Three things follow, and all are load-bearing rather than stylistic:

- Any implementation that removes a metadata row outside a cull — a `DELETE` for
  `delete_metadata`, a "tidy-up" sweep, a retention default — **breaks the rule**
  and must be treated as a defect, not an optimisation.
- Because the rule is system-wide, the permission is the existing
  `CULL_VERSIONS` rather than a new one, and its documented meaning widens from
  "purge old content versions" to "cull history" (§6.5.3).
- Restoring the rule is not optional polish. A guarantee that holds for content
  and quietly fails for metadata is worse than one that was never claimed, since
  every layer above — bridges, adapters, audit — is written assuming it holds.

---

## 2. Evidence — where metadata departs from the architecture

Every claim below is reproducible from the referenced source. Taken together they
describe one subsystem that was never held to the platform's standard, not six
unrelated faults.

### 2.1 Versioning was stubbed, not decided

In `core/src/filesystem.cpp`, four functions hard-code a sentinel where the
version should be, each carrying the same comment:

| Function | Version argument passed to the DB layer |
|---|---|
| `set_metadata` | `"current"` (literal) |
| `get_metadata` | `"current"` (literal) |
| `get_all_metadata` | `"current"` (literal) |
| `delete_metadata` | `"current"` (literal) |
| `get_metadata_for_version` | **the caller's `version_timestamp`** |
| `get_all_metadata_for_version` | **the caller's `version_timestamp`** |

```cpp
std::string version_timestamp = "current"; // Would use actual current version in practice
```

That comment is the clearest statement of the problem this document exists to
solve. Versioned metadata was *intended*; the schema carries a version column for
it; the proto exposes RPCs for it. The wiring was never finished, and the
placeholder became the permanent behaviour.

### 2.2 The versioned RPCs always miss

The writers are internally consistent — everything lands under the sentinel — so
the *unversioned* API works. But nothing ever writes a row under a real version
timestamp, while the two `*_for_version` readers query exactly that. Therefore:

- `GetMetadataForVersion` and `GetAllMetadataForVersion` return
  **"Metadata key not found" for every possible input**.
- Metadata does not participate in versioning at all: restoring an old content
  version does not restore the metadata that accompanied it.

Both RPCs are advertised in `proto/fileservice.proto` and reachable from every SDK
and bridge. The failure is indistinguishable from "this file has no metadata",
which is why it survived undetected.

### 2.3 Writes are destructive and in place

`set_metadata` overwrites the row; `delete_metadata` removes it. Neither leaves a
trace. This is the sharpest departure: the platform's guarantee is that committed
data is destroyed only by explicit, permissioned culling (§1.1), and for metadata
that guarantee is simply false — **any caller with `WRITE` silently destroys
history**, with no `CULL_VERSIONS` and no audit.

### 2.4 Changes are unattributed

`Database::set_metadata(file_uid, version_timestamp, key, value, tenant)` takes no
actor. The caller's identity is used for the permission check and then discarded.

Content revisions record `revised_by`, directory entries carry `created_by` and
`modified_by`, and every event carries `actor` — but who changed a metadata value,
and when, is not recorded anywhere in the system.

### 2.5 Mutations emit nothing

`FileSystem::set_metadata` ends at `return db_result;`. Compare `put`,
`restore_to_version` and every other mutation in the same file, which end with:

```cpp
emit_fs_event(tenant, FileEventType::FileUpdated, file_uid, user);
```

`delete_metadata` has the same omission, and the vocabulary has no metadata
member to emit even if they did — `core/include/fileengine/event.h` defines
`DirCreated`, `DirDeleted`, `FileCreated`, `FileUpdated`, `FileMoved`,
`FileRenamed`, `FileDeleted`, `FileRestored`, `AclChanged`, `RoleAssigned`,
`RoleMemberRemoved`, `RoleDeleted`. The contract doc's §3 table has no
`metadata.*` row.

### 2.6 Consequence — three services blocked

| Consumer | What it cannot do today |
|---|---|
| **convert_search_ai** | Synchronise the search index against metadata changes. Compounded by §3 — csai does not index the metadata bag at all |
| **cmis** (planned) | Emit a CMIS `updated` change-log event for a property-only edit. CMIS clients (CmisSync) sync primarily off `getContentChanges`; without this, property edits **never reach the desktop** until the client's ~6-hourly full crawl |
| **folder_actions** | Trigger any rule on a metadata change. Every one of its plug-in actions is driven off `fileengine:events` |

One absent event blocks three services — the argument for closing this in the
core rather than working around it per consumer. A CMIS-side workaround (poll
metadata during crawl) would hide a platform gap inside one adapter and leave the
other two unserved.

---

## 3. The consumer side was never built either

**convert_search_ai has never indexed the metadata bag.** Its only `metadata`
references are SSRF guards and the `xeokit3d` plugin reading IFC attributes out of
model files. It indexes content only.

This matters for sequencing: shipping `metadata.changed` alone does not make
metadata searchable, because there is no index for it to synchronise. The event is
the prerequisite, not the whole fix, and the csai-side work is tracked separately
(§5).

It also reinforces the framing. Metadata was not merely under-implemented in the
core — no consumer ever treated it as first-class either, because the core gave
them no way to.

---

## 4. Proposal — parity, not patches

Each element below closes one of the §1 parity gaps. They are presented
separately for review, but they are a single change: fixing the event without the
storage model leaves metadata mutable and unattributed, and fixing the storage
model without the event leaves three services blind to it.

| §1 gap | Closed by |
|---|---|
| Not versioned | §4.5 — its own timestamped series (§6.1) |
| Not immutable | §4.5 — append-only log; writes append rather than overwrite |
| No soft delete | §6.5.3 — `delete_metadata` becomes a tombstone append |
| Unattributed | §4.5 — every log entry carries its actor |
| Destroyable outside culling | §1.1 / §6.5.3 — culling becomes the only destructive path |
| Unobservable | §4.1–§4.4 — `metadata.changed` and the contract update |

### 4.1 Add the event type

`core/include/fileengine/event.h`:

```cpp
enum class FileEventType {
    …
    AclChanged,
    MetadataChanged,    // metadata keys were set and/or deleted on file_uid
    RoleAssigned,
    …
};
```

Contract string `metadata.changed`, matching the name already reserved in
EVENT_CONTRACT.md §6. Update `to_string()` in `core/src/event.cpp`.

Inserting mid-enum is safe here: the enumerator's numeric value is not
serialized or persisted anywhere — the wire contract is the string form via
`to_string()`, and a search finds no numeric use of `FileEventType`. Grouping it
next to `AclChanged` therefore costs nothing, and the check is recorded so an
implementer does not have to repeat it.

### 4.2 Carry the changed key

`FileEvent` already carries type-specific fields — `principal` + `permissions`
for `AclChanged`, `role` + `member` for the `Role*` events. Follow that precedent:

```cpp
    // Metadata events only (type == MetadataChanged). One event per write —
    // batch or single — listing what changed, so consumers update the affected
    // fields rather than re-fetching the whole bag. See §6.5.2.
    std::vector<std::string> metadata_keys_set;
    std::vector<std::string> metadata_keys_deleted;
```

**This is the load-bearing detail.** A bare "something about this object's
metadata changed" forces every consumer to re-fetch the entire bag per event.
The reconcile sweep has already been bitten by exactly this shape of
under-specified payload: `file.created` carries an empty `version`, which makes
version-equality idempotency double-run on every new file. Do not repeat it.

**One event per write, not per key** (§6.3): a batch of 20 keys is one
transaction, one metadata version, and one event. A single-key `SetMetadata`
fills exactly one entry, so consumers see one shape either way.

### 4.3 Emit at the write sites

`FileSystem` already uses **one helper per event family** — `emit_fs_event` for
lifecycle, `emit_acl_event` for grant/revoke (carrying `principal` +
`permissions`), `emit_role_event` for membership. Metadata follows that
precedent with a sibling rather than an overload:

```cpp
// core/include/fileengine/filesystem.h, beside emit_acl_event / emit_role_event
// One event per write — batch or single — naming what changed.
void emit_metadata_event(const std::string& tenant, const std::string& uid,
                         const std::vector<std::string>& keys_set,
                         const std::vector<std::string>& keys_deleted,
                         const std::string& user) noexcept;
```

Called once per committed write, after the log append succeeds. Emission stays
`noexcept`, fail-open and async on the existing bounded outbox
(EVENT_CONTRACT.md §6.4) — a metadata write must never block or roll back on a
broker outage.

> **§6.4 costs nothing.** `emit_fs_event` already enriches the envelope with
> `is_rendition`, "detected when the parent is a file" (`filesystem.h:285`). The
> metadata helper reuses that same enrichment, so flagging rendition metadata
> events needs no new detection logic — only that the helper not skip it.

**This is not a two-line change.** An earlier draft of this section described it
as one, which was true when the proposal was only "add a missing emit". It no
longer is: §6.1 replaces the metadata storage model underneath these call sites,
so `set_metadata` and `delete_metadata` are rewritten rather than appended to.
See §4.7 for the actual surface area.

### 4.4 Update the shared contract

`convert_search_ai/design_documents/EVENT_CONTRACT.md` is the cross-service source
of truth and must be updated in the same change:

- §3 event-type table — add the `metadata.changed` row.
- §2 envelope — document `metadata_keys_set` / `metadata_keys_deleted` alongside the
  existing `acl.changed` and `role.*` extra fields.
- §6 — remove `metadata.changed` from the "not yet in the publisher" list.
- `schema` stays at `1`: these are additive fields (§8).

### 4.5 Version, immutability and attribution (§2.1–§2.4)

Per §6.1/§6.2: give metadata **its own timestamped version series**, independent
of the content series, stored as an **append-only change log** and reconstructed
at a timestamp, with a temporal-join query for "the metadata sequence while
content version V was current". The `"current"` sentinel disappears — the live
bag is the latest entry per key, and history begins at the current base state.

In every case the `*_for_version` RPCs must stop silently returning "not found":
they either return real values under the semantics fixed in §6.5.1, or an
explicit error. Silent wrong answers are what §2.2 is about, and replacing them
with honest ones is part of the parity being restored.

Immutability and attribution come with the log rather than as separate work: an
append-only structure cannot overwrite, and every entry carries the actor the
old schema discarded (§2.4).

### 4.6 Batch write API

Per §6.3, a batch `SetMetadata` is in scope: multi-key, one transaction, one
metadata version, one event. It is what makes the event's key list meaningful,
and it makes CMIS `updateProperties` atomic.

### 4.7 Shape and size of the change

Bringing a subsystem to architectural parity is not a patch, and the work should
not be sized as one. The gap in §1 was open for the life of the metadata layer,
so closing it means **replacing the metadata storage model** — not adding an emit
and correcting a sentinel. Anyone estimating from §4.1–§4.3 alone would
underestimate it substantially, so the surface area is stated explicitly.

| Area | Change |
|---|---|
| Schema | New append-only `metadata_log(uid, seq, ts, actor, key, op, value)` + index `(uid, key, seq DESC)`; the old flat table becomes the `t0` base state |
| `FileSystem` | `set_metadata`, `delete_metadata`, `get_metadata`, `get_all_metadata`, `get_metadata_for_version`, `get_all_metadata_for_version` all rewritten against the log |
| New reads | `ListMetadataVersions`, `GetAllMetadataAt`, window flags on the `*_for_version` responses |
| New writes | Batch `SetMetadata` (§4.6) |
| Culling | `PurgeOldVersions` extended to collapse the metadata log at the oldest retained version's timestamp (§6.5.3) |
| Events | `MetadataChanged` type, plural payload fields, `emit_metadata_event` |
| Proto | New RPCs + messages, redefined `*_for_version` semantics, documented in comments |
| Cross-repo | `EVENT_CONTRACT.md` (§4.4); csai and folder_actions become able to consume |
| Migration | **None** — see below |

Only the last two rows are new *services* work; the rest is core.

#### No migration, and why that makes now the moment

**No deployment makes significant use of metadata, and losing the existing rows
on a schema change affects nothing.** That collapses the hardest part of this
work before it starts:

- The old `metadata` table is **dropped and replaced**, not migrated. No
  backfill, no rewriting sentinel rows into base entries, no dual-write, no
  cutover, no soak.
- Earlier drafts of this section proposed a four-step rollout — event first, log
  alongside, cut reads over, then new surface. That sequence existed to manage
  *migration* risk against live data. With no data worth preserving, the risk it
  hedged does not exist, and the steps are not worth their complexity.
- What remains is one coherent change. Any further splitting should be for
  **reviewability** — smaller PRs — not for risk management, and the pieces can
  land in any order that keeps the build green.

The one thing that does **not** become negotiable is the **API contract**. The
data may vanish; the endpoints may not. `GetMetadata` / `GetAllMetadata` /
`SetMetadata` / `DeleteMetadata` are wired into the REST bridge and the web UI's
Metadata tab (`FileDetailsDrawer.vue`), so they must keep working against an
empty store rather than start erroring. That is the whole of the compatibility
burden.

> **This is the cheap moment, and it will not last.** The reason this change is
> nearly free today is precisely that nothing depends on metadata yet. That
> stops being true shortly: the CMIS adapter's shape catalog puts business
> properties on documents and renders them inline in every listing entry, and
> csai's metadata indexing (§3) makes them searchable. Both are specified and
> both are coming. Doing this **after** they land turns a drop-and-replace into a
> genuine migration of data users care about, under exactly the parity guarantees
> §1 says are missing. The gap has been open for the life of the subsystem;
> closing it while it is still cheap is the argument for doing it now rather than
> minimally.

The same reasoning applies beyond metadata. **The platform as a whole is still
dev/alpha** — no production data, no external compatibility obligations, and
every bridge and service still under active construction. That is the window in
which an architectural deficiency costs a schema change instead of a migration,
a coordinated release and a back-compatibility shim.

Metadata's gap was found by accident, while specifying an unrelated adapter. That
it survived this long undetected is itself a signal: the standards in §1 are
stated in the architecture but not systematically checked against each subsystem.
Auditing the others against the same six properties — versioned, immutable,
soft-deleting, attributed, culling-only, observable — is work worth doing while it
is still this cheap, and is tracked outside this proposal.

---

## 5. Non-goals

- Building csai's metadata indexing (§3). That is csai-side work this proposal
  unblocks, not part of it.
- The CMIS adapter's own change log — it consumes this event; it does not
  motivate changing its shape beyond §4.2.
- Making metadata writes create a new file **version**. Decided against in §6.1;
  metadata gets its own series instead.

No longer a non-goal: the **batch write API**, pulled into scope by §6.3.

---

## 6. Decisions

**Decided 2026-08-26 (James).** §6.1/§6.2 are answered together by one model;
§6.3 and §6.4 are answered as recorded. §6.5 collects the follow-on details those
answers imply but do not themselves settle.

### 6.1 / 6.2 Versioning model — two independent timestamped series

**Decided: metadata gets its own version series, independent of the content
series.** Both are timestamped. A metadata write does **not** bump
`FileInfo.version` (answering §6.1: no), and versioned metadata is fixed not by
attaching metadata to content versions but by giving it a timeline of its own
(answering §6.2 with an option none of the three originally listed).

```
content  series:  ──V1─────────────V2──────────────────V3──▶   (PutFile)
metadata series:  ────M1───M2────────────M3────M4────────────▶ (Set/Delete/Batch)
                       └────────┘         └──────────┘
                  metadata current       metadata current
                  while V1 was           while V2 was
```

**Required query.** The API must answer: *"give me the metadata version sequence
covering the period while content version V was current."* Content version `V`
is current over `[V.ts, next(V).ts)` — or `[V.ts, ∞)` when latest — and the
answer is every metadata version whose timestamp falls in that window.

This is a temporal join between two series, and it is strictly more expressive
than the version-scoped metadata the proto originally implied: it preserves the
fact that metadata may change *several times* during one content version's life,
which a single snapshot-per-version model discards.

#### Storage — event-sourced, with compaction

**Decided: metadata is an append-only change stream, reconstructed at a
timestamp. Culling folds the stream below the horizon into a new base state.**

This supersedes an earlier recommendation in this document for validity-interval
rows (`valid_from`/`valid_to`). Intervals answer the temporal query adequately,
but event sourcing fits better for three reasons specific to this system:

- **The event and the record are the same fact.** The core is emitting
  `metadata.changed` regardless (§4). If the stored delta *is* the event payload,
  the version series, the audit trail and the notification cannot drift apart.
- **`ListMetadataVersions` becomes a range scan**, not a reconstruction. §6.1's
  required query is literally "the log entries in this window"; under intervals
  the sequence is re-derived from distinct `valid_from` values — recovering
  something the interval form discarded.
- **Batch = one append.** §6.3's "one write, one metadata version, one event"
  is structurally true rather than an invariant to maintain across N row
  closures and N inserts.

It is also consistent with a core that is already pervasively versioned and
immutable for content; metadata stops being the one mutable-with-history-columns
exception.

```
metadata_log (uid, seq, ts, actor, key, op, value)

  key=a  ──set──────────set─────────────────set──▶
  key=b  ─────set──────────────del────────────────▶
  key=c  ──────────set───────────────────────────▶
                        ╎                    ╎
                   cull horizon         state(T): latest
                                        entry per key ≤ T
```

##### One table, no derived copy

**The log is the only metadata store.** No materialized current-state table, no
snapshot rows alongside the deltas — nothing that duplicates or denormalizes what
the log already holds, and therefore nothing that can drift from it.

The concern this has to answer is read cost. `GetAllMetadata` on the live bag
becomes by far the hottest metadata read once the consumers now being specified
arrive — every CMIS `getObject`, every listing entry carrying properties, csai
indexing — and naive replay would make that hot path pay for the cold path. The
load is prospective rather than current (§4.7: metadata is barely used today),
but designing for it costs nothing here, and retrofitting it later would cost the
migration this plan is timed to avoid.

It does not need a second table, because "latest entry per key" is an indexed
lookup, not a replay:

```sql
-- current bag; index on (uid, key, seq DESC) makes this one seek per key
SELECT key, value FROM (
    SELECT DISTINCT ON (key) key, value, op
    FROM metadata_log
    WHERE uid = $1
    ORDER BY key, seq DESC
) latest
WHERE op <> 'delete';
```

Cost is one index seek per distinct key — the same order as reading K rows out of
a current-state table, without the table. **State at an arbitrary instant is the
identical query** with `AND ts <= $2` added, so current and historical reads share
one code path instead of splitting into a fast path and a replay path.

> **Implementation trap:** the tombstone filter must run **outside** the
> `DISTINCT ON`, as above. Filtering `op <> 'delete'` inside the subquery picks
> the latest *non-delete* entry, so a deleted key silently reappears with its
> previous value. This is the one place the query is easy to get subtly wrong.

##### The durable log is **not** the Redis stream

Worth stating explicitly, because "event sourcing" invites the shortcut: the
`fileengine:events` Redis stream **cannot** be the source of truth. It is
trimmed (`MAXLEN ~`) and its publisher is deliberately **fail-open** — on outage
or outbox overflow it drops events and increments a counter
(EVENT_CONTRACT.md §6.4). Treating it as the metadata log would turn a designed,
tolerable notification loss into silent data loss.

The durable log lives in PostgreSQL alongside the rest of the tenant schema; the
Redis event is a *notification derived from* a committed log append.

##### Compaction and culling — the only destructive operation

This is where §1.1's governing principle becomes code. Pre-cut metadata history
is lost exactly as pre-cut payload versions are lost — same cut, same permission,
same irreversibility — and no other path in the core may destroy committed data.

That has teeth here: `delete_metadata` must write an entry with
`op = 'delete'`. Implementing it as a `DELETE` against the log would make it a
second destructive path, breaking the principle and silently discarding history
that `GetAllMetadataAt` is required to reconstruct.

Consequently culling is **explicit, permissioned and audited**, exactly as
`PurgeOldVersions` is — not a background retention sweep driven by config. It
takes a caller, a target and an audit record, and it is gated by
**`CULL_VERSIONS`**, the existing permission that already exists precisely
because it destroys data irreversibly and therefore must be granted explicitly
rather than arriving inside a bundle like "full control".


##### Collapsing history into a new initial state

**The cut time is not chosen — it is derived.** Metadata culling mirrors the
payload cull:

```
T_cut = timestamp of the OLDEST RETAINED content version
```

Culling then **constructs** a base state at that instant and makes it the log's
new starting point:

1. Fold the log at `T_cut` — for every key live at that instant, take its latest
   value; keys whose latest entry is a tombstone simply do not appear.
2. Write that folded set as synthesized entries stamped `T_cut`, marked
   `op = 'base'`.
3. Delete every entry below `T_cut`.

The log now begins at `T_cut`, and nothing before it exists.

> **Why the payload cut determines the metadata cut.** §6.1's whole point is that
> you can ask for the metadata sequence covering a content version's window. If
> metadata were culled to an *independent* horizon, a cut later than the oldest
> retained version would leave surviving content versions whose metadata windows
> are no longer reconstructable — the exact query the two-series model exists to
> answer, broken by a retention setting. A cut *earlier* than the oldest retained
> version would free nothing worth having.
>
> So the only sound metadata cut is the oldest retained version's timestamp. It
> is fully determined by the payload cull, which is why it is derived rather than
> parameterised, and why there is no independent metadata-retention knob to get
> wrong.

The resulting invariant is worth stating plainly: **every retained content
version has fully reconstructable metadata.** For the oldest retained version,
state at the start of its window is carried by the base at `T_cut` and every
change during the window is at or after `T_cut`, so it survives; newer versions
follow trivially.

> **Why construct a base rather than keep the surviving rows.** The obvious
> cheaper move — keep each key's latest pre-horizon entry at its *original*
> timestamp and delete the rest — is wrong, and quietly so. Different keys would
> retain different original timestamps, so `GetAllMetadataAt(T)` for a `T` below
> the cut would return a bag containing the keys whose survivor predates `T` and
> silently omitting the rest: a plausible, well-formed, **incomplete** answer.
> Collapsing to a single initial state at `T_cut` makes the boundary sharp, so a
> query below it can fail honestly instead.

Three consequences, each requiring code:

- **Queries below the cut are an explicit error.** `GetAllMetadataAt(T < T_cut)`
  and `ListMetadataVersions` over a window entirely below the cut return "history
  culled to `T_cut`" — never an empty or partial result. A window straddling the
  cut returns what exists and flags the truncation. Same "no silent empties"
  principle as §6.5.1.
- **Base entries must be distinguishable from writes.** They are a starting
  point, not changes. Left unmarked, `ListMetadataVersions` would report a mass
  "every key changed at `T_cut`" that never happened, and consumers reading the
  log as a change sequence would act on it.
- **Culling emits no `metadata.changed`.** Folding history is not a metadata
  change — nothing about the object's current state moved. Emitting would put a
  fabricated change for every key of every culled object onto
  `fileengine:events`, driving a csai re-index storm and flooding the CMIS change
  log (`cmis/SPECIFICATION.md` §15) with updates no client can act on. The cull
  is audited instead, which is where a destructive operation belongs.

**State is preserved, sequence is not.** After a cull the bag reconstructs
exactly at any instant at or after `T_cut`; the individual changes below it are
**gone, irrecoverably, exactly as the culled payload versions are** (§1.1). That
is the trade being bought, and it should be documented in the proto rather than
discovered by a caller.

##### Schema change, not migration

There is nothing to migrate: no deployment makes significant use of metadata, so
the existing rows are not worth carrying across (§4.7). The old `metadata` table
is dropped and `metadata_log` created in its place; every object simply starts
with an empty log.

Had the rows needed preserving, the mechanism was available and cheap — the
`"current"`-sentinel rows are structurally an initial base state, so writing them
as `op = 'base'` entries at `t0` would have produced exactly what a cull produces,
readable by the same code path. It is recorded here because it is the fallback if
some deployment turns out to hold metadata worth keeping, but it is **not** part
of the plan.

What must survive is the **API contract**, not the data: the unversioned reads and
writes keep working unchanged against an empty store, because the REST bridge and
the web UI's Metadata tab call them.

#### API surface

| RPC | Behaviour |
|---|---|
| `ListMetadataVersions(uid, content_version?)` | The metadata-version sequence — a range scan over the log. With `content_version` set, restricted to that version's window: **the query this decision requires**, and the drill-down behind §6.5.1's flags. Returns `{timestamp, changed_by, keys_set[], keys_deleted[]}` per entry; values are fetched via `GetAllMetadataAt` |
| `GetAllMetadataAt(uid, timestamp)` | The bag at an arbitrary instant — latest entry per key at or before `timestamp`; explicit error below the current base (§6.1) |
| `GetMetadataForVersion` / `GetAllMetadataForVersion` | **Redefined:** state at the *end* of that version's window, plus `changed_in_window` / `deleted_in_window` flags (§6.5.1) |
| `GetMetadata` / `GetAllMetadata` | Unchanged semantics: the live bag — the same query with no timestamp bound |
| `PurgeOldVersions(uid, keep_count)` | **Extended, not new:** also collapses the metadata log to a base state at the oldest retained version's timestamp. `CULL_VERSIONS`, audited, emits no events (§6.5.3) |

### 6.3 Batch writes — one event, listing the keys

**Decided: a batch write emits exactly one event, whose payload lists the keys
altered.** This brings the batch API *into* scope (it was previously deferred),
because the event shape is defined in terms of it.

Consequences:
- One batch = one transaction = one **metadata version** = one event. The three
  stay aligned, which is what makes the §6.1 sequence meaningful — a bulk tagger
  applying 20 keys produces one point on the metadata timeline, not 20.
- A single-key `SetMetadata` is the degenerate case: one event, one-element list.
  Consumers get one shape, always.
- `updateProperties` in the CMIS adapter becomes atomic, closing the limitation
  recorded in `cmis/SPECIFICATION.md` §8.5.2.

### 6.4 Rendition metadata events — flagged

**Decided: yes.** Metadata events on sidecar files and other hidden children of
file entities carry `is_rendition: true`, exactly as content events on those
children already do (EVENT_CONTRACT.md §3), because consumers mostly ignore them.

This matters more for metadata than for content: out-of-band producers (markup,
csai previews, difference renditions) write rendition metadata frequently, and
unflagged events would put that traffic in front of every consumer on the stream.

### 6.5 Follow-on details

#### 6.5.1 What the `*_for_version` RPCs return — decided

With two series, "the metadata for content version V" is ambiguous: V has a
*window*, not an instant.

**Decided: return the state at the END of the window, plus flags telling the
caller whether that state is the whole story, plus an operation to get the full
series when it is not.** Cheap answer by default, honest about what it omits,
with a drill-down when the caller cares.

```
GetAllMetadataForVersion(uid, V) →
    metadata                    // state at the END of V's window
    bool changed_in_window      // metadata was written during the window
    bool deleted_in_window      // at least one key was DELETED during the window
```

- `changed_in_window` is true when one or more metadata versions fall in
  `[V.ts, next(V).ts)`. False means the end state held for the whole window and
  no drill-down is worthwhile.
- `deleted_in_window` earns a **separate** flag because deletion is the case
  end-of-window state cannot express. A modified key is at least visible in the
  end state with its new value; a deleted key is simply *absent*, and a caller
  diffing against an earlier read cannot distinguish "deleted during this
  version" from "never existed". Without this flag, end-of-window semantics would
  lose information silently — the failure mode this whole section exists to
  remove.
- Both flags are deliberately **conservative**: a key deleted and then re-set
  inside the window still sets `deleted_in_window`. The flag means "a deletion
  occurred, look closer", not "something is missing from the map".
- For the single-key `GetMetadataForVersion`, both flags are **scoped to that
  key** rather than to the whole bag, which is what makes them actionable at that
  granularity.

**Drill-down:** `ListMetadataVersions(uid, V)` returns the comprehensive change
series for the window — every metadata version in it, with timestamp, actor, and
its `keys_set` / `keys_deleted`.

The series carries keys, not values, and that is sufficient: a caller wanting the
values at any point in the window calls `GetAllMetadataAt(uid, T)` for a `T` it
picked out of the series. Keeping values out of the series stops one audit query
from returning the entire value history of a heavily-edited object. If profiling
later shows the extra round-trips dominate, add an opt-in `include_values` flag
rather than making it the default.

#### 6.5.2 Mixed batches in one event — stands

§6.3 says the payload lists the altered keys, but a batch may both set and delete,
which a single `metadata_deleted` bool cannot describe. Two repeated fields:

```cpp
    // Metadata events only (type == MetadataChanged). One event per write,
    // batch or single; a single-key write fills exactly one entry.
    std::vector<std::string> metadata_keys_set;
    std::vector<std::string> metadata_keys_deleted;
```

This supersedes the singular `metadata_key` / `metadata_deleted` sketch in §4.2.

The same split is what makes §6.5.1's `deleted_in_window` derivable: because log
entries record set and delete keys separately, the flag is a scan of the window's
entries for a non-empty `keys_deleted`, not a reconstruction that diffs states at
each end. The two decisions compose — had the payload used one list plus a flag,
deletions inside a batch would have been invisible to the window read.

#### 6.5.3 One destructive operation, covering both series

The two series are independent for *reading* and *writing*, but **not for
culling**. `PurgeOldVersions` is the single destructive operation, and it culls
both: it drops content versions below the retained set, then collapses the
metadata log to a base state at the oldest retained version's timestamp (§6.1).

| | Behaviour |
|---|---|
| Operation | `PurgeOldVersions(uid, keep_count)` — culls content **and** metadata |
| Metadata cut | Derived: oldest retained content version's timestamp. Not a parameter |
| Permission | `CULL_VERSIONS`, meaning widened from "purge old content versions" to "cull history" |
| Trigger | Explicit caller; **no background sweep** |
| Audited | Yes — it is the only destructive path in the system |
| Events | None (§6.1) |

**There is no independent `PurgeOldMetadata`.** An earlier draft of this section
proposed one, with an open question about whether it should take a `keep_count`
or a horizon. Both are wrong: any metadata cut other than the oldest retained
version's timestamp either breaks reconstruction for surviving content versions
or frees nothing. The parameter does not exist because the correct value is fully
determined, and offering the knob would only create a way to corrupt the
invariant.

The practical consequence is that metadata log growth is bounded by the same
control that already bounds version history. Growth is real for files rewritten
by automation — folder_actions rules, csai enrichment, rendition producers — and
whatever schedules `PurgeOldVersions` in a deployment now bounds both.

`CULL_VERSIONS` keeps its posture: granted explicitly, never bundled into a
"full control" role. Downstream, `cmis/SPECIFICATION.md` §12.2 already excludes
it from `cmis:all` for exactly this reason, and that exclusion now protects
metadata history too.

**Remaining detail:** a purged content version no longer exists, so
`ListMetadataVersions(uid, purged_version)` has nothing to resolve — return an
explicit "no such version" error, not an empty sequence (§6.5.1's principle).
Note this is now the *only* failure mode here: for every version that still
exists, the metadata sequence is guaranteed reconstructable.

---

## 7. Acceptance

1. A metadata set and a metadata delete each produce exactly one
   `metadata.changed` on `fileengine:events`, carrying the correct
   `metadata_keys_set` / `metadata_keys_deleted` and the standard envelope
   (`tenant`, `actor`, `file_uid`, `event_id`).
2. **A batch write of N keys produces exactly one event** listing all N, and
   creates exactly one metadata version (§6.3). A mixed set-and-delete batch
   populates both lists (§6.5.2).
3. Emission is fail-open: with the broker down, `SetMetadata` still succeeds.
4. A metadata write on a rendition child carries `is_rendition: true` (§6.4).
5. EVENT_CONTRACT.md §2/§3/§6 updated; `schema` still `1`.
6. **`ListMetadataVersions(uid, content_version)` returns the metadata versions
   falling within that content version's window**, and the sequence is correct
   when metadata changed several times while one content version was current
   (§6.1) — the case a snapshot-per-version model would have lost.
7. `GetMetadataForVersion` returns real values under the §6.5.1 semantics — state
   at the end of the version's window — or an explicit error; **not** a silent
   "not found". The window flags must be correct and lossless:
   - For a window containing a set, a modify and a delete: the map is the end
     state, `changed_in_window` and `deleted_in_window` are both true, and
     `ListMetadataVersions(uid, V)` lists every one of those changes.
   - Specifically tested: a key **set then deleted inside one window** is absent
     from the map and still discoverable via the flag plus drill-down — the
     information end-of-window semantics would otherwise lose silently.
   - A window with **no** metadata writes returns `changed_in_window = false`,
     and the single-key variant scopes both flags to its own key.
8. A regression test asserting (7), because the current behaviour is
   indistinguishable from "this file has no metadata".
9. The unversioned API is **contract-compatible across the schema change**:
   `GetMetadata`, `GetAllMetadata`, `SetMetadata` and `DeleteMetadata` behave
   correctly against an empty store — an absent key reads as absent, not as an
   error — and a set-then-get round-trip works on a freshly replaced schema. The
   *data* is expected not to survive (§4.7); the *endpoints* must, since the REST
   bridge and the web UI's Metadata tab depend on them.
10. **Reconstruction is exact:** for a file with a long metadata history,
    `GetAllMetadataAt(uid, T)` equals the bag that `GetAllMetadata` returned at
    time `T`, for arbitrary `T`.
11. **A delete is a tombstone, not a removal** — after `DeleteMetadata`, the key
    is absent from `GetAllMetadata` but `GetAllMetadataAt` before the delete
    still returns it. Guards §1.1: culling is the only destructive operation, so
    no other call may make committed data unreachable.
12. **Culling collapses to a new initial state:** every
    `GetAllMetadataAt(uid, T ≥ T_cut)` answer is byte-identical to its pre-cull
    value, and every query strictly below `T_cut` returns an explicit
    "history culled" error — never an empty or partial bag.
13. **The metadata cut mirrors the payload cut:** after
    `PurgeOldVersions(uid, keep_count)`, the metadata base is stamped at exactly
    the oldest **retained** content version's timestamp — and consequently
    `ListMetadataVersions(uid, V)` is complete for **every** surviving version
    `V`, including the oldest. This is the invariant the coupling exists to
    protect, so it is tested against a file with several versions and metadata
    churn inside each window.
14. **Culling emits no `metadata.changed`**, and base entries are not reported as
    changes by `ListMetadataVersions`. A cull of an object with 50 keys puts zero
    events on `fileengine:events`.
14. A hot-path guard: `GetAllMetadata` on an object with thousands of log entries
    costs one index seek per live key and performs no full-log scan (§6.1) — with
    no second table involved.
