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
