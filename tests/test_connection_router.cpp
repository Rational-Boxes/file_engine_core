// Exhaustive unit tests for read/write connection routing during primary/replica
// failover (REPLICATION_FAILOVER.md). Pure pointer logic — no database needed.
//
// Build: g++ -std=c++17 -I core/include tests/test_connection_router.cpp -o router_tests
#include "fileengine/connection_router.h"

#include <cassert>
#include <cstdio>

using fileengine::DbOp;
using fileengine::select_pool;

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
    return 0;
}
