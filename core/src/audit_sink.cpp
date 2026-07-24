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

#include "audit_sink.h"

#include "server_logger.h"

#include <hiredis/hiredis.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace fileengine {

RedisAuditSink::RedisAuditSink(Options options) : opts_(std::move(options)) {}

RedisAuditSink::~RedisAuditSink() {
    stop();
    disconnect();
    if (wal_fd_ >= 0) {
        ::close(wal_fd_);
        wal_fd_ = -1;
    }
}

// ---------------------------------------------------------------- WAL --------

bool RedisAuditSink::wal_append(const std::string& line) {
    if (wal_fd_ < 0) return false;
    const char* p = line.data();
    std::size_t n = line.size();
    while (n > 0) {
        ssize_t w = ::write(wal_fd_, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return ::fsync(wal_fd_) == 0;
}

void RedisAuditSink::wal_truncate() {
    // Called with mutex_ held and pending_ empty: everything is delivered, so
    // the WAL can be reset to zero. O_APPEND writes then resume at offset 0.
    if (wal_fd_ >= 0) {
        if (::ftruncate(wal_fd_, 0) != 0) {
            SERVER_LOG_WARN("AuditSink", "WAL truncate failed; backlog will re-deliver on restart");
        }
    }
}

void RedisAuditSink::wal_recover() {
    std::ifstream in(opts_.wal_path);
    if (!in) return;
    std::string line;
    std::size_t recovered = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        pending_.push_back(line + "\n");
        ++recovered;
    }
    if (recovered) {
        SERVER_LOG_INFO("AuditSink", "recovered " + std::to_string(recovered) +
                                         " un-truncated audit entr(ies) from WAL for re-delivery");
    }
}

// -------------------------------------------------------------- lifecycle ----

void RedisAuditSink::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;

    wal_fd_ = ::open(opts_.wal_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0640);
    if (wal_fd_ < 0) {
        SERVER_LOG_ERROR("AuditSink", "cannot open audit WAL '" + opts_.wal_path +
                                          "': " + std::string(std::strerror(errno)) +
                                          " — durable publish will fail (fail-closed ops rejected)");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        wal_recover();  // reload any un-truncated backlog before the worker starts
    }
    worker_ = std::thread(&RedisAuditSink::run, this);
}

void RedisAuditSink::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

// ---------------------------------------------------------------- publish ----

bool RedisAuditSink::publish(AuditEntry entry) {
    std::string line = to_json(entry);
    line.push_back('\n');  // one envelope per WAL line
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!wal_append(line)) {
            SERVER_LOG_ERROR("AuditSink", "audit WAL append failed — entry NOT durable");
            return false;  // fail-closed caller must reject the guarded op
        }
        pending_.push_back(std::move(line));
    }
    cv_.notify_one();
    return true;  // durably captured in the WAL
}

std::size_t RedisAuditSink::buffered() {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

// ---------------------------------------------------------------- worker -----

void RedisAuditSink::run() {
    while (true) {
        std::string line;
        bool stopping;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !running_.load() || !pending_.empty(); });
            stopping = !running_.load();
            if (pending_.empty()) return;  // drained and stopping
            line = pending_.front();       // peek; pop only after a confirmed deliver
        }

        bool ok = false;
        try {
            ok = xadd(line);
        } catch (...) {
            ok = false;
        }

        if (ok) {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.pop_front();
            delivered_.fetch_add(1);
            if (pending_.empty()) wal_truncate();
        } else if (stopping) {
            // Give up delivering now; the WAL persists it for the next start().
            return;
        } else {
            // Durable in the WAL — back off and retry, waking early on stop.
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(opts_.retry_ms),
                         [this] { return !running_.load(); });
        }
    }
}

// ----------------------------------------------------------------- redis -----

void RedisAuditSink::disconnect() {
    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
}

void RedisAuditSink::ensure_connected() {
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

bool RedisAuditSink::xadd(const std::string& line) {
    ensure_connected();
    // The line carries a trailing '\n' from the WAL; strip it for the payload.
    std::size_t len = line.size();
    if (len && line[len - 1] == '\n') --len;

    auto* reply = static_cast<redisReply*>(redisCommand(
        ctx_, "XADD %s MAXLEN ~ %lld * payload %b",
        opts_.stream.c_str(), opts_.stream_maxlen, line.data(), len));

    if (!reply || ctx_->err) {
        if (reply) freeReplyObject(reply);
        disconnect();
        return false;
    }
    if (reply->type == REDIS_REPLY_ERROR) {
        freeReplyObject(reply);
        return false;
    }
    freeReplyObject(reply);
    return true;
}

} // namespace fileengine
