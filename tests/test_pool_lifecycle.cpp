// Copyright (C) 2026 James Hickman
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// Does a connection come back, and does somebody waiting for it get woken?
//
// The load probe (tests/thread_lifecycle_probe.py) drives the real path through
// the bridge and shows a clean lifecycle — but it never manages to exhaust the
// pool, because the operations finish faster than clients can stack up. So the
// interesting path stays untested there: what happens when demand exceeds the
// pool and callers have to queue.
//
// That path was broken. release() pushed a connection back onto the queue and
// never notified the condition variable, and acquire() waited on it with no
// timeout. The first caller to find the pool empty therefore slept forever — a
// returning connection could not wake it, because nothing ever signalled. The
// result is a worker thread that never finishes its request, which is also why a
// graceful shutdown could not complete: gRPC's Shutdown() deadline cancels
// pending RPCs, but it cannot interrupt a handler blocked in our own code.
//
// This test forces exactly that condition: more concurrent callers than the pool
// has connections. Against the fixed pool it finishes in well under a second.
// Against the broken one it never finishes, which is why every wait here is
// bounded and reported as a failure rather than left to hang.
//
// It needs a live Postgres (the pool builds real connections). Point it with
// FE_TEST_PG_* or accept the dev defaults.

#include "fileengine/connection_pool.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << "\n";
    if (!ok) ++failures;
}

std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

} // namespace

int main() {
    const std::string host = env_or("FE_TEST_PG_HOST", "localhost");
    const int         port = std::stoi(env_or("FE_TEST_PG_PORT", "5434"));
    const std::string db   = env_or("FE_TEST_PG_DB", "fileengine");
    const std::string user = env_or("FE_TEST_PG_USER", "postgres");
    const std::string pass = env_or("FE_TEST_PG_PASSWORD", "postgres");

    // Deliberately tiny: two connections and eight callers guarantees contention
    // without needing a large pool or a slow query.
    constexpr int kPoolSize = 2;
    constexpr int kCallers  = 8;

    std::cout << "connection pool lifecycle: " << kCallers << " callers over a pool of "
              << kPoolSize << "\n";

    fileengine::ConnectionPool pool(host, port, db, user, pass, kPoolSize);
    if (!pool.initialize()) {
        std::cout << "  [skip] no database at " << host << ":" << port
                  << " — set FE_TEST_PG_* to point at one\n";
        return 0;   // Not a failure: this test is about behaviour, not availability.
    }

    // Keep the wait short so a regression fails fast rather than stalling a build.
    pool.set_acquire_timeout(5s);

    {
        const auto s = pool.stats();
        check(s.size == kPoolSize, "pool reports its configured size");
        check(s.available == kPoolSize, "every connection starts available");
        check(s.in_use == 0, "nothing is checked out before any caller runs");
    }

    std::atomic<int> completed{0};
    std::atomic<int> gave_up{0};

    std::vector<std::future<void>> callers;
    callers.reserve(kCallers);
    for (int i = 0; i < kCallers; ++i) {
        callers.push_back(std::async(std::launch::async, [&pool, &completed, &gave_up]() {
            auto conn = pool.acquire();
            if (!conn) {
                ++gave_up;
                return;
            }
            // Hold it long enough that the callers genuinely overlap; with 8
            // callers over 2 connections this forces at least 6 of them to wait.
            std::this_thread::sleep_for(50ms);
            pool.release(conn);
            ++completed;
        }));
    }

    // Bounded: the broken pool never finishes, and a test that hangs tells an
    // operator nothing and blocks a build.
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    bool all_done = true;
    for (auto& f : callers) {
        if (f.wait_until(deadline) != std::future_status::ready) {
            all_done = false;
            break;
        }
    }

    check(all_done,
          "every caller finished — a connection returning wakes somebody waiting for it");
    if (!all_done) {
        // Don't join stuck threads; report and get out with the evidence.
        const auto s = pool.stats();
        std::cout << "        stuck with in_use=" << s.in_use
                  << " waiters=" << s.waiters
                  << " available=" << s.available << "\n";
        std::cout << "        this is the lost-wakeup: release() returned a connection "
                     "without signalling the condition variable\n";
        return 1;
    }

    check(gave_up == 0, "no caller had to give up waiting");
    check(completed == kCallers, "every caller got a connection and used it");

    {
        const auto s = pool.stats();
        check(s.in_use == 0, "every connection was handed back");
        check(s.available == kPoolSize, "the pool refilled to its full size");
        check(s.waiters == 0, "nobody is left waiting");
        check(s.acquired_total == static_cast<std::uint64_t>(kCallers),
              "acquire count matches the number of callers");
        check(s.released_total == s.acquired_total,
              "releases balance acquires — the leak check, in miniature");
        // The point of the exercise: prove the callers actually queued, so a pass
        // means the waiting path works rather than that it was never taken.
        check(s.wait_events > 0,
              "callers did have to wait, so the contended path was exercised");
        check(s.wait_timeouts == 0, "no acquire timed out");
        std::cout << "  (longest single wait: " << s.longest_wait_ms << "ms across "
                  << s.wait_events << " waits)\n";
    }

    // Shutdown must release anyone still blocked rather than stranding them.
    pool.shutdown();
    check(pool.stats().shutting_down, "the pool reports that it is shutting down");

    std::cout << (failures == 0 ? "PASS" : "FAIL") << " (" << failures << " failure(s))\n";
    return failures == 0 ? 0 : 1;
}
