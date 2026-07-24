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

#include "event.h"

#include "json.hpp"

namespace fileengine {

const char* to_string(FileEventType type) {
    switch (type) {
        case FileEventType::DirCreated:   return "dir.created";
        case FileEventType::DirDeleted:   return "dir.deleted";
        case FileEventType::FileCreated:  return "file.created";
        case FileEventType::FileUpdated:  return "file.updated";
        case FileEventType::FileMoved:    return "file.moved";
        case FileEventType::FileRenamed:  return "file.renamed";
        case FileEventType::FileDeleted:  return "file.deleted";
        case FileEventType::FileRestored: return "file.restored";
        case FileEventType::AclChanged:        return "acl.changed";
        case FileEventType::RoleAssigned:      return "role.assigned";
        case FileEventType::RoleMemberRemoved: return "role.member_removed";
        case FileEventType::RoleDeleted:       return "role.deleted";
    }
    return "unknown";
}

std::string to_json(const FileEvent& e) {
    nlohmann::json j;
    j["event_id"]     = e.event_id;
    j["type"]         = to_string(e.type);
    j["tenant"]       = e.tenant;
    j["file_uid"]     = e.file_uid;
    j["parent_uid"]   = e.parent_uid;
    j["name"]         = e.name;
    j["path"]         = e.path;
    j["is_folder"]    = e.is_folder;
    j["is_rendition"] = e.is_rendition;
    j["size"]         = e.size;
    j["version"]      = e.version;
    j["actor"]        = e.actor;
    j["ts"]           = e.ts;
    j["schema"]       = e.schema;
    if (e.type == FileEventType::AclChanged) {
        j["principal"]   = e.principal;
        j["permissions"] = e.permissions;
    }
    if (e.type == FileEventType::RoleAssigned ||
        e.type == FileEventType::RoleMemberRemoved ||
        e.type == FileEventType::RoleDeleted) {
        j["role"]   = e.role;
        j["member"] = e.member;
    }
    return j.dump();
}

} // namespace fileengine
