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

// Per-service credentials for the gRPC surface
// (design_documents/PROPOSAL_service_authentication.md).
//
// The core could not previously tell WHICH internal service was calling it.
// Every RPC arrived with an end-user identity and nothing about its origin, so
// the platform could record who did something but not through which door — and
// `source_iface`, designed for exactly that, was hardcoded to "grpc" at all four
// emit sites, asserting the one thing already implied by the call arriving.
//
// A caller now presents `fesvc_<service_id>.<secret>` in call metadata. The
// secret both proves the caller is legitimate and names it, so authentication
// and attribution are one operation: the caller never asserts an identity, it
// demonstrates one, and there is no claim to forge.
//
// This header holds the parts that are a property of the API rather than of a
// deployment — the token format, the hashing, and the capability DEFINITIONS.
// The assignments (which service holds which capabilities) live in the database
// beside the secret, so onboarding a service is a row rather than a release
// (§6.4) — which matters because a core restart is a platform outage.

#include "types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fileengine {

// ── Token format (§3.3) ─────────────────────────────────────────────────────
//
//     fesvc_<service_id>.<secret>
//     └──┬─┘ └────┬────┘ └──┬──┘
//        │        │         └─ 256 bits, URL-safe base64
//        │        └─ indexes the map: http_bridge, cmis, audit_service, …
//        └─ scannable prefix
//
// The prefix is not decoration: the platform already uses distinctive prefixes
// (`fesk_`, `fesks_`) so leaked-credential scanners can flag exposure. A token
// pasted into a ticket or echoed into a log becomes greppable.
//
// The separator is `.` and not `_` because service ids contain underscores —
// `http_bridge`, `webdav_bridge`, `folder_actions` — so an underscore separator
// cannot be parsed unambiguously. Dots never appear in service ids.
inline constexpr const char* kServiceTokenPrefix = "fesvc_";

// The metadata key the token rides in (§3.1). A dedicated header rather than
// `authorization: Bearer`, because the bridges already carry end-user bearer
// tokens under that name, and two different credentials sharing one header is
// how the wrong one gets validated — or logged by a redaction rule written for
// the other. gRPC requires metadata keys to be lowercase.
inline constexpr const char* kServiceTokenMetadataKey = "x-fe-service-token";

struct ParsedToken {
    std::string service_id;
    std::string secret;
    bool        valid = false;
};

// Split a presented token. Never logs, and never returns the secret in an error.
ParsedToken parse_service_token(const std::string& token);

// A 256-bit URL-safe secret, and the full token around it.
//
// Tokens are ALWAYS generated, never chosen. That is the guardrail the storage
// design rests on: a fast MAC is sufficient only because the secret is
// full-entropy machine randomness with no dictionary to run against it. The
// moment a path lets an operator type a memorable token the assumption fails —
// quietly, because the stored value looks identical.
std::string generate_service_secret();
std::string format_service_token(const std::string& service_id, const std::string& secret);

// HMAC-SHA256(secret, pepper), following the pattern ldap_manager already uses.
// The pepper defends a bare database read and deliberately lives outside the
// database, so a single dump yields no usable token.
std::vector<std::uint8_t> hash_service_secret(const std::string& pepper,
                                              const std::string& secret);

// Constant-time equality. The lookup is indexed by service_id, but the
// comparison must never short-circuit.
bool constant_time_equals(const std::vector<std::uint8_t>& a,
                          const std::vector<std::uint8_t>& b);

// Burn the same work on a miss as on a hit, so an unknown service id does not
// return faster than a bad secret and thereby enumerate valid service names.
// The value of that enumeration is low — service names are guessable — but the
// mitigation is a few lines, which is the right threshold for this kind of leak.
void dummy_verify(const std::string& pepper);

// ── Capabilities (§6.1) ─────────────────────────────────────────────────────
//
// Enumerating 41 RPCs across ~13 services would be 500-odd cells that drift and
// get got wrong. Each RPC belongs to exactly ONE capability, and services are
// granted capabilities — nine sets instead of five hundred cells.
//
// Wire form is the lowercase string; it is stored in the database and read by
// the CLI, so append, never rename.
enum class Capability {
    Read,            // Stat, Exists, ListDirectory, GetFile, GetVersion, metadata reads, permission checks
    Write,           // Touch, MakeDirectory, PutFile, streaming upload, Rename, Move, Copy, metadata writes
    Delete,          // RemoveFile, RemoveDirectory, UndeleteFile, ListDirectoryWithDeleted
    Restore,         // RestoreToVersion
    Acl,             // GrantPermission, RevokePermission, GetResourceAcls
    Roles,           // role create/delete/assign/remove and the role queries, ListClaims
    Admin,           // GetStorageUsage, TriggerSync
    Destroy,         // PurgeOldVersions, EraseFile — irreversible destruction
    Accountability,  // ListAccountabilityRecords
    // The erasure ATTESTATION surface, deliberately separate from Destroy.
    //
    // A consumer (csai, discussion, difference) has to read the erasures it owes
    // and report back — but giving it Destroy so it can read its own work queue
    // would also let it erase arbitrary files, which is a wildly larger grant
    // than the job needs. Splitting them keeps "must purge my derived copy"
    // apart from "may destroy the original".
    Erasure,         // ListPendingErasures, AcknowledgeErasure, GetErasureStatus
};

const char* to_string(Capability c);
bool parse_capability(const std::string& name, Capability& out);

// Every capability, for the reserved `cli` identity and for CLI validation.
const std::vector<Capability>& all_capabilities();

// Capabilities that cannot be granted in the same operation that issues a
// credential, and that require an explicit confirmation flag (§6.4). Onboarding
// a service and arming it are separate acts.
bool is_high_risk(Capability c);

// ── The RPC → capability map ────────────────────────────────────────────────
//
// `method` is the gRPC full method name, e.g.
// "/fileengine_rpc.FileService/GetFile".
//
// DEFAULT DENY, INCLUDING FOR NEW RPCs. A method belonging to no capability is
// callable by nobody — so adding an RPC and forgetting to classify it fails
// loudly at first use rather than silently defaulting to open, which is the
// opposite of an allowlist that grants by omission.
//
// `cli` holds all nine capabilities but is NOT exempted from the mechanism: an
// unclassified RPC belongs to none of its capabilities either, so it stays
// unreachable by everyone. That distinction is load-bearing — "cli may call any
// method" would re-open exactly the hole default-deny exists to close, at the
// one identity most likely to hold it.
bool capability_for_method(const std::string& method, Capability& out);

// Every classified method, for the test that enumerates the service descriptor
// and fails if anything is unclassified (§9.8).
const std::vector<std::string>& classified_methods();

// ── A resolved caller ───────────────────────────────────────────────────────
struct ServiceIdentity {
    std::string             service_id;     // http_bridge, cmis, cli:alice, …
    std::vector<Capability> capabilities;   // the granted set, from the database
    int                     pepper_version = 0;

    bool has(Capability c) const;

    // `cli:*` is a reserved class holding every capability, and the ONLY
    // identity permitted the full set. It pays for that with a transport
    // constraint instead: it may connect only over loopback, checked against the
    // peer address rather than assumed from the bind — an established
    // connection's peer address is not something a caller can assert, unlike a
    // header. An administrator on the box already has full system access, so
    // restricting what the CLI may ask would withhold nothing and only make the
    // supported tool weaker than the unsupported paths beside it.
    bool is_cli() const;
};

inline constexpr const char* kCliIdentityPrefix = "cli:";

}  // namespace fileengine
