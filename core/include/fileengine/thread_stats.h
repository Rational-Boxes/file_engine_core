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

#pragma once

#include <cstdint>
#include <string>

namespace fileengine {

// What this process's threads are actually doing, read from the kernel rather
// than inferred.
//
// A thread count on its own says almost nothing here. The gRPC sync server parks
// a fixed set of pollers that sit in interruptible sleep for the life of the
// process, so a large total is normal and expected. The question worth asking is
// how many threads are NOT waiting: `running` is work in progress, and
// `uninterruptible` is the one to alarm on, because a thread stuck in D state is
// blocked in the kernel on I/O and cannot be interrupted or timed out.
//
// Linux-specific by construction (it reads /proc/self/task). On a platform
// without it, `available` is false and the counts are zero — a monitoring
// endpoint should say "unknown" rather than report zeros as fact.
struct ThreadStats {
    bool available = false;

    int total = 0;

    // Kernel thread states, per proc(5):
    //   R running or runnable        — doing work now
    //   S interruptible sleep        — waiting (a parked poller, a CV wait)
    //   D uninterruptible sleep      — blocked in the kernel, usually I/O
    //   T stopped / traced
    //   Z zombie
    int running        = 0;
    int sleeping       = 0;
    int uninterruptible = 0;
    int stopped        = 0;
    int zombie         = 0;
    int other          = 0;

    // total - sleeping. The number a healthy idle server should hold near zero:
    // with no requests in flight every worker ought to be parked waiting for one.
    int not_waiting() const { return total - sleeping; }
};

// Reads /proc/self/task. Cheap for the thread counts involved here (a few
// hundred small reads at most) but not free — call it from a monitoring
// endpoint, not from a request path.
ThreadStats read_thread_stats();

} // namespace fileengine
