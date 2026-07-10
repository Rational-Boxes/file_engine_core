#include "audit_sink_factory.h"

#include "config_loader.h"
#include "server_logger.h"

#include <utility>

namespace fileengine {

std::shared_ptr<IAuditSink> make_audit_sink(const Config& config) {
    if (!config.audit_enabled) {
        return std::make_shared<NullAuditSink>();
    }

#ifdef FILEENGINE_HAS_EVENTS
    RedisAuditSink::Options opts;
    // The audit sink shares the deployment's Redis broker with the event sink,
    // but publishes to a SEPARATE, durable stream (§2). Connection params come
    // from the shared FILEENGINE_REDIS_* config (populated regardless of whether
    // the fail-open event stream is enabled).
    opts.host          = config.events_redis_host;
    opts.port          = config.events_redis_port;
    opts.password      = config.events_redis_password;
    opts.db            = config.events_redis_db;
    opts.stream        = config.audit_stream;
    opts.stream_maxlen = config.audit_stream_maxlen;
    opts.wal_path      = config.audit_wal_path;

    auto sink = std::make_shared<RedisAuditSink>(std::move(opts));
    sink->start();
    SERVER_LOG_INFO("AuditSink", "durable audit emission enabled -> " +
                                     config.events_redis_host + ":" +
                                     std::to_string(config.events_redis_port) +
                                     " (stream '" + config.audit_stream + "', WAL '" +
                                     config.audit_wal_path + "')");
    return sink;
#else
    SERVER_LOG_WARN("AuditSink",
                    "FILEENGINE_AUDIT_ENABLED is set but core was built without event/"
                    "hiredis support (FILEENGINE_ENABLE_EVENTS=OFF); auditing disabled");
    return std::make_shared<NullAuditSink>();
#endif
}

} // namespace fileengine
