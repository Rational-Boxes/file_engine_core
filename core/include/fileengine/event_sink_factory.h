#pragma once

#include "event_sink.h"

#include <memory>

namespace fileengine {

struct Config;

// Build the configured event sink, or nullptr when events are disabled at
// runtime (config.events_enabled == false) or not compiled in
// (FILEENGINE_ENABLE_EVENTS=OFF). A returned sink is already start()ed.
std::shared_ptr<IEventSink> make_event_sink(const Config& config);

} // namespace fileengine
