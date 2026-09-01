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

#include "fileengine/service_credential.h"

#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <map>
#include <cstring>

namespace fileengine {

// ── Token format ────────────────────────────────────────────────────────────

ParsedToken parse_service_token(const std::string& token) {
    ParsedToken out;
    const std::string prefix = kServiceTokenPrefix;
    if (token.rfind(prefix, 0) != 0) return out;          // wrong or absent prefix

    // Split on the FIRST '.' after the prefix. Service ids contain underscores
    // but never dots, so this is unambiguous where an underscore separator
    // would not be.
    const std::size_t dot = token.find('.', prefix.size());
    if (dot == std::string::npos) return out;

    out.service_id = token.substr(prefix.size(), dot - prefix.size());
    out.secret     = token.substr(dot + 1);
    // Both halves must be non-empty. A token like "fesvc_." parses structurally
    // but names nobody, and letting it through would mean an empty service_id
    // reaching the map lookup.
    out.valid = !out.service_id.empty() && !out.secret.empty();
    return out;
}

std::string generate_service_secret() {
    // 256 bits, URL-safe base64 without padding — matching token_urlsafe(32) as
    // already used for the user service credentials.
    unsigned char raw[32];
    if (RAND_bytes(raw, sizeof(raw)) != 1) return {};

    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(43);
    int bits = 0;
    std::uint32_t acc = 0;
    for (unsigned char b : raw) {
        acc = (acc << 8) | b;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(kAlphabet[(acc >> bits) & 0x3F]);
        }
    }
    if (bits > 0) out.push_back(kAlphabet[(acc << (6 - bits)) & 0x3F]);
    return out;
}

std::string format_service_token(const std::string& service_id, const std::string& secret) {
    return std::string(kServiceTokenPrefix) + service_id + "." + secret;
}

// ── Hashing ─────────────────────────────────────────────────────────────────

std::vector<std::uint8_t> hash_service_secret(const std::string& pepper,
                                              const std::string& secret) {
    // A fast MAC rather than a slow KDF, and the reason matters: password
    // hashing needs bcrypt/argon2 because human-chosen passwords have little
    // entropy. This secret is 256 bits of machine randomness, so there is no
    // dictionary to run and no candidate space to search — slowing the hash
    // buys nothing. The pepper defends the narrower case of an attacker with
    // both the dump and candidate secrets.
    //
    // Refuse rather than degrade when the pepper is missing. Hashing under an
    // empty pepper would store values that verify perfectly and are worthless
    // the moment the database leaks — a failure with no visible symptom.
    if (pepper.empty()) return {};

    unsigned int len = 0;
    std::vector<std::uint8_t> out(EVP_MAX_MD_SIZE);
    if (HMAC(EVP_sha256(),
             pepper.data(), static_cast<int>(pepper.size()),
             reinterpret_cast<const unsigned char*>(secret.data()), secret.size(),
             out.data(), &len) == nullptr) {
        return {};
    }
    out.resize(len);
    return out;
}

bool constant_time_equals(const std::vector<std::uint8_t>& a,
                          const std::vector<std::uint8_t>& b) {
    // Compare the length first — it is not secret, and CRYPTO_memcmp requires
    // equal lengths — then every byte without short-circuiting.
    if (a.size() != b.size() || a.empty()) return false;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

void dummy_verify(const std::string& pepper) {
    // Same work as a real verification, discarded. Without this an unknown
    // service id returns before any HMAC runs while a known one pays for a full
    // hash and compare, and the difference enumerates valid service names.
    static const std::string kDecoy =
        "0000000000000000000000000000000000000000000";
    const auto hashed = hash_service_secret(pepper.empty() ? kDecoy : pepper, kDecoy);
    // Compare against itself so the work cannot be optimised away, and so the
    // result is never usable as a signal.
    volatile bool sink = constant_time_equals(hashed, hashed);
    (void)sink;
}

// ── Capabilities ────────────────────────────────────────────────────────────

const char* to_string(Capability c) {
    switch (c) {
        case Capability::Read:           return "read";
        case Capability::Write:          return "write";
        case Capability::Delete:         return "delete";
        case Capability::Restore:        return "restore";
        case Capability::Acl:            return "acl";
        case Capability::Roles:          return "roles";
        case Capability::Admin:          return "admin";
        case Capability::Destroy:        return "destroy";
        case Capability::Accountability: return "accountability";
        case Capability::Erasure:        return "erasure";
    }
    return "unknown";
}

bool parse_capability(const std::string& name, Capability& out) {
    for (Capability c : all_capabilities()) {
        if (name == to_string(c)) { out = c; return true; }
    }
    return false;
}

const std::vector<Capability>& all_capabilities() {
    static const std::vector<Capability> kAll = {
        Capability::Read, Capability::Write, Capability::Delete, Capability::Restore,
        Capability::Acl, Capability::Roles, Capability::Admin, Capability::Destroy,
        Capability::Accountability, Capability::Erasure,
    };
    return kAll;
}

bool is_high_risk(Capability c) {
    // `destroy` irreversibly removes committed data; `accountability` reads the
    // security log, which across tenants reconstructs who did what to whom
    // platform-wide. Neither may ride along with a credential issue.
    // `erasure` joins them: acknowledging an erasure you did not perform closes
    // a contractual obligation that was never met, and does it in the very
    // record an auditor is shown. A false compliance claim is not a lesser
    // failure than destroying data — it is the one this feature exists to make
    // impossible to make by accident.
    return c == Capability::Destroy || c == Capability::Accountability ||
           c == Capability::Erasure;
}

// ── The RPC → capability map ────────────────────────────────────────────────

namespace {

// Bare method names; the interceptor sees the full "/package.Service/Method"
// form and takes the segment after the last '/'.
//
// EVERY RPC in fileservice.proto appears exactly once. A method missing from
// here is callable by nobody, including `cli` — that is the intended failure
// mode for a newly added and unclassified RPC, and there is a test that
// enumerates the service descriptor and fails if one appears.
const std::map<std::string, Capability>& method_map() {
    static const std::map<std::string, Capability> kMap = {
        // read — everything that observes without changing anything.
        {"Stat",                      Capability::Read},
        {"Exists",                    Capability::Read},
        {"ListDirectory",             Capability::Read},
        {"GetFile",                   Capability::Read},
        {"StreamFileDownload",        Capability::Read},
        {"GetVersion",                Capability::Read},
        {"ListVersions",              Capability::Read},
        {"GetMetadata",               Capability::Read},
        {"GetAllMetadata",            Capability::Read},
        {"GetMetadataForVersion",     Capability::Read},
        {"GetAllMetadataForVersion",  Capability::Read},
        {"CheckPermission",           Capability::Read},
        {"GetEffectivePermissions",   Capability::Read},

        // write — creates and revises. Deliberately excludes anything that
        // removes, so a service can be allowed to produce derived output
        // without being able to destroy the input.
        {"Touch",                     Capability::Write},
        {"MakeDirectory",             Capability::Write},
        {"PutFile",                   Capability::Write},
        {"StreamFileUpload",          Capability::Write},
        {"Rename",                    Capability::Write},
        {"Move",                      Capability::Write},
        {"Copy",                      Capability::Write},
        {"SetMetadata",               Capability::Write},
        {"DeleteMetadata",            Capability::Write},

        // delete — the soft, recoverable kind. UndeleteFile and the
        // with-deleted listing sit here because they are the same concern:
        // a service that cannot delete has no reason to enumerate or restore
        // what was deleted.
        {"RemoveFile",                Capability::Delete},
        {"RemoveDirectory",           Capability::Delete},
        {"UndeleteFile",              Capability::Delete},
        {"ListDirectoryWithDeleted",  Capability::Delete},

        // restore — moving a file back to an earlier version rewrites what is
        // current, so it is separable from both write and delete.
        {"RestoreToVersion",          Capability::Restore},

        {"GrantPermission",           Capability::Acl},
        {"RevokePermission",          Capability::Acl},
        {"GetResourceAcls",           Capability::Acl},

        {"CreateRole",                Capability::Roles},
        {"DeleteRole",                Capability::Roles},
        {"AssignUserToRole",          Capability::Roles},
        {"RemoveUserFromRole",        Capability::Roles},
        {"GetRolesForUser",           Capability::Roles},
        {"GetUsersForRole",           Capability::Roles},
        {"GetAllRoles",               Capability::Roles},
        {"ListClaims",                Capability::Roles},

        {"GetStorageUsage",           Capability::Admin},
        {"TriggerSync",               Capability::Admin},

        // destroy — the irreversible kind.
        {"PurgeOldVersions",          Capability::Destroy},
        {"EraseFile",                 Capability::Destroy},

        // erasure — the attestation surface. Separate from destroy on purpose:
        // a consumer must read what it owes and report back without thereby
        // being able to erase anything itself.
        {"ListPendingErasures",       Capability::Erasure},
        {"AcknowledgeErasure",        Capability::Erasure},
        {"GetErasureStatus",          Capability::Erasure},

        // accountability — reading the guaranteed security record.
        {"ListAccountabilityRecords", Capability::Accountability},
    };
    return kMap;
}

std::string bare_method(const std::string& full_method) {
    const std::size_t slash = full_method.rfind('/');
    return slash == std::string::npos ? full_method : full_method.substr(slash + 1);
}

}  // namespace

bool capability_for_method(const std::string& method, Capability& out) {
    const auto& m = method_map();
    const auto it = m.find(bare_method(method));
    if (it == m.end()) return false;      // default deny
    out = it->second;
    return true;
}

const std::vector<std::string>& classified_methods() {
    static const std::vector<std::string> kMethods = [] {
        std::vector<std::string> names;
        names.reserve(method_map().size());
        for (const auto& [name, _] : method_map()) names.push_back(name);
        return names;
    }();
    return kMethods;
}

// ── ServiceIdentity ─────────────────────────────────────────────────────────

bool ServiceIdentity::has(Capability c) const {
    return std::find(capabilities.begin(), capabilities.end(), c) != capabilities.end();
}

bool ServiceIdentity::is_cli() const {
    return service_id.rfind(kCliIdentityPrefix, 0) == 0;
}

}  // namespace fileengine
