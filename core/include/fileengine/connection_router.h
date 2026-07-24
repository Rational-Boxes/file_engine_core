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

#ifndef FILEENGINE_CONNECTION_ROUTER_H
#define FILEENGINE_CONNECTION_ROUTER_H

// Read/write connection routing for primary/replica failover
// (design_documents/REPLICATION_FAILOVER.md).
//
// The decision is a pure function so it is exhaustively unit-testable without a
// database (see tests/test_connection_router.cpp):
//
//   - WRITE operations always use the primary connection. (During a primary
//     outage writes are rejected upstream at the gRPC layer; one that slips
//     through still never touches the read-only replica.)
//   - READ operations use the replica only while failed over to it, otherwise
//     the primary. With no replica configured, reads use the primary.

namespace fileengine {

enum class DbOp { Read, Write };

// Pick the pool a given operation must use. ``PoolPtr`` is any pointer-like type
// (raw pointer in production, sentinel pointer in tests). ``secondary`` may be
// null when no replica is configured.
template <typename PoolPtr>
PoolPtr select_pool(DbOp op, PoolPtr primary, PoolPtr secondary, bool using_secondary) {
    if (op == DbOp::Write) {
        return primary;  // writes are never routed to the read-only replica
    }
    return (using_secondary && secondary) ? secondary : primary;
}

// The failover state driven by the connection monitor. Extracted as a pure
// transition so the disconnect/recovery state machine is unit-testable without a
// database or a background thread (the DB is modelled by ``primary_reachable``).
struct FailoverState {
    bool primary_available = true;
    bool using_secondary = false;
    bool readonly_mode = false;

    bool operator==(const FailoverState& o) const {
        return primary_available == o.primary_available
            && using_secondary == o.using_secondary
            && readonly_mode == o.readonly_mode;
    }
    bool operator!=(const FailoverState& o) const { return !(*this == o); }
};

// Compute the next state from a fresh primary-reachability probe:
//   - up   + unreachable -> enter read-only fallback (use the replica if present);
//   - down + reachable   -> resume normal operation;
//   - otherwise          -> unchanged.
inline FailoverState next_failover_state(FailoverState cur, bool primary_reachable, bool has_secondary) {
    if (cur.primary_available && !primary_reachable) {
        return FailoverState{/*primary_available=*/false, /*using_secondary=*/has_secondary,
                             /*readonly_mode=*/true};
    }
    if (!cur.primary_available && primary_reachable) {
        return FailoverState{/*primary_available=*/true, /*using_secondary=*/false,
                             /*readonly_mode=*/false};
    }
    return cur;
}

}  // namespace fileengine

#endif  // FILEENGINE_CONNECTION_ROUTER_H
