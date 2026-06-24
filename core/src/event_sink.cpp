#include "event_sink.h"

#include "server_logger.h"

#include <exception>

namespace fileengine {

QueueingEventSink::QueueingEventSink(std::size_t capacity)
    : capacity_(capacity ? capacity : 1) {}

QueueingEventSink::~QueueingEventSink() { stop(); }

void QueueingEventSink::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;  // already running
    worker_ = std::thread(&QueueingEventSink::run, this);
}

void QueueingEventSink::stop() {
    if (!running_.exchange(false)) return;  // not running
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void QueueingEventSink::publish(const FileEvent& event) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= capacity_) {
            // Outbox full — drop the OLDEST event to make room for the newest, so
            // the freshest activity wins (reconcile covers the dropped one).
            queue_.pop();
            dropped_.fetch_add(1);
        }
        queue_.push(event);
    } catch (...) {
        dropped_.fetch_add(1);
        return;
    }
    cv_.notify_one();
}

std::size_t QueueingEventSink::depth() {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void QueueingEventSink::run() {
    while (true) {
        FileEvent event;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !running_.load() || !queue_.empty(); });
            if (queue_.empty()) {
                // Woken with nothing left to drain => we are stopping.
                return;
            }
            event = std::move(queue_.front());
            queue_.pop();
        }
        try {
            deliver(event);
            emitted_.fetch_add(1);
        } catch (const std::exception& ex) {
            dropped_.fetch_add(1);
            SERVER_LOG_ERROR("EventSink", std::string("event delivery failed: ") + ex.what());
        } catch (...) {
            dropped_.fetch_add(1);
            SERVER_LOG_ERROR("EventSink", "event delivery failed (unknown error)");
        }
    }
}

} // namespace fileengine
