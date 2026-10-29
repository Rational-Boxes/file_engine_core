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

#include "fileengine/version_id.h"
#include "fileengine/IStorage.h"
#include "fileengine/utils.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <thread>
#include <unistd.h>

namespace fileengine {
namespace version_id {

std::string with_suffix(const std::string& base, int n) {
    return n == 0 ? base : base + "-" + std::to_string(n);
}

Result<Reservation> reserve(IStorage& storage, const std::string& file_uid, const std::string& tenant,
                            const std::function<std::string()>& now) {
    auto clock = now ? now : [] { return Utils::get_timestamp_string(); };
    std::string exhausted;   // a millisecond whose ten ids are all taken
    // Bounded in time, so a clock that never advances (or a directory that
    // always says EEXIST) fails the write instead of spinning forever.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        const std::string base = clock();
        if (base == exhausted) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }
        for (int n = 0; n <= kMaxSuffix; ++n) {
            const std::string version = with_suffix(base, n);
            const std::string path = storage.get_storage_path(file_uid, version, tenant);
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
            if (ec) return Result<Reservation>::err("Failed to create storage directory: " + ec.message());
            const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (fd >= 0) {
                ::close(fd);
                return Result<Reservation>::ok(Reservation{version, path});
            }
            if (errno != EEXIST) {
                return Result<Reservation>::err("Failed to reserve version " + version + ": " +
                                                std::strerror(errno));
            }
        }
        exhausted = base;
    }
    return Result<Reservation>::err("Could not reserve a version id for " + file_uid);
}

} // namespace version_id
} // namespace fileengine
