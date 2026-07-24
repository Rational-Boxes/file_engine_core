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
