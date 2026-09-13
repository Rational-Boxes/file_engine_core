# Proposal: the read path under bulk load

**Status:** Draft — for review, nothing implemented
**Scope (cross-repo):** `file_engine_core` (folder mtime, a batch permission RPC);
`discussion_threaded_communication` (dashboard fan-out, client reuse);
`scripts/Ansible` (instrumentation, already-applied mitigations)
**Evidence:** measured on the DigitalOcean deployment 2026-09-13 during a bulk
WebDAV import into the `rationalboxes` tenant. Method and raw numbers in
`scripts/Ansible/docs/SCALING.md`.

---

## Summary

Under a bulk import the host ran at load 10 on 4 vCPU with **0% iowait** and
**Postgres holding 3.77 of 4 vCPU**. Everything else on the box — the core, the
doors, the consumers — together used under 15% of one core.

Essentially all of that database time is one query: the folder-mtime walk, which
descends a folder's entire subtree on every read of that folder. It is not a
missing index and not a permission check. It gets worse as a tenant grows.

Three recommendations follow, in order of payoff. **R1 is the one that matters**;
R2 is worth doing and will not change the load; R3 is what makes the next
diagnosis take minutes instead of an afternoon.

## What was ruled out, and why it is worth saying

Recorded because each was believed for a while, and each will be believed again
by the next person looking at this.

| Suspected | Actually |
|---|---|
| CSAI conversion / preview generation | Below the measurement threshold. `podman stats` samples too briefly and `ps` reports lifetime averages; both pointed here. Per-container cgroup `usage_usec` deltas did not. |
| The permission ancestor walk | The recursive CTE's *prefix* looks like one. Its full text is `subtree_newest_version` — descending, not ascending. |
| A missing index | `files(parent_uid)` and `versions(file_uid, version_timestamp)` both exist and are adequate. The work itself is the cost. |
| Disk | 0% iowait throughout. |
| Memory | 4.5 GB free throughout. |

---

## R1 — Stop recomputing folder mtime on every read

**Impact: very high. Effort: medium. Risk: medium (core change, core deploy).**

`apply_folder_recursive_mtime` → `subtree_newest_version`
(`core/src/database.cpp:768`) implements "a folder's mtime is the newest file
anywhere in its subtree". The rule is right — a folder showing the time of its
newest content is what users expect. Recomputing it from scratch per read is
what costs.

```sql
WITH RECURSIVE folders(uid) AS (SELECT $1::text UNION SELECT f.uid FROM files f
   JOIN folders fo ON f.parent_uid = fo.uid WHERE f.is_container AND NOT f.deleted)
SELECT v.version_timestamp, v.revised_by FROM files fi
  JOIN folders fo ON fi.parent_uid = fo.uid
  JOIN versions v ON v.file_uid = fi.uid
 WHERE NOT fi.is_container AND NOT fi.deleted
 ORDER BY v.version_timestamp DESC LIMIT 1;
```

`rationalboxes` holds 52,721 files and 49,443 versions. A folder near the root
scans most of that. It is applied **per directory FileInfo**, including from the
listing paths beside `apply_listing_provenance`, so a PROPFIND of a folder with
N subfolders pays N walks — and WebDAV issues PROPFINDs around almost every PUT.
The cost therefore rises with both corpus size and request rate, which is why it
degraded as the import ran rather than being visible from day one.

Three ways out, cheapest first. They compose — 1 can ship now and 3 later.

### 1. Do not compute it for every entry in a listing

A caller listing a folder gets each child's recursive mtime whether it asked for
one or not. If the listing surfaces do not need it, this removes the multiplier
immediately — one walk per request instead of one per child.

Cheapest change, smallest blast radius, and it can be measured before committing
to anything larger. **Requires checking what the SPA, WebDAV PROPFIND and CMIS
actually render** — if a file browser shows folder timestamps in a listing, this
changes what users see and is not free.

### 2. Cache it, invalidated by writes in the subtree

Keyed on folder uid, invalidated when a version lands anywhere beneath. Bounded
work, no schema change, and it fits the existing cache-invalidation channel.

The hard part is invalidation, which must walk ancestors on write — cheap
(O(depth)) but easy to get subtly wrong, and a stale folder mtime is the kind of
bug that is noticed months later.

### 3. Maintain it on write (recommended end state)

Denormalise the newest descendant version onto the folder row; when a version is
written, bubble it up the ancestor chain. O(depth) on write, O(1) on read, and
it inverts the cost onto the rarer operation.

Costs a migration and a backfill, and every path that writes or moves a version
must maintain it. A move is the awkward case: the timestamp has to be
recomputed for both the old and new parents. That is bounded, but it is real
work and wants tests around moves and deletes specifically.

**Recommendation:** measure 1 first, because it may be most of the win for a
fraction of the effort, then implement 3 as the durable answer.

---

## R2 — The dashboard fan-out

**Impact: high for dashboard latency, ~none for database load. Effort: low
(interim) to medium (batch RPC). Risk: low.**

`GET /discuss/dashboard/activity?limit=100` over-fetches `limit * 4` rows and
then runs **two permission calls per row**:

```python
rows = ...recent(tenant, limit=limit * 4)          # 400 rows
for r in rows:
    if await _readable(...) and await _live(...):  # two calls, per row
```

Up to **800 round trips for one page load**. Worse, `PermissionChecker.check()`
builds a core client and closes it in a `finally` on every call, so the
connection churn is on the same order as the RPCs.

`can_read` is cached with a TTL; `is_live` is deliberately not, so it runs for
every surviving row even when the cache is warm. The READ cache is also cold in
exactly the case that hurts — a tenant whose files were written minutes ago.

**Be clear about what this is not.** `CheckPermission` and `Exists` are point
lookups. Fixing this makes the dashboard fast and stops it hammering the core;
it does not address R1, and on its own it will not stop 504s during an import.

### Interim, no core change

- **Reuse one core client per request** instead of per check. Largest single
  constant-factor win here and it is local to `permissions.py`.
- **Give `is_live` a short TTL** (seconds). The docstring's reasoning for
  leaving it uncached — deletion must reflect promptly — survives a 5-second
  window, and it collapses the repeated checks inside one page render.
- **Lower the `limit * 4` multiplier**, which is a guess at how many rows get
  filtered out. On a tenant where the caller can read everything, 300 of those
  400 rows are wasted.

### The batch RPC

The core exposes only single-uid `Exists` and `CheckPermission`, so this is a
new RPC plus proto regeneration, `python_interface`, and the discussion client.

**A batch that loops internally buys nothing** — the point is for the core to
answer N uids in one pass, which means the batch shape has to reach the database
layer, not just the service boundary.

**Recommendation:** take the interim now; land the batch RPC in the same core
release as R1 so there is one rebuild and one deploy window.

---

## R3 — Instrument, so the next one is not archaeology

**Impact: compounding. Effort: low. Risk: low (one Postgres restart).**

This analysis was done by sampling `pg_stat_activity` by hand in a loop, because
**`pg_stat_statements` is not installed**. Per-query totals are the difference
between "Postgres is busy" and "this query is 94% of it".

Wired into the postgres role already but deliberately not deployed:
`shared_preload_libraries` is postmaster-level, so it restarts Postgres. Seconds
of downtime, in a window someone chooses.

Also worth collecting, cheaply:

- **Per-container CPU from cgroup `cpu.stat` deltas.** Not `podman stats`, not
  `ps` — both misattributed this incident.
- **Consumer-group lag** on the Redis streams, the input to any future
  autoscaling decision and useful long before anything autoscales.

---

## Already applied

| | |
|---|---|
| `csai_worker_cpus: "2.0"` | Hard quota so a conversion burst cannot take more than half the box. A guard against a real future burst — **not** what this incident was. |
| `/discuss/` `read_timeout: 180s` | Was inheriting nginx's 60s default, which is what the 504 was. A band-aid, labelled as one: honest slowness beats a false error. |

## Deliberately not recommended yet

**Moving Postgres to a managed instance** is the obvious lever and it is the
right destination, but doing it *before* R1 buys capacity to absorb avoidable
queries and makes the monthly bill a function of a fixable inefficiency. It
removes contention, not work. After R1, size it against a real workload.

**More vCPU** is the correct answer for a *planned* import window — resize up,
import, resize down, hourly billing — and it is not an answer to a query whose
cost grows with the corpus.

---

## Appendix: reproducing the measurement

```bash
# per-container CPU over a fixed window, from cgroup accounting
for d in /sys/fs/cgroup/machine.slice/libpod-*.scope; do
  awk '/^usage_usec/{print FILENAME, $2}' "$d/cpu.stat"; done   # sample, sleep 10, re-sample

# what the database is actually running
psql -c "select count(*), left(regexp_replace(query,'\s+',' ','g'),60)
           from pg_stat_activity where state='active' group by 2 order by 1 desc"

# the FULL query text — the prefix is misleading
psql -c "select regexp_replace(query,'\s+',' ','g') from pg_stat_activity
          where state='active' and query like '%RECURSIVE%' limit 1"
```
