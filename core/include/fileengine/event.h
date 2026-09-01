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

#include <cstdint>
#include <string>

namespace fileengine {

// Generic file-activity event types. Mirrors the shared contract documented in
// convert_search_ai/design_documents/EVENT_CONTRACT.md §3. The string forms
// (file.created, dir.deleted, …) are what consumers match on.
enum class FileEventType {
    DirCreated,
    DirDeleted,
    FileCreated,
    FileUpdated,
    FileMoved,
    FileRenamed,
    FileDeleted,
    FileRestored,
    // Erasure (§5.4.5). DELIBERATELY distinct from FileDeleted: a consumer
    // treats file.deleted as a SOFT delete — recoverable, since undelete
    // exists — so it may reasonably keep an index entry marked deleted rather
    // than destroying the vectors. Reusing that type for erasure would leave the
    // extracted text and embeddings exactly where they were, which is the
    // failure the whole feature exists to prevent. Different semantics, so a
    // different type.
    //
    // The event TRIGGERS; it does not guarantee. fileengine:events is fail-open
    // and drop-oldest by design, which is fine for a notification and
    // unacceptable for a contractual obligation — so consumers also poll
    // ListPendingErasures, and that pull is what the attestation counts.
    FileErased,
    AclChanged,         // permission grant/revoke on a resource
    RoleAssigned,       // a user was added to a role
    RoleMemberRemoved,  // a user was removed from a role
    RoleDeleted,        // a role was deleted (all members lose its grants)

    // A freshness HINT, never a data source (PROPOSAL_accountability_record.md
    // §4.3). It says only "at least seq N now exists for this tenant"; the
    // consumer reacts by reading the core's accountability table immediately,
    // out of schedule, rather than acting on the payload.
    //
    // This stream's own properties are exactly right for that job and exactly
    // wrong for anything more: fail-open, trimmed, drop-oldest. A lost hint
    // costs latency only, because the scheduled poll collects the record
    // regardless. Nothing is EVER only on the queue.
    //
    // The seq also gives the consumer a staleness check it otherwise could not
    // have: if the table read does not show N, it is reading state that has not
    // caught up — a replica behind the primary, say — and must retry rather than
    // advance its cursor. Without the assertion that condition is invisible,
    // because it looks identical to "no new records".
    AccountabilityCommitted,
};

// Contract string for an event type (e.g. "file.updated").
const char* to_string(FileEventType type);

// One file-activity event. Envelope matches EVENT_CONTRACT.md §2. Carries
// metadata only — never file content.
struct FileEvent {
    std::string   event_id;          // unique per emission; primary dedupe key
    FileEventType type = FileEventType::FileUpdated;
    std::string   tenant;
    std::string   file_uid;
    std::string   parent_uid;
    std::string   name;
    std::string   path;              // best-effort, advisory only
    bool          is_folder = false;
    bool          is_rendition = false;  // hidden-child rendition (parent is a file)
    int64_t       size = 0;
    std::string   version;           // source version after the change
    std::string   actor;             // acting user (from AuthenticationContext)
    std::string   ts;                // emit time
    int           schema = 1;        // contract schema version

    // ACL events only (type == AclChanged): the principal whose permissions on
    // file_uid changed, and the permission bits granted/revoked. Consumers use
    // these to invalidate any cached permission decision for the resource.
    std::string   principal;
    int           permissions = 0;

    // Role events only (RoleAssigned/RoleMemberRemoved/RoleDeleted): the role and
    // the affected member (empty for RoleDeleted, which affects all members).
    // Effective access changes without touching a resource ACL — consumers must
    // invalidate cached decisions for the member (or all members of the role).
    std::string   role;
    std::string   member;

    // AccountabilityCommitted only: the committed chain position being asserted.
    int64_t       accountability_seq = 0;
};

// Serialize to the contract JSON envelope.
std::string to_json(const FileEvent& event);

} // namespace fileengine
