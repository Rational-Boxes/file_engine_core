# Proposal: byte-range reads in the core gRPC API

**Status:** Design proposal — for review, nothing implemented
**Scope (cross-repo):** **`file_engine_core`** (proto, `IStorage`, `IObjectStore`,
`FileSystem::get_stream`, `GRPCFileService`, audit); `python_interface`
(`get_stream(offset, length)`); `http_bridge` (a real `Range`, replacing the
read-and-discard one); `webdav_bridge` (GET `Range`); `share_service` and
`convert_search_ai` (consumers). No schema change and no ACL change.

> **This is a deliberate reopening of "the core does not change."**
> `share_service/design_documents/OUTSIDE_SHARE_LINKS.md` §4 and its §14 say that
> a milestone wanting a core change is the signal to reopen §4 *deliberately
> rather than quietly*, and `convert_search_ai/design_documents/MEDIA_SHARE.md`
> §3-R16 declined the change once already and worked around it with a disk
> cache. That workaround stands on its own merits, but it is a cache in front of
> a missing capability, and the capability is missing platform-wide — not just
> for media. This document proposes adding it properly.

---

## 1. What is being asked for

> Serve bytes *m..n* of a stored file without reading, decrypting,
> decompressing or transmitting bytes *0..m-1*.

Three callers want it today, and none of them is exotic:

| Caller | Wants | Today |
|---|---|---|
| `http_bridge` `getContent` | HTTP `Range` for downloads and in-browser media | streams the whole file from the core and **discards** everything before the window (`src/http_server.cpp:774–789`) |
| `webdav_bridge` | `Range` on GET — macOS Finder, video players, and resumable clients all send it | same, or absent |
| `share_service` media door | seeking in a published video | avoided entirely by caching the whole rendition on local disk (`MEDIA_SHARE.md` §6.6) |

The cost of the workaround, concretely: a viewer seeking to the 90% mark of a
1.2 GB video makes the core read, decrypt, decompress and hand over 1.08 GB that
is thrown away — **per seek, per viewer**.

---

## 2. What exists today

`GetFileRequest` (`proto/fileservice.proto:328`) carries `uid`,
`version_timestamp` and `auth`. There is no offset and no length. It is shared by
the unary `GetFile` and by `StreamFileDownload`
(`proto/fileservice.proto:22`, `:65`).

`GRPCFileService::StreamFileDownload` (`core/src/grpc_service.cpp:2212`) is a
thin wrapper: it resolves tenant/user/roles, calls
`FileSystem::get_stream`, and writes each emitted chunk into a
`GetFileResponse`. `FileSystem::get_stream`
(`core/src/filesystem.cpp:880`) does the real work:

1. `validate_user_permissions(..., Permission::READ, ...)`;
2. resolve the version (explicit `version_timestamp`, else current);
3. resolve the storage path;
4. if the file is not on local disk, restore it **whole** from the object store,
   then fall through (`:956`);
5. open the local file, and stream **disk bytes → decrypt → decompress → emit**,
   with every emit bounded to `kMaxStreamChunkBytes` = 1 MiB
   (`core/src/filesystem.cpp:878`, `:892`).

So the read path is already a streaming pipeline with a bounded sink. It has no
notion of *where in the plaintext* it is.

---

## 3. The obstacle is the storage format, not the RPC

Adding two fields to a proto message is trivial. Making them mean what a caller
expects — *"this costs O(length), not O(offset+length)"* — runs into the on-disk
format, and **this is the part of the proposal that needs a decision.**

A stored version is written compress-then-encrypt, and read decrypt-then-decompress:

```
plaintext ──zlib deflate──> compressed ──AES-256-GCM──> [ IV(12) | ciphertext | tag(16) ]
```

### 3.1 zlib is not randomly accessible

`CompressStream` / `DecompressStream` (`core/include/fileengine/crypto_utils.h:39`,
`:53`) are a plain zlib deflate/inflate pair, and the header says so: the output
is *"a standard zlib stream"*. A zlib stream has **no seek points and no index**.
Byte *m* of the plaintext cannot be located without inflating everything before
it. This is not an implementation gap; it is the format.

### 3.2 GCM authenticates the whole object, and only at the end

`DecryptStream` (`crypto_utils.h:83`) consumes `[IV || ciphertext || tag]` and
verifies the tag at `finish()`. Its own header already warns that plaintext is
emitted *before* authentication completes and that a caller streaming to a
network **must treat a `finish()` failure as "discard what was sent"**.

GCM is CTR underneath, so ciphertext at a known offset *can* be decrypted in
isolation by computing the right counter block — the confidentiality layer seeks
fine. **The authentication layer does not.** A partial read cannot produce or
check a tag that covers only the part read. So a ranged read over a
GCM-encrypted object is, unavoidably, an **unauthenticated** read unless the
format changes.

That is a genuine security property being given up, not a technicality: the tag
is what detects a corrupted or tampered blob on disk or in the object store.

### 3.3 Where that leaves us

| Stored as | Confidentiality seek | Integrity | True random access? |
|---|---|---|---|
| plain (no encryption, no compression) | n/a | n/a | **yes** — `pread` |
| encrypted only | yes (CTR counter arithmetic) | **no** — tag covers the whole object | yes, unauthenticated |
| compressed (± encrypted) | — | — | **no** — zlib has no seek points |

A deployment with both enabled — which is the interesting one — gets nothing from
a naive offset field.

---

## 4. Decisions proposed

| Topic | Decision |
|---|---|
| The RPC | Add `offset` / `length` to `GetFileRequest`, and range metadata to `GetFileResponse`. Additive, wire-compatible, optional (§5). |
| The contract | **A range is always honoured; only the cost varies.** The server never refuses a range because the format is awkward — it satisfies it, and reports how it did (§6). |
| Honesty about cost | The response states whether the range was served by **seek** or by **scan**, so a caller (and an operator) can see the difference instead of inferring it from latency (§6.3). |
| Tier 1 | **Server-side scan-and-skip** for every format. Immediate, correct, and already a large win: the discarded bytes stop crossing gRPC (§6). |
| Tier 2 | **True seek for the formats that allow it**, plus *stop compressing incompressible content* — which is what makes media files seekable in practice (§7). |
| Tier 3 | **Storage format v2: a chunked, independently-authenticated, independently-compressed block format** with an index. The principled answer; new writes only; old files keep working (§8). |
| Integrity | An unauthenticated ranged read is **opt-in per deployment** and **recorded in the audit event**. It is never the silent default (§7.3). |
| Cold objects | Ranged `GET` against the object store rather than restoring the whole object (§9). |
| Abuse | Ranged reads are an amplification vector on tiers 1–2 and get an explicit bound (§11.2). |

---

## 5. The API change

```proto
message GetFileRequest {
    string uid = 1;
    string version_timestamp = 2;
    AuthenticationContext auth = 3;

    // Byte range over the PLAINTEXT, half-open [offset, offset+length).
    // offset 0 + length 0 == the whole file, so existing callers are unchanged.
    int64 offset = 4;   // >= 0
    int64 length = 5;   // 0 = to end of file
}

message GetFileResponse {
    bool   success = 1;
    string error = 2;
    bytes  data = 3;

    // Set on the FIRST frame of a stream only; absent/zero on later frames.
    int64  total_size   = 4;   // plaintext length of the whole version
    int64  range_start  = 5;   // first plaintext byte in this response stream
    int64  range_length = 6;   // number of plaintext bytes that will follow
    bool   ranged       = 7;   // true when the server applied a range
    string range_method = 8;   // "seek" | "scan" — how it was satisfied (§6.3)
}
```

Notes that matter:

- **Additive and optional.** Proto3 defaults make an old client's request
  `offset = 0, length = 0`, which is exactly today's behaviour. No consumer has
  to change to keep working, which is what allows this to land before the
  consumers do.
- **The range is over plaintext**, never over stored bytes. A caller must never
  need to know whether the deployment compresses or encrypts.
- **`total_size` removes a round-trip.** Today a bridge must `Stat` to learn the
  size before it can answer a `Range` request, and a media player asks for
  `bytes=0-` first precisely to discover it. Returning it on the first frame
  collapses that into one call.
- **Out-of-range is an error, not a truncation.** `offset >= total_size` returns
  `success = false` with a distinguishable error the bridges map to
  **416 Range Not Satisfiable**. `offset + length > total_size` is **clamped**,
  which is what HTTP requires (`bytes=100-999999` on a short file is legal).
- **`GetFile` (unary) honours it too**, and gains a genuinely new capability:
  reading a window out of a file larger than the gRPC message limit, without
  streaming. Small but useful — it is how a caller reads a header, a magic
  number, or a trailer cheaply.

### 5.1 `IStorage` and `IObjectStore`

`IStorage` (`core/include/fileengine/IStorage.h:34`) has only
`read_file(storage_path) -> vector<uint8_t>` — whole object. Two additions:

```cpp
// Stored-byte range, not plaintext: the caller (FileSystem) does the mapping.
virtual Result<void> read_file_range(const std::string& storage_path,
                                     int64_t offset, int64_t length,
                                     const std::function<bool(const uint8_t*, size_t)>& on_chunk,
                                     const std::string& tenant = "") = 0;
virtual Result<int64_t> file_size(const std::string& storage_path,
                                  const std::string& tenant = "") = 0;
```

`IObjectStore` already has `read_file_stream` (`IObjectStore.h:67`); it gains the
same offset/length pair, which for S3 maps directly onto a `Range` header (§9).

---

## 6. Tier 1 — honest ranges for every file, immediately

**Every format gets a correct range on day one**, by applying the window at the
emit sink rather than at the read. `FileSystem::get_stream` already funnels every
byte through one lambda (`filesystem.cpp:892`); the change is to give that lambda
a plaintext cursor, drop bytes before `offset`, pass through bytes inside the
window, and **stop the read early** once the window is filled.

Early termination is the part that makes this worth doing on its own: the read
loop already aborts when `on_chunk` returns false (`:897`, `:1045`), so a request
for the first 2 MB of a 4 GB file reads 2 MB and stops — a tail-seek is the only
case that still scans.

### 6.1 What this is and is not worth

| | Before | Tier 1 |
|---|---|---|
| Bytes read from disk | whole file | `offset + length` (early stop) |
| Bytes decrypted / decompressed | whole file | `offset + length` |
| **Bytes across gRPC** | **whole file** | **`length`** |
| Bytes to the HTTP client | `length` | `length` |

The gRPC row is the one that changes the system's behaviour: serialising,
copying and transmitting a gigabyte between two processes is far more expensive
than reading it, and it is what currently makes a seek in a large file visible as
a stall in unrelated traffic. Tier 1 removes that for every deployment, every
format, with no format change and no security trade.

It does **not** make a tail-seek cheap on a compressed or encrypted file. That is
tiers 2 and 3.

### 6.2 Correctness detail: the GCM tag on a partial read

If the read stops early, `DecryptStream::finish()` is never reached and the tag
is never verified. Tier 1 must therefore:

- **not** call `finish()` on an early-terminated ranged read (calling it on a
  truncated stream is a guaranteed spurious failure), and
- report `range_method = "scan"` and set the unauthenticated flag in the audit
  event (§11.1), because the bytes it returned were not covered by a verified
  tag.

A **full-file** read (`offset = 0, length = 0`) keeps today's behaviour exactly,
tag verification included. This is the line that must not blur: adding ranges
must not quietly weaken the integrity guarantee of ordinary downloads.

### 6.3 `range_method` is part of the contract

`"seek"` means cost proportional to `length`; `"scan"` means cost proportional to
`offset + length`. Exposing it:

- lets `http_bridge` and `share_service` decide whether to cache locally (a
  `"scan"` answer is a good reason to; a `"seek"` answer is a reason not to
  bother), turning `MEDIA_SHARE.md` §6.6's cache from a necessity into an
  optimisation with an actual signal behind it;
- gives operators a metric — a deployment whose media reads are all `"scan"` is
  one compression setting away from being fast (§7.1);
- and keeps the document honest. A range API that silently costs O(offset) is
  worse than none, because callers design around the promise rather than the
  behaviour.

---

## 7. Tier 2 — true seek where the format allows it

### 7.1 The cheapest win in this document: stop compressing incompressible data

> **Promoted to its own document.** This was endorsed on its own merits as a core
> performance change, and is now specified in
> **`PROPOSAL_selective_compression.md`** — including the prerequisite this
> section did not surface: the read path currently decides whether to decompress
> by asking the *current* configuration rather than recording what was applied,
> which is a latent silent-corruption bug and a hard blocker for any mixed
> corpus. The summary below stands; that document is authoritative.

Compression is currently all-or-nothing per deployment
(`IStorage::is_compression_enabled`, `IStorage.h:45`). Applying zlib to a VP9
WebM, an MP3, a JPEG, an MP4 or a ZIP costs CPU on write and on **every** read
and returns approximately nothing — these formats are already entropy-coded.

Skipping compression for them:

1. **makes exactly the files that need seeking seekable** (media is the entire
   motivating case);
2. removes a decompression pass from every read of every media file;
3. shrinks nothing, because there was nothing to shrink.

The mechanism settled on is a **trial deflate** rather than a content-type
table: the core has no MIME concept in `FileInfo` and deliberately should not
gain one, so the decision is made from the bytes. The per-version `compressed`
marker it requires is needed by tier 3 regardless — it is tier 3's first
increment, and it is also the fix for the latent bug above.

> This is worth doing even if nothing else in this proposal is accepted, and it
> is tracked separately for that reason.

### 7.2 Seeking an encrypted, uncompressed object

With compression out of the way, a GCM object is CTR-mode ciphertext with a
fixed 12-byte IV prefix: plaintext byte *m* is ciphertext byte *m*, decryptable
by starting the counter at block `m / 16` (with GCM's standard offset for the
J0 block) and discarding `m % 16` bytes. Cost is O(1) plus one `pread`.

This needs a small addition to `crypto_utils`: a seeking decryptor that takes a
starting plaintext offset and does **not** attempt tag verification. Keeping it a
separate type from `DecryptStream` is deliberate — an `offset` parameter on the
existing class would make it possible to get an unauthenticated read by passing
a number, where a distinctly-named type makes every call site state what it is
doing.

### 7.3 The integrity decision, stated plainly

A tier-2 ranged read returns bytes whose GCM tag was never checked. The
consequences are narrow but real: a corrupted disk block or a tampered object
store would be caught on a full download and not on a ranged one.

Proposed posture:

- **Off by default.** `storage.allow_unauthenticated_ranges` (default `false`).
  With it off, a ranged read of an encrypted object falls back to tier 1 —
  correct, authenticated up to the point it stops, and merely slower.
- **Recorded, always.** The audit event for a ranged read carries the range and
  whether the bytes were authenticated (§11.1). "We cannot tell which reads were
  unauthenticated" is not an acceptable state for this platform.
- **Superseded by tier 3.** Once a version is stored in the v2 block format, its
  ranged reads are authenticated per block and the flag is irrelevant for it.

Turning the flag on is a deployment saying *"for ranged reads I will take
integrity from the filesystem and the object store rather than from GCM."* For a
single-tenant box with checksummed storage that is defensible. It should be a
sentence someone wrote down, not a default someone inherited.

---

## 8. Tier 3 — storage format v2: chunked, independently authenticated

The principled answer, and the one that makes every trade above disappear.

Instead of one deflate stream inside one GCM object, store a version as a
sequence of **independent blocks** over fixed-size plaintext windows:

```
[ header: magic | version=2 | plaintext_block_size | flags | block_count ]
[ block 0: IV(12) | ciphertext | tag(16) ]
[ block 1: IV(12) | ciphertext | tag(16) ]
...
[ index: for each block, its stored offset and stored length ]   <- trailer
```

- Each block is **compressed independently** (and skipped for that block if it
  does not shrink — §7.1 applied per block rather than per file).
- Each block is **encrypted independently with its own IV and its own tag**, so a
  ranged read verifies every block it touches. **Random access and
  authentication stop being in tension.**
- The index makes plaintext offset → stored offset O(1). It is a trailer so that
  writing stays single-pass and streaming, exactly as today.
- Block size is a tunable trade: 1 MiB matches `kMaxStreamChunkBytes`, bounds
  read amplification to at most one block either side of the window, and costs
  28 bytes of overhead per MiB (~0.003%).

**Compatibility is the easy part**, because the format is self-describing:
existing files have no v2 header and are read by exactly today's code path, which
stays. New writes use v2. Nothing is migrated; old versions age out naturally
through the existing version lifecycle, and a deployment that wants them
converted can re-write them with an offline tool. **No flag day, no downtime, no
"re-encrypt everything" window** — which is the property that makes this
proposable at all.

Independent benefits worth noting, since they change the cost/benefit:

- **A corrupted block is localised.** Today one flipped bit fails the tag for the
  whole file; with v2 the damage is one block, which is the difference between a
  lost video and a glitched second of it.
- **Parallel reads and writes** become possible — blocks are independent.
- **Deduplication and delta sync** get a natural unit, if that is ever wanted.

---

## 9. The cold path

`FileSystem::get_stream` currently restores the **entire** object from the object
store before serving anything (`filesystem.cpp:956–995`), with an explicit
comment that a whole-buffer read would defeat streaming. A ranged read of an
archived 4 GB file should not pull 4 GB from S3.

S3 (and MinIO) support ranged `GET` natively, and the stored-byte range is
computable from the plaintext range on tiers 2 and 3. So:

- **tier 3 / tier 2:** ranged `GET` for exactly the blocks needed; no local
  restore at all for a small window. This is the largest single cost saving in
  the proposal for archived content.
- **tier 1:** keep the existing restore-then-stream, because a scan needs the
  bytes from the start anyway.
- **Restore policy needs a thought**, and this document does not settle it: a
  scrubbing viewer would otherwise repeatedly ranged-GET an object that should
  have been restored once. Suggest restoring on the *second* ranged miss within
  a window, which is a heuristic, not a decision (§15-Q4).

---

## 10. Consumers

| Repo | Change |
|---|---|
| `python_interface` | `get_stream(uid, version="", offset=0, length=0)`; expose `total_size` / `range_method` from the first frame. Default arguments keep every existing call working. |
| `http_bridge` | Replace the read-and-discard window in `getContent` (`http_server.cpp:774–789`) with a real ranged request. **Also fix the malformed response:** it currently emits `Content-Range: bytes <start>-/*` for an open-ended range — no last-byte-pos and unknown total — which is not a valid `Content-Range` and is why media seeking through the bridge is unreliable. With `total_size` on the first frame the header becomes `bytes s-e/total`, and `Content-Length` can be set. |
| `webdav_bridge` | Honour `Range` on GET. Finder, Office and several sync clients send it; today they get a whole file or nothing. |
| `share_service` | The media cache (`MEDIA_SHARE.md` §6.6) stays, but as an **edge cache**, not as the mechanism that makes seeking possible. Its justification changes from "the core cannot do ranges" to "not crossing a process boundary for a hot object", and `range_method` tells it when caching is actually worth it. |
| `convert_search_ai` | Reading a header or a trailer without fetching the file — a cheap container probe before deciding to transcode. Not required; enabled. |

---

## 11. Security

### 11.1 Audit gains a range, and an integrity flag

`StreamFileDownload` currently emits `download_stream` with no size information
(`grpc_service.cpp:2261`). A ranged read should record `offset`, `length`,
`total_size`, and whether the returned bytes were authenticated.

This is an **improvement to the forensic record**, not a cost: today the audit
log cannot distinguish "read the whole document" from "read 4 KB of it", and for
a departed-employee review that is precisely the distinction that matters. It
also means the unauthenticated-range posture (§7.3) is answerable from the log
rather than from configuration.

One caution: a scrubbing media viewer generates many ranged reads, and one audit
event each would flood a hash-chained log. Coalesce per RPC, and let the door
above (which knows about sessions) decide what a *view* was —
`MEDIA_SHARE.md` §12 already takes this position for the media door.

### 11.2 Ranges are an amplification vector on tiers 1–2

On tier 1, a request for `bytes=<end-1>-<end>` costs the server a full-file
read, decrypt and decompress to return two bytes. **A loop of tail-range
requests is a cheap client-side denial of service**, and it is not hypothetical:
it is what a naive media player does on a slow connection.

Bounds, all of which belong in the core rather than in each door:

- a per-request **minimum window** below which the server serves a larger
  aligned block anyway (reading 1 MiB is no more expensive than reading 2 bytes
  by scan, and it makes the next request a hit);
- a per-caller cap on **scan bytes per interval** — the meaningful unit is bytes
  *scanned*, not requests, because that is what the work actually is;
- `range_method = "scan"` exposed as a metric so this is visible before it is a
  problem.

`MEDIA_SHARE.md` §6.9 meters the same pressure one layer up; the core-side bound
exists because the core has other callers, and because a bound that only one door
enforces is not a bound.

### 11.3 What does not change

Permission evaluation is untouched: `get_stream` validates READ before resolving
anything (`filesystem.cpp:907`), and a range narrows what is returned — it never
widens what is reachable. Service-auth capability gating
(`grpc_service.cpp:2218`) is likewise unchanged; a ranged read is the same
`read` capability as a whole read. **No new permission, no new capability, no
schema change.**

---

## 12. Testing

The parts that will actually break:

- **A range is byte-identical to the same window of a full download** — across
  plain, compressed, encrypted, and compressed+encrypted, and across a block
  boundary, at offset 0, at EOF-1, and spanning the whole file.
- **`offset >= total_size` is 416, and `offset + length > total_size` clamps.**
- **A full-file read still verifies the GCM tag**, and a corrupted object still
  fails it — the regression that would matter most and would be invisible.
- **An early-terminated read does not call `finish()`** and does not log a
  spurious tag failure.
- **`total_size` is the plaintext size** under every combination of compression
  and encryption. This is the field most likely to be quietly wrong on a
  compressed file.
- **Old clients are unaffected** — a request with no offset/length behaves
  exactly as before, asserted against the current behaviour rather than against
  the new code's idea of it.
- **v2 round-trips, and v1 files still read** (tier 3), including a v1 and a v2
  version of the *same file* coexisting.
- **A tail-range does not read the whole file** on tier 2/3 — asserted by
  counting bytes read from storage, not by timing.

---

## 13. Milestones

1. **B0 — the API, tier 1.** Proto fields; `FileSystem::get_stream` window +
   early stop; `total_size` / `range_method`; the `finish()` rule (§6.2); audit
   range fields; `python_interface`. **No format change, no security change.**
   Shippable alone, and already removes the discarded-bytes-across-gRPC cost.
2. **B1 — the consumers.** `http_bridge` real `Range` (**and the malformed
   `Content-Range` fix**), `webdav_bridge` GET `Range`. Independently valuable;
   fixes in-browser media seeking for authenticated users, which is a
   longer-standing complaint than the share-link case.
3. **B2 — do not compress incompressible content** — now
   `PROPOSAL_selective_compression.md`, milestones **C0–C3**. Its **C0** (record
   the transform per version) is a prerequisite for tier 2 *and* a standalone bug
   fix; C2 is what makes media uncompressed and therefore seekable. Tracked in
   that document, sequenced here.
4. **B3 — tier 2 seek** for uncompressed-encrypted objects, the seeking
   decryptor, `allow_unauthenticated_ranges` defaulting off, and the audit
   integrity flag.
5. **B4 — ranged object-store reads** (§9) plus the restore heuristic.
6. **B5 — storage format v2** (§8): the block format, the index, the writer, the
   self-describing reader, and the offline re-writer. The largest piece, and the
   one that retires §7.3's trade.
7. **B6 — abuse bounds and metrics** (§11.2): minimum window, scan-bytes
   budget, `range_method` and scanned-bytes metrics.

B0–B2 are self-contained and together deliver most of the practical benefit.
B5 is the one that deserves its own review.

---

## 14. What this means for `MEDIA_SHARE.md` §3-R16

R16 declined a core change and cached published renditions on `share_service`'s
disk instead. That reasoning was sound **given no ranged RPC**, and the cache
should stay — but its justification changes, and the document should say so:

- **Before:** the cache exists *because* the core cannot serve ranges.
- **After:** the core can serve ranges; the cache exists because not crossing a
  process boundary for a hot object is faster, and because it is the natural
  place a CDN would sit later. It becomes an optimisation with a measurable
  trigger (`range_method`), not a workaround.

The §6.6 requirement that **the cache never short-circuits the authority
re-check** is unaffected and remains non-negotiable.

---

## 15. Open questions

**Q1 — Is tier 3 in scope, or is tier 2 the destination?** Tier 3 is the correct
answer and the largest piece of work. Tier 2 plus §7.1 delivers seekable media on
a deployment willing to accept §7.3's trade. If tier 3 is not going to be built,
§7.3's flag stops being temporary and should be documented as permanent.

**Q2 — Block size for v2**, if built: 1 MiB aligns with `kMaxStreamChunkBytes`
and bounds read amplification; 256 KiB matches the current disk read size and
costs 4× the tag overhead (still ~0.01%). Wants measuring rather than arguing.

**Q3 — Does `GetFile` (unary) accept ranges too?** §5 says yes, and it is nearly
free. The argument against is one more way to read bytes.

**Q4 — The cold-path restore heuristic** (§9): when does repeated ranged access
to an archived object trigger a full restore? Proposed "second miss in a window";
no measurement behind that number yet.

**Q5 — Should compression become per-version rather than per-deployment
generally?** §7.1 needs the per-version marker anyway. Going further — letting a
tenant or a folder set it — is a larger policy question this document does not
open.
