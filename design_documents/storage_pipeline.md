# The storage pipeline — what is applied to stored bytes, and reading part of them

**Status:** Specification — implementation-ready, staged. Nothing implemented yet.
**Scope:** `file_engine_core`. One additive schema migration, two additive proto
fields, two additions each to `IStorage` and `IObjectStore`, and a new on-disk
storage format (§6) that coexists with the existing one rather than replacing
it. No ACL change, no new permission, no new capability, no change to the
tenancy model, and nothing already stored is rewritten.
**Consumers unblocked:** `http_bridge`, `webdav_bridge`, `share_service`,
`convert_search_ai`, `python_interface`.

> **Rationale and analysis live in two companion proposals** —
> [`PROPOSAL_selective_compression.md`](PROPOSAL_selective_compression.md) and
> [`PROPOSAL_byte_range_reads.md`](PROPOSAL_byte_range_reads.md). They record how
> these conclusions were reached, what was rejected, and why. **This document is
> the contract**: what must be true, in what order, and how it is verified. Where
> the two disagree, this one wins.

---

## 1. Why this is core work, and why it comes first

Three things were found while specifying an unrelated feature. Two are defects in
the core's storage pipeline that exist today and affect every deployment; the
third is a missing capability that three separate doors work around.

| | | Kind |
|---|---|---|
| **A** | The core does not record which transforms it applied to a stored version — it re-derives them from current configuration at read time | **latent silent-corruption defect** |
| **B** | Every payload is compressed, including the majority that cannot be compressed, costing CPU on write and on **every** read | performance defect |
| **C** | There is no way to read bytes *m..n* of a version without reading, decrypting, decompressing and transmitting bytes *0..m-1* | missing capability |

They are one piece of work because they are one mechanism. **A** must be fixed
before **B** is possible, because **B** deliberately creates the mixed corpus
that **A** cannot survive. **B** is what makes **C** cheap for the content that
most needs it, because a zlib stream has no seek points. Doing them separately
means doing **A** twice.

**This is a prerequisite, not a dependency of convenience.** Any feature serving
media, honouring HTTP `Range`, or reading a window out of a large file is
building on ground that is currently unsound in a way that has nothing to do with
that feature. The media-share work
(`convert_search_ai/design_documents/MEDIA_SHARE.md`) is the caller that surfaced
it; it is not the reason it matters.

**It is also a general optimisation on its own terms.** The read path is the hot
one on this platform — every preview, every WebDAV sync, every conversion sweep,
every download — and stage S2 removes work from it that returns nothing. That
stands whether or not anything ever asks for a byte range.

---

## 2. The pipeline as it is

A version is written **compress-then-encrypt** and read **decrypt-then-decompress**:

```
plaintext ──zlib deflate──> compressed ──AES-256-GCM──> [ IV(12) ‖ ciphertext ‖ tag(16) ]
```

- Compression: `CompressStream` / `DecompressStream`, plain zlib
  (`core/include/fileengine/crypto_utils.h:39`, `:53`).
- Encryption: `EncryptStream` / `DecryptStream`, AES-256-GCM, tag verified at
  `finish()` (`crypto_utils.h:66`, `:83`).
- Write applies them at `core/src/filesystem.cpp:460` and `:472`.
- Read applies the inverse at `:705` / `:722` (whole-buffer `get`) and `:1008`
  (streaming `get_stream`).
- `get_stream` funnels every emitted byte through one sink bounded to
  `kMaxStreamChunkBytes` = 1 MiB (`filesystem.cpp:878`, `:892`), and aborts the
  read when that sink returns false (`:897`).
- A version not present locally is restored **whole** from the object store
  before anything is served (`:956`).

Two properties of this pipeline govern everything below, and neither is an
implementation gap — both are what the formats are:

1. **A zlib stream has no seek points.** Plaintext byte *m* cannot be located
   without inflating everything before it.
2. **A GCM tag covers the whole object.** GCM is CTR underneath, so ciphertext at
   a known offset can be decrypted in isolation; the *tag* cannot be checked for
   part of an object. `DecryptStream`'s own header already warns that plaintext
   is emitted before authentication completes and that a caller streaming to a
   network must treat a `finish()` failure as *"discard what was sent"*.

---

## 3. Normative requirements

Numbered so tests and review can cite them. **MUST** / **MUST NOT** / **SHOULD**
carry their usual force.

### Recording the transform

- **SR-1** — Every stored version MUST carry a durable record of which transforms
  were applied to it. That record is written in the same transaction that records
  the version; it MUST NOT be possible for the two to disagree.
- **SR-2** — Every read path MUST derive the inverse transform from **SR-1's
  record**, never from the deployment's current configuration.
- **SR-3** — The migration that introduces SR-1 MUST backfill existing versions
  from the deployment's flags **as they are at migration time**, because that is
  the assumption under which the existing corpus is currently readable. No
  pre-existing version may change meaning.
- **SR-4** — After SR-1/SR-2, the compression and encryption settings MUST affect
  only subsequent writes. Changing either MUST NOT alter how an existing version
  is read.

> **Until SR-1 ships, `compression` and `encryption` are set-once-at-provisioning
> values, not tunables.** Turning compression off today makes every read hand the
> raw zlib stream to the client as the file's content, with no error; turning
> encryption off does the same with `[IV ‖ ciphertext ‖ tag]`. The opposite
> direction fails loudly. This warning belongs in the operator documentation
> before any of this is built.

### Deciding compression

- **SR-5** — Compression MUST be decided per payload, from the bytes, at write
  time. The decision MUST NOT depend on a filename, an extension, or a
  caller-supplied content type: `FileInfo` carries no content type
  (`core/include/fileengine/types.h:43`) and the core MUST NOT gain one for this.
- **SR-6** — The decision MUST be made by measurement: compress a sample of the
  payload and keep the compressed form only if it saves at least
  `storage.compression_min_gain` (default **10%**). Sample size is
  `storage.compression_sample_bytes` (default **128 KiB**), compressed at zlib
  level 1.
- **SR-7** — Where the payload arrives as a stream and the whole of it is not
  available, the decision MUST be made from the first buffer and applied
  unchanged to the remainder of that write.
- **SR-8** — Any error or ambiguity in the decision MUST resolve to *compress*.
  A wrong "compress" costs CPU; a wrong "skip" costs storage; neither can cost
  correctness, because SR-1 records what happened.
- **SR-9** — Highly compressible content MUST continue to be compressed. This is
  a regression requirement with a named corpus: IFC, CityJSON, OBJ, glTF-JSON,
  SVG, CSV, JSON, XML, SQL, logs and plain text.

### Reading a range

- **SR-10** — `GetFileRequest` gains `offset` and `length`. Both are expressed
  over the **plaintext**, never over stored bytes. `offset = 0, length = 0` MUST
  behave exactly as the request does today.
- **SR-11** — A range MUST always be satisfiable. The server MUST NOT refuse a
  range because the stored format makes it expensive; it satisfies it and reports
  **how** (SR-13).
- **SR-12** — The first frame of a response MUST carry `total_size` (plaintext
  length of the whole version), `range_start`, `range_length` and `ranged`.
  `total_size` MUST be the plaintext size under every combination of compression
  and encryption.
- **SR-13** — The first frame MUST carry `range_method`: `"seek"` when the cost
  was proportional to `length`, `"scan"` when it was proportional to
  `offset + length`. This is part of the contract, not diagnostics — a range API
  that silently costs O(offset) is worse than none, because callers design
  against the promise.
- **SR-14** — `offset >= total_size` MUST return a distinguishable error the
  doors map to **416**. `offset + length > total_size` MUST be **clamped**, not
  rejected.
- **SR-15** — A **full** read (`offset = 0, length = 0`) MUST verify the GCM tag
  exactly as it does today. This is the line that must not blur: adding ranges
  MUST NOT weaken the integrity guarantee of an ordinary download.
- **SR-16** — A read terminated early MUST NOT call `DecryptStream::finish()`,
  which on a truncated stream is a guaranteed spurious failure.
- **SR-17** — Bytes returned by a ranged read that were not covered by a verified
  tag MUST be recorded as such in the audit event (SR-20), and serving them at
  all MUST be gated by `storage.allow_unauthenticated_ranges` (default
  **false**). With it false, a ranged read of an encrypted v1 version falls back
  to a scan — correct, authenticated up to where it stops, merely slower.
  **This requirement is transitional and applies to v1 versions only.** v2 (§6)
  authenticates every block a range touches (SR-28), so once a deployment's
  content is v2 the flag has nothing left to govern. It MUST NOT be presented to
  operators as a permanent posture, and S5's completion is the point at which it
  can be removed.

### Cost, accounting and abuse

- **SR-18** — A ranged read of a version held only in the object store SHOULD
  fetch only the stored bytes it needs, rather than restoring the whole object
  (`filesystem.cpp:956`). Where the mapping from plaintext range to stored range
  is unavailable (a scan), restoring first is correct.
- **SR-19** — Ranged reads MUST be bounded against amplification. On a scan, a
  two-byte tail range costs a full-payload read, decrypt and decompress, and a
  loop of them is a cheap denial of service. The bound MUST live in the core,
  because the core has several callers and a bound only one door enforces is not
  a bound. Required elements: a minimum served window, and a per-caller budget
  measured in **bytes scanned** rather than requests, because bytes scanned is
  what the work is.
- **SR-20** — The audit event for a read MUST record `offset`, `length`,
  `total_size` and whether the returned bytes were authenticated. Events MUST be
  coalesced per RPC; a scrubbing media player would otherwise flood a
  hash-chained log.

### Compatibility

- **SR-21** — A client that does not set the new fields MUST observe today's
  behaviour precisely. This is what allows the core change to land before any
  consumer does.
- **SR-22** — No version written before this work may require rewriting,
  re-encrypting or re-compressing. Migration is additive; old versions age out
  through the existing version lifecycle.

---

## 4. Data model

Two additive columns on the per-tenant `versions` table, in the
`ADD COLUMN IF NOT EXISTS` style the table already uses for `revised_by`
(`core/src/database.cpp:4204`):

```sql
ALTER TABLE "<tenant>".versions
  ADD COLUMN IF NOT EXISTS compressed     BOOLEAN  NOT NULL DEFAULT <current compression flag>;
ALTER TABLE "<tenant>".versions
  ADD COLUMN IF NOT EXISTS encrypted      BOOLEAN  NOT NULL DEFAULT <current encryption flag>;
ALTER TABLE "<tenant>".versions
  ADD COLUMN IF NOT EXISTS storage_format SMALLINT NOT NULL DEFAULT 1;
```

**`storage_format` is in the S0 migration because v2 is in scope** (§6). Deciding
it now costs one column in a migration that is being written once; deciding it at
S5 costs a second migration across every tenant. It is the field SR-32 selects
the reader by, and a v2 version sets it to `2`. For v1 versions `compressed` and
`encrypted` retain their meaning; for v2 versions the authoritative copy of both
is the format header, and the columns are a redundant index kept in step by the
writer.

The defaults are **rendered from the deployment's live configuration at migration
time**, not written as literals. That is SR-3: the backfill cannot know what was
applied to a row written before the column existed, and the only safe assumption
is the one every existing read is already making.

`key_id` remains open (§11-D2), but v2's header carries one (§6.2), so the
per-version column is now the lesser half of that question rather than the whole
of it.

---

## 5. Interfaces

### 5.1 Proto

```proto
message GetFileRequest {
    string uid = 1;
    string version_timestamp = 2;
    AuthenticationContext auth = 3;
    int64  offset = 4;   // plaintext, >= 0
    int64  length = 5;   // 0 = to end of file
}

message GetFileResponse {
    bool   success = 1;
    string error = 2;
    bytes  data = 3;
    // First frame of a stream only.
    int64  total_size   = 4;
    int64  range_start  = 5;
    int64  range_length = 6;
    bool   ranged       = 7;
    string range_method = 8;   // "seek" | "scan"
}
```

Additive and optional; proto3 defaults make an old client's request
`offset = 0, length = 0` (SR-21). `GetFile` (unary) honours the range too, which
incidentally lets a caller read a window out of a version larger than the gRPC
message limit.

`total_size` on the first frame removes a round-trip the doors make today: a
bridge must `Stat` before it can answer a `Range`, and a media player asks for
`bytes=0-` first precisely to discover the length.

### 5.2 Storage interfaces

```cpp
// IStorage — stored-byte range; FileSystem owns the plaintext->stored mapping.
virtual Result<void> read_file_range(const std::string& storage_path,
                                     int64_t offset, int64_t length,
                                     const std::function<bool(const uint8_t*, size_t)>& on_chunk,
                                     const std::string& tenant = "") = 0;
virtual Result<int64_t> file_size(const std::string& storage_path,
                                  const std::string& tenant = "") = 0;
```

`IObjectStore::read_file_stream` (`IObjectStore.h:67`) gains the same
offset/length pair, which maps onto an S3 `Range` header (SR-18).

### 5.3 `FileSystem`

`get_stream` gains `offset` / `length` and a plaintext cursor on its existing
bounded sink; the early-abort path it already has (`filesystem.cpp:897`) becomes
the mechanism that stops a head-range read from touching the tail.

A new seeking decryptor joins `crypto_utils` for stage S3. It MUST be a
**distinct type**, not an `offset` parameter on `DecryptStream`: an unauthenticated
read should require naming the thing that does it, not passing a number.

---

## 6. Storage format v2 — authenticated random access

**In scope** *(D1 settled 2026-09-26: build it, keep authentication and random
access).* This is the end state. Stages S0–S4 are correct and useful on their own
and remain the path to it, but v2 is what removes the trade rather than managing
it — with v2, a ranged read is **fully authenticated**, and
`allow_unauthenticated_ranges` (SR-17) becomes a transitional flag with a defined
end rather than a permanent posture.

### 6.1 The idea

Instead of one deflate stream inside one GCM object, a version is stored as a
sequence of **independent blocks** over fixed-size *plaintext* windows. Each
block is compressed on its own, encrypted on its own, and carries its own
authentication tag. Plaintext offset → block index is arithmetic; block index →
stored offset is either arithmetic (uncompressed) or one index lookup.

Random access and authentication stop being in tension because the unit of
authentication becomes the unit of access.

### 6.2 Layout

```
┌ header (fixed 32 bytes, cleartext, authenticated via AAD) ──────────────┐
│ magic          "FEV2"                    4                             │
│ format_version uint16 = 2                2                             │
│ required_caps  uint16                    2   bit0 compressed-blocks     │
│                                              bit1 encrypted             │
│                                              bit2 uniform-stored-length │
│ advisory_caps  uint16                    2   hints; safe to ignore      │
│ block_size     uint32                    4   PLAINTEXT bytes per block  │
│ salt           bytes                     8   per-version nonce prefix   │
│ key_id         uint32                    4   0 = deployment key         │
│ reserved                                 6                             │
└─────────────────────────────────────────────────────────────────────────┘
[ block 0 ][ block 1 ] … [ block n-1 ]        each: ciphertext ‖ tag(16)
┌ trailer ────────────────────────────────────────────────────────────────┐
│ block_count    uint32                                                   │
│ plaintext_size uint64                                                   │
│ index          uint32 × block_count   stored length of each block       │
│                                       (ABSENT when flags.bit2 is set)   │
│ trailer_tag    bytes 16               AEAD over the header + this trailer│
└─────────────────────────────────────────────────────────────────────────┘
┌ footer (fixed 16 bytes, always last) ───────────────────────────────────┐
│ trailer_offset uint64 │ magic "FEV2" 4 │ reserved 4                     │
└─────────────────────────────────────────────────────────────────────────┘
```

Three things about this shape are deliberate:

- **Plaintext offsets are implicit.** Block *i* covers
  `[i·block_size, (i+1)·block_size)`, so the index stores only each block's
  *stored* length — 4 bytes per block, ~4 KiB for a 1 GiB file at 1 MiB blocks.
- **An uncompressed version needs no index at all.** With `uniform-stored-length`
  set, every block is `block_size + 16` bytes stored, so stored offset is pure
  arithmetic and the trailer is 28 bytes. **This is the media case** (S2 leaves
  media uncompressed), which is the case that most needs O(1) seek — so the
  hottest path is also the cheapest one.
- **`block_count` and the index live in the trailer, not the header**, so writing
  stays **single-pass and streaming**. A writer that had to patch a count into
  the header could not stream to an object store. The footer is fixed-size and
  last, so a reader finds the trailer with one read from the end.

### 6.3 The header is the authority, and that is the point

The most valuable property of v2 is not the block structure — it is that **a v2
blob describes itself**. Today a stored blob is an undifferentiated lump of
bytes whose meaning lives entirely outside it: in the deployment's configuration
(which §3's SR-1 shows is already the wrong place) and, after S0, in a database
column. Interpreting it requires that external state to be present, correct, and
in step.

- **SR-34** — A v2 blob MUST be fully interpretable from **the blob and the key
  alone**. A reader given the bytes and the correct key MUST be able to
  determine the format, the block size, the nonce basis, whether blocks are
  compressed, and the plaintext length, **with no database and no
  configuration**.
- **SR-35** — Where the header and the `versions` row disagree, **the header
  wins**. The columns added in §4 are an index for querying and for selecting a
  reader cheaply; they are not the source of truth. A reader MUST validate the
  header it actually found rather than trusting the column that led it there.

This is the robustness affordance, and it is worth more than the performance
work around it:

- **Disaster recovery stops depending on two systems agreeing.** The existing
  restore rehearsal already verifies by hashing content rather than counting
  rows; with v2 a recovered blob can be read with nothing but the key, so a
  restore that brings back object storage without a matching database is a
  recoverable situation rather than a forensic exercise.
- **A restore into a differently-configured instance is safe.** Today it is an
  unwritten requirement that the target's compression and encryption settings
  match the source's, and nothing checks it. With SR-34 the target's settings are
  irrelevant to reading what it was given.
- **Bugs become detectable rather than silent.** A mismatch between header and
  column is a loud, diagnosable inconsistency. The v1 equivalent — the defect in
  §3 — produces plausible-looking wrong bytes and no error at all.

**The asymmetry this creates for v1 is the strongest argument for the rewriter**
(§6.9). A corpus that is entirely v2 is a corpus that can be read from cold
storage with a key; a mixed corpus is only as recoverable as its database.

### 6.4 Unknown capabilities must refuse, not guess

`required_caps` and `advisory_caps` exist so that the next format change is
survivable, which the current format has no way to be.

- **SR-36** — A reader encountering a bit set in `required_caps` that it does not
  implement MUST refuse the read with a distinguishable error naming the
  unsupported capability. It MUST NOT attempt the read.
- **SR-37** — A reader encountering an unknown bit in `advisory_caps` MUST
  proceed normally. Advisory bits carry hints — a content-class marker, a
  dedup-friendliness flag — that affect efficiency and never correctness.
- **SR-38** — `format_version` and `magic` MUST be validated before any other
  field is read, and a blob whose footer magic or `trailer_offset` is
  inconsistent MUST be refused rather than parsed defensively.

The split is the same reasoning PNG's critical-versus-ancillary chunks encode,
and the failure it prevents is specific: **an old binary silently misreading a
newer blob.** Without it, adding any future capability — a different cipher, a
per-block dictionary, a new compressor — means either a flag day or a class of
bug that returns wrong bytes without erroring. With it, an old reader says
exactly what it cannot do.

This matters most in the window this specification creates deliberately: §7's
migration deploys a reader before it enables the writer, and mixed-version
readers will exist in a replicated deployment. A reader that refuses cleanly is
the difference between a rolling upgrade and an outage.

### 6.5 Normative requirements

**Crypto**

- **SR-23** — Each block MUST be encrypted with a nonce unique under its key. The
  nonce is `salt(8) ‖ block_index(4)`, with `salt` freshly generated per **write**
  (not per file, and not per version name). **Nonce reuse under one key is
  catastrophic for GCM**, so a rewritten version MUST generate a new salt rather
  than inherit one.
- **SR-24** — Each block's AEAD MUST authenticate, as associated data: the
  header bytes, the **block index**, and a **final-block flag**. Per-block tags
  alone prove each block is genuine but prove nothing about *order*,
  *completeness*, or *which file it came from* — an attacker could otherwise
  reorder blocks, splice blocks between versions encrypted under the same key, or
  truncate the file, and every individual tag would still verify.
- **SR-25** — The AAD MUST additionally bind the version's identity (`file_uid`
  and `version_timestamp`), so a block cannot be moved between versions.
- **SR-26** — The trailer MUST be authenticated (`trailer_tag`) over the header
  and the trailer. Otherwise an attacker rewrites the index and reorders or drops
  blocks without touching a single block tag.
- **SR-27** — Truncation MUST be detectable: a read that reaches the end without
  encountering the block whose AAD carries the final-block flag MUST fail. The
  footer's `trailer_offset` and magic MUST be validated before the trailer is
  trusted.

**Behaviour**

- **SR-28** — A ranged read over a v2 version MUST verify the tag of **every
  block it touches**, and MUST fail the request if any fails. There is no
  unauthenticated read path for v2; SR-17's flag has no effect on it.
- **SR-29** — A ranged read MUST read at most one block beyond each end of the
  requested window (the partial blocks the window lands in). `range_method` for
  a v2 version is always `"seek"`.
- **SR-30** — `block_size` MUST be recorded per version, never assumed. A
  deployment changing it MUST affect only subsequent writes.
- **SR-31** — Per-block compression MUST use the same decision rule as SR-5/SR-6,
  applied **per block**, so a file with a compressible header and an
  incompressible body stores each part appropriately. A block that does not
  shrink is stored uncompressed and flagged in its index entry.
- **SR-32** — The reader MUST be selected by the version's recorded
  `storage_format` (§4), not by sniffing the blob. v1 blobs are read by exactly
  today's code path, which stays.
- **SR-33** — Writing v2 MUST be a per-deployment setting that defaults to **on**
  once S5 ships, and MUST NOT rewrite anything already stored (SR-22).

### 6.6 Block size

`storage.block_size`, default **1 MiB** — it matches `kMaxStreamChunkBytes`
(`filesystem.cpp:878`), bounds read amplification to at most one block either
side of the window, and costs 16 bytes of tag plus 4 of index per MiB
(~0.002%). 256 KiB matches the current disk read size and costs 4× the overhead
(still ~0.008%) for finer seeks. This is a measurement, not an argument — S1's
method applies (§11-D3).

### 6.7 What v2 costs

Stated plainly, because it is not free:

- **Compression ratio drops.** Each block is deflated independently, so there is
  no shared dictionary across block boundaries. On 1 MiB blocks over text the
  loss is small — low single-digit percent — but it is a real regression against
  whole-stream compression and MUST be measured on the SR-9 corpus (a real IFC
  model) before v2 becomes the default for compressible content.
- **A coarse length side channel.** Per-block stored lengths reveal per-block
  compressibility, where v1 revealed only the total. For file storage at rest
  this is a marginal change from a leak that already exists, and it is noted
  rather than mitigated.
- **Two small reads before the first byte** — the footer, then the trailer.
  Both are cacheable per version, and for the uniform-stored-length case the
  trailer is 28 bytes.

### 6.8 What v2 buys beyond the range case

- **Corruption is localised.** Today one flipped bit fails the tag for the whole
  version; with v2 the damage is one block. For a video that is the difference
  between a lost file and a glitched second of it — and, more importantly, the
  rest of the file is still *provably* intact rather than merely readable.
- **Parallel read and write** become possible; blocks are independent.
- **Key rotation gets a path.** `key_id` in the header means a version states
  which key it was written under, which is the prerequisite for rotating without
  rewriting every blob (§11-D2).
- **Deduplication and delta sync** get a natural unit, if ever wanted.

### 6.9 Rewriting v1 content

**Nothing has to be rewritten.** The format is self-describing and the version
row records which one was used (SR-32), so v1 and v2 versions **of the same
file** coexist indefinitely and old versions age out through the existing
version lifecycle. **No flag day, no downtime, no re-encrypt window** — that
property is what makes v2 proposable at all and it MUST NOT be traded away for
implementation convenience.

An offline rewriter is nevertheless specified, and it has **two independent
purposes** that are worth separating, because only one of them needs v2:

| Mode | Available after | What it does | Why |
|---|---|---|---|
| **A — recompress** (v1 → v1) | **S0 + S2** | re-evaluates SR-5/SR-6 against the existing corpus and stores the payload uncompressed where compression was achieving nothing | **removes a useless inflate from every future read of that blob** — S2 applied retroactively |
| **B — convert** (v1 → v2) | S5 | the above, per block, plus the v2 framing | self-describing recovery (§6.3) and authenticated random access (SR-28) |

**Mode A is the one worth noticing.** S2 only improves content written after it
ships; the deployed corpus keeps paying an inflate on every read, forever, for
compression that saved nothing. If a rewrite pass is going to be run at all, that
pass is where the existing corpus gets the same benefit — and Mode A needs
neither v2 nor a security review, only S0's per-version record to write the
answer into.

**Candidate selection is a metadata query, not a corpus scan.** `versions.size`
is the plaintext size and the stored object's size is a `stat`; their ratio says
whether compression achieved anything **without reading, decrypting or
decompressing a single blob**. A ratio above `storage.recompress_candidate_ratio`
(default **0.95**) is a candidate; everything else is left alone. So the pass
touches only the blobs that stand to gain, which on a media-heavy tenant is most
of them and on a BIM-heavy tenant is almost none — and it can say which before it
starts.

**One pass or two.** Running Mode A now and Mode B after S5 rewrites the same
blobs twice and pays the offsite replication cost twice (below). Since v2 is
committed, the default is **one pass, Mode B, after S5**. Mode A earlier is worth
it only if the read-performance win on the existing corpus is wanted before S5
lands — which §9's measurement is what decides (§11-D8).

Either mode is **a tool, not a step**, and running it has a cost that is easy to
miss:

- **Every rewritten blob is a new object to replicate offsite.** The deployment
  mirrors content to a second provider; rewriting the corpus re-uploads the
  corpus. Rewriting is therefore paced, resumable, and scheduled against
  bandwidth — not run as a single pass. This is the cost that argues for doing it
  once rather than twice, and for the candidate filter above rather than a blanket
  sweep.
- **A rewrite is a new stored object, not an edit.** It MUST preserve the
  version's identity, timestamps and `revised_by` exactly; it MUST NOT create a
  new version, disturb ordering, or alter what any consumer's idempotency keys
  see. A rewrite that produced a new version name would silently re-trigger every
  downstream conversion and re-index in the platform.
- **It MUST be interruptible** at any point without damaging either
  representation: write the v2 object beside the v1 one, verify it reads back to
  an identical plaintext hash, then swap the pointer and delete the old — never
  the reverse order.

---

## 7. Migrating the deployed system

This lands on a live installation with real tenants, an offsite content mirror
and no maintenance window on offer. The stages in §8 are the engineering
sequence; this is the deployment sequence, and they are not the same thing.

### 7.1 The one-way door

**The moment the first v2 blob is written, the core cannot be rolled back.** An
older binary has no v2 reader, and SR-36 means it will refuse rather than
misread — which is the correct behaviour and still an outage for anything
touching that content.

Everything below is arranged around that single fact. The rule it produces:

> **Ship the reader first, and let it bake. Enable the writer separately, later,
> and per tenant.**

`storage.write_format` (default `1`) is therefore a distinct setting from
whatever code is deployed. Deploying S5 MUST NOT itself start writing v2.

### 7.2 The sequence

| Phase | Action | Rollback |
|---|---|---|
| **P0** | Answer §11-Q1: has `compression` or `encryption` ever been flipped on this deployment? If yes, there may already be versions affected by the §3 defect, and this becomes a repair before it is a migration. | n/a — a question, not a change |
| **P1** | Deploy **S0**: the migration and the switch from configuration to the version record. No behaviour change; every version reads exactly as before. | ordinary redeploy; columns are additive and unused by the old binary |
| **P2** | Soak. Verify via the restore rehearsal that a restored instance reads the corpus **without** matching configuration — the property S0 buys. | as P1 |
| **P3** | Deploy **S2** (selective compression) after §9's measurement. New writes only; the corpus is now mixed by design, which P1 made safe. | ordinary redeploy; mixed corpus stays readable either way |
| **P4** | Deploy **S3a** (ranges). Purely additive to the RPC. | ordinary redeploy |
| **P5** | Deploy **S5 code** with `write_format = 1`. **The reader ships and bakes; nothing writes v2.** | ordinary redeploy — this is the last fully reversible step |
| **P6** | Set `write_format = 2` on **one low-value tenant**. Verify by content hash, then let it run. | revert the setting; v2 blobs already written stay readable, because P5's reader is deployed everywhere |
| **P7** | Broaden per tenant. | as P6 |
| **P8** | Optionally run the rewriter (§6.9) — **Mode B by default**, so the existing corpus gets both the retroactive decompression and the v2 framing in one pass. Candidate-filtered, paced against offsite replication bandwidth, resumable. | v1 originals are deleted only after the v2 copy verifies its plaintext hash |

P5 and P6 being separate deployments is the whole design of this table. Merging
them is the mistake that turns a reversible rollout into a one-way door taken by
accident.

### 7.3 Replication and mixed readers

`REPLICATION_FAILOVER.md` governs the topology. Two constraints follow:

- **Every node must run the v2 reader before any node writes v2.** In a
  replicated deployment P5 must complete across the whole topology, not on one
  node. SR-36 makes a lagging node fail loudly rather than corrupt, which is the
  right failure — and still a failure.
- **A failover target is a reader.** A standby that has not taken P5 cannot serve
  v2 content after promotion. P5 covers standbys.

### 7.4 What verifies it

The existing **restore rehearsal harness** is the instrument, and it already has
the right shape: it verifies by hashing content rather than counting rows. Three
checks, at P2, P6 and P8:

1. A restored instance reads the corpus with **no configuration match** to the
   source (SR-34/SR-35).
2. Plaintext hashes are identical across the format change, per version, for a
   sample spanning compressible and incompressible content.
3. A v2 blob restored **without its database row** can still be decoded to the
   correct plaintext given the key — the §6.3 property, tested rather than
   asserted.

Check 3 is the one that justifies the whole header, and it is the one that will
be skipped if it is not written down here.

### 7.5 Interactions worth checking before P6

- **Offsite mirror.** v2 blobs are opaque bytes and replication is unaffected;
  the mirror's *volume* is affected only by P8, and then only for the blobs the
  candidate filter selects. Size the P8 window from that query before scheduling
  it — it is answerable up front and cheaply (§6.9).
- **Backups.** A backup taken mid-P7 contains both formats. That is fine by
  construction — but the restore procedure must not assume one format, and the
  rehearsal at P6 is what proves it.
- **The culler and the version lifecycle.** Unchanged; v2 is a storage
  representation, not a lifecycle concept.
- **Read-only periods.** During a failover the core refuses writes; a rewriter
  run must stop cleanly rather than accumulate failures.
- **Storage accounting.** Reported sizes are stored bytes and will move slightly
  in both directions (tag and index overhead up, useless compression removed).
  Nothing depends on the old numbers, but a dashboard may show a step.

### 7.6 Operator documentation owed

Two things must be written before P1 ships, not after:

1. **`compression` and `encryption` are set-once until S0.** Flipping either on
   an existing deployment corrupts reads silently in one direction (§3). This
   warning is owed today, independently of whether any of this is built.
2. **`write_format` is a one-way setting**, what P5-before-P6 means, and why a
   replicated deployment upgrades readers first.

---

---

## 8. Stages

Each stage is independently shippable and independently valuable. The gate
column is what must be true before the next stage is safe.

| Stage | Content | Gate it opens |
|---|---|---|
| **S0** | **SR-1 – SR-4.** The two columns, the live-flag backfill, both read paths switched from configuration to the version record. | Everything. Also fixes defect **A** on its own. |
| **S1** | **Measure** (§9). Corpus composition by stored bytes, real zlib ratios per format, CPU attributable to deflate/inflate. | Decides whether S2 is scheduled at all. |
| **S2** | **SR-5 – SR-9.** The trial deflate, wired into `put` and the streaming writer, with the §3 SR-9 regression corpus. Plus the **candidate query** (§6.9) — stored-size against `versions.size`, which quantifies what the *existing* corpus is paying for useless compression without reading a blob. | Media and other already-compressed content becomes uncompressed, and therefore seekable. The query decides §11-D8. |
| **S3a** | **SR-10 – SR-16, SR-20, SR-21.** Proto fields, the windowed sink with early stop, `total_size` / `range_method`, 416/clamp, the `finish()` rule, audit range fields, `python_interface`. **Scan-only — no format change, no security change.** | Correct ranges for every deployment. Stops discarded bytes crossing gRPC. |
| **S3b** | **SR-17.** True seek for uncompressed-encrypted versions; the seeking decryptor; `allow_unauthenticated_ranges` off by default; the audit integrity flag. | O(1) seek for media, on a deployment that accepts the trade. |
| **S4** | **SR-18, SR-19.** Ranged object-store reads and the restore heuristic; the amplification bounds and their metrics. | Safe to expose ranges to an unauthenticated door. |
| **S5** | **Storage format v2** — **SR-23 – SR-33** (§6). The block format, the AAD binding, the authenticated trailer, the streaming writer, the format-selected reader, and the offline rewriter. **In scope**, and the largest single piece; it warrants its own security review before merge, since it is new cryptographic framing. | Retires SR-17 entirely: ranged reads become fully authenticated, and corruption stops being whole-file. |

**S0 is the prerequisite in the strict sense** — it is a defect fix, it is
required before S2 can exist, and it should be scheduled on its own merits even
if every other stage is dropped.

**S0 → S2 → S3a is the shortest path that delivers the general optimisation**,
and S3a alone removes the largest practical cost (a gigabyte crossing a process
boundary to be discarded).

**S3b is now optional, and should probably be skipped.** It buys an
unauthenticated O(1) seek for v1 encrypted content, and S5 buys an authenticated
one for everything written afterwards. Building S3b is worthwhile only if the gap
between S3a and S5 is long enough that media seeking needs an interim answer that
the `share_service` cache does not already provide — and it does already provide
one. Decide this when S5 is scheduled, not before.

### 8.1 What each downstream consumer needs

| Consumer | Needs | Notes |
|---|---|---|
| `http_bridge` | S3a | Replaces read-and-discard (`src/http_server.cpp:774–789`) **and fixes a malformed `Content-Range: bytes <start>-/*`** — no last-byte-pos, unknown total — which is why in-browser seeking is unreliable today for ordinary authenticated users. |
| `webdav_bridge` | S3a | `Range` on GET; Finder, Office and several sync clients send it. |
| `share_service` media door | S2 + S3b for O(1) seek; **nothing** to ship | Its local cache (`MEDIA_SHARE.md` §6.6) carries the feature until then, and remains afterwards as an edge cache with `range_method` as its trigger. |
| `convert_search_ai` | S3a | Cheap container probes — read a header or a trailer without fetching the payload. |

**No consumer is blocked on this specification.** That is deliberate: the media
work ships on its cache, and each stage here improves it rather than enabling it.
S0 is the exception, and it is blocked on nothing.

---

## 9. Measurement (S1)

The ratios and savings asserted in the companion proposals are general
knowledge, not measurements of this corpus, and S2 MUST NOT be scheduled on
them. `PROPOSAL_read_path_performance.md` sets both the method and the standard:
that investigation ruled out CSAI conversion as a CPU source precisely because
`podman stats` and `ps` both pointed at it and per-container cgroup accounting
did not.

Required before S2:

1. **Corpus composition by stored bytes** — what share is already-compressed
   content. If it is small, the correct outcome of this document is S0 alone.
2. **Achieved zlib ratio per format**, sampled from real stored versions.
3. **CPU attributable to deflate/inflate**, from per-container cgroup
   `usage_usec` deltas over a fixed window under representative read load.

---

## 10. Acceptance criteria

The tests that must exist, phrased as the failures they prevent.

**S0**

- A version written with compression enabled, then read with the deployment flag
  **flipped off**, returns the correct plaintext. *(This is defect A, asserted
  directly; it currently returns the zlib stream as file content.)*
- The same for encryption.
- The migration backfills from live configuration, and every pre-existing version
  reads byte-identically before and after.

**S2**

- A real WebM and a real `.docx` are stored uncompressed and round-trip
  byte-for-byte.
- **A real IFC model is still compressed**, with the ratio in the expected band
  (SR-9). The fixture corpus must contain one.
- Compressed and uncompressed versions **of the same file** coexist and both
  round-trip.
- The streaming write path decides from its first buffer and applies it to the
  whole payload (SR-7).
- A payload smaller than the sample window, and an empty payload, both behave.

**S3a**

- A range is **byte-identical** to the same window of a full download — across
  plain, compressed, encrypted and compressed+encrypted, at offset 0, at
  EOF-1, spanning the whole file, and across a chunk boundary.
- `offset >= total_size` is 416; `offset + length > total_size` clamps.
- **A full read still verifies the tag**, and a corrupted object still fails —
  the regression that would matter most and be invisible (SR-15).
- An early-terminated read logs no spurious tag failure (SR-16).
- `total_size` is the plaintext size under every transform combination. *(The
  field most likely to be quietly wrong on a compressed version.)*
- A request with no offset/length behaves exactly as before, asserted against
  recorded current behaviour rather than against the new code's idea of it.

**S3b / S4**

- A tail range does **not** read the whole version — asserted by counting bytes
  read from storage, not by timing.
- With `allow_unauthenticated_ranges` false, a ranged read of an encrypted v1
  version is served by scan and is reported as such.
- The audit event carries the range and the integrity flag.

**S5 — the format.** These are the tests that decide whether the format is
sound, and most of them are adversarial rather than functional.

- A v2 version round-trips byte-for-byte, across every combination of
  compressed/uncompressed blocks, encrypted and not, at sizes of 0 bytes, 1
  byte, exactly one block, one block plus one byte, and many blocks.
- A ranged read reads **at most one block beyond each end** of the window
  (SR-29) — asserted by counting bytes read from storage.
- **Reordering two blocks is detected** (SR-24). Swap them on disk; the read
  must fail, not return scrambled plaintext.
- **Splicing a block from another version encrypted under the same key is
  detected** (SR-25). This is the test that proves the AAD carries identity and
  not just an index.
- **Truncation is detected** (SR-27) — remove the last block, and separately
  remove the trailer and footer; both must fail rather than returning a short
  file.
- **A rewritten index is detected** (SR-26). Edit the trailer to drop or
  reorder an entry without touching any block tag; the trailer tag must fail.
- **Nonces never repeat** (SR-23). Write the same payload twice and assert the
  salts differ; assert no two blocks within a version share a nonce.
- A corrupted block fails **that** range and does not fail ranges that do not
  touch it (SR-28, and the localisation claim in §6.8 — if this does not hold,
  the claim comes out of the document).
- **v1 and v2 versions of the same file coexist** and both round-trip (SR-32),
  selected by the recorded `storage_format` and not by sniffing.
- The writer is **single-pass**: assert it never seeks backwards, which is what
  keeps a direct-to-object-store upload possible (§6.2).
- **Compression ratio on the SR-9 corpus** is measured against v1 whole-stream
  compression, and the regression is within the band §6.7 predicts. If a real
  IFC model loses materially more than a few percent, block size is wrong or v2
  should not default on for compressible content.
- The offline rewriter produces a v2 version whose plaintext is identical to the
  v1 it replaced, and is interruptible without damaging either.
- **The rewriter does not create a version.** After a rewrite the version name,
  `revised_by`, timestamps and ordering are unchanged, and no consumer's
  idempotency key sees a new value — asserted against the rendition namer, which
  keys on the version name and would otherwise re-convert the entire corpus.
- **Mode A round-trips**: a v1 blob whose compression saved nothing is stored
  uncompressed and reads back to an identical plaintext hash, with
  `versions.compressed` updated in the same transaction as the swap.
- **The candidate query is honest**: on a fixture corpus containing both a real
  IFC model and a real video, it selects the video and not the IFC — asserted
  without decrypting anything.

---

## 11. Remaining decisions

**D1 — Format v2 is in scope** *(settled 2026-09-26: build it, keep
authentication and random access)*. Specified in §6 as SR-23 – SR-33 and
scheduled as S5. Three consequences already folded in: `storage_format` joins the
S0 migration (§4), SR-17 is explicitly transitional and must not be presented to
operators as a permanent posture, and S3b becomes optional and probably skippable
(§8).

**D6 — Block size** (§6.6): 1 MiB by default, but the ratio cost in §6.7 and the
seek granularity both depend on it, and neither has been measured on this corpus.
Folds into S1's measurement.

**D8 — One rewrite pass or two?** §6.9's Mode A (retroactive decompression, v1 →
v1) is available at S0+S2 and needs no security review; Mode B folds it into the
v2 conversion at S5. Doing both means rewriting the same blobs twice and paying
the offsite replication twice, so the default is one pass at P8. Running Mode A
earlier is justified only if §9's measurement shows the existing corpus is
paying enough for useless inflation to be worth the duplicated work — which is a
number, not an opinion, and the candidate query in §6.9 produces it before
anything is rewritten.

**D7 — Does v2 default on for compressible content immediately?** SR-33 says v2
writing defaults on once S5 ships. If §10's ratio test shows a material loss on
the SR-9 corpus, the honest answer is v2-on for incompressible content (where
there is no ratio to lose and all the seek benefit) and a decision to make for
the rest. Cannot be settled before the measurement exists.

**D2 — Does `key_id` join the S0 migration?** Out of this document's scope, but
the column is cheap and the migration is being written once. Deciding it later
costs a second migration.

**D3 — Sample size and gain threshold** (SR-6): 128 KiB and 10% are reasoned, not
measured. S1 confirms or adjusts them.

**D4 — Cold-path restore heuristic** (SR-18): when does repeated ranged access to
an archived version trigger a full restore? No measurement behind any proposed
number yet.

**D5 — Should compression policy become per-tenant or per-folder?** The
per-version record makes it expressible. Whether it should be *exposed* is a
product question this document does not open.

---

## 12. Non-goals

- **No change to the permission model.** `get_stream` validates READ before
  resolving anything (`filesystem.cpp:907`); a range narrows what is returned and
  never widens what is reachable. Service-auth capability gating
  (`grpc_service.cpp:2218`) is unchanged — a ranged read is the same `read`
  capability as a whole read.
- **No change to the tenancy model**, the schema-per-tenant arrangement, or the
  event contract.
- **No rewriting of stored data** (SR-22). No flag day, no re-encrypt window.
- **Not a compression-algorithm change.** Replacing zlib with zstd is a separate
  question with its own compatibility story, and it is not made easier or harder
  by anything here.
