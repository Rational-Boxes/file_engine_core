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

#include <string>
#include <vector>

namespace fileengine {

// One audit record, as produced by an emitter and serialized to the envelope
// documented in audit_service/AUDIT_CONTRACT.md. The string forms of the enums
// below MUST match audit_service/src/audit_service/codes.py (the writer maps them
// to the SMALLINT columns). Keep the two in lockstep; append, never renumber.
enum class AuditCategory { Access, Mutate, Permission, User, Auth, Admin };
enum class AuditOutcome  { Ok, Denied, Error };
enum class AuditScope    { Tenant, Global };
enum class AuditTargetType { None, File, Dir, Role, Acl, Version, Principal };

const char* to_string(AuditCategory c);
const char* to_string(AuditOutcome o);
const char* to_string(AuditTargetType t);

// Fail-closed categories (usage_logging_and_auditing.md §6): the guarded
// operation must be rejected if the entry cannot be durably captured. The
// caller — not the sink — enforces this by checking IAuditSink::publish().
bool is_fail_closed(AuditCategory c);

struct AuditEntry {
    AuditScope    scope = AuditScope::Tenant;
    std::string   tenant;                 // tenant id (NOT the schema); required for scope=Tenant
    AuditCategory category = AuditCategory::Access;
    std::string   action;
    AuditOutcome  outcome = AuditOutcome::Ok;
    std::string   actor;
    std::vector<std::string> actor_roles;
    std::string   target_uid;
    std::string   target_name;
    AuditTargetType target_type = AuditTargetType::None;
    std::string   detail;                 // raw JSON text (object), or empty
    std::string   source_iface;           // grpc|rest|webdav|mcp
    std::string   source_addr;            // client IP (forwarded by the bridge)
    std::string   request_id;
    // Filled by the sink at publish time when left empty:
    std::string   event_id;               // idempotency key (UUID)
    std::string   ts;                      // ISO-8601 UTC emit time
};

// Serialize an entry to the envelope JSON (single line). Fills event_id/ts when
// empty. `detail`, if non-empty, is embedded as a JSON value (parsed); if it is
// not valid JSON it is stored as a string so nothing is ever lost.
std::string to_json(const AuditEntry& entry);

// ISO-8601 UTC timestamp with millisecond precision, e.g.
// "2026-07-10T12:00:00.123Z" — parseable by the consumer's envelope reader.
std::string audit_iso8601_now();

} // namespace fileengine
