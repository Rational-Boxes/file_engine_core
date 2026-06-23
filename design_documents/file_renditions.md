# File entities as containers — hidden renditions

A subtle filesystem change: a **file** entity can also act as a **container** for
hidden child entities. Under normal circumstances a file looks like a file; only
when a listing **specifically targets the file's UID** are its children shown.
The children are alternate-format **renditions** of the parent file.

## Concept

- A file row may have children (rows whose `parent_uid` = the file's UID).
- Those children are **renditions**: format-conversion copies of the parent.
- They are **deliberately hidden** in the file-browser and WebDAV
  representations — superficial, presentation-layer hiding.
- Because renditions are children of the file, **move and copy of the file carry
  the renditions with it**.

## Decisions (locked)

| Topic | Decision |
|-------|----------|
| Creation | Renditions are written as children with `parent_uid = <file-uid>` via existing gRPC calls. No new creation RPC — a **future external re-rendition service** populates them. |
| Naming | `<timestamp>-format.ext` (e.g. `20260623T0200Z-pdf.pdf`). The parent UUID is **not** part of the name; identity is by `parent_uid`. |
| Nesting | **One level** only. Renditions are leaves; targeting a rendition's UID shows nothing. |
| Discoverability | `rendition_count` on core `Stat`/`FileInfo` **and** `DirectoryEntry`. The REST bridge surfaces it (and a derived `has_renditions`) so the **frontend** knows to offer renditions. |
| Bridge exposure | WebDAV: **fully hidden** (a file is never a collection; its children are never listed). REST/frontend: hidden in normal browsing, but `has_renditions` is exposed and renditions are fetchable on demand. |

## Behaviour

- **List, normal:** listing a *directory* returns its entries; a file's renditions
  never appear (different parent). A file in the listing reports `rendition_count`.
- **List, targeted:** `ListDirectory(<file-uid>)` returns the file's renditions
  (core/gRPC path, for the conversion service and the REST renditions endpoint).
- **Move:** renditions follow automatically — they reference the stable file UID,
  so no reparenting is needed.
- **Copy:** deep-copy the file's renditions to the new file UID (names preserved;
  one level).
- **Delete:** cascades — removing a file removes its renditions.
- **WebDAV:** `PROPFIND` on a file returns a single file response (never its
  children), even at `Depth: 1`; a file is never advertised as a collection.

## Implementation stages

1. **Core** (`file_engine_core`)
   - Proto: add `rendition_count` to `DirectoryEntry` and `FileInfo`.
   - `types.h`: add `rendition_count` to the structs; map in `grpc_service`.
   - Database: populate `rendition_count` (count of non-deleted children) in
     `listDirectory` and `stat`; confirm `ListDirectory(file-uid)` returns
     children.
   - `FileSystem::copy`: deep-copy a file's renditions to the new UID.
   - `FileSystem::remove`: cascade to renditions.
2. **REST bridge** (`http_bridge`)
   - Expose `rendition_count` / `has_renditions` in directory listings and stat.
   - `GET /v1/files/{uid}/renditions` → list a file's renditions on demand.
3. **WebDAV bridge** (`webdav_bridge`)
   - Verify/harden: never list a file's children; never present a file as a
     collection (the PROPFIND-file path already does this).
4. **Frontend**
   - Use `has_renditions` to offer "show renditions"; fetch via the REST endpoint.

Out of scope here: the external re-rendition creation service.
