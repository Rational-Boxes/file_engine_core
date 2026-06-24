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
