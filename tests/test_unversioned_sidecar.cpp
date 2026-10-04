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

// The name half of the non-versioned sidecar rule (MEDIA_SHARE.md §8.4). The
// rule destroys history, so its reach must be exactly the two names
// share_service writes — never something an ordinary file can be called by
// accident or on purpose. (The PLACE half — a hidden child of a file, never a
// folder's child — is covered end to end by share_service's live suite.)
// Hermetic: no database.

#include <cassert>
#include <iostream>
#include <string>

#include "fileengine/filesystem.h"

using fileengine::FileSystem;

int main() {
    int failures = 0;
    auto expect = [&](const std::string& name, bool want) {
        bool got = FileSystem::is_unversioned_sidecar_name(name);
        if (got != want) {
            std::cerr << "FAIL: " << name << " -> " << got << ", wanted " << want << "\n";
            ++failures;
        }
    };
    expect("audience.csv", true);
    expect("audience-3f1c2b7e-9a4d-4c55-8e21-0b6f3a9d7c12.csv", true);
    expect("audience-3F1C2B7E-9A4D-4C55-8E21-0B6F3A9D7C12.csv", true);
    // Anything else keeps its history.
    expect("Audience.csv", false);
    expect("audience.CSV", false);
    expect("audience.csv.bak", false);
    expect("my-audience.csv", false);
    expect("audience-.csv", false);
    expect("audience-notes.csv", false);
    expect("audience-3f1c2b7e-9a4d-4c55-8e21-0b6f3a9d7c1.csv", false);
    expect("audience-3f1c2b7e-9a4d-4c55-8e21-0b6f3a9d7c12.csv.csv", false);
    expect("audience-3f1c2b7e9a4d4c558e210b6f3a9d7c12.csv", false);
    expect("20261004_050000.000-media.webm", false);
    expect("audience\n.csv", false);
    expect("", false);
    if (failures) return 1;
    std::cout << "unversioned sidecar name rule: all cases pass\n";
    return 0;
}
