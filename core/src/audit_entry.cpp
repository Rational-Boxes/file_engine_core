#include "audit_entry.h"

#include "json.hpp"
#include "utils.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace fileengine {

const char* to_string(AuditCategory c) {
    switch (c) {
        case AuditCategory::Access:     return "access";
        case AuditCategory::Mutate:     return "mutate";
        case AuditCategory::Permission: return "permission";
        case AuditCategory::User:       return "user";
        case AuditCategory::Auth:       return "auth";
        case AuditCategory::Admin:      return "admin";
    }
    return "access";
}

const char* to_string(AuditOutcome o) {
    switch (o) {
        case AuditOutcome::Ok:     return "ok";
        case AuditOutcome::Denied: return "denied";
        case AuditOutcome::Error:  return "error";
    }
    return "ok";
}

const char* to_string(AuditTargetType t) {
    switch (t) {
        case AuditTargetType::None:      return "";
        case AuditTargetType::File:      return "file";
        case AuditTargetType::Dir:       return "dir";
        case AuditTargetType::Role:      return "role";
        case AuditTargetType::Acl:       return "acl";
        case AuditTargetType::Version:   return "version";
        case AuditTargetType::Principal: return "principal";
    }
    return "";
}

bool is_fail_closed(AuditCategory c) {
    // permission | user | auth | admin are fail-closed by default (§6).
    return c == AuditCategory::Permission || c == AuditCategory::User ||
           c == AuditCategory::Auth || c == AuditCategory::Admin;
}

std::string audit_iso8601_now() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % 1000;
    std::tm tm_utc{};
    gmtime_r(&t, &tm_utc);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_utc);
    std::ostringstream ss;
    ss << buf << '.' << std::setw(3) << std::setfill('0') << ms.count() << 'Z';
    return ss.str();
}

std::string to_json(const AuditEntry& e) {
    nlohmann::json j;
    j["event_id"] = e.event_id.empty() ? Utils::generate_uuid() : e.event_id;
    j["ts"]       = e.ts.empty() ? audit_iso8601_now() : e.ts;
    j["scope"]    = (e.scope == AuditScope::Global) ? "global" : "tenant";
    if (!e.tenant.empty()) j["tenant"] = e.tenant;
    j["category"] = to_string(e.category);
    j["action"]   = e.action;
    j["outcome"]  = to_string(e.outcome);
    j["actor"]    = e.actor;
    if (!e.actor_roles.empty()) j["actor_roles"] = e.actor_roles;
    if (!e.target_uid.empty())  j["target_uid"]  = e.target_uid;
    if (!e.target_name.empty()) j["target_name"] = e.target_name;
    if (e.target_type != AuditTargetType::None) j["target_type"] = to_string(e.target_type);
    if (!e.detail.empty()) {
        // Embed as a JSON value when parseable; otherwise keep the raw text so
        // an audit entry is never dropped for a malformed detail blob.
        auto parsed = nlohmann::json::parse(e.detail, nullptr, /*allow_exceptions=*/false);
        j["detail"] = parsed.is_discarded() ? nlohmann::json(e.detail) : parsed;
    }
    if (!e.source_iface.empty()) j["source_iface"] = e.source_iface;
    if (!e.source_addr.empty())  j["source_addr"]  = e.source_addr;
    if (!e.request_id.empty())   j["request_id"]   = e.request_id;
    return j.dump();
}

} // namespace fileengine
