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

// Authenticate the calling service, and gate what it may call
// (PROPOSAL_service_authentication.md §3.2, §6).
//
// ONE interceptor, not 42 handlers. The core has 42 RPCs; any per-method check
// will eventually miss one, and the miss will be silent — a new handler simply
// works, for everybody. Enforcing centrally means coverage is a property of the
// mechanism rather than of anyone's diligence, and the acceptance test can prove
// it by enumerating the service descriptor instead of trusting a hand-written
// list.
//
// A rejected call never reaches a handler.

#include "fileengine/database.h"
#include "fileengine/service_credential.h"

#include <grpcpp/support/server_interceptor.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fileengine {

// The resolved caller for the RPC running on this thread.
//
// A thread_local mirroring the existing `t_audit_source_`, which already carries
// the bridge-forwarded client IP the same way — this is a second user of proven
// machinery rather than a new mechanism. gRPC dispatches each call on its own
// thread, so the value is per-call.
//
// Empty when service auth is not required and no token was presented. The audit
// path then falls back to "grpc", which is what it always said.
struct CallerContext {
    static const ServiceIdentity& current();
    static void set(const ServiceIdentity& identity);
    static void clear();
};

// Caches the service map so authentication is not a database round trip per RPC.
//
// Short TTL on purpose: the whole point of putting the map in the database was
// that adding a service or finishing a rotation takes effect without a core
// restart, and a long cache would give that back. Seconds, not minutes.
class ServiceMapCache {
public:
    explicit ServiceMapCache(std::chrono::seconds ttl) : ttl_(ttl) {}

    // Returns the cached identity for a token, or nullopt when absent/expired.
    // Keyed on the token itself so a rotated-out secret stops working within the
    // TTL rather than lingering for as long as the process runs.
    bool lookup(const std::string& token, ServiceIdentity& out);
    void store(const std::string& token, const ServiceIdentity& identity);
    void invalidate();

private:
    struct Entry {
        ServiceIdentity identity;
        std::chrono::steady_clock::time_point expires;
    };
    std::chrono::seconds ttl_;
    std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
};

struct ServiceAuthConfig {
    // Default REQUIRED, consistent with the loopback-bind decision: the safe
    // configuration is the default. The escape hatch exists for migration, must
    // be set deliberately, and warns on EVERY start rather than once — a warning
    // seen only in the boot nobody read is not a warning.
    bool        required = true;
    std::string pepper;
    std::string previous_pepper;      // set only during a pepper rotation
    int         pepper_version = 1;
    std::chrono::seconds cache_ttl{30};
};

// Validates the token, resolves the identity, gates the method, and stashes the
// result for the audit path.
class ServiceAuthInterceptor final : public grpc::experimental::Interceptor {
public:
    ServiceAuthInterceptor(grpc::experimental::ServerRpcInfo* info,
                           Database* db,
                           ServiceMapCache* cache,
                           const ServiceAuthConfig* config)
        : info_(info), db_(db), cache_(cache), config_(config) {}

    ~ServiceAuthInterceptor() override { CallerContext::clear(); }

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override;

private:
    // Returns an empty status when the call may proceed.
    grpc::Status authorize(const std::string& method,
                           const std::multimap<grpc::string_ref, grpc::string_ref>& metadata,
                           const std::string& peer);

    grpc::experimental::ServerRpcInfo* info_;
    Database*                          db_;
    ServiceMapCache*                   cache_;
    const ServiceAuthConfig*           config_;
};

class ServiceAuthInterceptorFactory final
    : public grpc::experimental::ServerInterceptorFactoryInterface {
public:
    ServiceAuthInterceptorFactory(Database* db, ServiceAuthConfig config)
        : db_(db), config_(std::move(config)), cache_(config_.cache_ttl) {}

    grpc::experimental::Interceptor* CreateServerInterceptor(
        grpc::experimental::ServerRpcInfo* info) override {
        return new ServiceAuthInterceptor(info, db_, &cache_, &config_);
    }

private:
    Database*         db_;
    ServiceAuthConfig config_;
    ServiceMapCache   cache_;
};

// True when the peer string names a loopback address. Used to bound the `cli`
// identity, which holds every capability and pays for it with this constraint
// instead of a capability list.
//
// Checked against the PEER rather than inferred from the bind: containers widen
// the bind to 0.0.0.0, so the bind alone constrains nothing there. The peer
// address of an established connection is not something the caller can assert,
// unlike a header — which is what makes this a real control rather than a
// declared one.
bool peer_is_loopback(const std::string& peer);

}  // namespace fileengine
