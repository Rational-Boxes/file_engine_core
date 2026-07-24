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

#include "event_sink_factory.h"

#include "config_loader.h"
#include "server_logger.h"

#ifdef FILEENGINE_HAS_EVENTS
#include "redis_event_sink.h"
#include <utility>
#endif

namespace fileengine {

std::shared_ptr<IEventSink> make_event_sink(const Config& config) {
    if (!config.events_enabled) return nullptr;

#ifdef FILEENGINE_HAS_EVENTS
    RedisEventSink::Options opts;
    opts.host            = config.events_redis_host;
    opts.port            = config.events_redis_port;
    opts.password        = config.events_redis_password;
    opts.db              = config.events_redis_db;
    opts.stream          = config.events_stream;
    opts.stream_maxlen   = config.events_stream_maxlen;
    opts.outbox_capacity = config.events_outbox_capacity;

    auto sink = std::make_shared<RedisEventSink>(std::move(opts));
    sink->start();
    SERVER_LOG_INFO("EventSink", "Redis event emission enabled -> " +
                                     config.events_redis_host + ":" +
                                     std::to_string(config.events_redis_port) +
                                     " (stream '" + config.events_stream + "')");
    return sink;
#else
    SERVER_LOG_WARN("EventSink",
                    "FILEENGINE_EVENTS_ENABLED is set but core was built without event "
                    "support (FILEENGINE_ENABLE_EVENTS=OFF); events disabled");
    return nullptr;
#endif
}

} // namespace fileengine
