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

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace fileengine {

// One RPC currently being served.
struct InFlightRpc {
    std::string   method;
    std::uint64_t age_ms = 0;
};

struct RpcStats {
    int           in_flight = 0;
    std::uint64_t started_total   = 0;
    std::uint64_t completed_total = 0;
    // The oldest RPC still running. This is the stuck-request signal: file
    // operations here finish in milliseconds, so anything measured in minutes is
    // wedged, and naming the method says where.
    std::uint64_t oldest_age_ms   = 0;
    std::string   oldest_method;
    std::map<std::string, int> by_method;   // in-flight, per method
    std::vector<InFlightRpc>   in_flight_detail;
};

// Process-wide register of in-flight RPCs.
//
// Kept separate from the service implementation so it can be driven by a gRPC
// interceptor: the interceptor is constructed once per RPC and destroyed when
// that RPC ends, which makes construction and destruction an exact match for the
// request's lifetime. Adding a guard to each of the 41 handlers by hand would
// cover the same ground with 41 chances to forget one.
class RpcTracker {
public:
    static RpcTracker& instance();

    // Returns an id used to close the entry out again.
    std::uint64_t begin(const std::string& method);
    void end(std::uint64_t id);

    RpcStats stats() const;

    // Test seam: forget everything. Never call this on a live server.
    void reset_for_test();

private:
    RpcTracker() = default;

    struct Entry {
        std::string method;
        std::chrono::steady_clock::time_point started;
    };

    mutable std::mutex mu_;
    std::map<std::uint64_t, Entry> in_flight_;
    std::uint64_t next_id_ = 1;
    std::uint64_t started_total_ = 0;
    std::uint64_t completed_total_ = 0;
};

} // namespace fileengine
