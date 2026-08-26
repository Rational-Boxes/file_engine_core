# Proposal: `metadata.changed` events + fixing versioned metadata

**Status:** Draft / research — for review
**Branch:** `design/metadata-change-events` (file_engine_core)
**Author:** finding surfaced 2026-08-26 while specifying the CMIS adapter (`cmis/SPECIFICATION.md`)
**Scope (cross-repo):** `file_engine_core` (publisher + metadata layer), `convert_search_ai` (contract doc + consumer), `folder_actions` (consumer), `cmis` (consumer, planned)

> This is a design proposal, not an implementation. Two defects are documented
> below; both were found by reading the code, and each is reproducible from the
> line references given. **§6 Open decisions** needs sign-off before any work.

---

## 1. Summary

Two independent defects in the metadata layer, found together:

**A — Metadata mutations emit no event.** `FileSystem::set_metadata` and
`delete_metadata` publish nothing at all — not even `file.updated`. Every
metadata change is invisible to `fileengine:events`, and therefore to every
consumer on the platform.

**B — Versioned metadata always misses.** The four unversioned metadata
functions hard-code the sentinel string `"current"` as the version, while the two
`*_for_version` variants pass the caller's real timestamp through. The versioned
readers therefore query a `(uid, version_timestamp, key)` tuple that **nothing
ever writes**, and always return "not found".

Defect A is already acknowledged at the publisher level —
`convert_search_ai/design_documents/EVENT_CONTRACT.md` §6 lists `metadata.changed`
under *"Not yet in the publisher (tracked in the core plan)"*. What is new is
(i) that it now has **three** blocked consumers rather than one, (ii) that the
consumer side was never built either (§3.2), and (iii) defect B, which appears
to be undocumented anywhere.

---

## 2. Defect A — no metadata event

### 2.1 Evidence

`core/src/filesystem.cpp`, `FileSystem::set_metadata` in full:

```cpp
auto context = get_tenant_context(tenant);
if (!context || !context->db) { /* err */ }
auto perm_result = validate_user_permissions(file_uid, user, roles,
                                             static_cast<int>(Permission::WRITE), tenant);
if (!perm_result.success || !perm_result.value) { /* err */ }
std::string version_timestamp = "current"; // Would use actual current version in practice
auto db_result = context->db->set_metadata(file_uid, version_timestamp, key, value, tenant);
return db_result;                          // <-- no emit_fs_event()
```

Compare the established pattern elsewhere in the same file — `put`,
`restore_to_version` and the rest all end with:

```cpp
emit_fs_event(tenant, FileEventType::FileUpdated, file_uid, user);
```

`delete_metadata` has the same omission.

### 2.2 Current event vocabulary

`core/include/fileengine/event.h`, `enum class FileEventType`:

`DirCreated`, `DirDeleted`, `FileCreated`, `FileUpdated`, `FileMoved`,
`FileRenamed`, `FileDeleted`, `FileRestored`, `AclChanged`, `RoleAssigned`,
`RoleMemberRemoved`, `RoleDeleted`.

There is no metadata member. The contract doc's §3 table has no `metadata.*` row.

### 2.3 Why it matters — three blocked consumers

| Consumer | What it cannot do today |
|---|---|
| **convert_search_ai** | Synchronise the search index against metadata changes. Compounded by §3.2 — csai does not index the metadata bag at all |
| **cmis** (planned) | Emit a CMIS `updated` change-log event for a property-only edit. CMIS clients (CmisSync) sync primarily off `getContentChanges`; without this, property edits **never reach the desktop** until the client's ~6-hourly full crawl |
| **folder_actions** | Trigger any rule on a metadata change. Every one of its plug-in actions is driven off `fileengine:events` |

A single missing event blocks three services. That is the argument for fixing it
in the publisher rather than working around it in each consumer — a CMIS-side
workaround (poll metadata during crawl) would paper over a platform gap in one
adapter and leave the other two unserved.

---

## 3. Defect B — versioned metadata is a stub

### 3.1 Evidence

In `core/src/filesystem.cpp`:

| Function | Version argument passed to the DB layer |
|---|---|
| `set_metadata` | `"current"` (literal) |
| `get_metadata` | `"current"` (literal) |
| `get_all_metadata` | `"current"` (literal) |
| `delete_metadata` | `"current"` (literal) |
| `get_metadata_for_version` | **the caller's `version_timestamp`** |
| `get_all_metadata_for_version` | **the caller's `version_timestamp`** |

All four unversioned functions carry the same comment: `// Would use actual
current version in practice`.

The writers are internally consistent — everything lands under the sentinel — so
the *unversioned* API works correctly. But nothing ever writes a row under a real
version timestamp, so:

- `GetMetadataForVersion` and `GetAllMetadataForVersion` **always** return
  "Metadata key not found", for every input.
- Metadata does not participate in versioning: restoring an old version does not
  restore the metadata that was current when that version was written.

Both RPCs are advertised in `proto/fileservice.proto` and are reachable from every
SDK and bridge. They are a stub that fails silently rather than a documented
limitation.

### 3.2 The consumer-side half

Independently: **convert_search_ai has never indexed the metadata bag.** Its only
`metadata` references are SSRF guards and the `xeokit3d` plugin reading IFC
attributes out of model files. It indexes content only.

So shipping `metadata.changed` alone would not make metadata searchable — csai
needs a metadata-indexing path before it has anything to synchronise. Both halves
are needed; the event is the prerequisite, not the whole fix.

---

## 4. Proposal

### 4.1 Add the event type

`core/include/fileengine/event.h`:

```cpp
enum class FileEventType {
    …
    AclChanged,
    MetadataChanged,    // a metadata key was set or deleted on file_uid
    RoleAssigned,
    …
};
```

Contract string `metadata.changed`, matching the name already reserved in
EVENT_CONTRACT.md §6. Update `to_string()` in `core/src/event.cpp`.

### 4.2 Carry the changed key

`FileEvent` already carries type-specific fields — `principal` + `permissions`
for `AclChanged`, `role` + `member` for the `Role*` events. Follow that precedent:

```cpp
    // Metadata events only (type == MetadataChanged): the key that changed and
    // whether it was removed. Consumers use these to update one field rather
    // than re-fetching the whole bag.
    std::string   metadata_key;
    bool          metadata_deleted = false;
```

**This is the load-bearing detail.** A bare "something about this object's
metadata changed" forces every consumer to re-fetch the entire bag per event.
The reconcile sweep has already been bitten by exactly this shape of
under-specified payload: `file.created` carries an empty `version`, which makes
version-equality idempotency double-run on every new file. Do not repeat it.

One event per key mutation, not one per batch — matching the current one-key-per-call
API (§6.3 revisits this).

### 4.3 Emit at the write sites

Two lines, following the existing pattern:

```cpp
// FileSystem::set_metadata, after a successful db_result
emit_fs_event(tenant, FileEventType::MetadataChanged, file_uid, user, key, /*deleted=*/false);

// FileSystem::delete_metadata, after a successful db_result
emit_fs_event(tenant, FileEventType::MetadataChanged, file_uid, user, key, /*deleted=*/true);
```

`emit_fs_event` needs an overload carrying the key. Emission stays fail-open and
async on the existing bounded outbox (EVENT_CONTRACT.md §6.4) — a metadata write
must never block or roll back on a broker outage.

### 4.4 Update the shared contract

`convert_search_ai/design_documents/EVENT_CONTRACT.md` is the cross-service source
of truth and must be updated in the same change:

- §3 event-type table — add the `metadata.changed` row.
- §2 envelope — document `metadata_key` / `metadata_deleted` alongside the
  existing `acl.changed` and `role.*` extra fields.
- §6 — remove `metadata.changed` from the "not yet in the publisher" list.
- `schema` stays at `1`: these are additive fields (§8).

### 4.5 Fix versioned metadata (defect B)

Options in §6.2. Whichever is chosen, the `*_for_version` RPCs must stop
silently returning "not found" — either they work, or they return an explicit
`unimplemented` error and the proto comments say so.

---

## 5. Non-goals

- Building csai's metadata indexing (§3.2). That is csai-side work this proposal
  unblocks, not part of it.
- The CMIS adapter's own change log — it consumes this event; it does not
  motivate changing its shape beyond §4.2.
- Making metadata writes create a new file **version**. Considered and rejected
  in §6.1.

---

## 6. Open decisions

### 6.1 Should a metadata write bump `FileInfo.version`?

**Recommend no.** A property edit is not a new content revision, and versioning
every tag change would inflate version history, trigger `file.updated`
re-extraction and re-embedding in csai for unchanged content, and interact badly
with `PurgeOldVersions`.

But note the consequence for consumers that use the version as a change token:
the CMIS adapter maps `cmis:changeToken` to `FileInfo.version`, so with this
recommendation a property-only update cannot be guarded by CMIS optimistic
locking. The adapter can maintain its own property-level token instead — but the
decision belongs here, not there.

### 6.2 How should versioned metadata be fixed?

1. **Write against the real current version.** Replace the `"current"` sentinel
   with the actual current version timestamp at write time. Correct, and makes
   metadata genuinely version-scoped — but it is a **data migration**: existing
   rows sit under `"current"` and would become invisible to the unversioned
   readers unless backfilled or read with a fallback.
2. **Snapshot on version creation.** Keep writes under `"current"`; copy the bag
   to the new version timestamp when a version is written. Preserves current
   behaviour, gives correct history going forward, no migration of live reads.
3. **Withdraw the feature.** Return an explicit error from the `*_for_version`
   RPCs and document metadata as file-scoped, not version-scoped.

**Recommend (2)** — it is the only option that fixes history without a migration
of data that services are actively reading. (3) is the honest fallback if
version-scoped metadata is not wanted at all.

### 6.3 One event per key, or a batch write API?

The current API is one key per call, so N properties = N round-trips and N
events, non-atomically. A batch `SetMetadata` (multi-key, one transaction, one
event) would fix atomicity for CMIS `updateProperties` and reduce event volume
for bulk taggers.

Additive and small, but it is a proto change with several consumers.
**Out of scope for this proposal; raise separately if wanted.**

### 6.4 Should renditions emit metadata events?

Rendition children are flagged `is_rendition: true` (EVENT_CONTRACT.md §3).
Metadata writes on a rendition should presumably carry the same flag so
consumers can ignore them, but confirm — out-of-band rendition producers
(markup, csai previews) write metadata frequently, and unflagged events would
add noise to every consumer.

---

## 7. Acceptance

1. A metadata set and a metadata delete each produce exactly one
   `metadata.changed` on `fileengine:events`, carrying `metadata_key`, the
   correct `metadata_deleted`, and the standard envelope (`tenant`, `actor`,
   `file_uid`, `event_id`).
2. Emission is fail-open: with the broker down, `SetMetadata` still succeeds.
3. EVENT_CONTRACT.md §2/§3/§6 updated; `schema` still `1`.
4. Per §6.2, either `GetMetadataForVersion` returns real per-version values, or
   it returns an explicit unimplemented error — **not** a silent "not found".
5. A regression test asserting (4), because the current behaviour is
   indistinguishable from "this file has no metadata".
