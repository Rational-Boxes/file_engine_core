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

// The guaranteed accountability record
// (design_documents/PROPOSAL_accountability_record.md).
//
// A single append-only table per tenant, written in the SAME TRANSACTION as the
// operation it describes. That is the whole point: the audit sink is
// best-effort, silently absent when unconfigured, node-local and non-
// transactional (§3.2), so there is no moment at which "the operation happened"
// and "a record exists" are atomic. Here there is.
//
// Four properties this header exists to enforce, rather than merely document:
//
//   1. `actor` is never defaulted. An operation that cannot name its actor is a
//      bug and must fail rather than record "" — an unattributed record looks
//      like coverage without being it (§5.1).
//   2. `detail` is a SCHEMA-CONSTRAINED per-action structure, not an open map.
//      The chain is immutable, hash-chained and never culled, so anything it
//      captures the platform has committed to keeping forever. If there is no
//      field to put content in, content cannot arrive by accident (§5.4.7).
//   3. The record captures IDENTIFIERS AND STRUCTURE, NEVER PAYLOAD. No
//      filenames, no metadata values, no free text. Names are resolved from the
//      uid at read time, so an erasure automatically stops the log disclosing
//      them.
//   4. The canonical byte form and the chain hash are defined ONCE, here, and
//      reproduced by the consumer (audit_service/src/audit_service/
//      accountability.py). Both sides must agree byte for byte or every
//      verification fails; keep them in lockstep.

#include "types.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fileengine {

// ── Categories ──────────────────────────────────────────────────────────────
// The record's own taxonomy, deliberately narrower than AuditCategory: this
// table covers only operations that are security-relevant AND need a
// guaranteed, chained, never-culled record (§4.1). Wire form is the lowercase
// string; append, never rename.
enum class AccountabilityCategory {
    Authorization,   // who could do what: ACL grants and revokes
    Identity,        // who is who: role definition and membership
    Destruction,     // culling, and (when it lands) erasure
    Lifecycle        // tenant create/delete — global scope only
};

const char* to_string(AccountabilityCategory c);

// ── Actor context ───────────────────────────────────────────────────────────
// Carried from the boundary that established the identity down to the layer
// that owns the transaction. The bridges are the trust boundary (§7.4): they
// authenticate and present a resolved identity, and the core records exactly
// that — no better and no worse than the platform's identity model.
// Whether an ACL write is an act in its own right or a mechanical consequence
// of creating a resource. Only the former opens a chain entry.
//
// This is the one place the scope rule (§4.1) has to be expressed in code
// rather than in a table of actions, because a single DB call serves both:
// apply_default_acls and inherited-rule propagation run on every resource
// creation. Those are already attributed by the resource's own row, they grant
// nobody anything they were not being given by the act of creating the thing,
// and recording them would put the per-tenant chain lock on the content path —
// which §5.3.2 rules out and §8's acceptance 17 tests for.
//
// It is deliberately a per-call-site value in code, never a configuration knob:
// §4.2 guarantee 3 is that no deployment setting can turn the record off.
enum class AccountabilityMode {
    Record,          // a deliberate act — chain it
    PartOfCreation   // a side effect of creating a resource — the resource's row attributes it
};

struct AccountabilityContext {
    std::string              actor;         // REQUIRED; "system" is explicit, never a default
    std::vector<std::string> actor_roles;   // roles as presented at the time
    std::string              source_iface;  // grpc | rest | webdav | cmis | cli | system
    std::string              source_addr;   // client IP forwarded by the bridge
    AccountabilityMode       mode = AccountabilityMode::Record;

    bool valid() const { return !actor.empty(); }

    static AccountabilityContext system(const std::string& why_iface = "system") {
        AccountabilityContext c;
        c.actor = "system";
        c.source_iface = why_iface;
        return c;
    }
};

// ── detail ──────────────────────────────────────────────────────────────────
// A closed set of typed scalar fields. There is deliberately no way to store a
// string that did not come from an enumerated field name, and no field in any
// schema below holds content, a filename, or a metadata value.
class AccountabilityDetail {
public:
    void set(const std::string& key, const std::string& value);
    void set(const std::string& key, std::int64_t value);
    void set(const std::string& key, int value) { set(key, static_cast<std::int64_t>(value)); }
    void set(const std::string& key, bool value);

    bool empty() const { return fields_.empty(); }

    // Canonical JSON object: keys sorted, compact separators, ASCII-escaped.
    // This exact text is what is hashed and what is stored in the JSONB column.
    std::string canonical_json() const;

    const std::map<std::string, std::string>& raw() const { return fields_; }

private:
    // Values are pre-rendered to their JSON token at set() time (quoted+escaped
    // for strings, bare for numbers/bools), so canonical_json is a pure join and
    // cannot be perturbed by a locale or a floating-point formatter.
    std::map<std::string, std::string> fields_;
};

// ── Actions ─────────────────────────────────────────────────────────────────
// The complete in-scope set (§4.1). An action not listed here cannot be
// recorded — validate_record rejects it — which is what makes the scope a
// property of the code rather than a convention.
namespace accountability_action {
inline constexpr const char* kAclGrant     = "acl.grant";
inline constexpr const char* kAclRevoke    = "acl.revoke";
inline constexpr const char* kRoleCreate   = "role.create";
inline constexpr const char* kRoleDelete   = "role.delete";
inline constexpr const char* kRoleAssign   = "role.assign";
inline constexpr const char* kRoleRemove   = "role.remove";
inline constexpr const char* kCullVersions = "cull.versions";
inline constexpr const char* kTenantCreate = "tenant.create";
inline constexpr const char* kTenantDelete = "tenant.delete";
}  // namespace accountability_action

// ── The record ──────────────────────────────────────────────────────────────
struct AccountabilityRecord {
    AccountabilityContext   ctx;
    AccountabilityCategory  category = AccountabilityCategory::Authorization;
    std::string             action;        // one of accountability_action::*
    std::string             target_uid;    // the uid ONLY — names are resolved at read time
    std::string             target_type;   // file | dir | acl | role | principal | tenant | version
    std::string             principal;     // for authorization changes: whose access changed
    AccountabilityDetail    detail;

    // Only for rows on the GLOBAL chain: the tenant the lifecycle event is
    // about. Tenant-scoped rows leave it empty — their tenant is implicit in the
    // schema they live in, and the schema name is not reversible to the tenant
    // id, so putting it in the hash there would make a tenant's chain
    // unverifiable from its own data.
    std::string             global_tenant;
};

// Rebuild a detail from the JSONB text Postgres hands back, re-canonicalizing
// it so a consumer's re-hash matches the write path exactly. Fails rather than
// flattens if a value is not a scalar — nothing the schemas allow ever is.
Result<AccountabilityDetail> detail_from_json(const std::string& text);

// Rejects a record that could not be honestly written: no actor, an action
// outside the in-scope set, a category that does not match the action, or a
// detail field the action's schema does not enumerate. Returns the reason.
Result<void> validate_record(const AccountabilityRecord& rec);

// ── Canonical form + chain ──────────────────────────────────────────────────
// canonical_record produces the exact bytes hashed for a row. It is a compact
// JSON array (fixed field order, no keys) so that neither side can disagree
// about key ordering, and absent optional fields are JSON null rather than "" so
// "no principal" and "the principal named empty-string" cannot collide.
//
//   [seq, ts_micros, actor, roles_csv|null, source_iface|null, source_addr|null,
//    category, action, target_uid|null, target_type|null, principal|null,
//    detail_canonical|null, tenant|null]
//
// The trailing `tenant` is AccountabilityRecord::global_tenant: null for
// tenant-scoped rows (constant per chain, and the schema name is not reversible
// to the tenant id) and the tenant column for global rows — the same split the
// audit chain already uses.
//
// seq IS included here, unlike the audit chain: under §5.3.2 seq is assigned
// under the same lock that orders the chain, so it is part of what the row
// attests, and a renumbering must break verification.
std::string canonical_record(std::int64_t seq,
                             std::int64_t ts_micros,
                             const AccountabilityRecord& rec);

// hash = SHA-256(prev_hash ‖ canonical). prev_hash is empty for the first row of
// a chain, and contributes zero bytes there.
std::vector<std::uint8_t> chain_hash(const std::vector<std::uint8_t>& prev_hash,
                                     const std::string& canonical);

// Postgres bytea text I/O ("\x0011ff"). Used for both directions so the hex
// case and prefix are decided in one place.
std::string      bytea_hex(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> parse_bytea_hex(const std::string& text);

// The reserved tenant key for the global (cross-tenant) chain. A tenant may not
// use it: `*` is stripped by validate_schema_name, so no real tenant can
// collide with it, and the pull endpoint uses it to select the global table.
inline constexpr const char* kGlobalChainKey = "*global*";

// ── A record as read back ───────────────────────────────────────────────────
// What the pull endpoint returns and what a verifier re-hashes.
struct StoredAccountabilityRecord {
    std::int64_t              seq = 0;
    std::int64_t              ts_micros = 0;
    std::string               ts_iso;        // RFC-3339 UTC, microsecond precision
    AccountabilityRecord      record;
    std::vector<std::uint8_t> prev_hash;
    std::vector<std::uint8_t> hash;
};

}  // namespace fileengine
