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

// The operator credential the CLI presents, and nothing else.
//
// Split out of service_admin.cpp so the STATIC cli can have it. That binary
// makes gRPC calls and must authenticate like any other caller, but it is
// built without libfileengine_core — so it cannot carry the credential
// ADMINISTRATION commands, which need ConfigLoader, ServerLogger and Database.
// Presenting a token and issuing one are different jobs with different
// dependencies, and they belong in different translation units.

#include "service_admin.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>

#include <grpcpp/client_context.h>

namespace cli {
namespace {

// Loaded once. Returns empty when there is none, which keeps the CLI usable
// against a core that does not require service auth.
const std::string& cli_token() {
    static const std::string token = [] {
        if (const char* env = std::getenv("FILEENGINE_CLI_TOKEN")) {
            if (*env) return std::string(env);
        }
        const char* home = std::getenv("HOME");
        if (home == nullptr || *home == 0) return std::string();
        const std::string path = std::string(home) + "/.config/fileengine/credentials";

        // The permissions are LOAD-BEARING, not hygiene: this file holds the
        // plaintext for an identity that carries every capability. Refuse
        // rather than fix — silently tightening someone's file would hide that
        // it had been readable, and by whom, for however long.
        struct stat st {};
        if (::stat(path.c_str(), &st) != 0) return std::string();
        if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
            std::cerr << "Refusing to read " << path << ": mode is "
                      << std::oct << (st.st_mode & 07777) << std::dec
                      << ", which is readable beyond your account.\n"
                      << "This file holds a credential with every capability. Run:\n"
                      << "  chmod 600 " << path << "\n";
            return std::string();
        }
        std::ifstream in(path);
        std::string line;
        std::getline(in, line);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        return line;
    }();
    return token;
}

}  // namespace

void attach_cli_token(grpc::ClientContext& context) {
    if (!cli_token().empty()) {
        context.AddMetadata("x-fe-service-token", cli_token());
    }
}

}  // namespace cli
