# The storage pipeline — what is applied to stored bytes, and reading part of them

**Status:** Specification — implementation-ready, staged. Nothing implemented
yet. **All design decisions settled** (§11, 2026-09-26); what remains is
measurement (§9) and build order (§8).
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
- **SR-6a** — A **signature fast path** MUST run before SR-6. Where the payload's
  leading bytes match a known already-compressed container or codec (§3.1), the
  payload is stored uncompressed and the trial deflate is skipped entirely. This
  is a *recognition of the bytes*, not a content type: it reads the payload
  itself and depends on no filename, no caller-supplied MIME and no database
  field, so SR-5 stands unchanged.
- **SR-6b** — The signature table MUST contain only formats that are compressed
  **by construction**. A format that is merely *often* compressed MUST be left to
  SR-6's measurement. Being wrong in the permissive direction is the one way this
  mechanism can lose real storage, because a signature match skips the
  measurement that would have caught it.
- **SR-7** — Where the payload arrives as a stream and the whole of it is not
  available, the decision MUST be made from the first buffer and applied
  unchanged to the remainder of that write.
- **SR-8** — Any error or ambiguity in the decision MUST resolve to *compress*.
  A wrong "compress" costs CPU; a wrong "skip" costs storage; neither can cost
  correctness, because SR-1 records what happened.
- **SR-9** — Highly compressible content MUST continue to be compressed. This is
  a regression requirement with a named corpus: IFC, CityJSON, OBJ, glTF-JSON,
  SVG, CSV, JSON, XML, SQL, logs and plain text.

#### 3.1 The signature table

Matched against the payload's leading bytes, before any trial deflate.
Compressed by construction, therefore skipped:

| Family | Signature |
|---|---|
| ZIP container — also `.docx` / `.xlsx` / `.pptx`, ODF, `.jar`, `.epub` | `50 4B 03 04`, `50 4B 05 06`, `50 4B 07 08` |
| gzip | `1F 8B` |
| bzip2 | `42 5A 68` (`BZh`) |
| xz | `FD 37 7A 58 5A 00` |
| zstd | `28 B5 2F FD` |
| 7-Zip | `37 7A BC AF 27 1C` |
| RAR | `52 61 72 21 1A 07` |
| JPEG | `FF D8 FF` |
| PNG | `89 50 4E 47 0D 0A 1A 0A` |
| GIF | `GIF87a`, `GIF89a` |
| WebP | `RIFF` … `WEBP` at offset 8 |
| HEIC / AVIF | `ftypheic`, `ftypheix`, `ftypmif1`, `ftypavif` at offset 4 |
| MP4 / MOV / 3GP | `ftyp` at offset 4 |
| Matroska / WebM | `1A 45 DF A3` |
| Ogg | `4F 67 67 53` (`OggS`) |
| MP3 | `49 44 33` (`ID3`), or frame sync `FF Ex` / `FF Fx` |
| FLAC | `66 4C 61 43` (`fLaC`) |

**Deliberately absent, each for a reason** — these are the entries a well-meaning
extension of this table would add and should not:

- **PDF (`%PDF`)** — its streams are internally compressed, but the
  cross-reference table, object headers and metadata are not, and a PDF commonly
  still yields 5–15%. Left to measurement.
- **TIFF (`II*\0` / `MM\0*`)** — may be LZW- or JPEG-compressed, or entirely
  uncompressed. The signature does not say which.
- **Legacy Office (`D0 CF 11 E0`, OLE2 `.doc` / `.xls` / `.ppt`)** — not
  compressed at all, and compresses very well.
- **No known-compressible list.** The opposite fast path — recognising IFC,
  CityJSON, JSON, XML and going straight to compress — is not specified, because
  it would save one trial deflate of 128 KiB against a full-payload compress that
  is happening anyway. The skip list earns its place by avoiding a whole-payload
  deflate on write and an inflate on **every** read; a compress list would not.

SR-6 remains the authority for everything unmatched, which is what keeps this
table from needing to be complete.

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

- **SR-18** — A ranged read of a version held only in the object store MUST
  **restore the whole object into the local cache first**, then serve the range
  from it — today's behaviour (`filesystem.cpp:956`), retained deliberately
  *(D4)*. Ranged `GET` against the object store is **not** specified.

  The reasoning is that a ranged read of an archived version is almost never
  isolated: the caller is a media player that will go on to request most of the
  file, or a client resuming a download. Serving each window with its own remote
  round-trip would fetch the object repeatedly and pay latency on every seek,
  where one restore pays it once and every subsequent range is local. The cost is
  a slow first byte on a cold archived object, which is what archived means.

  This also removes the restore heuristic an earlier draft needed, and with it a
  tuning parameter nobody had a number for.
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
ALTER TABLE "<tenant>".versions
  ADD COLUMN IF NOT EXISTS key_id         INTEGER  NOT NULL DEFAULT 0;
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

`key_id` is included *(D2 settled: apply all appropriate migrations)*. `0` means
"the deployment key", which is what every existing version was written under. It
is the prerequisite for ever rotating the encryption key without rewriting every
blob, and it costs one column in a migration that is being written once — where
adding it later costs a migration across every tenant. For v2 versions the header
carries the same value (§6.2) and the header is authoritative (SR-35).

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
│                                              bit3 identity block follows │
│ advisory_caps  uint16                    2   hints; safe to ignore      │
│ block_size     uint32                    4   PLAINTEXT bytes per block  │
│ salt           bytes                     8   per-version nonce prefix   │
│ key_id         uint32                    4   0 = deployment key         │
│ reserved                                 6                             │
├ identity block (present iff required_caps bit3) ────────────────────────┤
│ uid_len        uint16                    2                             │
│ ver_len        uint16                    2                             │
│ file_uid       bytes            uid_len                                │
│ version_ts     bytes            ver_len                                │
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
  compressed, the plaintext length, **and which version the blob is**, with no
  database and no configuration.

  The last of those is why the header carries an **identity block** — the
  version's `file_uid` and `version_timestamp` (§6.2). Those two are what every
  block's AAD binds (SR-25), so without recording them a blob could be decoded
  only by someone who already knew which version it was. In practice that meant
  reading it off the storage *path*, which made the recovery property depend on
  an undocumented layout convention rather than on the bytes. An earlier draft
  of this requirement listed everything except identity and was therefore
  incomplete rather than wrong.

- **SR-34a** — Where the caller supplies an identity and the header records a
  different one, the read MUST be refused with a distinguishable error naming
  both. Either the wrong blob was fetched or the database and the bytes have
  diverged, and both are worth stopping for rather than discovering as an
  authentication failure several structures in.

- **SR-34b** — Where the caller supplies **no** identity, the header's MUST be
  adopted. This is the recovery path, and it is what makes SR-34 operationally
  true rather than merely arguable.

- **SR-34c** — The header MUST NOT record the file's **name**. A name is
  mutable, so a rename — which must never require rewriting or re-encrypting a
  payload — would leave the header stating something false rather than merely
  incomplete. A filename is also content-grade PII on this platform (audit
  events reference files by uid for exactly that reason), and a cleartext header
  would push it into the object store and the offsite mirror, where erasing a
  database row does not reach it. `file_uid` is an opaque identifier and carries
  neither problem.
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

**This property applies to v2 content only.** A v1 blob still depends on its
database row or on matching configuration, and since there is no rewrite pass
(§6.9) the v1 remainder is permanent rather than transitional. That residual is
accepted deliberately; it shrinks as content churns, and §6.9 records what is
being relied on in the meantime.

### 6.4 Unknown capabilities must refuse, not guess

`required_caps` and `advisory_caps` exist so that the next format change is
survivable, which the current format has no way to be.

- **SR-36** — A reader encountering a bit set in `required_caps` that it does not
  implement MUST refuse the read with a distinguishable error naming the
  unsupported capability. It MUST NOT attempt the read. The identity block's
  presence is a **required** capability rather than an advisory one for this
  reason: its length decides where the blocks begin, so a reader that ignored
  the bit would compute every block offset wrongly and fail with a puzzling tag
  error instead of a clear refusal.
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

`storage.block_size`, default **1 MiB** *(D6 settled: acceptable default)* — it
matches `kMaxStreamChunkBytes`
(`filesystem.cpp:878`), bounds read amplification to at most one block either
side of the window, and costs 16 bytes of tag plus 4 of index per MiB
(~0.002%). 256 KiB matches the current disk read size and costs 4× the overhead
(still ~0.008%) for finer seeks, and remains available per deployment. 1 MiB
stands unless §9's measurement contradicts it.

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

### 6.9 Existing content is left alone

**There is no rewrite pass** *(D8 settled)*. v1 versions stay v1 and are read by
exactly today's code path; v2 populates naturally as new content is written and
as existing files are revised. No rewriter tool is in scope, and P8 is gone from
§7.

This is the right call for the cost it avoids — rewriting the corpus means
re-uploading the corpus to the offsite mirror — and it rests on an explicit
premise worth writing down rather than leaving implicit:

> **The deployment's configuration is valid, so S0's backfill is correct.** Every
> pre-existing version is read today under the assumption that current
> configuration describes it; S0 records that assumption rather than changing it
> (SR-3). Accepting it as true is what makes leaving v1 content untouched safe.

Two consequences follow, and both should be stated rather than discovered:

- **The v1 reader is permanent, not transitional.** Content that is never
  revised is never rewritten, so a cold archive of finished projects can remain
  v1 indefinitely. The v1 path must be maintained, tested and carried through
  future refactors as a first-class code path — not marked deprecated and quietly
  allowed to rot. A test asserting v1 reads must exist for as long as any v1
  blob does.
- **§6.3's recovery property applies only to v2 content.** A v2 blob can be read
  from cold storage with a key alone; a v1 blob still needs its database row or
  matching configuration. That residual is accepted, and it **shrinks on its own**
  as content churns — which is the argument for letting it shrink rather than
  forcing it. The backup and restore procedure must keep assuming a mixed corpus
  indefinitely, not treat v1 as an edge case that will age out by some date.

If a rewrite is ever wanted, the shape it would take — verify to an identical
plaintext hash beside the original, swap the pointer, delete the old, never
create a new version, pace against replication bandwidth — is recorded here so it
does not have to be rediscovered. It is not scheduled.

---

## 7. Migrating the deployed system

This lands on a live installation with real tenants, an offsite content mirror
and no maintenance window on offer. The stages in §8 are the engineering
sequence; this is the deployment sequence, and they are not the same thing.

### 7.1 Two one-way doors, not one

**Storage format v2 is the obvious one** (below). The subtler one is **S2**, and
it was missed in the first draft of this section: once the measurement is on, an
already-compressed payload is stored UNCOMPRESSED, and a binary that decides by
configuration — every binary before S0 — will try to inflate those bytes and
fail. The blob is not corrupt and nothing is lost, but it is unreadable until
the newer binary is back.

That is why **S2 is a setting and not a consequence of deploying the code**
(`FILEENGINE_SELECTIVE_COMPRESSION`, default **off**). With it off the first
deploy writes byte-for-byte what its predecessor wrote, so P1 and P2 are
genuinely reversible; P3 turns it on, per tenant, after the binary has proven
itself. Without the switch, the schema fix and the write-behaviour change would
ship together and the rollback plan below would be fiction.

It also revises a documented assumption elsewhere:
`scripts/Ansible/playbooks/predeploy_capture.yml` captures databases and the
directory but deliberately **not** file content, on the stated grounds that "a
bad patch cannot damage the object store, because the core writes immutable
version keys and a failed deploy never rewrites them". That premise still holds
literally — nothing here rewrites an existing key — but from P3 onward the
conclusion does not: new keys are written in a representation the rollback
target cannot read. **A content snapshot is therefore a genuine precondition for
P3 and P6, where it was not for previous deploys.**

### 7.1.1 The v2 one-way door

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
| **P3** | Turn **S2** on (`FILEENGINE_SELECTIVE_COMPRESSION`, default off) after §9's measurement, per tenant. New writes only; the corpus is now mixed by design, which P1 made safe. | **not a redeploy** — revert the setting, but versions already written uncompressed stay readable only on this binary or later (see below) |
| **P4** | Deploy **S3a** (ranges). Purely additive to the RPC. | ordinary redeploy |
| **P5** | Deploy **S5 code** with `write_format = 1`. **The reader ships and bakes; nothing writes v2.** | ordinary redeploy — this is the last fully reversible step |
| **P6** | Set `write_format = 2` on **one low-value tenant**. Verify by content hash, then let it run. | revert the setting; v2 blobs already written stay readable, because P5's reader is deployed everywhere |
| **P7** | Broaden per tenant. | as P6 |

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
checks, at P2 and P6:

1. A restored instance reads the corpus with **no configuration match** to the
   source (SR-34/SR-35).
2. Plaintext hashes are identical across the format change, per version, for a
   sample spanning compressible and incompressible content, **and across both
   formats** — a v1 and a v2 version of the same file must both restore
   correctly, since that mix is permanent.
3. A v2 blob restored **without its database row** can still be decoded to the
   correct plaintext given the key — the §6.3 property, tested rather than
   asserted.

Check 3 is the one that justifies the whole header, and it is the one that will
be skipped if it is not written down here.

### 7.5 Interactions worth checking before P6

- **Offsite mirror.** v2 blobs are opaque bytes, so replication is unaffected in
  both mechanism and volume — there is no rewrite pass to re-upload anything
  (§6.9). New and revised content replicates as it always has.
- **Backups.** Every backup from P6 onward contains both formats, **permanently**
  — v1 content is never converted (§6.9). The restore procedure must assume a
  mixed corpus as its steady state rather than as a transitional condition, and
  the rehearsal at P6 is what proves it.
- **The culler and the version lifecycle.** Unchanged; v2 is a storage
  representation, not a lifecycle concept.
- **Read-only periods.** During a failover the core refuses writes, so no new v2
  content is produced; reads of both formats continue unaffected.
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
| **S2** | **SR-5 – SR-9**, including the **signature fast path** (SR-6a/6b and §3.1). Wired into `put` and the streaming writer, with the §3 SR-9 regression corpus. | Media and other already-compressed content becomes uncompressed, and therefore seekable. |
| **S3a** | **SR-10 – SR-16, SR-20, SR-21.** Proto fields, the windowed sink with early stop, `total_size` / `range_method`, 416/clamp, the `finish()` rule, audit range fields, `python_interface`. **Scan-only — no format change, no security change.** | Correct ranges for every deployment. Stops discarded bytes crossing gRPC. |
| **S3b** | **SR-17.** True seek for uncompressed-encrypted versions; the seeking decryptor; `allow_unauthenticated_ranges` off by default; the audit integrity flag. | O(1) seek for media, on a deployment that accepts the trade. |
| **S4** | **SR-18, SR-19.** Ranged object-store reads and the restore heuristic; the amplification bounds and their metrics. | Safe to expose ranges to an unauthenticated door. |
| **S5** | **Storage format v2** — **SR-23 – SR-38** (§6). The block format, the capability header, the AAD binding, the authenticated trailer, the streaming writer and the format-selected reader. **No rewriter** (§6.9). The largest single piece; it warrants its own security review before merge, since it is new cryptographic framing. | Retires SR-17 for v2 content: ranged reads become fully authenticated, and corruption stops being whole-file. |

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
   This is answerable **without reading a blob**: `versions.size` is the
   plaintext size and the stored object's size is a `stat`, so their ratio says
   what compression actually achieved, per version, across the whole corpus. Run
   this first; it is an hour's work and it sizes everything else.
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
  byte-for-byte — **the `.docx` via the signature fast path** (SR-6a), asserted
  by observing that no trial deflate ran.
- **The exclusions hold** (§3.1): a PDF, a TIFF and a legacy `.doc` each reach
  SR-6's measurement rather than being skipped on a signature.
- A file whose leading bytes coincidentally resemble a signature but is not that
  format still round-trips — the fast path may only skip compression, never
  change how bytes are stored or read.
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
- **The identity block round-trips, and recovery works without it being
  supplied** (SR-34/SR-34b) — a blob decodes, including a ranged read, given
  nothing but the bytes and the key.
- **An identity disagreement is refused** and the error names both sides
  (SR-34a); every single-bit edit of the identity block is rejected *with the
  caller supplying no identity*, so the AAD is what catches it rather than the
  cross-check (SR-24).
- **The header contains no filename** (SR-34c) — asserted, so adding one later
  breaks a test rather than passing as a convenience.
- The writer is **single-pass**: assert it never seeks backwards, which is what
  keeps a direct-to-object-store upload possible (§6.2).
- **Compression ratio on the SR-9 corpus** is measured against v1 whole-stream
  compression, and the regression is within the band §6.7 predicts. If a real
  IFC model loses materially more than a few percent, block size is wrong or v2
  should not default on for compressible content.
- **A v1 version still reads, and is tested as a first-class path** (§6.9) —
  not as a deprecated one. This test outlives every other test here, because v1
  content is permanent.

---

## 11. Decisions

All settled as of 2026-09-26. Recorded with their reasoning so they are not
reopened by accident.

| | Decision |
|---|---|
| **D1** | **Format v2 is in scope.** §6, SR-23 – SR-38, stage S5. |
| **D2** | **Apply all appropriate migrations.** `compressed`, `encrypted`, `storage_format` and `key_id` all land in S0 (§4) — one migration, written once. |
| **D3** | **128 KiB sample, 10% minimum gain** (SR-6). Confirmed as defaults; §9 may adjust them. |
| **D4** | **A cold ranged read restores the whole object**, then serves locally (SR-18). No ranged object-store GET, and no restore heuristic to tune. |
| **D5** | **Compression stays a per-deployment setting**, on by default, opt-out only. No per-tenant or per-folder policy; the per-version record makes it expressible but nothing exposes it. |
| **D6** | **1 MiB blocks** (§6.6). |
| **D7** | **A signature fast path** for formats compressed by construction (SR-6a/6b, §3.1), ahead of the trial deflate — recognition of the bytes, not a content type, so SR-5 stands. |
| **D8** | **No rewrite pass.** v1 content is left alone and v2 populates naturally (§6.9). |

Two of these have consequences that reach beyond the decision itself, repeated
here because they are the ones most likely to be forgotten:

- **D8 makes the v1 reader permanent.** Content never revised is never converted,
  so v1 is a first-class path to be maintained and tested indefinitely, not a
  deprecation. A mixed corpus is the steady state for backup and restore, not a
  transition.
- **D5 plus SR-2 means the deployment flag now only governs new writes.** That is
  the intended behaviour and the opposite of today's, where the flag silently
  reinterprets everything already stored.

## 12. Non-goals

- **No change to the permission model.** `get_stream` validates READ before
  resolving anything (`filesystem.cpp:907`); a range narrows what is returned and
  never widens what is reachable. Service-auth capability gating
  (`grpc_service.cpp:2218`) is unchanged — a ranged read is the same `read`
  capability as a whole read.
- **No change to the tenancy model**, the schema-per-tenant arrangement, or the
  event contract.
- **No rewriting of stored data** (SR-22, D8). No flag day, no re-encrypt
  window, and no migration pass over the existing corpus — v1 content is read as
  it always was, permanently (§6.9).
- **No per-tenant or per-folder compression policy** (D5). Compression stays a
  deployment setting, on unless opted out.
- **Not a compression-algorithm change.** Replacing zlib with zstd is a separate
  question with its own compatibility story, and it is not made easier or harder
  by anything here.
