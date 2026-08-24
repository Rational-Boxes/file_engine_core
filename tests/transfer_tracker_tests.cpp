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
//
// Counters for large uploads that did not commit.
//
// These exist because a failed multipart upload is otherwise invisible: no
// object appears, nothing references it, the abandoned parts are absent from a
// normal bucket listing, and the local copy survives so nothing is lost. The
// only symptom is a user reporting that a big file "didn't work". What is
// asserted here is the SHAPE of that signal — that the stages stay separate, so
// a graph can distinguish a bad link from a bad store, and that stranded residue
// is counted apart from ordinary failure.

#include "fileengine/transfer_tracker.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using fileengine::TransferTracker;
using Stage = fileengine::TransferTracker::Stage;

static TransferTracker& fresh() {
    auto& t = TransferTracker::instance();
    t.reset_for_test();
    return t;
}

// A new process has nothing to report. Worth pinning: a counter that starts
// non-zero makes every rate wrong for the first scrape.
static void test_starts_empty() {
    auto s = fresh().stats();
    assert(s.completed == 0);
    assert(s.completed_bytes == 0);
    assert(s.aborted_open == 0);
    assert(s.aborted_part == 0);
    assert(s.aborted_complete == 0);
    assert(s.aborted_bytes == 0);
    assert(s.abort_failed == 0);
    std::cout << "  starts empty\n";
}

// The stage is the diagnosis, so the three must never bleed into each other:
// "open" is a local fault, "part" is the link, "complete" is the store.
static void test_stages_are_counted_separately() {
    auto& t = fresh();
    t.record_aborted(Stage::OpenFailed, 0);
    t.record_aborted(Stage::PartFailed, 1000);
    t.record_aborted(Stage::PartFailed, 2000);
    t.record_aborted(Stage::CompleteFailed, 8 * 1024 * 1024);

    auto s = t.stats();
    assert(s.aborted_open == 1);
    assert(s.aborted_part == 2);
    assert(s.aborted_complete == 1);
    std::cout << "  stages counted separately\n";
}

// The frustration signal: bytes a user waited to send and did not get. It
// accumulates across every stage, because the waste is the same whatever killed
// it — and an upload that died before reading a byte contributes nothing.
static void test_aborted_bytes_accumulate_across_stages() {
    auto& t = fresh();
    t.record_aborted(Stage::OpenFailed, 0);
    t.record_aborted(Stage::PartFailed, 1500);
    t.record_aborted(Stage::CompleteFailed, 2500);

    assert(t.stats().aborted_bytes == 4000);
    std::cout << "  aborted bytes accumulate across stages\n";
}

// A failed abort is not just another failure — it leaves parts stranded in the
// bucket. It has to be countable on its own or there is no way to alert on
// "garbage is accumulating" as distinct from "uploads are failing".
static void test_abort_failure_is_separate_from_abort() {
    auto& t = fresh();
    t.record_aborted(Stage::PartFailed, 4096);
    // The upload failed AND the cleanup failed.
    t.record_abort_failed();

    auto s = t.stats();
    assert(s.aborted_part == 1);
    assert(s.abort_failed == 1);

    // ...and a failure whose cleanup SUCCEEDED must not touch it, or the
    // residue counter would just track the failure counter and say nothing.
    t.record_aborted(Stage::PartFailed, 4096);
    assert(t.stats().aborted_part == 2);
    assert(t.stats().abort_failed == 1);
    std::cout << "  abort failure counted apart from abort\n";
}

// Success and failure are disjoint: one upload lands in exactly one of them.
static void test_completed_is_independent() {
    auto& t = fresh();
    t.record_completed(10 * 1024 * 1024);
    t.record_aborted(Stage::CompleteFailed, 9 * 1024 * 1024);

    auto s = t.stats();
    assert(s.completed == 1);
    assert(s.completed_bytes == 10 * 1024 * 1024);
    assert(s.aborted_complete == 1);
    assert(s.aborted_bytes == 9 * 1024 * 1024);
    // Bytes must not be double-counted into the other total.
    assert(s.completed_bytes != s.aborted_bytes);
    std::cout << "  completed independent of aborted\n";
}

// Uploads run concurrently — the sync worker and request threads both reach
// this. Counters that lose increments under contention would understate exactly
// the incident they exist to reveal.
static void test_concurrent_updates_do_not_lose_counts() {
    auto& t = fresh();
    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&t] {
            for (int j = 0; j < kPerThread; ++j) {
                t.record_aborted(Stage::PartFailed, 100);
                t.record_completed(200);
            }
        });
    }
    for (auto& th : threads) th.join();

    auto s = t.stats();
    assert(s.aborted_part == kThreads * kPerThread);
    assert(s.completed == kThreads * kPerThread);
    assert(s.aborted_bytes == static_cast<std::uint64_t>(kThreads) * kPerThread * 100);
    assert(s.completed_bytes == static_cast<std::uint64_t>(kThreads) * kPerThread * 200);
    std::cout << "  no counts lost under concurrency\n";
}

// Monotonic: these are Prometheus counters, and a counter that goes down is read
// as a process restart. Only the test hook may reset them.
static void test_counters_are_monotonic() {
    auto& t = fresh();
    t.record_completed(1);
    auto first = t.stats().completed;
    t.record_completed(1);
    assert(t.stats().completed > first);
    std::cout << "  counters are monotonic\n";
}

int main() {
    std::cout << "TransferTracker tests\n";
    test_starts_empty();
    test_stages_are_counted_separately();
    test_aborted_bytes_accumulate_across_stages();
    test_abort_failure_is_separate_from_abort();
    test_completed_is_independent();
    test_concurrent_updates_do_not_lose_counts();
    test_counters_are_monotonic();
    std::cout << "All TransferTracker tests passed\n";
    return 0;
}
