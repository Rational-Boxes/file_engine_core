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

#include "fileengine/thread_stats.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fileengine {

namespace {

// Pull the state character out of /proc/<pid>/task/<tid>/stat.
//
// The field is the third, but the second is the executable name in parentheses
// and MAY CONTAIN SPACES AND PARENTHESES. Splitting on whitespace is therefore
// wrong; proc(5) says to scan from the LAST ')' instead, which is what makes
// this reliable for a process whose name happens to contain either.
bool state_char_from_stat(const std::string& line, char& out) {
    const auto close = line.rfind(')');
    if (close == std::string::npos) return false;
    auto i = close + 1;
    while (i < line.size() && line[i] == ' ') ++i;
    if (i >= line.size()) return false;
    out = line[i];
    return true;
}

} // namespace

ThreadStats read_thread_stats() {
    ThreadStats s;

    std::error_code ec;
    const std::filesystem::path task_dir{"/proc/self/task"};
    if (!std::filesystem::exists(task_dir, ec) || ec) {
        return s; // not Linux, or /proc not mounted: available stays false
    }

    std::filesystem::directory_iterator it{task_dir, ec};
    if (ec) {
        return s;
    }

    for (const auto& entry : it) {
        std::ifstream stat_file(entry.path() / "stat");
        if (!stat_file) {
            // A thread can exit between listing the directory and opening its
            // stat file. That is ordinary, not an error — skip it.
            continue;
        }
        std::string line;
        std::getline(stat_file, line);

        char state = '?';
        if (!state_char_from_stat(line, state)) {
            continue;
        }

        ++s.total;
        switch (state) {
            case 'R': ++s.running; break;
            case 'S': ++s.sleeping; break;
            case 'D': ++s.uninterruptible; break;
            case 'T':
            case 't': ++s.stopped; break;
            case 'Z': ++s.zombie; break;
            default:  ++s.other; break;
        }
    }

    // Only claim the numbers are real if we actually read some.
    s.available = s.total > 0;
    return s;
}

} // namespace fileengine
