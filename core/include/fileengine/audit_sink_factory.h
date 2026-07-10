#pragma once

#include "audit_sink.h"

#include <memory>

namespace fileengine {

struct Config;

// Build the process's audit sink. Returns a durable RedisAuditSink when auditing
// is enabled and the core was built with event/hiredis support; otherwise a
// NullAuditSink (never nullptr), so call sites can always publish() without a
// null-check and disabled auditing never blocks an operation. The returned sink
// is already start()ed.
std::shared_ptr<IAuditSink> make_audit_sink(const Config& config);

} // namespace fileengine
