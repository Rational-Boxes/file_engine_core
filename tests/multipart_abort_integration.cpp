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
//
// Does the multipart abort guard actually fire, against a real object store?
//
// The unit tests cover the counters. They cannot cover the guard: AbortGuard is
// a local struct inside the multipart function and S3Storage takes no injectable
// client, so the only way to reach it is to make a real multipart upload fail.
//
// WHY THIS TEST LEAVES NOTHING BEHIND. It drives the abort path exclusively, and
// an aborted multipart upload produces no object and no surviving parts — that
// is the whole point of aborting. S3 objects here are immutable and S3Storage
// has no delete, so a test that uploaded successfully would leave a permanent
// object in somebody's bucket. This one does not, which is why the happy path is
// opt-in below rather than default.
//
//   FILEENGINE_S3_ENDPOINT / _BUCKET / _REGION / _ACCESS_KEY / _SECRET_KEY
//     Required. Without them the test skips rather than fails, so it stays safe
//     in a checkout with no infrastructure.

#include "fileengine/s3_storage.h"
#include "fileengine/transfer_tracker.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

using fileengine::S3Storage;
using fileengine::TransferTracker;

namespace {

std::string env_or(const char* name, const std::string& fallback = "") {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

// Must exceed S3Storage's PART_SIZE (8 MiB) or the upload takes the single
// PutObject path and never creates a multipart upload to abort.
constexpr std::uintmax_t kOverPartSize = 9u * 1024 * 1024;

std::filesystem::path make_big_file() {
    auto p = std::filesystem::temp_directory_path() /
             ("fe-multipart-itest-" + std::to_string(::getpid()) + ".bin");
    std::ofstream out(p, std::ios::binary);
    std::vector<char> block(1024 * 1024, 'x');
    for (int i = 0; i < 9; ++i) out.write(block.data(), block.size());
    out.close();
    return p;
}

}  // namespace

int main() {
    const std::string endpoint = env_or("FILEENGINE_S3_ENDPOINT");
    const std::string bucket   = env_or("FILEENGINE_S3_BUCKET");
    const std::string access   = env_or("FILEENGINE_S3_ACCESS_KEY");
    const std::string secret   = env_or("FILEENGINE_S3_SECRET_KEY");
    const std::string region   = env_or("FILEENGINE_S3_REGION", "us-east-1");

    if (endpoint.empty() || bucket.empty() || access.empty() || secret.empty()) {
        std::cout << "SKIP: no object store configured "
                     "(FILEENGINE_S3_ENDPOINT/_BUCKET/_ACCESS_KEY/_SECRET_KEY)\n";
        return 0;
    }

    // chmod 000 does not stop root, and this test's whole mechanism is an
    // unreadable file. Better to skip than to pass for the wrong reason.
    if (::geteuid() == 0) {
        std::cout << "SKIP: running as root — an unreadable file is still readable\n";
        return 0;
    }

    std::cout << "Multipart abort integration test (endpoint " << endpoint << ")\n";

    S3Storage s3(endpoint, region, bucket, access, secret, /*path_style=*/true);
    auto init = s3.initialize();
    if (!init.success) {
        std::cout << "SKIP: object store unreachable: " << init.error << "\n";
        return 0;
    }

    const auto file = make_big_file();
    assert(std::filesystem::file_size(file) > kOverPartSize - 1);

    // Readable by nobody. file_size() still works (stat needs no read
    // permission), so the code stats it, creates the multipart upload, and only
    // then fails to open it — which is precisely the window the guard covers.
    std::filesystem::permissions(file, std::filesystem::perms::none);

    auto& tracker = TransferTracker::instance();
    tracker.reset_for_test();

    // A tenant nobody else uses, so nothing here can collide with real data.
    const std::string tenant = "itest-multipart-" + std::to_string(::getpid());
    auto result = s3.store_file_from_path("abort-probe", "v1", file.string(), tenant);

    const auto stats = tracker.stats();
    std::filesystem::permissions(file, std::filesystem::perms::owner_all);
    std::filesystem::remove(file);

    // 1. The upload failed, and said so rather than reporting a phantom success.
    assert(!result.success);
    std::cout << "  upload refused: " << result.error << "\n";

    // 2. The guard fired, and attributed it to the right stage. "open" means the
    //    fault was local — no byte ever crossed the wire — which is exactly what
    //    an unreadable file should look like on a graph.
    assert(stats.aborted_open == 1);
    assert(stats.aborted_part == 0);
    assert(stats.aborted_complete == 0);
    std::cout << "  counted as an open-stage abort\n";

    // 3. Nothing was transferred, so nothing was wasted. A non-zero value here
    //    would mean the stage attribution and the byte count disagree.
    assert(stats.aborted_bytes == 0);

    // 4. Nothing committed.
    assert(stats.completed == 0);

    // 5. The abort itself was accepted by the store. This is the assertion that
    //    matters most: it means no parts were stranded, which is the residue the
    //    bucket lifecycle rule would otherwise have to reap. If this fires, the
    //    guard ran but the cleanup did not land.
    assert(stats.abort_failed == 0);
    std::cout << "  abort accepted by the store — no parts stranded\n";

    std::cout << "All multipart abort integration assertions passed\n";
    return 0;
}
