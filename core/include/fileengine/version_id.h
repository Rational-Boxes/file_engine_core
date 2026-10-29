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

// A NEW VERSION'S ID MUST BE UNIQUE PER FILE, OR TWO WRITES BECOME ONE BLOB.
//
// A version id is the write's start time to the millisecond
// (Utils::get_timestamp_string), and the blob lives at a path made from
// {file_uid, version id}. Two writes to one file that started in the same
// millisecond therefore opened the SAME path, each truncating it and streaming
// into it, while the version insert's ON CONFLICT DO UPDATE let both report
// success. The result was one version whose bytes interleave two uploads —
// under v2, blocks bound to the same AAD from two different plaintexts, so it
// reads back as corrupt — and two callers told their save had landed.
//
// The id is now CLAIMED before any byte is written: the blob path is created
// with O_CREAT|O_EXCL, which exactly one writer can win. A loser takes the
// next id:
//
//     20261029_120000.123      the first write in that millisecond
//     20261029_120000.123-1    a second one
//     ...
//     20261029_120000.123-9    the tenth
//
// and past -9 waits for the next millisecond and starts again from its bare
// timestamp. The suffix stops at one digit on purpose: version ids are
// compared as TEXT everywhere ("newest" is the lexical maximum, and range
// queries compare against a bare timestamp), and "-10" would sort before "-9".
// With one digit, every id still sorts in the order it was claimed, and a
// suffixed id still sorts after its bare timestamp and before the next
// millisecond. Parsers that read the leading "YYYYMMDD_HHMMSS" are unaffected.
//
// The database is the second guard: insert_version no longer overwrites an
// existing row, so a clash the filesystem could not see (a blob already culled
// from the local cache) fails the write instead of re-pointing a version.

#include "types.h"
#include <functional>
#include <string>

namespace fileengine {

class IStorage;

namespace version_id {

// The largest clash suffix. Ten writes per file per millisecond.
constexpr int kMaxSuffix = 9;

// "<base>" for n == 0, else "<base>-<n>".
std::string with_suffix(const std::string& base, int n);

struct Reservation {
    std::string version;   // the claimed id
    std::string path;      // its blob path, created empty and owned by the caller
};

// Claim a new version id for `file_uid`: create its blob path exclusively and
// return both. The caller writes the blob there (truncating the empty file is
// fine — it is theirs) and removes it if the write fails. `now` supplies the
// millisecond timestamp and is replaceable for tests.
Result<Reservation> reserve(IStorage& storage, const std::string& file_uid, const std::string& tenant,
                            const std::function<std::string()>& now = nullptr);

} // namespace version_id
} // namespace fileengine
