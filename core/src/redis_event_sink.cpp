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

#include "redis_event_sink.h"

#include <hiredis/hiredis.h>

#include <stdexcept>
#include <utility>

namespace fileengine {

RedisEventSink::RedisEventSink(Options options)
    : QueueingEventSink(options.outbox_capacity), opts_(std::move(options)) {}

RedisEventSink::~RedisEventSink() {
    stop();        // join the worker before tearing down the connection it uses
    disconnect();
}

void RedisEventSink::disconnect() {
    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
}

void RedisEventSink::ensure_connected() {
    if (ctx_ && ctx_->err == 0) return;
    disconnect();

    ctx_ = redisConnect(opts_.host.c_str(), opts_.port);
    if (!ctx_ || ctx_->err) {
        std::string err = ctx_ ? ctx_->errstr : "allocation failed";
        disconnect();
        throw std::runtime_error("redis connect failed: " + err);
    }

    if (!opts_.password.empty()) {
        auto* r = static_cast<redisReply*>(redisCommand(ctx_, "AUTH %s", opts_.password.c_str()));
        bool ok = r && r->type != REDIS_REPLY_ERROR;
        if (r) freeReplyObject(r);
        if (!ok) {
            disconnect();
            throw std::runtime_error("redis AUTH failed");
        }
    }

    if (opts_.db != 0) {
        auto* r = static_cast<redisReply*>(redisCommand(ctx_, "SELECT %d", opts_.db));
        bool ok = r && r->type != REDIS_REPLY_ERROR;
        if (r) freeReplyObject(r);
        if (!ok) {
            disconnect();
            throw std::runtime_error("redis SELECT failed");
        }
    }
}

void RedisEventSink::deliver(const FileEvent& event) {
    ensure_connected();

    // Single shared stream; the tenant travels in the JSON payload.
    const std::string payload = to_json(event);

    // XADD <stream> MAXLEN ~ <maxlen> * payload <json>
    auto* reply = static_cast<redisReply*>(redisCommand(
        ctx_, "XADD %s MAXLEN ~ %lld * payload %b",
        opts_.stream.c_str(), opts_.stream_maxlen, payload.data(), payload.size()));

    if (!reply || ctx_->err) {
        std::string err = ctx_ ? ctx_->errstr : "null reply";
        if (reply) freeReplyObject(reply);
        disconnect();   // force a fresh connection on the next event
        throw std::runtime_error("redis XADD failed: " + err);
    }
    if (reply->type == REDIS_REPLY_ERROR) {
        std::string err = reply->str ? reply->str : "redis error";
        freeReplyObject(reply);
        throw std::runtime_error("redis XADD error: " + err);
    }
    freeReplyObject(reply);
}

} // namespace fileengine
