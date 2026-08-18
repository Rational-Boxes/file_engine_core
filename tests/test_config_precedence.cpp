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

// Configuration precedence: environment > .env > .conf > built-in default.
//
// The merge used to decide "did the operator set this?" by comparing the parsed
// value against the built-in default, which quietly broke two whole classes of
// setting and gave no sign that it had:
//
//   * A value that HAPPENS TO EQUAL the default could not override a file.
//     FILEENGINE_HTTP_THREAD_POOL=10 was indistinguishable from unset, so a .env
//     saying 4 won and the environment was ignored.
//   * Booleans became one-way switches. Whichever way the default pointed, the
//     other value looked like "not set" — so encryption could be turned on but
//     never off, and sync off but never on.
//
// Both are the sort of thing that surfaces as "I set the variable and nothing
// happened", which is why they are pinned here.
//
// These run against a temporary working directory holding a crafted .env, so the
// repository's own .env is never read or written.

#include "fileengine/config_loader.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << "\n";
    if (!ok) ++failures;
}

/// Run the loader with `cwd` as the working directory, so it reads that .env.
fileengine::Config load_in(const fs::path& dir) {
    const fs::path previous = fs::current_path();
    fs::current_path(dir);
    char arg0[] = "test_config_precedence";
    char* argv[] = {arg0, nullptr};
    fileengine::Config cfg = fileengine::ConfigLoader::load_config(1, argv);
    fs::current_path(previous);
    return cfg;
}

void write_env(const fs::path& dir, const std::string& body) {
    std::ofstream out(dir / ".env");
    out << body;
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "fe_config_precedence_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::cout << "configuration precedence\n";

    // --- .env beats the built-in default ---------------------------------
    write_env(dir, "FILEENGINE_HTTP_THREAD_POOL=4\n");
    unsetenv("FILEENGINE_HTTP_THREAD_POOL");
    check(load_in(dir).thread_pool_size == 4, ".env overrides the built-in default");

    // --- environment beats .env ------------------------------------------
    setenv("FILEENGINE_HTTP_THREAD_POOL", "7", 1);
    check(load_in(dir).thread_pool_size == 7, "the environment overrides .env");

    // --- THE REGRESSION --------------------------------------------------
    // 10 is the built-in default for this setting. Under the old merge that made
    // it indistinguishable from unset, so the .env value (4) won and the operator's
    // explicit 10 was silently discarded.
    setenv("FILEENGINE_HTTP_THREAD_POOL", "10", 1);
    check(load_in(dir).thread_pool_size == 10,
          "an environment value EQUAL to the default still overrides .env");
    unsetenv("FILEENGINE_HTTP_THREAD_POOL");

    // --- booleans must work in BOTH directions ---------------------------
    // encrypt_data defaults false: the old merge could turn it on and never off.
    write_env(dir, "FILEENGINE_ENCRYPT_DATA=true\n");
    check(load_in(dir).encrypt_data, ".env can turn a false-by-default flag on");
    setenv("FILEENGINE_ENCRYPT_DATA", "false", 1);
    check(!load_in(dir).encrypt_data,
          "the environment can turn a flag back OFF, not just on");
    unsetenv("FILEENGINE_ENCRYPT_DATA");

    // sync_enabled defaults true: the mirror image of the case above.
    write_env(dir, "FILEENGINE_S3_SYNC_SUPPORT=false\n");
    check(!load_in(dir).sync_enabled, ".env can turn a true-by-default flag off");
    setenv("FILEENGINE_S3_SYNC_SUPPORT", "true", 1);
    check(load_in(dir).sync_enabled,
          "the environment can turn a flag back ON, not just off");
    unsetenv("FILEENGINE_S3_SYNC_SUPPORT");

    // --- a string whose value equals its default -------------------------
    write_env(dir, "FILEENGINE_S3_REGION=eu-west-2\n");
    setenv("FILEENGINE_S3_REGION", "us-east-1", 1);   // the built-in default
    check(load_in(dir).s3_region == "us-east-1",
          "a string set to its default value still overrides .env");
    unsetenv("FILEENGINE_S3_REGION");

    // --- unset means unset -----------------------------------------------
    write_env(dir, "");
    check(load_in(dir).thread_pool_size == 10,
          "with nothing set anywhere, the built-in default stands");

    fs::remove_all(dir);
    std::cout << (failures == 0 ? "PASS" : "FAIL") << " (" << failures << " failure(s))\n";
    return failures == 0 ? 0 : 1;
}
