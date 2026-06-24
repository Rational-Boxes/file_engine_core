#pragma once

#include "event.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>

namespace fileengine {

// Broker-agnostic event sink. publish() is noexcept to enforce the fail-open
// contract: an emission failure must never disturb the filesystem operation
// that triggered it.
class IEventSink {
public:
    virtual ~IEventSink() = default;
    virtual void publish(const FileEvent& event) noexcept = 0;
    virtual void start() {}
    virtual void stop() {}
};

// No-op sink — used in tests and as the disabled/not-compiled-in fallback.
class NullEventSink : public IEventSink {
public:
    void publish(const FileEvent&) noexcept override {}
};

// Base sink with a bounded in-memory outbox drained by a single worker thread,
// mirroring FileSystem's async object-store backup worker. publish() only
// enqueues (dropping + counting when the outbox is full); a subclass delivers
// each event to the transport via deliver(). Drains the queue on stop().
class QueueingEventSink : public IEventSink {
public:
    explicit QueueingEventSink(std::size_t capacity);
    ~QueueingEventSink() override;

    void publish(const FileEvent& event) noexcept override;
    void start() override;
    void stop() override;

    std::uint64_t emitted() const { return emitted_.load(); }
    std::uint64_t dropped() const { return dropped_.load(); }
    std::size_t   depth();

protected:
    // Deliver one event to the transport. Runs on the worker thread; may throw —
    // the loop catches the failure and counts it as a drop.
    virtual void deliver(const FileEvent& event) = 0;

private:
    void run();

    std::size_t capacity_;
    std::queue<FileEvent> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> running_{false};

    std::atomic<std::uint64_t> emitted_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

} // namespace fileengine
