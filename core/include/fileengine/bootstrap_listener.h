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

// Bootstrap enrolment (PROPOSAL_service_authentication.md §3.6).
//
// There is a genuine chicken-and-egg: the core creates its schema on first
// start, so nothing can be written to the service map before the core has run —
// which means packaging cannot register a bootstrap credential at install time,
// because at install time there is no database to register it in.
//
// The answer is exactly ONE unauthenticated operation: enrol the first `cli:*`
// credential. The CLI generates its own secret, the core stores the hash, and
// the path closes. Nothing has to place a secret on the host before the database
// exists.
//
// ── Why this is NOT a gRPC method ───────────────────────────────────────────
//
// Two reasons, and the first is the one that matters.
//
// Putting an unauthenticated RPC on FileService would force the auth interceptor
// to exempt it. That is precisely the shape the design rejects everywhere else:
// "one interceptor, not 42 handlers" is only worth anything if it has no
// exceptions, and the exception would sit on the one operation that mints
// credentials. A separate listener keeps the interceptor exception-free.
//
// Second, the peer check needs SO_PEERCRED, and gRPC's C++ API does not expose
// the underlying descriptor. Serving this directly on a Unix socket makes the
// kernel's answer available, which is the whole point of choosing a socket over
// loopback TCP.
//
// ── Why a socket and not loopback ───────────────────────────────────────────
//
// Loopback is not a privilege — ANY local process can connect to it. Left there,
// the window between schema creation and the deployment's enrol call is one in
// which an unprivileged local user could claim the first `cli:*` credential and
// its full capability set. Short and mid-provisioning, but real.
//
// Filesystem permissions restore exactly the property an out-of-band secret
// would have provided, with no secret to distribute: the mechanism becomes "a
// socket you must be permitted to open" rather than "a file you must read". And
// SO_PEERCRED means the core CHECKS the caller rather than inferring it from
// permissions a misconfiguration could widen — unforgeable in a way no
// credential is.

#include "fileengine/accountability.h"

#include <atomic>
#include <string>
#include <thread>

namespace fileengine {

class Database;

struct BootstrapConfig {
    // Created at startup only when bootstrap is incomplete, and removed on
    // successful enrolment. On an established system it is never created at
    // all, so the surface is absent rather than merely closed.
    std::string socket_path = "/run/fileengine/bootstrap.sock";
    std::string pepper;
    int         pepper_version = 1;
};

// One-shot enrolment listener. Serves a single line-oriented request:
//
//     ENROL <service_id>\n   →   OK <token>\n   |   ERR <message>\n
//
// and nothing else. Not a mode in which unauthenticated calls are generally
// accepted — a single operation that creates a credential and does no more.
class BootstrapListener {
public:
    BootstrapListener(Database* db, BootstrapConfig config);
    ~BootstrapListener();

    // Returns false when the socket could not be created. Does NOT start when
    // bootstrap is already complete — the caller checks that first, so an
    // established system never has the socket at all.
    bool start();
    void stop();

    bool running() const { return running_.load(); }

private:
    void serve();
    // Returns the reply line. Verifies the peer's uid before doing anything.
    std::string handle(int client_fd);

    Database*       db_;
    BootstrapConfig config_;
    int             listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread     thread_;
};

}  // namespace fileengine
