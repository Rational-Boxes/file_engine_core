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

#include "fileengine/bootstrap_listener.h"

#include "fileengine/database.h"
#include "fileengine/server_logger.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>

namespace fileengine {

BootstrapListener::BootstrapListener(Database* db, BootstrapConfig config)
    : db_(db), config_(std::move(config)) {}

BootstrapListener::~BootstrapListener() { stop(); }

bool BootstrapListener::start() {
    const std::filesystem::path path(config_.socket_path);

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    // A stale socket from a previous run would make bind() fail with EADDRINUSE.
    // Removing it is safe here precisely because we only reach this point when
    // bootstrap is incomplete — there is no established system whose socket we
    // could be stepping on.
    std::filesystem::remove(path, ec);

    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        SERVER_LOG_ERROR("Bootstrap", std::string("could not create the enrolment socket: ") +
                                      std::strerror(errno));
        return false;
    }

    // Narrow the mode BEFORE bind, so the socket never exists on disk with a
    // wider mode than intended — even briefly. Doing it afterwards leaves a
    // window in which the file is world-connectable.
    const mode_t previous_umask = ::umask(0177);   // clears group and other

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, config_.socket_path.c_str(), sizeof(addr.sun_path) - 1);

    bool ok = ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    if (ok) ok = ::listen(listen_fd_, 4) == 0;
    ::umask(previous_umask);

    if (!ok) {
        SERVER_LOG_ERROR("Bootstrap", std::string("could not bind the enrolment socket at ") +
                                      config_.socket_path + ": " + std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    // Belt and braces: umask should have produced 0600 already, but a umask a
    // supervisor set oddly could interfere, and this file is the whole control.
    ::chmod(config_.socket_path.c_str(), S_IRUSR | S_IWUSR);

    running_.store(true);
    thread_ = std::thread([this] { serve(); });

    SERVER_LOG_SECURITY("Bootstrap",
                        "Enrolment socket open at " + config_.socket_path +
                        " — the service map is empty and one unauthenticated cli "
                        "enrolment is permitted. It closes on use.");
    return true;
}

void BootstrapListener::stop() {
    if (!running_.exchange(false)) return;
    if (listen_fd_ >= 0) {
        // Shutting the descriptor down wakes the blocking accept().
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
    std::error_code ec;
    std::filesystem::remove(config_.socket_path, ec);
}

std::string BootstrapListener::handle(int client_fd) {
    // The kernel's answer about who is connecting. This is what makes the
    // control real rather than inferred: file permissions can be widened by a
    // misconfiguration, and this cannot be forged by the caller at all.
    ucred peer{};
    socklen_t len = sizeof(peer);
    if (::getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &peer, &len) != 0) {
        return "ERR could not determine the peer's identity";
    }

    // root, or the uid the core itself runs as. Anyone else is refused even if
    // the socket's mode somehow let them connect.
    const uid_t self = ::geteuid();
    if (peer.uid != 0 && peer.uid != self) {
        SERVER_LOG_SECURITY("Bootstrap",
                            "Refused enrolment from uid " + std::to_string(peer.uid) +
                            " (pid " + std::to_string(peer.pid) + ") — only root or the "
                            "core's own user may enrol");
        return "ERR not permitted";
    }

    // Read one line. Bounded: this is an unauthenticated surface, so a caller
    // that never sends a newline must not be able to grow a buffer without
    // limit.
    std::string request;
    char buf[256];
    while (request.size() < 512) {
        const ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        request.append(buf, static_cast<std::size_t>(n));
        if (request.find('\n') != std::string::npos) break;
    }
    const std::size_t newline = request.find('\n');
    if (newline != std::string::npos) request.resize(newline);
    while (!request.empty() && (request.back() == '\r' || request.back() == ' ')) {
        request.pop_back();
    }

    static const std::string kVerb = "ENROL ";
    if (request.rfind(kVerb, 0) != 0) {
        return "ERR expected: ENROL <cli:name>";
    }
    const std::string service_id = request.substr(kVerb.size());
    if (service_id.empty()) return "ERR expected: ENROL <cli:name>";

    AccountabilityContext ctx;
    // No credential was presented — by definition, that is the point of this
    // path — so the actor is the enrolment itself, and the uid the kernel
    // reported is the only evidence about who. It goes into the record.
    ctx.actor        = "bootstrap";
    ctx.source_iface = "bootstrap";
    ctx.source_addr  = "uid=" + std::to_string(peer.uid);

    auto issued = db_->enrol_bootstrap_credential(service_id, config_.pepper,
                                                  config_.pepper_version, ctx);
    if (!issued.success) {
        SERVER_LOG_SECURITY("Bootstrap", "Enrolment refused: " + issued.error);
        return "ERR " + issued.error;
    }

    SERVER_LOG_SECURITY("Bootstrap",
                        "Enrolled '" + service_id + "' from uid " +
                        std::to_string(peer.uid) + ". The enrolment path is now closed.");
    // The token, exactly once. It is not stored anywhere and cannot be recovered.
    return "OK " + issued.value;
}

void BootstrapListener::serve() {
    while (running_.load()) {
        const int client = ::accept(listen_fd_, nullptr, nullptr);
        if (client < 0) {
            if (running_.load() && (errno == EINTR || errno == EAGAIN)) continue;
            break;   // shut down, or the descriptor went away
        }

        const std::string reply = handle(client) + "\n";
        ::send(client, reply.data(), reply.size(), MSG_NOSIGNAL);
        ::close(client);

        // Single-shot in the strongest sense: on success the listener stops and
        // the socket is removed, so the surface is gone rather than merely
        // refusing. A failed attempt leaves it open — otherwise a typo in the
        // service id would strand the deployment with no way to enrol and no
        // way to reopen short of break-glass.
        if (reply.rfind("OK ", 0) == 0) {
            running_.store(false);
            if (listen_fd_ >= 0) {
                ::close(listen_fd_);
                listen_fd_ = -1;
            }
            std::error_code ec;
            std::filesystem::remove(config_.socket_path, ec);
            break;
        }
    }
}

}  // namespace fileengine
