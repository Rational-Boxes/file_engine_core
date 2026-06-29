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

}  // namespace fileengine

#endif  // FILEENGINE_CONNECTION_ROUTER_H
