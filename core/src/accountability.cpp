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

#include "fileengine/accountability.h"

#include <openssl/evp.h>

#include "json.hpp"

#include <algorithm>
#include <sstream>

namespace fileengine {

const char* to_string(AccountabilityCategory c) {
    switch (c) {
        case AccountabilityCategory::Authorization: return "authorization";
        case AccountabilityCategory::Identity:      return "identity";
        case AccountabilityCategory::Destruction:   return "destruction";
        case AccountabilityCategory::Lifecycle:     return "lifecycle";
    }
    return "authorization";
}

namespace {

// JSON string escaping that matches Python's json.dumps(ensure_ascii=True)
// byte for byte. This is not a stylistic choice: the consumer re-hashes each row
// from the values it received, so a single differing escape makes every chain
// verification fail. ASCII-escaping everything above U+007F also removes any
// question of what "the UTF-8 bytes" were when a name contains non-Latin text.
std::string json_quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"')       { out += "\\\""; ++i; continue; }
        if (c == '\\')      { out += "\\\\"; ++i; continue; }
        if (c == '\n')      { out += "\\n";  ++i; continue; }
        if (c == '\r')      { out += "\\r";  ++i; continue; }
        if (c == '\t')      { out += "\\t";  ++i; continue; }
        if (c == '\b')      { out += "\\b";  ++i; continue; }
        if (c == '\f')      { out += "\\f";  ++i; continue; }
        if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
            ++i;
            continue;
        }
        if (c < 0x80) { out.push_back(static_cast<char>(c)); ++i; continue; }

        // Decode one UTF-8 sequence to a code point, then emit \uXXXX (with a
        // surrogate pair above the BMP), exactly as ensure_ascii does. A byte
        // that is not valid UTF-8 is emitted as U+FFFD rather than passed
        // through, so the canonical form is always valid ASCII JSON.
        std::uint32_t cp = 0xFFFD;
        size_t len = 0;
        if ((c & 0xE0) == 0xC0)      { cp = c & 0x1Fu; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; len = 4; }
        if (len == 0 || i + len > s.size()) {
            cp = 0xFFFD;
            len = 1;
        } else {
            bool ok = true;
            for (size_t k = 1; k < len; ++k) {
                unsigned char cc = static_cast<unsigned char>(s[i + k]);
                if ((cc & 0xC0) != 0x80) { ok = false; break; }
                cp = (cp << 6) | (cc & 0x3Fu);
            }
            if (!ok) { cp = 0xFFFD; len = 1; }
        }
        char buf[16];
        if (cp > 0xFFFF) {
            std::uint32_t v = cp - 0x10000u;
            std::snprintf(buf, sizeof(buf), "\\u%04x\\u%04x",
                          0xD800u + (v >> 10), 0xDC00u + (v & 0x3FFu));
        } else {
            std::snprintf(buf, sizeof(buf), "\\u%04x", cp);
        }
        out += buf;
        i += len;
    }
    out.push_back('"');
    return out;
}

// The per-action detail schema (§5.4.7). An action's allowed field names are
// enumerated here and nowhere else; anything not on its list is refused at
// validate_record rather than silently stored. Every field below is a scalar
// describing the SHAPE of the change — a permission mask, an effect, a
// keep-count — and none of them can hold a filename, a metadata value or any
// other payload.
struct ActionSchema {
    const char*            action;
    AccountabilityCategory category;
    std::vector<const char*> fields;
};

const std::vector<ActionSchema>& action_schemas() {
    static const std::vector<ActionSchema> kSchemas = {
        {accountability_action::kAclGrant,  AccountabilityCategory::Authorization,
         {"principal_type", "effect", "mask", "mask_before", "mask_after"}},
        {accountability_action::kAclRevoke, AccountabilityCategory::Authorization,
         {"principal_type", "effect", "mask", "mask_before", "mask_after", "row_removed"}},
        {accountability_action::kRoleCreate, AccountabilityCategory::Identity,
         {"role"}},
        {accountability_action::kRoleDelete, AccountabilityCategory::Identity,
         {"role", "memberships_removed"}},
        {accountability_action::kRoleAssign, AccountabilityCategory::Identity,
         {"role"}},
        {accountability_action::kRoleRemove, AccountabilityCategory::Identity,
         {"role"}},
        {accountability_action::kCullVersions, AccountabilityCategory::Destruction,
         {"keep_count", "versions_removed", "cut_ts"}},
        {accountability_action::kTenantCreate, AccountabilityCategory::Lifecycle,
         {"schema"}},
        {accountability_action::kTenantDelete, AccountabilityCategory::Destruction,
         {"schema"}},

        // Service credentials. Note what is NOT enumerated: there is no field
        // for a secret, a hash or the pepper, so the sharpest possible version
        // of the §5.4.7 leak — a permanently-retained log holding every service
        // secret ever issued — is not representable.
        {accountability_action::kServiceTokenIssued, AccountabilityCategory::Identity,
         {"service_id", "pepper_version", "capabilities"}},
        {accountability_action::kServiceTokenRotated, AccountabilityCategory::Identity,
         {"service_id", "pepper_version", "capabilities"}},
        {accountability_action::kServiceTokenPruned, AccountabilityCategory::Identity,
         {"service_id", "removed"}},
        {accountability_action::kServiceTokenRevoked, AccountabilityCategory::Identity,
         {"service_id", "removed"}},
        {accountability_action::kServiceBootstrapEnrolled, AccountabilityCategory::Identity,
         {"service_id", "pepper_version", "peer_uid"}},
        {accountability_action::kServiceBootstrapReopened, AccountabilityCategory::Identity,
         {"service_id"}},
        {accountability_action::kServiceCapGranted, AccountabilityCategory::Authorization,
         {"service_id", "capability", "high_risk"}},
        {accountability_action::kServiceCapRevoked, AccountabilityCategory::Authorization,
         {"service_id", "capability"}},
    };
    return kSchemas;
}

const ActionSchema* find_schema(const std::string& action) {
    for (const auto& s : action_schemas()) {
        if (action == s.action) return &s;
    }
    return nullptr;
}

}  // namespace

// ── AccountabilityDetail ────────────────────────────────────────────────────

void AccountabilityDetail::set(const std::string& key, const std::string& value) {
    fields_[key] = json_quote(value);
}

void AccountabilityDetail::set(const std::string& key, std::int64_t value) {
    fields_[key] = std::to_string(value);
}

void AccountabilityDetail::set(const std::string& key, bool value) {
    fields_[key] = value ? "true" : "false";
}

std::string AccountabilityDetail::canonical_json() const {
    // std::map iterates in byte order, which is the same order Python's
    // sort_keys=True produces for the ASCII field names the schemas allow.
    std::string out = "{";
    bool first = true;
    for (const auto& [key, token] : fields_) {
        if (!first) out += ",";
        first = false;
        out += json_quote(key);
        out += ":";
        out += token;
    }
    out += "}";
    return out;
}

// ── Validation ──────────────────────────────────────────────────────────────

Result<void> validate_record(const AccountabilityRecord& rec) {
    // §5.1: an operation that cannot name its actor is a bug. Recording "" would
    // be worse than recording nothing, because it looks like coverage.
    if (!rec.ctx.valid()) {
        return Result<void>::err("accountability record has no actor");
    }
    const ActionSchema* schema = find_schema(rec.action);
    if (schema == nullptr) {
        return Result<void>::err("accountability action is not in scope: " + rec.action);
    }
    if (schema->category != rec.category) {
        return Result<void>::err("accountability category does not match action " + rec.action);
    }
    for (const auto& [key, _] : rec.detail.raw()) {
        if (std::find_if(schema->fields.begin(), schema->fields.end(),
                         [&](const char* f) { return key == f; }) == schema->fields.end()) {
            // The §5.4.7 rule made mechanical: if there is no field to put
            // content in, content cannot arrive by accident.
            return Result<void>::err("detail field '" + key + "' is not enumerated for action " +
                                     rec.action);
        }
    }
    return Result<void>::ok();
}

// ── Canonical form + chain ──────────────────────────────────────────────────

std::string canonical_record(std::int64_t seq,
                             std::int64_t ts_micros,
                             const AccountabilityRecord& rec) {
    std::string roles_csv;
    for (size_t i = 0; i < rec.ctx.actor_roles.size(); ++i) {
        if (i) roles_csv += ",";
        roles_csv += rec.ctx.actor_roles[i];
    }

    auto opt = [](const std::string& s) {
        return s.empty() ? std::string("null") : json_quote(s);
    };

    std::ostringstream out;
    out << "[" << seq
        << "," << ts_micros
        << "," << json_quote(rec.ctx.actor)
        << "," << (rec.ctx.actor_roles.empty() ? std::string("null") : json_quote(roles_csv))
        << "," << opt(rec.ctx.source_iface)
        << "," << opt(rec.ctx.source_addr)
        << "," << json_quote(to_string(rec.category))
        << "," << json_quote(rec.action)
        << "," << opt(rec.target_uid)
        << "," << opt(rec.target_type)
        << "," << opt(rec.principal)
        << "," << (rec.detail.empty() ? std::string("null") : json_quote(rec.detail.canonical_json()))
        << "," << opt(rec.global_tenant)
        << "]";
    return out.str();
}

std::vector<std::uint8_t> chain_hash(const std::vector<std::uint8_t>& prev_hash,
                                     const std::string& canonical) {
    // EVP rather than the SHA256_* one-shot API: the latter is deprecated from
    // OpenSSL 3.0 and the build treats new warnings as noise worth avoiding.
    std::vector<std::uint8_t> out(EVP_MAX_MD_SIZE);
    unsigned int out_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) return {};
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        (!prev_hash.empty() &&
         EVP_DigestUpdate(ctx, prev_hash.data(), prev_hash.size()) != 1) ||
        EVP_DigestUpdate(ctx, canonical.data(), canonical.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, out.data(), &out_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }
    EVP_MD_CTX_free(ctx);
    out.resize(out_len);
    return out;
}

Result<AccountabilityDetail> detail_from_json(const std::string& text) {
    AccountabilityDetail detail;
    if (text.empty()) return Result<AccountabilityDetail>::ok(detail);
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        return Result<AccountabilityDetail>::err(std::string("detail is not JSON: ") + e.what());
    }
    if (!parsed.is_object()) {
        return Result<AccountabilityDetail>::err("detail is not a JSON object");
    }
    // Rebuilding through set() is what makes the read path byte-identical to the
    // write path: Postgres stores JSONB in its own normalized form (its key
    // order and spacing are not ours), so re-canonicalizing here is the only way
    // a consumer's re-hash can match. Safe because every schema field is a
    // scalar — a nested value could not survive the round trip, so it is
    // refused rather than quietly flattened.
    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
        const auto& v = it.value();
        if (v.is_string())            detail.set(it.key(), v.get<std::string>());
        else if (v.is_boolean())      detail.set(it.key(), v.get<bool>());
        else if (v.is_number_integer()) detail.set(it.key(), v.get<std::int64_t>());
        else {
            return Result<AccountabilityDetail>::err("detail field '" + it.key() +
                                                     "' is not a scalar");
        }
    }
    return Result<AccountabilityDetail>::ok(detail);
}

std::string bytea_hex(const std::vector<std::uint8_t>& bytes) {
    static const char* kHex = "0123456789abcdef";
    std::string out = "\\x";
    out.reserve(2 + bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

std::vector<std::uint8_t> parse_bytea_hex(const std::string& text) {
    std::vector<std::uint8_t> out;
    size_t i = (text.rfind("\\x", 0) == 0) ? 2 : 0;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (; i + 1 < text.size(); i += 2) {
        int hi = nibble(text[i]);
        int lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) break;
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

}  // namespace fileengine
