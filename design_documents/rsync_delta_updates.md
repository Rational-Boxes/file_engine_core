# Rsync-Style Delta Updates for Large Files — Research & Design Notes

**Status:** Research / pre-design
**Branch:** `feature/rsync-delta-updates`
**Date:** 2026-06-17

## 1. Goal

Add a gRPC endpoint to `FileService` that lets a client update a large file by sending only the differences between its local copy and the server's current (or any prior) version, using the rsync binary-delta algorithm. The result must produce a new version row in the existing version history — i.e. delta updates are first-class writes that participate in versioning, ACL enforcement, and S3 backup just like a normal `PutFile`.

Primary motivation: large files (gigabyte-scale archives, VM images, datasets) where a full re-upload over `StreamFileUpload` wastes bandwidth and storage churn when only a small region has changed.

## 2. The rsync binary-delta algorithm — short refresher

Two roles, with the file always reconstructed on the side that *doesn't* have the up-to-date copy:

1. **Receiver (has old file B)** splits B into fixed-size blocks (typically 512 B – 8 KiB; auto-tuned to √filesize). For each block it computes:
   - A **weak rolling checksum** (Adler-32-like, cheap, O(1) to slide one byte).
   - A **strong checksum** (MD4/MD5/BLAKE2 — cryptographic strength to disambiguate collisions).
   These pairs form a **signature** of B, sent to the sender.
2. **Sender (has new file A)** walks A byte-by-byte, maintaining a rolling weak checksum over a sliding window of block-size. On each step:
   - If the weak hash matches a block in the receiver's signature, verify with the strong hash.
   - On a verified match, emit a **COPY(offset, length)** instruction referencing the receiver's block, advance the window by block-size.
   - Otherwise, emit a **LITERAL(bytes)** instruction for the byte under the trailing edge, slide by 1 byte.
   The output stream is the **delta**.
3. **Receiver** applies the delta against B to reconstruct A: COPY instructions reach into B, LITERAL instructions are taken from the stream.

Key property: signature is small (~1/block_size of file), delta is bounded by the changed region plus a small per-instruction overhead. Both are streamable.

## 3. Mapping rsync to our architecture

The asymmetry matters. In the rsync wire protocol the sender and receiver each compute one half. In our case the **client has the new file** (it's pushing an update) and the **server has the old file** (the current version we want to base on). So the roles are:

| Step | Side | Cost |
|---|---|---|
| 1. Generate signature of basis version | **Server** | O(file size) read + hash, ~1/Bsize bytes out |
| 2. Compute delta of new file vs signature | **Client** | O(file size) walk + rolling hash |
| 3. Apply delta to basis to produce new content | **Server** | O(file size) random reads into basis |

This is the reverse of how rsync-the-program works locally (because there the receiver has the network-distant side), but it's the right shape for "client uploads a delta against a server-side basis." It's the same shape `zsync` and `Dropbox`'s delta sync use.

The alternative — client sends signature of its old copy, server computes delta against new file the server doesn't have — doesn't make sense here. The client is the one initiating the change.

## 4. Proposed gRPC surface

Three RPCs, mirroring the three algorithm phases. Streaming on both sides so neither party has to hold an entire signature or delta in memory.

```proto
// Get a rolling-hash signature for a specific version of a file.
// Client uses this as input to its local delta computation.
rpc GetDeltaSignature(GetDeltaSignatureRequest)
    returns (stream DeltaSignatureChunk);

// Upload a delta computed against a known basis version; server applies it
// and creates a new version row. Returns the new version's timestamp.
rpc StreamDeltaUpload(stream DeltaUploadRequest)
    returns (DeltaUploadResponse);

// Optional: download a delta from version A -> version B without
// re-transmitting the whole file. Useful for clients pulling updates.
rpc StreamDeltaDownload(DeltaDownloadRequest)
    returns (stream DeltaDownloadChunk);
```

Message sketches (final shapes TBD):

```proto
message GetDeltaSignatureRequest {
  string uid = 1;
  string basis_version_timestamp = 2; // empty = current head version
  uint32 block_size = 3;              // 0 = server picks; clamp 512..65536
  HashAlgorithm strong_hash = 4;      // MD5 default; BLAKE2B preferred
  AuthenticationContext auth = 5;
}

message DeltaSignatureChunk {
  string uid = 1;
  string basis_version_timestamp = 2; // echoed in first chunk
  uint32 block_size = 3;              // echoed in first chunk
  uint64 file_size = 4;               // echoed in first chunk
  bytes signature_data = 5;           // serialized (weak32, strong[N]) pairs
  bool last_chunk = 6;
  string error = 7;
}

message DeltaUploadRequest {
  // First message: header only.
  string uid = 1;
  string basis_version_timestamp = 2;
  uint32 block_size = 3;
  uint64 expected_new_size = 4;       // for sanity check / progress
  bytes new_file_sha256 = 5;          // end-to-end integrity check
  AuthenticationContext auth = 6;

  // Subsequent messages: delta instruction stream.
  bytes delta_data = 7;               // serialized COPY/LITERAL ops
  uint32 chunk_index = 8;
  bool last_chunk = 9;
}

message DeltaUploadResponse {
  bool success = 1;
  string new_version_timestamp = 2;
  uint64 bytes_written = 3;
  uint64 bytes_reused_from_basis = 4; // for telemetry
  string error = 5;
}
```

Notes:
- `basis_version_timestamp` empty means "current head," matching the convention in `GetFileRequest` (`proto/fileservice.proto:199`).
- End-to-end SHA-256 of the reconstructed file is sent up-front by the client and verified server-side after reconstruction. This catches a corrupted delta or a basis mismatch that the rolling hash quietly missed.
- All three RPCs accept `AuthenticationContext` and follow the same trusted-upstream model used elsewhere.

## 5. Library choice — build vs. buy

Rolling hash + block matching is small but the *correct, fast* implementation has subtle invariants (window arithmetic, strong-hash truncation lengths, instruction framing). I strongly recommend NOT writing it from scratch.

Candidates:

| Option | Pros | Cons |
|---|---|---|
| **librsync** (Andrew Tridgell-blessed, used by rdiff, Dropbox, Duplicity) | Mature, stable C API, exactly the algorithm we want, packaged in Fedora/Debian/Arch | Pulls in a libc-style C dep; LGPL-2.1+ (compatible but requires attention if we ever go closed-source) |
| **xdelta3** | VCDIF format (RFC 3284), better compression on some inputs | GPL-2.0, different algorithm family (Bentley-McIlroy + secondary compression), heavier |
| **bsdiff/bsdiff4** | Excellent for binary patches | Not streaming; loads both files into RAM. Wrong shape for GB-scale. |
| **In-house** | No new dep | Months of work to match librsync's robustness; we'd own the bugs |

**Recommendation: librsync.** Its `rs_sig_*`, `rs_loadsig_*`, `rs_delta_*`, `rs_patch_*` job-based APIs are streaming-native — they consume and produce buffers in chunks, which maps 1:1 to gRPC stream messages. Dropbox famously used it in production for years before replacing it with their own implementation at massive scale; we are nowhere near that scale.

CMake integration: `pkg_check_modules(LIBRSYNC librsync)` and link `librsync.so`. Packaging: add `librsync-devel` (RPM) / `librsync2 / librsync-dev` (Debian) / `librsync` (Arch) to build deps; runtime dep on the shared lib. Already present in all three target distros, no third-party repos needed.

## 6. Integration with existing layers

### 6.1 FileSystem layer (`core/include/fileengine/filesystem.h`)

Three new methods, mirroring the RPCs:

```cpp
Result<std::vector<uint8_t>> compute_signature(
    const std::string& file_uid,
    const std::string& basis_version_timestamp,  // empty = head
    uint32_t block_size,
    const std::string& user,
    const std::vector<std::string>& roles,
    const std::string& tenant);

Result<std::string> apply_delta(  // returns new_version_timestamp
    const std::string& file_uid,
    const std::string& basis_version_timestamp,
    const std::vector<uint8_t>& delta_blob,
    const std::array<uint8_t, 32>& expected_sha256,
    const std::string& user,
    const std::vector<std::string>& roles,
    const std::string& tenant);

Result<std::vector<uint8_t>> compute_delta_between_versions(
    const std::string& file_uid,
    const std::string& from_version_timestamp,
    const std::string& to_version_timestamp,
    uint32_t block_size,
    const std::string& user,
    const std::vector<std::string>& roles,
    const std::string& tenant);
```

ACL semantics:
- `compute_signature` and `compute_delta_between_versions` require `VIEW_VERSIONS` + `READ` (and `RETRIEVE_BACK_VERSION` when the basis isn't head).
- `apply_delta` requires `WRITE` (it produces a new head version).

### 6.2 Storage layer

The existing `Storage::store_file()` / `S3Storage::store_file()` interfaces (`core/include/fileengine/storage.h`) take a full byte vector and return a path. The reconstructed new version is a full byte vector — so we can land in those existing methods unchanged. **No partial-write or random-write API is needed on the storage side.**

What we *do* need is to read the basis version's full content into memory (or, better, a memory-mapped buffer) so librsync's patch job can do COPY-instruction reads against it. The existing `Storage::read_file()` already returns the full plaintext (encryption and compression are transparent), so this works out of the box.

**Memory floor concern.** For a 10 GB basis we don't want to hold 10 GB in heap. Two mitigations to consider in implementation:
- **Memory-mapped read of the local storage path.** Local storage stores encrypted/compressed blobs — we'd need a temp file of the decrypted plaintext or a streaming decryptor over the mmap. Adds complexity.
- **Server-side temp file.** Decrypt+decompress the basis to a private temp file under the systemd `PrivateTmp` mount, mmap it, run librsync against it, then `store_file()` the result and unlink. Simpler. Cost: one extra full-file disk read+write per delta upload.

Recommend the temp-file approach for Phase 1. Revisit if profiling shows it's the bottleneck.

### 6.3 Database layer

No schema change required. `apply_delta` produces a new `versions` row via the existing `Database::insert_version()` path — same `(file_uid, version_timestamp, size, storage_path)` shape as a regular `put`. The delta machinery is invisible to the version table.

**Optional future enhancement (out of scope for Phase 1):** add columns `parent_version_timestamp` and `delta_storage_path` to the versions table to enable *storing* deltas instead of full files. That would multiply our storage efficiency for write-heavy version chains, at the cost of read amplification (reconstructing version N requires walking the chain). Not recommended for v1 — keep versions self-contained.

### 6.4 gRPC service layer

Follow the patterns already in `core/src/grpc_service.cpp` (`StreamFileUpload` around line 1272, `StreamFileDownload` around line 1334):
- Read-only-mode short-circuit at start.
- Extract auth from the first request message.
- `validate_user_permissions()` against the right permission set before doing real work.
- 64 KiB chunk size for output streams (`grpc_service.cpp:1359`), matching `StreamFileDownload`.

## 7. Versioning interaction

Two questions that need explicit answers, both already implied by the existing model but worth stating:

1. **What is the basis?** Default is the current head version (`basis_version_timestamp` empty). Allowing the client to pick an arbitrary prior version is useful when two clients are racing — the loser can re-base against the new head and retry without a full upload.
2. **Is the new version always head?** Yes. A successful `StreamDeltaUpload` produces a version exactly as a normal `PutFile` would: new timestamp, full storage path, async S3 backup queued through the existing background worker. The delta is *not* persisted; the reconstructed file is.

This keeps the read path (`GetFile`, `GetVersion`, `RestoreToVersion`) unchanged.

## 8. Security, ACL, encryption

- ACL: enforced at the FileSystem layer per §6.1. The gRPC handler is a thin permission check + transport layer, same as existing RPCs.
- Encryption: librsync operates on plaintext. The basis is decrypted into the temp file before signature/patch. The new reconstructed file is re-encrypted by the existing `Storage::store_file()` path. **No plaintext touches stable storage** other than the `PrivateTmp` working file, which is wiped at process exit by systemd.
- Trust model: the client is trusted to supply a valid delta. End-to-end SHA-256 of the expected reconstruction (sent in the upload header) is verified after patching. On mismatch we reject the version, do not insert the row, and unlink the temp output.
- The strong-hash choice in the signature matters. MD5 is rsync's historical default but is collision-broken. BLAKE2b-128 (librsync 2.2+) is fast and strong; recommend it as default with a fallback to MD5 only for compatibility if we ever support older clients.

## 9. Failure modes & edge cases

- **Basis version deleted between signature fetch and delta upload.** Signature request takes a read-lock-like snapshot conceptually; we can either (a) include the basis timestamp in the upload request and look it up fresh (fails cleanly if gone), or (b) pin via an existing version retention pin. (a) is simpler — fail fast with a clear error.
- **Client computes against the wrong basis (timestamp mismatch).** Server returns `INVALID_BASIS_VERSION` after a quick `Database::get_version_storage_path()` check before any work.
- **Delta apply succeeds but SHA-256 mismatches.** Discard, return `INTEGRITY_CHECK_FAILED`. No version row inserted.
- **Concurrent uploads on the same UID.** Existing put path already serializes via the database; same lock holds for delta apply since the insert is the same.
- **Empty / tiny files.** For files below some threshold (say, 4× block_size) the delta overhead exceeds the file size. Server should hint clients to fall back to `StreamFileUpload`. Could expose a `min_delta_size` server config.
- **librsync internal errors.** Map to gRPC `INTERNAL` with `error` populated; log full librsync error string via `ServerLogger`.

## 10. Performance & sizing

Back-of-envelope, 1 GiB file, 2 KiB blocks:
- Signature: ~512 K entries × (4 weak + 16 strong) bytes = ~10 MiB on the wire. Streaming-friendly.
- Delta (single 1 MiB edit in the middle): ~1 MiB + small framing overhead. Massive win vs. 1 GiB re-upload.
- Server-side temp file: 1 GiB write + 1 GiB read during patch + 1 GiB write of result = ~3 GiB local I/O per upload. Comparable to a normal put (1 GiB write + S3 backup read) — within an order of magnitude, acceptable.

Block size tuning: librsync's `rs_sig_args()` picks √filesize as a default; we should expose `block_size` in the proto but default to whatever librsync recommends. Force-clamp to [512, 65536] to bound signature size.

## 11. Open questions

1. Do we want bidirectional delta sync (server pushing deltas to clients via `StreamDeltaDownload`) in v1, or defer? My take: define the proto now, implement signature + upload first, ship download in v1.1 once the surface is proven.
2. Should we expose a `compute_delta_between_versions` RPC for offline diffing of two versions on the server? Useful for backup tooling and audit; cheap to add given librsync is already linked.
3. Telemetry: bytes-reused-from-basis is a great metric for showing the value of the feature. Wire it into the existing observability work landing on `monitoring` branch (re: `design_documents/monitoring_and_telemetry.md`).
4. Do we need a client-side helper library, or do we expect callers to drive librsync themselves? A thin C++ helper in `cli/` for `fileengine_cli` to demonstrate the protocol would be valuable for testing and as a reference impl.

## 12. Test plan (sketch)

New test file `tests/test_delta_updates.cpp`. Scenarios:
- Round-trip: random 100 MiB file, mutate 1 MiB region, upload delta, verify reconstructed SHA-256 matches.
- Append-only growth: simulate log-file growth, confirm bytes-reused is high.
- Whole-file rewrite: confirm graceful fallback / sensible delta size.
- Basis-version mismatch: ensure server rejects.
- ACL: missing `WRITE`, missing `VIEW_VERSIONS`, missing `RETRIEVE_BACK_VERSION` (when basis ≠ head).
- Encryption on/off, compression on/off — confirm delta works either way.
- Tenant isolation: signature for tenant A's file is not visible to tenant B.

Integration with the existing tenant-scoped harness in `tests/` and the comprehensive ACL suite (`tests/test_acl_rbac_comprehensive.cpp`) — same fixtures, new file.

## 13. Suggested implementation phasing

1. **Phase 1 — Signature + upload.** Add librsync dep, three FileSystem methods, two RPCs (`GetDeltaSignature`, `StreamDeltaUpload`), temp-file-based reconstruction, end-to-end SHA-256 verification, unit + integration tests. CLI subcommand `fileengine_cli delta-put <uid> <new-file>` as reference client.
2. **Phase 2 — Delta download.** `StreamDeltaDownload` for pulling version diffs.
3. **Phase 3 — Optional storage savings.** Versions-table extension for stored deltas. Gated by a config flag, off by default. Needs migration plan and careful read-path testing.

## 14. Internet research — what other systems actually do

After the initial design sketch above, I did a focused literature/internet sweep on real-world binary-delta protocols. The landscape is broader than just "rsync-style" — there are three meaningfully different algorithm families in active production use, each with different trade-offs for our use case. Findings below; revised recommendation at the end of this section.

### 14.1 The three families

| Family | How it splits the basis | What's matched | Resilience to insertions | Example implementations |
|---|---|---|---|---|
| **Rsync rolling-hash** (Tridgell 1996) | Fixed-size blocks on the *basis*; sender walks the new file with a rolling weak hash to find matches | Block-aligned regions in basis ↔ arbitrary offsets in new file | High — sliding window finds matches at any byte offset | librsync, rdiff, rsync(1), zsync |
| **Content-defined chunking (CDC)** | Both files split into variable-size chunks at content-determined boundaries (rolling hash → boundary condition) | Whole chunks by strong hash | High — boundary moves with content, so an insertion only resyncs the *one* chunk it falls in | Dropbox, casync, restic, borg, FastCDC |
| **Dictionary / suffix-array diffs** | Treat the basis as a literal dictionary | Arbitrary-length string matches via suffix array or LZ77-like scan | High but RAM-hungry | bsdiff, xdelta3, zstd `--patch-from` |

The original §3–§5 of this design assumed family 1 (rsync rolling-hash). The research shows that's a defensible default but not the only viable choice, and the picture for family 2 specifically deserves closer attention.

### 14.2 librsync — current state (family 1)

- **Maintenance**: actively maintained on GitHub at `librsync/librsync`. Latest tagged release is **2.3.4 (Feb 2023)**. ~1,675 commits to master. Notable production consumers: Dropbox (historically), rdiff-backup, Duplicity. ([source](https://github.com/librsync/librsync))
- **License**: LGPL-2.1. Dynamic linking is fine for our use; static linking would require source-availability of modifications.
- **API confirmed streaming-native**: `rs_job_t` state-machine driven by `rs_job_iter(job, rs_buffers_t*)`. Caller supplies input/output buffers; the job returns `RS_BLOCKED` when it needs more input or output space, `RS_DONE` on completion. This maps 1:1 onto gRPC bidirectional streams. ([source](https://librsync.github.io/api_streaming.html))
- **Buffer sizing**: docs recommend **≥32 KiB buffers** — smaller has high relative overhead. Our existing `StreamFileDownload` uses 64 KiB, which fits.
- **Hash algorithms**: librsync ships MD4 (legacy, default for compatibility), MD5, and **BLAKE2** (CC0-licensed, included in-tree). BLAKE2 is the right choice for new code — fast, no known cryptographic weaknesses, none of MD5's history.
- **Explicitly NOT in scope** for librsync: the rsync wire protocol, file metadata (perms, names), networking. It is purely the delta math. We'd own the gRPC framing — which is what we want.

### 14.3 zsync — the role-inversion variant

This was the most interesting finding. zsync uses the rsync algorithm but **flips who does the expensive work** ([paper](https://zsync.moria.org.uk/paper/)):

- Server pre-computes a **`.zsync` control file** (the signature) *once* at publish time.
- Each client downloads the control file and **does its own block-matching** against its local copy.
- Client then HTTP-range-requests only the missing byte ranges from the original published file.

Server cost: zero per-download CPU. Bandwidth: identical to rsync. Used widely for Linux ISO distribution.

**Relevance to us:** if signature computation becomes a hot path (we serve many delta uploads against the same head version), we could **cache the signature alongside the version row** instead of recomputing per upload. That gets us most of zsync's win without changing the protocol direction. Concretely: when a version is created, compute its signature in the background worker (alongside the existing S3 backup) and store it as a sidecar file. `GetDeltaSignature` then streams the cached blob instead of regenerating it. Worth adding as an optional optimization in §13 Phase 1.

We should **not** invert the protocol direction (zsync-style) for our primary upload path. Our use case is *upload* (client → server), not *publish* (server → many clients), so client-side delta computation doesn't reduce a hot server CPU path — it's the natural place for the work to happen anyway.

### 14.4 Dropbox / casync — content-defined chunking (family 2)

Dropbox's delta sync, as publicly described, is **not the rsync algorithm** ([Dropbox writeup](https://akshayghalme.com/blogs/how-dropbox-delta-sync-works/)):

1. Client splits the file into **content-defined chunks** using a rolling hash with a boundary-condition trigger (target ~4 MiB chunks).
2. Each chunk is hashed with **SHA-256**.
3. Client sends the *list of chunk hashes* (the new file's manifest) to the server.
4. Server replies "I have these, send me those" — pure set difference.
5. Client uploads only the unknown chunks.
6. Server assembles the new version from existing chunks + newly received chunks.

Compared to rsync:
- **No rolling-hash work on the server.** Server just does hash-set lookups. Much cheaper.
- **Natural deduplication**: identical chunks across files / users / versions are stored once. This is a massive bonus we'd inherit for free.
- **Storage model change is required.** Versions stop being self-contained blobs and become chunk manifests. Read path reassembles. This is a deeper architectural change.

`casync` (Lennart Poettering, 2017, [docs](https://github.com/systemd/casync)) is essentially the same idea hardened into a tool for distributing OS/VM images: rolling-hash chunk boundaries → SHA-256 → content-addressed object store → manifest file references chunks by hash. It uses **FastCDC** ([USENIX ATC '16 paper](https://www.usenix.org/system/files/conference/atc16/atc16-paper-xia.pdf)) as the modern chunking algorithm — 3–10× faster than the original Rabin fingerprinting.

**FastCDC specifics**: uses XOR-based "Gear" hash over a small precomputed table instead of Rabin's polynomial arithmetic. Configurable min/avg/max chunk sizes; the *expected* chunk size is achieved by checking the low N bits of the hash. Modern implementations exist in C, Go, Rust.

### 14.5 zstd `--patch-from`, xdelta3, bsdiff (family 3)

Less suitable for our case but worth noting so future-us doesn't relitigate:

- **`zstd --patch-from`** ([Facebook wiki](https://github.com/facebook/zstd/wiki/Zstandard-as-a-patching-engine)): treats the basis as a dictionary, compresses the new file against it. Reported as **fastest decompressor across all scenarios**, fastest *creator* for binary data, with patch sizes comparable to xdelta3. Used in container image distribution. **Disqualifier**: requires the entire dictionary in memory (configurable cap, but max practically ~2 GiB). Won't survive a 10 GiB VM image basis.
- **`xdelta3`** (RFC 3284 / VCDIFF): block-move family, mature, GPL-2.0. Block sizes smaller than rsync. Strong for source/text changes; loses to bsdiff on recompiled binaries. License is a problem if we ever want closed-source distribution.
- **`bsdiff`**: produces the smallest patches of any tool for recompiled binaries (~3× smaller than xdelta3 in one [2019 comparison](https://zork.net/~st/jottings/delta-compression-tests-2019.html)) but is **not streaming** — needs both whole files in RAM. Hard disqualifier at our scale.

### 14.6 Hash algorithm guidance from the field

Synthesized across sources:

- **MD4/MD5 for strong hash**: rsync's historical defaults. Collisions are demonstrable but the threat model for delta sync is *misalignment*, not adversarial collision injection — still, no reason to ship a known-broken primitive in new code.
- **BLAKE2 / BLAKE3**: the modern recommendation. BLAKE2 is in librsync; BLAKE3 is faster still (SIMD + tree hashing) but newer. Dropbox is reported to have switched from SHA-256 to BLAKE-family for performance reasons.
- **SHA-256**: Dropbox and casync use it for content-addressing in CDC schemes; performance fine at the chunk granularity these systems use (multi-MiB chunks make the SHA cost a single-digit percentage of disk-read cost).
- **Rolling weak hash**: Adler-32 (rsync) is still fine; "Gear" hash (FastCDC) and Rabin fingerprint (classic CDC) are the CDC-side equivalents.

For rsync-style: BLAKE2 strong + Adler-32 weak. For CDC: BLAKE2 or SHA-256 content hash + Gear weak.

### 14.7 Patch-size benchmarks (qualitative summary)

From the 2019 head-to-head ([source](https://zork.net/~st/jottings/delta-compression-tests-2019.html)) and the zstd team's own benchmarks:

- bsdiff < xdelta3-9 < Flips on recompiled binaries (bsdiff a clean winner at ~3× smaller — but RAM-only).
- xdelta3 ≈ zstd ≈ SmartVersion on patch size for general inputs.
- librsync/rdiff was *not included* in the 2019 comparison, an annoying gap. We should benchmark librsync ourselves against representative inputs (a few hundred-MB log files, a VM image, a recompiled binary) before shipping.

### 14.8 Revised recommendation

The original §5 recommendation (librsync, fixed-block rsync algorithm) **still stands for Phase 1**, but for sharpened reasons:

1. **Lowest architectural impact.** Versions remain self-contained blobs. No storage-model change, no chunk store, no manifest table, no chunk GC. The existing `Storage::store_file` / `S3Storage::store_file` / `Database::insert_version` path absorbs delta uploads with zero schema change.
2. **Battle-tested implementation available.** librsync, LGPL-2.1, BLAKE2 included, streaming-native API that maps 1:1 to gRPC streams.
3. **The cost we accept**: ~2× server I/O per upload (read basis temp file, write reconstructed file), and no automatic deduplication.

**Strongly consider as Phase 3+** (replacing the speculative "stored deltas" idea in original §13): a **content-defined chunking storage model** for files that opt in, à la Dropbox/casync. This gives:
- Automatic cross-version deduplication (a 10 GiB VM image with 100 versions stops being a 1 TiB problem).
- Natural cross-file dedup (often huge for log archives, VM images, dataset snapshots).
- Free delta-on-read: any two versions can be diffed by comparing manifests, no rolling-hash sweep needed.
- Map cleanly onto S3 as a content-addressed object store.

The cost is real: it's a months-long project that touches every layer (storage, database, S3 backup, version semantics, GC). Not a Phase 1 ask. But it's the right long-term direction if delta sync proves valuable in production — and noting it here means Phase 1 won't paint us into a corner that blocks it. Specifically: the gRPC surface in §4 above can coexist with a CDC-based Phase 3 (the rsync RPCs become a compatibility/fallback path for non-CDC files), so there's no protocol lock-in.

**One Phase 1 enhancement worth pulling in** (from the zsync insight, §14.3): cache the rsync signature as a sidecar to each version when it's written, so `GetDeltaSignature` is a streamed-blob read rather than a full-file rehash. Small extra work, large latency win for repeat-uploads against the same head.

## 15. Bidirectional sync and the proxy/gateway architecture

The original §4 proto surface defined both `StreamDeltaUpload` and `StreamDeltaDownload`, with download deferred to Phase 2. After thinking about the deployment shape, **download deserves first-class Phase 1 design even if it ships in Phase 2**, because the production load profile is download-dominated and the architecture decisions on the download path constrain everything else.

### 15.1 The symmetry that buys this cheaply

The rsync algorithm is symmetric: one side computes a signature of its copy, the other side walks its copy against that signature to emit a delta, then the first side reconstructs from delta + its copy. Which side plays which role is just a wiring choice.

- **Upload path:** server holds basis → server emits signature → client emits delta → server reconstructs new version.
- **Download path:** client holds basis (old version it already has) → client emits signature → server emits delta → client reconstructs new version.

Same three primitives (`rs_sig`, `rs_delta`, `rs_patch`), same gRPC streaming framing, mirrored direction. Implementing download after upload is approximately a 30% incremental effort, not a new project.

### 15.2 Asymmetric load profile, asymmetric optimization

Uploads are 1 client → 1 server, bounded by however many concurrent writers the service has. Downloads fan out: one server-side head version may be pulled by N clients within a single update window. The cost model is therefore:

| Path | Per-event server cost | Multiplier | Where the leverage is |
|---|---|---|---|
| Upload | Signature compute + delta apply | × concurrent writers | Already low; signature cache is a nice-to-have |
| Download | Delta compute per client | **× concurrent readers** | Must be made fan-out-friendly or it dominates load |

The download fan-out problem is exactly what zsync solved (§14.3): pre-compute the signature once at publish time, serve it as a static blob, let each client compute its own delta locally against its old copy, then have the client range-request only the bytes it's missing. **No per-client server CPU.** This is the single most important pattern to import for the download path.

### 15.3 Two viable download protocols, depending on storage model

| Storage model | Download protocol | Client receives |
|---|---|---|
| Phase 1 (whole-blob versions) | **zsync-style**: client downloads cached signature for its known version + cached signature for head + HTTP-range requests for byte ranges that don't match | Byte ranges of the head version's blob |
| Phase 3 (CDC chunked storage) | **manifest-diff**: client fetches new version's chunk manifest, computes set difference vs the manifest of its old version, fetches missing chunks by content hash | Individual content-addressed chunks |

Both fit naturally behind a proxy/CDN. Both make the per-client server CPU O(1) — just static blob serving. The CDC version is strictly better at scale (free cross-version dedup, cross-tenant dedup of public chunks if we ever expose that, and chunks live in cache forever because they're immutable by content hash). The zsync version is what we can ship before any storage rewrite.

### 15.4 Proxy / gateway responsibilities

A gateway between clients and the core gRPC service. Concrete responsibilities, in order of importance:

1. **AuthN termination.** Terminate the actual user-facing auth scheme (JWT, mTLS, OIDC, session cookie). Map the verified identity to an `AuthenticationContext` message and forward to the gRPC service. This matches the trusted-upstream model the codebase already assumes (`proto/fileservice.proto:69-74`); the gRPC service stays an authorization engine, not an authentication engine.
2. **Tenant gating.** Resolve the tenant from the authenticated identity and refuse cross-tenant requests at the edge. Defense in depth on top of the per-tenant schema isolation already in `Database` / `TenantManager`.
3. **Protocol conversion.** gRPC ↔ HTTP-range. The download path in particular must be reachable from any HTTP/1.1 client — browsers, `curl`, image fetchers, CI runners. Upload can stay gRPC-only for now (large streaming uploads are awkward over plain HTTP anyway).
4. **Signature and chunk caching with `Cache-Control: immutable`.** Signatures are immutable per `(uid, version_timestamp, block_size, hash_algo)`. CDC chunks are immutable per content hash. Both are CDN-trivial.
5. **Rate limiting, request size limits, DoS shielding.** All the things a gRPC service should not do itself.
6. **Audit logging and observability** — request traces with the resolved tenant/user, downstream metrics (bytes-saved-by-delta is the headline number).

Open questions to settle when we design the proxy:
- Build vs. integrate with an existing gateway (Envoy with gRPC-JSON transcoding + custom filters, vs. a thin Go/Rust service we own). Envoy gets us most of items 1, 3, 5 for free.
- Where ACL beyond tenant gating lives — in the proxy (low latency, but duplicates rules) or downstream (single source of truth, but every request hits the gRPC service). My lean: tenant + coarse role gating at the proxy, fine-grained ACL at the gRPC service.

### 15.5 What "minimize simple download sync" means concretely

Restating as success criteria the implementation can target:

1. **A client downloading a version it has never seen → no delta machinery, plain HTTP-range download of the version blob from cache.** Don't make first-time downloads pay for delta overhead.
2. **A client refreshing a version it already has → signature fetch + range fetch only.** Zero per-request server CPU; ≥99% bandwidth saved when the change is small.
3. **A client refreshing a version a peer also has → same cache entry served.** CDN-cache hit rate is the metric to track.
4. **Proxy + CDN can serve the entire download path from cache for static versions.** A version is static the moment its row is committed; signatures and chunks become CDN objects with no invalidation logic ever.

Criteria 1 and 4 are achievable in Phase 1 with whole-blob storage. Criterion 3 is partial in Phase 1 (works only when multiple clients pull the same exact byte ranges, which is common but not guaranteed). Phase 3 (CDC) is what unlocks criterion 3 at full strength because chunks dedup across versions.

### 15.6 Revised phasing (replacing §13)

1. **Phase 1A — Upload with versioning.** As §13 Phase 1. librsync, three gRPC RPCs (still defining all three protos including download, but only implementing upload + signature), FileSystem methods, temp-file reconstruction, end-to-end SHA-256, CLI demo.
2. **Phase 1B — Download with signature caching (new in this revision).** Implement `StreamDeltaDownload`. Persist signatures as sidecar objects at version-write time (so they're CDN-cacheable when the proxy lands). Wire up the bytes-saved metric.
3. **Phase 2 — Gateway / proxy.** Envoy-or-equivalent in front of the gRPC service: auth termination, gRPC-JSON / HTTP-range transcoding for the download path, tenant gating, signature/chunk cache headers. CDN integration is a config knob, not a code change.
4. **Phase 3 — Content-defined chunking storage model.** Migrate (opt-in per file or per tenant) to FastCDC-chunked content-addressed storage with manifest-based versions. Both delta RPCs become manifest-diff under the hood; old whole-blob versions continue to work via the librsync path for backward compatibility.

This phasing keeps Phase 1 narrow (the original goal — prove the pipeline) but explicitly designs for the download fan-out problem from the start so we don't have to retrofit it. Phase 2 is the highest-leverage operational win for a small amount of integration work. Phase 3 is a meaningful architecture change earned by Phase 1/2 production data.

## 16. CLI support for diff-based file sync

The CLI (`cli/src/fileengine_cli.cpp`, ~1420 lines, simple `<verb> <args>` dispatch starting at line ~1253) is a first-class client of the delta-sync feature, not just a demo. It already has the right shape: `upload`/`download` for full transfers, `versions`/`getversion`/`restore` for the existing version model. The new delta commands slot in as parallel verbs.

This closes the open question in §11.4 (build a client helper library, or expect callers to drive librsync themselves): **the CLI drives librsync directly via a thin C++ helper in `cli/include/` and serves as the reference implementation other clients can crib from.**

### 16.1 New CLI verbs

```text
delta-upload <uid> <local-file>
    Push <local-file> as a new version of <uid>, transmitting only the delta
    against the current head. Falls back to a full StreamFileUpload if the
    file is below the delta-worthwhile threshold or the server reports no
    suitable basis.

delta-download <uid> <local-file> [version-timestamp]
    Update <local-file> in place to match <uid>'s head version (or the given
    version-timestamp). Computes a local signature, requests a delta from
    the server, applies it. Falls back to StreamFileDownload if <local-file>
    does not exist or no basis match is found.

delta-sync <uid> <local-file>
    Bidirectional one-shot reconciliation. If <local-file> is newer than the
    server's head, delta-upload. If the server's head is newer than the
    local file's recorded version, delta-download. If both have diverged
    since the common ancestor, report a conflict and exit non-zero. Used by
    daemons/cron and scripted clients.

delta-stat <uid> <local-file>
    Dry-run: report how many bytes would transfer, what the basis version
    would be, and which direction sync would go. Useful for previewing
    cost before kicking off a large sync.
```

All four follow the existing dispatch pattern at `cli/src/fileengine_cli.cpp:1253` — one `else if (command == "...")` arm each.

### 16.2 Client-side state

The CLI today is stateless — every command builds the connection, runs the RPC, exits. Delta sync introduces a small amount of mandatory client-side state because the client needs to know **which server version its local file corresponds to** in order to use that version as the basis.

Proposal: a sidecar manifest file `<local-file>.feversion` (one per synced file) holding:

```
uid: <file-uid>
version_timestamp: <server-version-the-local-file-matches>
sha256: <hex>
size: <bytes>
synced_at: <iso8601>
```

Created on first `delta-download`, updated after every successful `delta-upload`/`delta-download`/`delta-sync`. Validated before each operation by re-hashing the local file — if the SHA-256 has drifted, the client knows the local file has changed since last sync (so `delta-upload` is needed) without having to walk the rolling-hash signature first.

Alternative: a single per-directory or per-user state file (`.fileengine-sync.toml`). Cleaner for `delta-sync` operating over many files; messier when files move. **My lean: per-file sidecar for v1**, optional roll-up later. Sidecars are easy to ignore (`*.feversion` in `.gitignore`), trivial to garbage-collect (orphaned sidecars are obvious), and survive file renames if we key on UID.

### 16.3 Helper library shape

To avoid duplicating librsync orchestration between CLI and any future SDK clients, factor the delta-sync glue into a small client-side helper, e.g. `cli/include/fileengine/delta_client.h`:

```cpp
class DeltaClient {
public:
    DeltaClient(std::shared_ptr<FileService::Stub> stub, AuthCtx auth);

    // Streams a signature from the server, streams a delta back.
    Result<std::string> upload(
        const std::string& file_uid,
        const std::string& local_path,
        const std::string& basis_version_timestamp = ""); // empty = head

    // Computes local signature, streams it up, applies streamed delta.
    Result<void> download(
        const std::string& file_uid,
        const std::string& local_path,
        const std::string& target_version_timestamp = ""); // empty = head

    // Reads/writes the .feversion sidecar.
    Result<LocalVersionState> read_state(const std::string& local_path);
    Result<void>              write_state(const std::string& local_path,
                                          const LocalVersionState& state);

    // One-shot reconciler used by `delta-sync`.
    enum class SyncResult { NoChange, UploadedDelta, DownloadedDelta,
                            UploadedFull, DownloadedFull, Conflict };
    Result<SyncResult> sync(const std::string& file_uid,
                            const std::string& local_path);
};
```

Lives in the CLI's include tree so it doesn't bloat the core library. Other clients (a future GUI, a sync daemon, third-party scripts via FFI) can reuse it by linking against this header-and-helper pair.

### 16.4 Fallback policy (must be explicit)

The CLI must decide when to use delta sync vs. full transfer. Get this wrong and you either waste cycles computing deltas on tiny files or stall on huge ones because of a transient delta-protocol failure. Rules:

1. **File size below threshold** (config: default 4 × `block_size`, so ~32 KiB at the default block size): always use `upload`/`download` (full transfer). Delta overhead exceeds the file.
2. **No sidecar present and `delta-download` requested**: do a full `download` and write the sidecar. First-time downloads never pay delta overhead.
3. **Sidecar's `version_timestamp` no longer exists on the server** (basis deleted via version purging): fall back to full transfer, log a warning.
4. **Server returns `INTEGRITY_CHECK_FAILED` after delta apply**: retry once with a full transfer (cheap insurance against a librsync edge case or basis-bit-rot).
5. **`--no-delta` flag**: force full transfer. Useful for benchmarking and for diagnostic situations where a user suspects delta corruption.
6. **`--require-delta` flag**: fail rather than fall back. Useful in tests and in environments where falling back to multi-GB full uploads is unacceptable.

### 16.5 Build dependency

The CLI gains a build-time dependency on `librsync` (header + lib) — same dep the server uses, so no new packaging concern. CMake's `cli/CMakeLists.txt` adds `pkg_check_modules(LIBRSYNC librsync)` and links to `${LIBRSYNC_LIBRARIES}`. The helper library's API is the only thing that touches librsync; everything else in the CLI sees opaque `Result<T>` returns, same as the rest of the file.

### 16.6 Tests

Append to the test plan in §12:

- CLI integration tests for each new verb against a running server, in `tests/test_cli_delta.sh` (matching the existing shell-test pattern, e.g. `tests/test_acl_roles.sh`).
- Sidecar lifecycle: create on first download, update after upload, detect drift via SHA-256 mismatch, recover gracefully from corruption.
- Fallback policy: each rule in §16.4 exercised by a dedicated test case.
- `delta-sync` round-trip: edit on side A → sync → edit on side B → sync → verify both sides converge to the latest version with expected version-history entries.

### 16.7 Phasing impact

This fits inside the existing phasing without expansion:

- **Phase 1A** ships `delta-upload` + `delta-stat` alongside the upload RPCs.
- **Phase 1B** adds `delta-download` and `delta-sync` once download RPCs land.
- **Phase 2** (proxy/gateway) gains an HTTP-range fallback the CLI can opt into if it's behind a network that blocks gRPC.

## 17. References

### Foundational
- A. Tridgell & P. Mackerras, *The rsync algorithm*, ANU TR-CS-96-05 (1996). [PDF](https://openresearch-repository.anu.edu.au/items/15a1c428-0ad3-49d6-bb54-9238250cbbf0)
- A. Tridgell, *Efficient Algorithms for Sorting and Synchronization*, PhD thesis, ANU (1999).
- "How rsync works", samba.org/rsync. [link](https://rsync.samba.org/how-rsync-works.html)

### librsync (recommended for Phase 1)
- librsync project, github.com/librsync/librsync — 2.3.4 (Feb 2023), LGPL-2.1, BLAKE2 included.
- Streaming API reference. [link](https://librsync.github.io/api_streaming.html)
- librsync README & NEWS. [link](https://librsync.github.io/)

### zsync (role-inversion / signature-caching insight)
- C. Atkins, *zsync: optimised rsync over HTTP* (2005). [paper](https://zsync.moria.org.uk/paper/)
- zsync project page. [link](https://zsync.moria.org.uk/)

### Content-defined chunking (Phase 3 candidate)
- W. Xia et al., *FastCDC: A Fast and Efficient Content-Defined Chunking Approach for Data Deduplication*, USENIX ATC '16. [PDF](https://www.usenix.org/system/files/conference/atc16/atc16-paper-xia.pdf)
- L. Poettering, *casync — A tool for distributing file system images* (2017). [blog](https://0pointer.net/blog/casync-a-tool-for-distributing-file-system-images.html) · [github](https://github.com/systemd/casync)
- "Distributing filesystem images and updates with casync", LWN. [link](https://lwn.net/Articles/726625/)
- Dropbox delta-sync explainer (third-party, but consistent with Dropbox's published talks). [link](https://akshayghalme.com/blogs/how-dropbox-delta-sync-works/)

### Alternative families (considered, not recommended for Phase 1)
- RFC 3284, *The VCDIFF Generic Differencing and Compression Data Format* (xdelta3 wire format).
- jmacd/xdelta on GitHub. [link](https://github.com/jmacd/xdelta)
- Facebook, *Zstandard as a patching engine* (`zstd --patch-from`). [wiki](https://github.com/facebook/zstd/wiki/Zstandard-as-a-patching-engine)
- *Delta compression tests, 2019 edition* — empirical comparison of bsdiff/xdelta3/Flips. [link](https://zork.net/~st/jottings/delta-compression-tests-2019.html)
