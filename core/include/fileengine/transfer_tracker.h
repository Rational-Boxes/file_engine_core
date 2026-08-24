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

#include <atomic>
#include <cstdint>

namespace fileengine {

/**
 * Counters for large object-store transfers that did NOT commit.
 *
 * A multipart upload that fails is invisible from every other angle. The object
 * never appears, so nothing references it; the local copy survives, so no data
 * is lost; and the abandoned parts are not in a normal bucket listing. The user
 * simply sees a large file that did not arrive, possibly after a long wait, and
 * the platform has nothing to say about it.
 *
 * That is the failure worth measuring. Small files take the single-PutObject
 * path, so everything counted here is by definition a big transfer — the ones
 * that cost the most time before failing and are the most frustrating to retry.
 *
 * `abort_failed` is the subset that leaves residue: the upload failed AND the
 * cleanup failed, so parts remain in the bucket until the store's
 * AbortIncompleteMultipartUpload lifecycle rule reaps them. A rising value there
 * means garbage is accumulating and the object store is unreliable in a way that
 * the ordinary failure count does not distinguish.
 *
 * Header-only and free-standing: a counter that needs a build-system change to
 * exist is a counter that does not get added.
 */
class TransferTracker {
public:
    /** Where a multipart upload gave up. Low cardinality, and diagnostic. */
    enum class Stage {
        OpenFailed,       // never read a byte — a local problem, not the network
        PartFailed,       // died mid-transfer, the usual "big upload over a bad link"
        CompleteFailed,   // every part landed and the commit failed — the cruellest
    };

    struct Stats {
        std::uint64_t completed = 0;
        std::uint64_t completed_bytes = 0;
        std::uint64_t aborted_open = 0;
        std::uint64_t aborted_part = 0;
        std::uint64_t aborted_complete = 0;
        std::uint64_t aborted_bytes = 0;   // transferred, then thrown away
        std::uint64_t abort_failed = 0;    // cleanup itself failed -> residue
    };

    static TransferTracker& instance() {
        static TransferTracker t;
        return t;
    }

    void record_completed(std::uint64_t bytes) {
        completed_.fetch_add(1, std::memory_order_relaxed);
        completed_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    }

    /** `bytes` is what actually made it across before giving up — the wasted work. */
    void record_aborted(Stage stage, std::uint64_t bytes) {
        switch (stage) {
            case Stage::OpenFailed:     aborted_open_.fetch_add(1, std::memory_order_relaxed); break;
            case Stage::PartFailed:     aborted_part_.fetch_add(1, std::memory_order_relaxed); break;
            case Stage::CompleteFailed: aborted_complete_.fetch_add(1, std::memory_order_relaxed); break;
        }
        aborted_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    }

    /** The abort call itself failed: parts are stranded until lifecycle reaps them. */
    void record_abort_failed() {
        abort_failed_.fetch_add(1, std::memory_order_relaxed);
    }

    Stats stats() const {
        return Stats{
            completed_.load(std::memory_order_relaxed),
            completed_bytes_.load(std::memory_order_relaxed),
            aborted_open_.load(std::memory_order_relaxed),
            aborted_part_.load(std::memory_order_relaxed),
            aborted_complete_.load(std::memory_order_relaxed),
            aborted_bytes_.load(std::memory_order_relaxed),
            abort_failed_.load(std::memory_order_relaxed),
        };
    }

    void reset_for_test() {
        completed_ = 0; completed_bytes_ = 0;
        aborted_open_ = 0; aborted_part_ = 0; aborted_complete_ = 0;
        aborted_bytes_ = 0; abort_failed_ = 0;
    }

private:
    TransferTracker() = default;

    std::atomic<std::uint64_t> completed_{0};
    std::atomic<std::uint64_t> completed_bytes_{0};
    std::atomic<std::uint64_t> aborted_open_{0};
    std::atomic<std::uint64_t> aborted_part_{0};
    std::atomic<std::uint64_t> aborted_complete_{0};
    std::atomic<std::uint64_t> aborted_bytes_{0};
    std::atomic<std::uint64_t> abort_failed_{0};
};

}  // namespace fileengine
