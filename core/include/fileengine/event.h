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
    AclChanged,         // permission grant/revoke on a resource
    RoleAssigned,       // a user was added to a role
    RoleMemberRemoved,  // a user was removed from a role
    RoleDeleted,        // a role was deleted (all members lose its grants)
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
};

// Serialize to the contract JSON envelope.
std::string to_json(const FileEvent& event);

} // namespace fileengine
