# Proposal: stop compressing payloads that do not compress

**Status:** Design proposal — for review, nothing implemented
**Scope:** `file_engine_core` only. No proto change, no ACL change, no API
change. One additive schema migration, in the style the versions table already
uses.
> **Superseded as the contract by [`storage_pipeline.md`](storage_pipeline.md).**
> That document is the specification — the normative requirements, the stages and
> the acceptance criteria. This one remains as the **rationale**: how the
> conclusions were reached, what was measured, what was rejected and why. Read it
> for the argument; implement from the specification. Where they disagree, the
> specification wins.

**Companion:** `PROPOSAL_byte_range_reads.md` §7.1, which needs this and where
the idea first appeared. This document stands alone: the optimisation is worth
doing whether or not byte ranges are ever built.

---

## Summary

The core compresses every stored payload with zlib when compression is enabled
for the deployment (`core/src/filesystem.cpp:460`), and decompresses on every
read (`:722`, `:1008`). For a large and growing share of what this platform
stores — video, images, Office documents, PDFs, archives, and the renditions the
conversion pipeline produces — **that work returns essentially nothing**, because
the payload is already entropy-coded. It is CPU spent once on write and again on
*every* read, to save a percent or two.

Three things follow, in order of importance:

1. **R1 is a prerequisite and also a bug fix.** The read path decides whether to
   decompress by asking the *current* configuration, not by recording what was
   actually done to the blob. That is already unsafe today, and it makes a mixed
   corpus — which is exactly what this proposal creates — impossible. **§3.1.**
2. **R2 decides per payload, empirically**, not from a type table — because the
   core has no concept of content type and should not gain one. **§5.**
3. The saving is real but should be **measured before it is claimed**, on the
   deployment, with the method in §7.

---

## 1. Why this is worth doing at all

The read path is the hot one. A stored file is written once and read many
times — by the SPA's preview, by WebDAV sync clients, by `convert_search_ai`'s
conversion and reconcile sweeps, by `difference_service`, by every download. Each
read of a compressed blob pays an inflate.

Where the payload is already compressed, that inflate is pure overhead on both
sides:

| Stored content | zlib typically saves |
|---|---|
| Video (MP4, WebM, MOV), audio (MP3, Opus) | ~0% |
| Images (JPEG, PNG, WebP, GIF, HEIC) | ~0% |
| `.docx` / `.xlsx` / `.pptx` / ODF | **~0% — these are ZIP containers** |
| Archives (zip, gz, xz, 7z), already-encrypted blobs | ~0%, sometimes negative |
| The pipeline's own renditions (WebM, PNG thumbnails, XKT) | ~0% |
| **IFC, CityJSON, OBJ, SQL, logs, JSON, plain text** | **70–90% — must keep compressing** |

The last row is why this is a *selective* change rather than a switch. This
platform's BIM content is enormous ASCII, and turning compression off wholesale
would be a serious regression in exactly the corpus that benefits most.

A second, quieter benefit: `PROPOSAL_byte_range_reads.md` §3.1 records that a
zlib stream has no seek points, so a compressed version can never support a cheap
ranged read. Not compressing what does not compress makes those files randomly
accessible as a side effect — the media case in `MEDIA_SHARE.md` is the immediate
beneficiary, and it needs no format change to get there.

---

## 2. Three problems, and the order they must be fixed in

| | Problem | Severity |
|---|---|---|
| **P1** | The read path infers the transform from current config, not from what was written | **latent data-loss bug today**, and a hard blocker for anything else here |
| **P2** | Incompressible payloads are compressed anyway | the performance cost this proposal is about |
| **P3** | Compression is a deployment-wide boolean, with no per-content or per-tenant nuance | scope note only; §9-Q3 |

**P1 must be fixed first and independently.** It is not merely a prerequisite —
it is worth landing on its own.

---

## 3. P1 — the core does not record what it did to the bytes

### 3.1 What the code does

On write (`filesystem.cpp:460`, `:472`):

```cpp
if (context->storage && context->storage->is_compression_enabled()) { ...compress... }
if (context->storage && context->storage->is_encryption_enabled())  { ...encrypt...  }
```

On read (`filesystem.cpp:705`, `:722`; and again in the streaming path at
`:1008`):

```cpp
const bool do_compress = context->storage->is_compression_enabled();
const bool do_encrypt  = context->storage->is_encryption_enabled();
```

**The read asks the same question the write asked, at a different time.** Nothing
on the version row, and nothing in the blob, records what was actually applied.
The two agree only for as long as the configuration never changes.

### 3.2 What happens when it changes

| Change | Result |
|---|---|
| compression **on → off** | reads skip the inflate and hand the **raw zlib stream to the client as the file's content**. No error. **Silent corruption.** |
| compression **off → on** | reads attempt to inflate plaintext, zlib fails, request errors with *"Failed to decompress data"*. Loud, and recoverable. |
| encryption **on → off** | reads return `[IV ‖ ciphertext ‖ tag]` as the file. No error. **Silent.** |
| encryption **off → on** | GCM tag check fails. Loud. |

One direction fails loudly and one corrupts silently — **and the silent direction
is the one an operator takes to improve performance**, which is precisely what
this proposal invites them to do.

> **Operational warning, valid today and independent of everything below:** do
> not turn `compression` (or `encryption`) off in a deployment that has already
> stored data with it on. Every existing file becomes unreadable-as-written, with
> no error to say so. Until R1 lands, these flags are **set-once at
> provisioning**, not tunables.

This should be checked against the live deployment's history before anything
else in this document is scheduled — if either flag has ever been flipped, there
may already be affected versions.

### 3.3 R1 — record the transform on the version row

Two additive columns, using the `ADD COLUMN IF NOT EXISTS` migration pattern the
versions table already uses for `revised_by` (`core/src/database.cpp:4204`):

```sql
ALTER TABLE "<tenant>".versions
  ADD COLUMN IF NOT EXISTS compressed BOOLEAN NOT NULL DEFAULT true;
ALTER TABLE "<tenant>".versions
  ADD COLUMN IF NOT EXISTS encrypted  BOOLEAN NOT NULL DEFAULT true;
```

**The defaults are the whole design of the migration, and they are wrong on
purpose in the safe direction.** A backfill cannot know what was applied to a row
written before the column existed, so it must guess — and the only safe guess is
*whatever the deployment is configured for right now*, because that is the
assumption every existing read is already making and therefore the assumption
under which the corpus is currently readable.

So the migration is: **add the columns with defaults taken from the deployment's
current flags at migration time**, not from a literal. A deployment running
compression+encryption backfills `true/true`; one running neither backfills
`false/false`. Reads then behave exactly as they do today for every pre-existing
version, and differ only for versions written afterwards. Nothing changes
meaning; the system merely starts writing down what it was already assuming.

The read path then becomes:

```cpp
const bool do_compress = version_row.compressed;   // what was DONE, not what is configured
const bool do_encrypt  = version_row.encrypted;
```

Consequences worth stating:

- **The flags become genuinely tunable.** Turning compression off stops being a
  corruption event and becomes a decision that applies to new writes.
- **Restore rehearsals get safer.** A restored blob carries its own truth on the
  version row rather than depending on the target instance's configuration
  matching the source's — which is currently an unwritten requirement of any
  restore.
- **Key rotation gets a hook** it does not have today: once `encrypted` is a
  per-version fact, a `key_id` column beside it is the natural next step. Not
  proposed here, but the column is cheap and the shape is the same.
- `PROPOSAL_byte_range_reads.md` §8's v2 block format supersedes both columns
  with a self-describing header. The columns remain correct for v1 blobs, which
  will exist for years, so this is not wasted work.

---

## 4. Decisions proposed

| Topic | Decision |
|---|---|
| Prerequisite | **R1 first, on its own** — record per version what was applied (§3.3). Nothing else here is safe before it. |
| How to decide | **Empirically, from the bytes.** Compress a sample; keep the result only if it actually saved something (§5.2). |
| Type tables | **No MIME in the core.** `FileInfo` has no content type and deliberately so; the core is trusted-upstream and leaves interpretation to the doors. A magic-byte short-circuit for a handful of signatures is optional and secondary (§5.1). |
| Threshold | Keep the compressed form only if it saves **≥ 10%** of the sample. Below that the inflate on every read is not paid for. |
| Scope of the decision | Per **version**, at write time. Never re-evaluated, because the answer is recorded and the bytes do not change. |
| Failure mode | Any doubt ⇒ **compress**. A wrong "compress" costs CPU; a wrong "skip" costs storage. Both are recoverable; neither is corruption, because R1 records what happened. |

---

## 5. R2 — deciding per payload

### 5.1 Why not a type list

The obvious implementation is an extension or MIME allowlist. It is the wrong
tool here for three reasons:

1. **The core has no content type.** `FileInfo` (`core/include/fileengine/types.h:43`)
   carries `name`, `size`, ownership and versioning — no MIME field. Adding one
   would introduce a concept the core has deliberately avoided, and would make
   the storage layer's behaviour depend on a value a door supplied.
2. **A name is not a format.** This platform has already been bitten by exactly
   this: MIME detection flattening `.md` and other conventions to `text/plain`,
   and a curated extension list having to outrank a library's guess. A storage
   optimisation should not inherit that class of bug.
3. **A table needs maintaining and will be wrong.** Every new format — the next
   BIM export, a new rendition kind, whatever a customer uploads — arrives
   unclassified, and the default is then either "compress everything unknown"
   (which is today) or a silent regression.

A **magic-byte short-circuit** is still worth having as a cheap fast path: the
first few bytes of a file identify ZIP (`PK\x03\x04`), gzip (`\x1f\x8b`), PNG,
JPEG, WebM/Matroska, MP4/`ftyp`, and a handful of others with certainty and no
work. It avoids even the trial deflate for the common cases. But it is an
optimisation *of* the mechanism in §5.2, never a replacement for it, and it must
be a small, closed list of unambiguous signatures — not a format registry
growing inside the storage layer.

### 5.2 The trial deflate

The mechanism is one deflate of a sample:

- Take the first **128 KiB** of the payload (or all of it, if smaller).
- Deflate at a **low level** (zlib level 1) — this is a measurement, not the
  final compression, and level 1 predicts compressibility well at a fraction of
  the cost.
- If it saved **< 10%**, store the payload **uncompressed** and record
  `compressed = false`.
- Otherwise compress normally, at the configured level, and record
  `compressed = true`.

Properties that make this the right mechanism:

- **Self-correcting.** It is right about formats nobody has thought of, about a
  `.dat` that happens to be a ZIP, and about an encrypted blob a user uploaded.
- **Cheap.** One level-1 deflate of ≤128 KiB, against a write that is already
  doing a full compress and a full encrypt. Where it says "skip", it *saves* the
  full-payload deflate and every future inflate — so on incompressible content it
  pays for itself immediately.
- **No new concepts.** No type field, no registry, no door-supplied hint.

Two honest limitations:

- **A sample is a sample.** A file whose first 128 KiB is a compressible header
  followed by an incompressible body (or the reverse) gets the wrong answer. This
  is a performance heuristic, not a correctness one, and being wrong costs what
  the system does today. For payloads above some size it is worth sampling from
  two or three offsets and taking the best case — cheap, and it removes the
  pathological case.
- **The streaming write path can only see the head.** `put_stream`
  (`filesystem.h:97`) does not have the whole payload, so the decision must be
  made from the first buffer and then committed to for the rest of the stream.
  That is acceptable — and it is another reason the decision must be *recorded*
  rather than recomputed.

### 5.3 What must keep being compressed

Worth stating as a test rather than a hope, because a regression here would be
expensive and quiet:

**IFC, CityJSON, OBJ, glTF-JSON, SVG, CSV, JSON, XML, SQL, logs and plain text
must all still be compressed.** These are the payloads where zlib earns 70–90%,
and several of them are the largest single files this platform stores. The
fixture corpus for this change must include a real IFC model, and the test must
assert not just that it was compressed but that the ratio is in the expected
band.

### 5.4 Where it goes

One function, called from both write paths:

```cpp
// Storage-layer decision: does this payload benefit from compression?
// Returns false for payloads a trial deflate shows are already compressed.
bool should_compress(const uint8_t* head, size_t head_len);
```

Called in `FileSystem::put` before the compress block (`filesystem.cpp:460`) and
from the streaming writer's first buffer. Its result goes into the version row's
`compressed` column in the same transaction that records the version — the two
must not be able to disagree.

---

## 6. What it costs and what it saves

**Saves, per read of an incompressible payload:** one full-payload inflate,
every time, forever. This is the dominant term, because reads outnumber writes
and because the pipeline services read the same files repeatedly.

**Saves, per write:** one full-payload deflate, minus the trial deflate of
≤128 KiB.

**Costs, per write of a compressible payload:** one level-1 deflate of ≤128 KiB —
against a full deflate of the whole payload it is noise.

**Storage:** unchanged for compressible content; for incompressible content
storage goes *down* slightly, because zlib framing on incompressible data adds
~0.03% rather than removing anything.

**A second-order effect worth naming:** the object-store sync
(`IStorage::sync_to_object_store`) and the offsite backup path move stored bytes,
not plaintext. Nothing there changes size meaningfully, but the CPU on the
restore path drops for the same reason the read path's does.

---

## 7. Measure before claiming

The numbers above are the expected shape, not a measurement of this deployment,
and this document should not be implemented on the strength of them. The existing
read-path work (`PROPOSAL_read_path_performance.md`) establishes the method and
the honesty standard: that investigation ruled out CSAI conversion as a CPU
source precisely because `podman stats` and `ps` both pointed at it and cgroup
accounting did not.

What to measure, on the live tenant:

1. **Corpus composition by stored bytes** — how much of what is stored is already
   compressed. If it is 5%, this optimisation is not worth scheduling; if it is
   60%, it is.
2. **Actual zlib ratio achieved per format**, from a sample of real stored
   versions — the table in §1 is from general knowledge, not from this corpus.
3. **CPU attributable to deflate/inflate**, via per-container cgroup `usage_usec`
   deltas over a fixed window during a representative read load, not via `ps`.

If (1) is small, the right outcome of this document is R1 alone — which is a bug
fix and should land regardless.

---

## 8. Testing

- **R1 first:** a version written with compression on, then read with the
  deployment flag flipped off, returns the **correct plaintext** — the silent
  corruption case from §3.2, asserted directly. Same for encryption.
- The migration backfills from the deployment's current flags, and every
  pre-existing version reads identically before and after.
- An incompressible payload (a real WebM and a real `.docx`) is stored
  uncompressed, and round-trips byte-for-byte.
- **A real IFC model is still compressed**, with a ratio in the expected band
  (§5.3).
- A mixed corpus reads correctly: compressed and uncompressed versions **of the
  same file** coexist and both round-trip.
- The streaming write path makes the decision from its first buffer and applies
  it consistently to the whole payload.
- A payload smaller than the sample window is handled (no out-of-range read).
- Empty payloads keep today's behaviour exactly.

---

## 9. Milestones

1. **C0 — R1: record the transform.** The two columns, the flags-at-migration-time
   backfill, both read paths switched from config to the version row, and the
   §8 corruption test. **Independent, and a bug fix — worth landing on its own
   regardless of the rest of this document.**
2. **C1 — measure** (§7). A day's work, and it decides whether C2 is scheduled.
3. **C2 — R2: the trial deflate.** `should_compress`, wired into `put` and the
   streaming writer, with the IFC regression test.
4. **C3 — the magic-byte fast path** (§5.1), if C1 shows the trial deflate's cost
   is worth removing for the common cases. Optional.

C0 unblocks the ranged-read proposal's tier 2, which needs uncompressed media to
be seekable — but C0 is worth doing even if that proposal is rejected entirely.

---

## 10. Open questions

**Q1 — Has either flag ever been flipped on the live deployment?**
**Answered 2026-09-28: no — compression and encryption have both been on since
launch.** C0 is therefore a prophylactic rather than a repair, and
`storage_pipeline.md` §7.2.1 records the two cheap checks that confirm it against
the bytes rather than against recollection. The original wording follows.

*(Superseded.)* (§3.2.) This
is a question about history, not design, and it should be answered before C0 is
scheduled — if the answer is yes, there may be existing affected versions and C0
becomes a repair rather than a prophylactic.

**Q2 — Sample size and threshold.** 128 KiB and 10% are reasoned, not measured.
C1 should confirm them against the real corpus.

**Q3 — Should compression become per-tenant or per-folder policy?** P3. A tenant
storing only media might want it off entirely; one storing only BIM wants it
always on. The per-version column makes this expressible; whether it should be
*exposed* is a product question this document does not open.

**Q4 — Should `key_id` join `encrypted` on the version row?** Out of scope, but
the column is cheap and it is the prerequisite for ever rotating the encryption
key without re-writing every blob. Worth deciding while the migration is being
written rather than adding a second one later.
