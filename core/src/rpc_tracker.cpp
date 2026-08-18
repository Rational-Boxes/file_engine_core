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

#include "fileengine/rpc_tracker.h"

namespace fileengine {

RpcTracker& RpcTracker::instance() {
    static RpcTracker t;
    return t;
}

std::uint64_t RpcTracker::begin(const std::string& method) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::uint64_t id = next_id_++;
    in_flight_.emplace(id, Entry{method, std::chrono::steady_clock::now()});
    ++started_total_;
    return id;
}

void RpcTracker::end(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    // erase() returns how many were removed, so a double-end cannot inflate the
    // completed count past what actually started.
    if (in_flight_.erase(id) > 0) {
        ++completed_total_;
    }
}

RpcStats RpcTracker::stats() const {
    RpcStats s;
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(mu_);
    s.in_flight       = static_cast<int>(in_flight_.size());
    s.started_total   = started_total_;
    s.completed_total = completed_total_;

    for (const auto& [id, e] : in_flight_) {
        (void)id;
        const auto age = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - e.started).count());
        ++s.by_method[e.method];
        s.in_flight_detail.push_back(InFlightRpc{e.method, age});
        if (age > s.oldest_age_ms) {
            s.oldest_age_ms = age;
            s.oldest_method = e.method;
        }
    }
    return s;
}

void RpcTracker::reset_for_test() {
    std::lock_guard<std::mutex> lock(mu_);
    in_flight_.clear();
    started_total_ = 0;
    completed_total_ = 0;
    next_id_ = 1;
}

} // namespace fileengine
