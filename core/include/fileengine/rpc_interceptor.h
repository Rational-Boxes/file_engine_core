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

#include "fileengine/rpc_tracker.h"

#include <grpcpp/support/server_interceptor.h>

#include <memory>
#include <string>

namespace fileengine {

// Counts in-flight RPCs by riding their lifetime.
//
// gRPC builds one interceptor per RPC and destroys it when that RPC ends, so the
// constructor and destructor bracket the request exactly — including the paths a
// handler never returns from normally (cancellation, a client that goes away
// mid-stream). That is the property that makes this trustworthy as a
// stuck-request signal: an entry can only linger if the RPC genuinely has.
class RpcLifetimeInterceptor final : public grpc::experimental::Interceptor {
public:
    explicit RpcLifetimeInterceptor(const std::string& method)
        : id_(RpcTracker::instance().begin(method)) {}

    ~RpcLifetimeInterceptor() override { RpcTracker::instance().end(id_); }

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override {
        // Observe only — every hook proceeds untouched. An interceptor that
        // altered the request would be a functional change riding along with
        // instrumentation, which is not what this is for.
        methods->Proceed();
    }

private:
    std::uint64_t id_;
};

class RpcLifetimeInterceptorFactory final
    : public grpc::experimental::ServerInterceptorFactoryInterface {
public:
    grpc::experimental::Interceptor* CreateServerInterceptor(
        grpc::experimental::ServerRpcInfo* info) override {
        return new RpcLifetimeInterceptor(info && info->method() ? info->method() : "unknown");
    }
};

} // namespace fileengine
