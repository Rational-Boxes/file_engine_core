#!/usr/bin/env python3
# Copyright (C) 2026 James Hickman
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

"""Drive real uploads through the HTTP bridge and watch the core's threads.

**Why this exists.** The core was found holding ~150 threads with no file
operation in a long while, and a graceful shutdown that would not complete. Two
questions came out of that, and neither could be answered from the outside:

  1. Are those threads a leak, or are they parked workers doing nothing wrong?
  2. Does a request's resources — worker thread, database connection — come back
     when the request ends?

This answers both by observation rather than inference. It pushes more concurrent
create+upload operations through the bridge than the core has worker threads,
samples the core's monitoring endpoint throughout, and then checks that
everything the run consumed was given back.

**It deliberately goes through the bridge**, not straight at gRPC: that is the
path real traffic takes, so the request lifecycle it exercises is the real one.

Usage:
    python3 tests/thread_lifecycle_probe.py [--concurrency N] [--files N]

Requires the stack to be up (scripts/start_backend_services.sh) and the core's
monitoring listener reachable. Exits non-zero if a lifecycle check fails, so it
can gate a build.

**Forcing real contention.** Against a default core the operations finish faster
than clients can stack up, so the pool never actually drains and the queueing
path — the interesting one — goes untested. Shrink the core instead of pushing
harder: FILEENGINE_HTTP_THREAD_POOL sets BOTH the gRPC worker count and the
database pool size, so starting the core with it at 2 makes a modest load
saturate it. That is how the waiting path was verified:

    kill $(cat /tmp/fileengine_core.pid)
    FILEENGINE_HTTP_THREAD_POOL=2 ./build/core/fileengine_server &
    CORE_THREAD_POOL=2 python3 tests/thread_lifecycle_probe.py \
        --files 300 --concurrency 32

which drove the pool to 0 free with 14 callers queued, and still came back to
2/2 free with nothing in flight.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed

BRIDGE = os.environ.get("BRIDGE_URL", "http://localhost:8090")
MONITOR = os.environ.get("CORE_MONITOR_URL", "http://localhost:8081")
USER = os.environ.get("FE_USER", "testuser@rationalboxes.com")
#: REQUIRED, no fallback — a hardcoded test password is a credential in git.
#: Missing means SKIP rather than fail (see load_stress.py).
PASSWORD = os.environ.get("FE_PASSWORD", "")
TENANT = os.environ.get("FE_TENANT", "default")

# The core's configured gRPC worker count (FILEENGINE_HTTP_THREAD_POOL). The
# point of the run is to ask for MORE work than that at once — a pool that only
# ever sees less demand than it has workers never has to hand anything back, so
# it would never show a lifecycle problem.
CORE_THREAD_POOL = int(os.environ.get("CORE_THREAD_POOL", "10"))


def _auth_header() -> str:
    raw = f"{USER}:{PASSWORD}".encode()
    return "Basic " + base64.b64encode(raw).decode()


def _request(method: str, path: str, body: bytes | None = None,
             content_type: str = "application/json", timeout: int = 60):
    req = urllib.request.Request(BRIDGE + path, data=body, method=method)
    req.add_header("Authorization", _auth_header())
    req.add_header("X-Tenant", TENANT)
    if body is not None:
        req.add_header("Content-Type", content_type)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        payload = resp.read()
        return resp.status, payload


def monitor() -> dict:
    with urllib.request.urlopen(MONITOR + "/poolz", timeout=10) as resp:
        return json.loads(resp.read())


def metrics_text() -> str:
    with urllib.request.urlopen(MONITOR + "/metrics", timeout=10) as resp:
        return resp.read().decode()


# --------------------------------------------------------------------- workload
def upload_one(index: int, size_bytes: int) -> tuple[bool, str]:
    """Create a file and write a version of its content — one full round trip."""
    name = f"threadprobe-{os.getpid()}-{index}.bin"
    try:
        status, payload = _request(
            "POST", "/v1/dirs/root/files", json.dumps({"name": name}).encode())
        if status not in (200, 201):
            return False, f"create returned {status}"
        uid = json.loads(payload)["uid"]

        # Distinct bytes per file so nothing can be deduplicated into a no-op.
        content = (f"{index}:".encode() + b"x" * size_bytes)
        status, _ = _request("PUT", f"/v1/files/{uid}/content", content,
                             content_type="application/octet-stream")
        if status not in (200, 204):
            return False, f"upload returned {status}"
        return True, uid
    except urllib.error.HTTPError as e:
        return False, f"HTTP {e.code}: {e.read()[:200]!r}"
    except Exception as e:  # noqa: BLE001 - report whatever went wrong
        return False, f"{type(e).__name__}: {e}"


class Sampler(threading.Thread):
    """Polls the core's monitoring endpoint while the load runs."""

    def __init__(self, interval: float = 0.25):
        super().__init__(daemon=True)
        self.interval = interval
        self.samples: list[dict] = []
        self._stop = threading.Event()

    def run(self) -> None:
        while not self._stop.is_set():
            try:
                self.samples.append(monitor())
            except Exception:  # noqa: BLE001 - a missed sample is not fatal
                pass
            self._stop.wait(self.interval)

    def stop(self) -> None:
        self._stop.set()


def peak(samples: list[dict], *path: str) -> int:
    best = 0
    for s in samples:
        node = s
        for key in path:
            node = node.get(key, {}) if isinstance(node, dict) else {}
        if isinstance(node, (int, float)):
            best = max(best, int(node))
    return best


# ----------------------------------------------------------------------- checks
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--files", type=int, default=40,
                    help="total create+upload operations (default: 40)")
    ap.add_argument("--concurrency", type=int, default=CORE_THREAD_POOL * 2,
                    help="simultaneous operations; must exceed the core's worker count")
    ap.add_argument("--size", type=int, default=64 * 1024,
                    help="bytes per uploaded file (default: 64 KiB)")
    ap.add_argument("--settle", type=float, default=3.0,
                    help="seconds to wait after the load before the final read")
    args = ap.parse_args()

    if not PASSWORD:
        print("FE_PASSWORD is not set (the LDAP test-user password); skipping.",
              file=sys.stderr)
        return 77   # ctest SKIP_RETURN_CODE

    if args.concurrency <= CORE_THREAD_POOL:
        print(f"refusing to run: concurrency {args.concurrency} does not exceed the core's "
              f"{CORE_THREAD_POOL} workers, so nothing would ever have to queue",
              file=sys.stderr)
        return 2

    print(f"→ {args.files} uploads, {args.concurrency} at a time, "
          f"against a core configured for {CORE_THREAD_POOL} workers")

    try:
        before = monitor()
    except Exception as e:  # noqa: BLE001
        print(f"cannot reach the core monitor at {MONITOR}: {e}", file=sys.stderr)
        print("is the stack up? scripts/start_backend_services.sh", file=sys.stderr)
        return 2

    b_threads = before.get("threads", {})
    b_pool = before.get("db_pool", {})
    b_rpc = before.get("rpc", {})
    print(f"  baseline: {b_threads.get('total')} threads "
          f"({b_threads.get('not_waiting')} not waiting), "
          f"pool {b_pool.get('in_use')}/{b_pool.get('size')} in use, "
          f"{b_rpc.get('in_flight')} RPCs in flight")

    sampler = Sampler()
    sampler.start()

    started = time.time()
    failures: list[str] = []
    with ThreadPoolExecutor(max_workers=args.concurrency) as pool:
        futures = [pool.submit(upload_one, i, args.size) for i in range(args.files)]
        for f in as_completed(futures):
            ok, detail = f.result()
            if not ok:
                failures.append(detail)
    elapsed = time.time() - started

    sampler.stop()
    sampler.join(timeout=5)

    print(f"  {args.files - len(failures)}/{args.files} uploads succeeded in {elapsed:.1f}s")
    if failures:
        for detail in failures[:5]:
            print(f"    ! {detail}")

    # Under load: what did the core actually do?
    print(f"  peak while loaded: {peak(sampler.samples, 'rpc', 'in_flight')} RPCs in flight, "
          f"{peak(sampler.samples, 'db_pool', 'in_use')} connections in use, "
          f"{peak(sampler.samples, 'db_pool', 'waiters')} waiting for one, "
          f"{peak(sampler.samples, 'threads', 'not_waiting')} threads not waiting")

    # Let anything in flight finish before judging the resting state.
    time.sleep(args.settle)
    after = monitor()
    a_threads = after.get("threads", {})
    a_pool = after.get("db_pool", {})
    a_rpc = after.get("rpc", {})
    a_wd = after.get("watchdog", {})

    print(f"  at rest:  {a_threads.get('total')} threads "
          f"({a_threads.get('not_waiting')} not waiting), "
          f"pool {a_pool.get('in_use')}/{a_pool.get('size')} in use, "
          f"{a_rpc.get('in_flight')} RPCs in flight")

    problems: list[str] = []

    if failures:
        problems.append(f"{len(failures)} upload(s) failed")

    # 1. Every request must finish. An RPC still in flight after the load has
    #    stopped is a wedged handler — the thing that also blocks shutdown.
    if a_rpc.get("in_flight", 0) != 0:
        problems.append(
            f"{a_rpc['in_flight']} RPC(s) still in flight after settling "
            f"(oldest: {a_rpc.get('oldest_method')} at {a_rpc.get('oldest_age_ms')}ms)")

    # 2. Started must equal completed. A gap means a request began and never
    #    ended, whatever the in-flight gauge happens to read.
    started_total = a_rpc.get("started_total", 0) - b_rpc.get("started_total", 0)
    completed_total = a_rpc.get("completed_total", 0) - b_rpc.get("completed_total", 0)
    if started_total != completed_total:
        problems.append(
            f"{started_total} RPCs started but {completed_total} completed during the run")

    # 3. Every connection must come back. This is the leak check: `outstanding`
    #    is acquired-minus-released, and a pool that ends a quiet period with any
    #    connection checked out has lost one for good.
    if a_pool.get("in_use", 0) != 0 or a_pool.get("outstanding", 0) != 0:
        problems.append(
            f"database connections not returned: in_use={a_pool.get('in_use')}, "
            f"outstanding={a_pool.get('outstanding')}")

    if a_pool.get("available", 0) != a_pool.get("size", 0):
        problems.append(
            f"pool did not refill: {a_pool.get('available')} of {a_pool.get('size')} available")

    # 4. Nobody should still be waiting, and nobody should have given up.
    if a_pool.get("waiters", 0) != 0:
        problems.append(f"{a_pool['waiters']} caller(s) still waiting for a connection")
    if a_pool.get("wait_timeouts", 0) != b_pool.get("wait_timeouts", 0):
        problems.append(
            f"{a_pool['wait_timeouts'] - b_pool.get('wait_timeouts', 0)} acquire timeout(s) "
            "— a connection was held and never returned, or the pool is undersized")

    # 5. Threads must go back to being parked. The count itself is NOT the test:
    #    the gRPC sync server keeps its pollers for the life of the process, so a
    #    high total is expected. What matters is that they are waiting again, and
    #    that none ended up stuck in uninterruptible I/O.
    if a_threads.get("available"):
        if a_threads.get("uninterruptible", 0) > 0:
            problems.append(
                f"{a_threads['uninterruptible']} thread(s) in uninterruptible sleep — "
                "blocked in the kernel, cannot be cancelled or timed out")
        # One runnable thread is this very monitoring request being served.
        if a_threads.get("not_waiting", 0) > 2:
            problems.append(
                f"{a_threads['not_waiting']} threads not waiting while idle — "
                "workers should be parked once the load stops")
        grew = a_threads.get("total", 0) - b_threads.get("total", 0)
        if grew > args.concurrency:
            problems.append(
                f"thread count grew by {grew} and did not come back down "
                f"(before {b_threads.get('total')}, after {a_threads.get('total')})")

    # 6. The recovery watchdog must have kept probing throughout. A watchdog that
    #    stalls under load is one that would not notice an outage under load.
    if a_wd:
        if not a_wd.get("running"):
            problems.append("the recovery watchdog is not running")
        age = a_wd.get("last_probe_age_seconds", -1)
        interval = a_wd.get("interval_seconds", 30)
        if age > 2 * interval:
            problems.append(
                f"the recovery watchdog last probed {age}s ago, over twice its {interval}s "
                "interval — it is stuck, and would not notice a database outage")

    print()
    if problems:
        print("FAIL — the request lifecycle is not clean:")
        for p in problems:
            print(f"  ✗ {p}")
        print("\nFull monitor snapshot:")
        print(json.dumps(after, indent=2))
        return 1

    print("PASS — every request completed, every connection was returned, "
          "and the workers went back to waiting.")
    print(f"  threads {a_threads.get('total')} total / {a_threads.get('sleeping')} parked, "
          f"pool {a_pool.get('available')}/{a_pool.get('size')} free, "
          f"{a_rpc.get('completed_total')} RPCs completed since boot, "
          f"watchdog probing every {a_wd.get('interval_seconds')}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
