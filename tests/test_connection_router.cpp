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

// Exhaustive unit tests for read/write connection routing during primary/replica
// failover (REPLICATION_FAILOVER.md). Pure pointer logic — no database needed.
//
// Build: g++ -std=c++17 -I core/include tests/test_connection_router.cpp -o router_tests
#include "fileengine/connection_router.h"

#include <cassert>
#include <cstdio>

using fileengine::DbOp;
using fileengine::select_pool;
using fileengine::FailoverState;
using fileengine::next_failover_state;

// Exercise the connection-monitor's disconnect/recovery state machine. The
// database is "mocked" by the boolean `primary_reachable` (true = the probe
// reached the primary, false = it did not).
static void test_failover_state_machine() {
    const FailoverState HEALTHY{true, false, false};

    // healthy + reachable -> unchanged
    assert(next_failover_state(HEALTHY, /*reachable=*/true, /*has_secondary=*/true) == HEALTHY);

    // healthy + UNREACHABLE + replica present -> read-only, using the replica
    {
        FailoverState n = next_failover_state(HEALTHY, false, /*has_secondary=*/true);
        assert(!n.primary_available && n.using_secondary && n.readonly_mode);
    }
    // healthy + UNREACHABLE + NO replica -> read-only, but not using a (missing) replica
    {
        FailoverState n = next_failover_state(HEALTHY, false, /*has_secondary=*/false);
        assert(!n.primary_available && !n.using_secondary && n.readonly_mode);
    }

    // degraded + still unreachable -> stays degraded (no flapping)
    {
        FailoverState degraded{false, true, true};
        assert(next_failover_state(degraded, false, true) == degraded);
    }
    // degraded + reachable again -> full recovery
    {
        FailoverState degraded{false, true, true};
        FailoverState n = next_failover_state(degraded, true, true);
        assert(n.primary_available && !n.using_secondary && !n.readonly_mode);
    }

    // full sequence: healthy -> down -> down -> recovered, and pools route accordingly
    int primary_obj = 1, secondary_obj = 2;
    int* P = &primary_obj;
    int* S = &secondary_obj;
    FailoverState s = HEALTHY;
    assert(select_pool(DbOp::Read, P, S, s.using_secondary) == P);          // healthy -> primary
    s = next_failover_state(s, /*reachable=*/false, true);                  // primary lost
    assert(s.readonly_mode && select_pool(DbOp::Read, P, S, s.using_secondary) == S);  // read replica
    assert(select_pool(DbOp::Write, P, S, s.using_secondary) == P);         // write still primary
    s = next_failover_state(s, false, true);                               // still down
    assert(s.readonly_mode);
    s = next_failover_state(s, /*reachable=*/true, true);                   // recovered
    assert(!s.readonly_mode && select_pool(DbOp::Read, P, S, s.using_secondary) == P);

    std::puts("failover_state_machine: OK");
}

int main() {
    // Sentinel "pools" — only identity matters.
    int primary_obj = 1, secondary_obj = 2;
    int* P = &primary_obj;
    int* S = &secondary_obj;
    int* NUL = nullptr;

    // --- WRITES always go to the primary, regardless of state ---
    assert(select_pool(DbOp::Write, P, S, /*using_secondary=*/false) == P);
    assert(select_pool(DbOp::Write, P, S, /*using_secondary=*/true) == P);   // never the replica
    assert(select_pool(DbOp::Write, P, NUL, false) == P);
    assert(select_pool(DbOp::Write, P, NUL, true) == P);

    // --- READS: primary in normal operation ---
    assert(select_pool(DbOp::Read, P, S, /*using_secondary=*/false) == P);
    assert(select_pool(DbOp::Read, P, NUL, false) == P);

    // --- READS: replica only while failed over AND a replica exists ---
    assert(select_pool(DbOp::Read, P, S, /*using_secondary=*/true) == S);    // failover -> replica
    assert(select_pool(DbOp::Read, P, NUL, /*using_secondary=*/true) == P);  // no replica -> primary

    // --- transition sequence: healthy -> failover -> recovered ---
    bool failed_over = false;
    assert(select_pool(DbOp::Read, P, S, failed_over) == P);   // healthy: read primary
    failed_over = true;
    assert(select_pool(DbOp::Read, P, S, failed_over) == S);   // degraded: read replica
    assert(select_pool(DbOp::Write, P, S, failed_over) == P);  // degraded: write still primary
    failed_over = false;
    assert(select_pool(DbOp::Read, P, S, failed_over) == P);   // recovered: read primary again

    std::puts("connection_router: OK");

    test_failover_state_machine();

    std::puts("all failover unit tests passed");
    return 0;
}
