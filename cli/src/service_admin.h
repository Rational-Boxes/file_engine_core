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

// Service-credential administration (PROPOSAL_service_authentication.md §3.6).
//
// Deliberately a separate translation unit. These commands need the core's
// headers, and including those in fileengine_cli.cpp brings fileengine::FileType
// into scope alongside the proto's identically-named enum — after which every
// unqualified use in that file changes meaning silently. Isolation here is a
// correctness measure, not organisation.
//
// Each takes argv positioned at the command word, and returns a process exit
// code: 0 success, 1 operational failure, 2 usage error.

namespace cli {

int service_token_command(int argc, char** argv);
int service_command(int argc, char** argv);
int bootstrap_command(int argc, char** argv);

}  // namespace cli
