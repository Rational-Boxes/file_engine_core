#pragma once

#include "event_sink.h"

#include <cstddef>
#include <string>

struct redisContext;  // forward-declared; hiredis is only pulled into the .cpp

namespace fileengine {

// Redis Streams implementation of the event sink. Every event is XADD'd to a
// single shared stream (the tenant is an event field; consumers are multi-tenant
// aware) with a single JSON "payload" field. Connection is lazy with
// reconnect-on-failure; while disconnected the bounded outbox (from
// QueueingEventSink) absorbs events up to capacity then drops the oldest — the
// filesystem operation is never affected (fail-open).
class RedisEventSink : public QueueingEventSink {
public:
    struct Options {
        std::string host = "localhost";
        int         port = 6379;
        std::string password;
        int         db = 0;
        std::string stream = "fileengine:events";
        long long   stream_maxlen = 100000;
        std::size_t outbox_capacity = 10000;
    };

    explicit RedisEventSink(Options options);
    ~RedisEventSink() override;

protected:
    void deliver(const FileEvent& event) override;

private:
    void ensure_connected();   // throws on failure
    void disconnect();

    Options opts_;
    redisContext* ctx_ = nullptr;
};

} // namespace fileengine
