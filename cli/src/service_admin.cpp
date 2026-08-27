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

#include "service_admin.h"

#include "fileengine/config_loader.h"
#include "fileengine/database.h"
#include "fileengine/server_logger.h"
#include "fileengine/service_credential.h"

#include <grpcpp/client_context.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

namespace cli {

namespace {

// argv is positioned at the COMMAND WORD, so argv[0] is "service-token",
// argv[1] the subcommand, argv[2] onward its arguments.
//
// Every read goes through this rather than indexing directly: argv[n] past argc
// is not nullptr in general, and even when it is, std::string(nullptr) throws
// rather than yielding an empty string — so a missing argument became a crash
// instead of a usage message.
std::string arg_at(int argc, char** argv, int index) {
    if (index >= argc || argv[index] == nullptr) return {};
    return argv[index];
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Service-credential administration (PROPOSAL_service_authentication.md §3.6)
// ═══════════════════════════════════════════════════════════════════════════
//
// This goes through the Database layer directly rather than through new RPCs,
// and that is deliberate on two counts. New RPCs would have to be classified
// into a capability and would then be reachable over the network by whatever
// held it — putting credential minting on the same surface the credentials
// protect. And the `cli` identity is loopback-only by design (§6.1), so
// administration already runs on the core host; there is nothing to reach.
//
// The CLI is the enforcement point for invariants a schema cannot express:
// tokens are generated and never chosen, values are peppered HMACs, changes are
// recorded, and rotation overlaps. A hand-written UPDATE breaks every one of
// them silently — which is why psql is not a supported path.


// The pepper comes from the same configuration the core reads, so the CLI
// cannot accidentally hash under a different one and store a credential that
// will never verify.
struct ServiceAdminContext {
    std::shared_ptr<fileengine::Database> db;
    std::string pepper;
    int         pepper_version = 1;
    fileengine::AccountabilityContext actor;
};

// Records name a PERSON, on evidence. The actor is the operator running the
// command; attributing every administrative action to a generic "cli" would
// make the record technically complete and practically useless.
//
// There is deliberately no --actor override: a self-reported name would be
// forgeable by anyone holding the same token, which on the identity permitted
// every capability is the worst possible place for that weakness. Automation
// uses its own credential (cli:ansible) and is named by it.
std::string operator_identity() {
    if (const char* explicit_id = std::getenv("FILEENGINE_CLI_IDENTITY")) {
        if (*explicit_id) return explicit_id;
    }
    // Falls back to the OS user, which is evidence of a sort on a host where
    // reaching the CLI already requires an account.
    if (const char* user = std::getenv("USER")) {
        if (*user) return std::string("cli:") + user;
    }
    return "cli:unknown";
}

bool load_service_admin_context(ServiceAdminContext& out) {
    // A real (empty) argv, not nullptr: load_config walks argv unconditionally
    // and constructs std::strings from it. Reading the same configuration the
    // core does is the point — the CLI must hash under the same pepper and
    // reach the same database, or it stores credentials the core cannot verify.
    char program_name[] = "fileengine_cli";
    char* fake_argv[] = {program_name, nullptr};
    fileengine::Config config = fileengine::ConfigLoader::load_config(1, fake_argv);

    // Silence the core logger's console output for this process. The Database
    // layer logs at INFO on stdout, and stdout is where the secret goes and
    // where nothing else may go — `TOKEN=$(fileengine_cli service-token issue x)`
    // must capture a token, not a token with a log line stuck to it.
    //
    // This does not silence anything that matters: SECURITY entries are above
    // ERROR and therefore go to stderr regardless, and the durable trail is the
    // accountability record written in the same transaction as the change, not
    // the console.
    fileengine::ServerLogger::getInstance().initialize(
        "error", config.log_file_path, /*log_to_console=*/false,
        /*log_to_file=*/false, config.log_rotation_size_mb, config.log_retention_days,
        config.log_redact_names);
    out.pepper         = config.service_token_pepper;
    out.pepper_version = config.service_token_pepper_version;
    if (out.pepper.empty()) {
        std::cerr << "FILEENGINE_SERVICE_TOKEN_PEPPER is not set.\n"
                  << "Credentials are stored as HMAC(secret, pepper); without it the CLI "
                     "would store hashes the core cannot verify.\n";
        return false;
    }
    out.db = std::make_shared<fileengine::Database>(
        config.db_host, config.db_port, config.db_name, config.db_user, config.db_password, 2);
    if (!out.db->connect()) {
        std::cerr << "Could not connect to the core database.\n";
        return false;
    }
    out.actor.actor        = operator_identity();
    out.actor.source_iface = "cli";
    return true;
}

void print_secret_once(const std::string& token) {
    // stdout and nowhere else. Never a log, never the audit record, never
    // echoed back by a later command — `list` cannot print it because the query
    // does not select it, and there is nothing to recover it from afterwards.
    std::cout << token << std::endl;
    std::cerr << "\nThis is the only time this secret is shown. Store it now.\n"
              << "It is not recoverable: the database holds only HMAC(secret, pepper).\n"
              << "Beware `set -x` and CI jobs that archive stdout — that is the realistic leak.\n";
}

// ── The CLI's own credential (§6.1) ─────────────────────────────────────────

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

int service_token_command(int argc, char** argv) {
    const std::string sub = arg_at(argc, argv, 1);
    ServiceAdminContext ctx;

    if (sub == "list") {
        if (!load_service_admin_context(ctx)) return 1;
        auto listed = ctx.db->list_service_credentials();
        if (!listed.success) { std::cerr << listed.error << "\n"; return 1; }
        std::cout << "SERVICE_ID           PEPPER  CREATED               LAST_USED             CAPABILITIES\n";
        for (const auto& row : listed.value) {
            std::string caps;
            for (size_t i = 0; i < row.capabilities.size(); ++i) {
                if (i) caps += ",";
                caps += row.capabilities[i];
            }
            printf("%-20s %-7d %-21s %-21s %s\n", row.service_id.c_str(), row.pepper_version,
                   row.created_at.c_str(),
                   row.last_used_at.empty() ? "(never)" : row.last_used_at.c_str(),
                   caps.c_str());
        }
        return 0;
    }

    const std::string service_id = arg_at(argc, argv, 2);
    if (service_id.empty()) {
        std::cerr << "usage: fileengine_cli service-token <issue|rotate|prune|revoke|list> <service_id>\n";
        return 2;
    }
    if (!load_service_admin_context(ctx)) return 1;

    if (sub == "issue" || sub == "rotate") {
        // rotate ADDS alongside the existing secret. Replacing in place would
        // strand every instance still presenting the old one, which is why
        // rotation without overlap never actually gets done.
        auto issued = ctx.db->issue_service_credential(service_id, ctx.pepper,
                                                       ctx.pepper_version, ctx.actor,
                                                       /*rotate=*/sub == "rotate");
        if (!issued.success) { std::cerr << issued.error << "\n"; return 1; }
        print_secret_once(issued.value);
        if (sub == "rotate") {
            std::cerr << "\nBoth secrets are now valid. Roll " << service_id
                      << " onto this one, then run:\n"
                      << "  fileengine_cli service-token prune " << service_id << "\n";
        }
        return 0;
    }
    if (sub == "prune") {
        auto pruned = ctx.db->prune_service_credentials(service_id, ctx.actor);
        if (!pruned.success) { std::cerr << pruned.error << "\n"; return 1; }
        std::cerr << "Removed " << pruned.value << " superseded credential(s) for "
                  << service_id << ".\n";
        return 0;
    }
    if (sub == "revoke") {
        auto revoked = ctx.db->revoke_service_credentials(service_id, ctx.actor);
        if (!revoked.success) { std::cerr << revoked.error << "\n"; return 1; }
        std::cerr << "Revoked " << revoked.value << " credential(s) for " << service_id
                  << ". It can no longer authenticate.\n";
        return 0;
    }
    std::cerr << "unknown subcommand: " << sub << "\n";
    return 2;
}

int service_command(int argc, char** argv) {
    const std::string sub = arg_at(argc, argv, 1);
    const std::string service_id = arg_at(argc, argv, 2);
    if (service_id.empty()) {
        std::cerr << "usage: fileengine_cli service <capabilities|grant|revoke-cap> <service_id> [capability]\n";
        return 2;
    }
    ServiceAdminContext ctx;
    if (!load_service_admin_context(ctx)) return 1;

    if (sub == "capabilities") {
        auto caps = ctx.db->service_capabilities(service_id);
        if (!caps.success) { std::cerr << caps.error << "\n"; return 1; }
        for (auto c : caps.value) std::cout << fileengine::to_string(c) << "\n";
        return 0;
    }

    const std::string cap_name = arg_at(argc, argv, 3);
    fileengine::Capability capability;
    if (!fileengine::parse_capability(cap_name, capability)) {
        // Validated against the compiled definitions, so a typo is rejected
        // rather than stored as a capability nothing will ever match.
        std::cerr << "unknown capability: '" << cap_name << "'\nknown: ";
        for (auto c : fileengine::all_capabilities()) std::cerr << fileengine::to_string(c) << " ";
        std::cerr << "\n";
        return 2;
    }

    if (sub == "grant") {
        if (fileengine::is_high_risk(capability)) {
            // Onboarding a service and arming it are separate acts. The
            // confirmation is what keeps a dangerous grant deliberate rather
            // than something that rides along with a routine command.
            bool confirmed = false;
            for (int i = 4; i < argc; ++i) {
                if (arg_at(argc, argv, i) == "--i-understand-this-is-high-risk") confirmed = true;
            }
            if (!confirmed) {
                std::cerr << "'" << cap_name << "' is a high-risk capability.\n"
                          << (capability == fileengine::Capability::Destroy
                                  ? "  destroy irreversibly removes committed data.\n"
                                  : "  accountability reads the security log, which across "
                                    "tenants reconstructs who did what to whom.\n")
                          << "Re-run with --i-understand-this-is-high-risk to grant it.\n";
                return 2;
            }
        }
        auto granted = ctx.db->grant_service_capability(service_id, capability, ctx.actor);
        if (!granted.success) { std::cerr << granted.error << "\n"; return 1; }
        std::cerr << "Granted '" << cap_name << "' to " << service_id << ".\n";
        return 0;
    }
    if (sub == "revoke-cap") {
        auto revoked = ctx.db->revoke_service_capability(service_id, capability, ctx.actor);
        if (!revoked.success) { std::cerr << revoked.error << "\n"; return 1; }
        std::cerr << "Revoked '" << cap_name << "' from " << service_id << ".\n";
        return 0;
    }
    std::cerr << "unknown subcommand: " << sub << "\n";
    return 2;
}

int bootstrap_command(int argc, char** argv) {
    // Talks to the Unix socket, not to the database and not over gRPC: at this
    // point there is no credential to present, and the socket's permissions plus
    // the kernel's SO_PEERCRED answer are what stand in for one.
    const std::string sub = arg_at(argc, argv, 1);
    if (sub != "enrol" && sub != "enroll") {
        std::cerr << "usage: fileengine_cli bootstrap enrol <cli:name>\n";
        return 2;
    }
    const std::string service_id = arg_at(argc, argv, 2);
    if (service_id.rfind("cli:", 0) != 0) {
        std::cerr << "bootstrap enrolment creates a cli identity, e.g. cli:ansible\n";
        return 2;
    }

    std::string socket_path = "/run/fileengine/bootstrap.sock";
    if (const char* override_path = std::getenv("FILEENGINE_BOOTSTRAP_SOCKET")) {
        if (*override_path) socket_path = override_path;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { std::cerr << "could not create a socket\n"; return 1; }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "could not reach the enrolment socket at " << socket_path << ": "
                  << std::strerror(errno) << "\n"
                  << "It exists only while bootstrap is incomplete — on an established "
                     "system it is never created. If credentials have been lost, see the "
                     "break-glass procedure.\n";
        ::close(fd);
        return 1;
    }

    const std::string request = "ENROL " + service_id + "\n";
    ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
    std::string reply;
    char buf[512];
    while (true) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        reply.append(buf, static_cast<std::size_t>(n));
        if (reply.find('\n') != std::string::npos) break;
    }
    ::close(fd);
    while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r')) reply.pop_back();

    if (reply.rfind("OK ", 0) != 0) {
        std::cerr << (reply.rfind("ERR ", 0) == 0 ? reply.substr(4) : reply) << "\n";
        return 1;
    }
    print_secret_once(reply.substr(3));
    std::cerr << "\nThe enrolment path is now closed. Use this credential to issue the rest:\n"
              << "  fileengine_cli service-token issue <service_id>\n";
    return 0;
}

}  // namespace cli
