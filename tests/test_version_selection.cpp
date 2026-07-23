// Regression coverage for FileSystem::newest_version — the helper that centralizes
// the "current version == front() of a newest-first list" contract. It exists
// because backup_to_object_store previously used .back() and therefore archived
// the OLDEST version instead of the current one. Database::list_versions returns
// timestamps ORDER BY version_timestamp DESC (newest first); these tests pin that
// the helper honors that ordering. Hermetic: no database, no object store.

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "fileengine/filesystem.h"

using namespace fileengine;

int main() {
    std::cout << "Running FileSystem::newest_version selection tests..." << std::endl;

    // list_versions order: newest first.
    std::vector<std::string> desc = {
        "20260103_090000.000",  // newest / current
        "20260102_090000.000",
        "20260101_090000.000",  // oldest
    };
    assert(FileSystem::newest_version(desc) == "20260103_090000.000");
    // The core regression: newest must be the front, NEVER the back (the old bug
    // returned .back(), i.e. the oldest version).
    assert(FileSystem::newest_version(desc) != desc.back());

    // Single version: it is both newest and oldest.
    std::vector<std::string> one = {"20250601_000000.000"};
    assert(FileSystem::newest_version(one) == one.front());

    // No versions: empty sentinel (callers treat this as "no version to act on").
    assert(FileSystem::newest_version({}).empty());

    std::cout << "All newest_version selection tests passed!" << std::endl;
    return 0;
}
