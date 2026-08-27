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

#include "fileengine/service_auth_interceptor.h"

#include "fileengine/server_logger.h"

// ServerRpcInfo::server_context() returns a forward-declared type; the full
// definition is needed to call peer() on it.
#include <grpcpp/server_context.h>

namespace fileengine {

namespace {
thread_local ServiceIdentity t_caller;
thread_local grpc::Status    t_verdict = grpc::Status::OK;
}  // namespace

const ServiceIdentity& CallerContext::current() { return t_caller; }
void CallerContext::set(const ServiceIdentity& identity) { t_caller = identity; }
void CallerContext::clear() {
    t_caller = ServiceIdentity{};
    // Reset the verdict too. gRPC reuses worker threads, so a refusal left
    // behind would refuse the NEXT unrelated call on that thread — a failure
    // that would appear only under concurrency and look entirely random.
    t_verdict = grpc::Status::OK;
}

const grpc::Status& CallerContext::verdict() { return t_verdict; }
void CallerContext::set_verdict(const grpc::Status& status) { t_verdict = status; }

grpc::Status service_auth_guard() { return CallerContext::verdict(); }

// ── Cache ───────────────────────────────────────────────────────────────────

bool ServiceMapCache::lookup(const std::string& token, ServiceIdentity& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(token);
    if (it == entries_.end()) return false;
    if (std::chrono::steady_clock::now() >= it->second.expires) {
        entries_.erase(it);
        return false;
    }
    out = it->second.identity;
    return true;
}

void ServiceMapCache::store(const std::string& token, const ServiceIdentity& identity) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Only successful resolutions are cached. Caching a failure would mean a
    // credential issued moments ago stays rejected for the TTL, which is the
    // shape of bug that gets diagnosed as "the token doesn't work" and worked
    // around by reissuing.
    if (identity.service_id.empty()) return;
    entries_[token] = Entry{identity, std::chrono::steady_clock::now() + ttl_};
}

void ServiceMapCache::invalidate() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

// ── Loopback ────────────────────────────────────────────────────────────────

namespace {

// Is `text` a dotted quad in 127.0.0.0/8?
//
// Parsed rather than prefix-matched. `peer.rfind("ipv4:127.", 0) == 0` would
// also accept "ipv4:127.0.0.1.evil.com" — gRPC does not produce peer strings
// like that, but a control that is correct only because of what its input
// happens to look like is one refactor away from being wrong, and this one
// decides whether the full-capability identity is accepted.
bool is_loopback_v4(const std::string& text) {
    int octets[4] = {-1, -1, -1, -1};
    int index = 0;
    int value = -1;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            value = (value < 0 ? 0 : value * 10) + (c - '0');
            if (value > 255) return false;
        } else if (c == '.') {
            if (value < 0 || index > 3) return false;
            octets[index++] = value;
            value = -1;
        } else {
            return false;                      // anything else is not a quad
        }
    }
    if (value < 0 || index != 3) return false; // needs exactly four octets
    octets[3] = value;
    return octets[0] == 127;
}

}  // namespace

bool peer_is_loopback(const std::string& peer) {
    // gRPC peer strings look like "ipv4:127.0.0.1:54321", "ipv6:[::1]:54321" or
    // "unix:/run/fileengine/bootstrap.sock". A Unix socket peer is by definition
    // local, and more strongly bounded than loopback TCP.
    if (peer.rfind("unix:", 0) == 0) return true;

    if (peer.rfind("ipv4:", 0) == 0) {
        // Strip the trailing ":port" and validate what remains as an address.
        const std::size_t colon = peer.rfind(':');
        if (colon == 4) return false;          // "ipv4:" with nothing after it
        return is_loopback_v4(peer.substr(5, colon - 5));
    }

    if (peer.rfind("ipv6:", 0) == 0) {
        // Bracketed literal: take exactly what is inside the brackets.
        const std::size_t open = peer.find('[');
        const std::size_t close = peer.find(']', open == std::string::npos ? 0 : open);
        if (open == std::string::npos || close == std::string::npos) return false;
        const std::string address = peer.substr(open + 1, close - open - 1);
        if (address == "::1") return true;
        // v4-mapped loopback, e.g. ::ffff:127.0.0.1
        const std::string mapped_prefix = "::ffff:";
        if (address.rfind(mapped_prefix, 0) == 0) {
            return is_loopback_v4(address.substr(mapped_prefix.size()));
        }
        return false;
    }

    return false;
}

// ── Enforcement ─────────────────────────────────────────────────────────────

grpc::Status ServiceAuthInterceptor::authorize(
        const std::string& method,
        const std::multimap<grpc::string_ref, grpc::string_ref>& metadata,
        const std::string& peer) {

    std::string token;
    const auto it = metadata.find(kServiceTokenMetadataKey);
    if (it != metadata.end()) {
        token.assign(it->second.data(), it->second.length());
    }

    if (token.empty()) {
        if (!config_->required) {
            // Migration mode: accept and record nothing. The audit path falls
            // back to "grpc", so records still saying "grpc" are exactly the
            // callers not yet migrated — which makes the rollout self-tracking.
            return grpc::Status::OK;
        }
        // The message names no secret and no service. On the rejection path
        // above all — it is where a credential is most likely to be echoed.
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "no service token presented");
    }

    ServiceIdentity identity;
    if (!cache_->lookup(token, identity)) {
        if (db_ == nullptr) {
            return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "service map unavailable");
        }
        auto resolved = db_->resolve_service_token(token, config_->pepper,
                                                   config_->previous_pepper,
                                                   config_->pepper_version);
        if (!resolved.success) {
            // A database failure is NOT an authentication failure. Saying
            // UNAUTHENTICATED here would tell a caller their credential is bad
            // when the truth is the core cannot check it, and would send an
            // operator hunting the wrong problem.
            SERVER_LOG_SECURITY("ServiceAuth",
                                "Could not resolve a service token: " + resolved.error);
            return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                "service map temporarily unavailable");
        }
        identity = resolved.value;
        cache_->store(token, identity);
    }

    if (identity.service_id.empty()) {
        // Unknown id and wrong secret are deliberately indistinguishable, and
        // cost the same — the resolve path burns equivalent work on a miss.
        SERVER_LOG_SECURITY("ServiceAuth",
                            "Rejected an unrecognised service token from peer " + peer +
                            " calling " + method);
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "service token not recognised");
    }

    // `cli` holds every capability, so it carries the most conditions rather
    // than the fewest. Loopback alone would let ANY local process act as cli,
    // and the token alone would let it act from anywhere — both are required.
    if (identity.is_cli() && !peer_is_loopback(peer)) {
        SERVER_LOG_SECURITY("ServiceAuth",
                            "Refused a cli identity (" + identity.service_id +
                            ") from non-loopback peer " + peer);
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                            "cli identities may only connect over loopback");
    }

    // Capability gating. Default deny for anything unclassified — including for
    // cli, which is granted the full set rather than exempted from the
    // mechanism, so a newly added and unclassified RPC stays unreachable by
    // everyone. That is the property worth having; "cli may call any method"
    // would re-open the hole at the one identity most likely to hold it.
    Capability needed;
    if (!capability_for_method(method, needed)) {
        SERVER_LOG_SECURITY("ServiceAuth",
                            "Denied " + method + " to " + identity.service_id +
                            " — the method belongs to no capability. A new RPC is "
                            "unreachable until it is classified.");
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                            "method is not classified into a capability");
    }
    if (!identity.has(needed)) {
        // Independent of the end-user axis: this refuses the call even when the
        // user identity presented would otherwise be authorized.
        SERVER_LOG_SECURITY("ServiceAuth",
                            "Denied " + method + " to " + identity.service_id +
                            " — lacks capability '" + to_string(needed) + "'");
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                            std::string("service lacks the '") + to_string(needed) +
                            "' capability");
    }

    CallerContext::set(identity);
    return grpc::Status::OK;
}

void ServiceAuthInterceptor::Intercept(grpc::experimental::InterceptorBatchMethods* methods) {
    // POST_RECV_INITIAL_METADATA is the first point at which the client's
    // metadata is available, and it is before the handler runs — which is the
    // requirement: a rejected call must never reach a handler.
    if (methods->QueryInterceptionHookPoint(
            grpc::experimental::InterceptionHookPoints::POST_RECV_INITIAL_METADATA)) {
        const std::string method = (info_ && info_->method()) ? info_->method() : "";
        std::string peer;
        const auto* server_context = info_ ? info_->server_context() : nullptr;
        if (server_context != nullptr) peer = server_context->peer();

        const auto* metadata = methods->GetRecvInitialMetadata();
        static const std::multimap<grpc::string_ref, grpc::string_ref> kEmpty;
        const grpc::Status status = authorize(method, metadata ? *metadata : kEmpty, peer);

        // Record the verdict for the handler to return. Deliberately NOT
        // Hijack(): see CallerContext::verdict.
        CallerContext::set_verdict(status);
    }
    methods->Proceed();
}

}  // namespace fileengine
