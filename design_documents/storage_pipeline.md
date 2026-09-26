# The storage pipeline — what is applied to stored bytes, and reading part of them

**Status:** Specification — implementation-ready, staged. Nothing implemented yet.
**Scope:** `file_engine_core`. One additive schema migration, two additive proto
fields, two additions each to `IStorage` and `IObjectStore`. No ACL change, no
new permission, no new capability, no change to the tenancy model.
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
  **false**). With it false, a ranged read of an encrypted version falls back to
  a scan — correct, authenticated up to where it stops, merely slower.

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
  ADD COLUMN IF NOT EXISTS compressed BOOLEAN NOT NULL DEFAULT <current compression flag>;
ALTER TABLE "<tenant>".versions
  ADD COLUMN IF NOT EXISTS encrypted  BOOLEAN NOT NULL DEFAULT <current encryption flag>;
```

The defaults are **rendered from the deployment's live configuration at migration
time**, not written as literals. That is SR-3: the backfill cannot know what was
applied to a row written before the column existed, and the only safe assumption
is the one every existing read is already making.

A third column is **not** specified here but is the obvious next one and should
be decided while this migration is being written rather than added by a second
one later: `key_id`, which is the prerequisite for ever rotating the encryption
key without rewriting every blob (§9-D2).

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

## 6. Stages

Each stage is independently shippable and independently valuable. The gate
column is what must be true before the next stage is safe.

| Stage | Content | Gate it opens |
|---|---|---|
| **S0** | **SR-1 – SR-4.** The two columns, the live-flag backfill, both read paths switched from configuration to the version record. | Everything. Also fixes defect **A** on its own. |
| **S1** | **Measure** (§7). Corpus composition by stored bytes, real zlib ratios per format, CPU attributable to deflate/inflate. | Decides whether S2 is scheduled at all. |
| **S2** | **SR-5 – SR-9.** The trial deflate, wired into `put` and the streaming writer, with the §3 SR-9 regression corpus. | Media and other already-compressed content becomes uncompressed, and therefore seekable. |
| **S3a** | **SR-10 – SR-16, SR-20, SR-21.** Proto fields, the windowed sink with early stop, `total_size` / `range_method`, 416/clamp, the `finish()` rule, audit range fields, `python_interface`. **Scan-only — no format change, no security change.** | Correct ranges for every deployment. Stops discarded bytes crossing gRPC. |
| **S3b** | **SR-17.** True seek for uncompressed-encrypted versions; the seeking decryptor; `allow_unauthenticated_ranges` off by default; the audit integrity flag. | O(1) seek for media, on a deployment that accepts the trade. |
| **S4** | **SR-18, SR-19.** Ranged object-store reads and the restore heuristic; the amplification bounds and their metrics. | Safe to expose ranges to an unauthenticated door. |
| **S5** | **Storage format v2** — chunked, per-block compressed and independently authenticated, with an index. Deferred; specified in `PROPOSAL_byte_range_reads.md` §8. | Retires SR-17's trade entirely: random access and authentication stop being in tension. |

**S0 is the prerequisite in the strict sense** — it is a defect fix, it is
required before S2 can exist, and it should be scheduled on its own merits even
if every other stage is dropped.

**S0 → S2 → S3a is the shortest path that delivers the general optimisation**,
and S3a alone removes the largest practical cost (a gigabyte crossing a process
boundary to be discarded).

### 6.1 What each downstream consumer needs

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

## 7. Measurement (S1)

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

## 8. Acceptance criteria

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
- With `allow_unauthenticated_ranges` false, a ranged read of an encrypted
  version is served by scan and is reported as such.
- The audit event carries the range and the integrity flag.

---

## 9. Remaining decisions

**D1 — Is S5 (format v2) in scope?** It is the correct end state and the largest
piece. If it is not going to be built, **SR-17's `allow_unauthenticated_ranges`
stops being a transitional flag** and must be documented as a permanent
deployment posture, with the integrity consequence stated in the operator
documentation rather than in a proposal. This is the one decision that changes
what the specification means rather than when it lands.

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

## 10. Non-goals

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
