# Monitoring & Telemetry — REST API and Integration Plan

**Status:** Research / design draft
**Branch:** `monitoring`
**Audience:** people deciding what to build before any code lands

The goal is real-time, comprehensive monitoring of a running
fileengine_server — both for operators (Is it up? Healthy? Falling
behind?) and for higher-level systems (capacity planners, SLO
dashboards, paging). The server's existing surface is gRPC-only; we
want a REST API and an exposition format that the well-known
monitoring stacks already speak.

---

## 1. What already exists

| Source                  | Signal                                                  | Surface |
|-------------------------|---------------------------------------------------------|---------|
| `grpc::health::v1::Health` (enabled in server.cpp:212) | Liveness — does the server respond?    | gRPC only |
| `GetStorageUsage` RPC   | total / used / available bytes, usage %                 | gRPC |
| `StorageTracker`        | per-file access counts; most/least accessed; largest files; per-tenant usage | in-process |
| `FileCuller`            | `get_culled_file_count`, `get_culled_byte_count`, configured threshold       | in-process |
| `CacheManager`          | cache_size_bytes, max_cache_size_bytes, usage %, hit/miss path               | in-process |
| `ConnectionPoolManager` | `is_primary_available`, `is_using_secondary`, `is_server_in_readonly_mode`   | in-process |
| `ObjectStoreSync`       | sync activity, health monitoring (per CLAUDE.md)        | in-process |
| `acl_audit` table       | full grant/revoke history with actor + before/after bits | SQL |
| `ServerLogger`          | leveled text log to stdout + `/var/log/fileengine`      | file/stdout |

Nothing is HTTP. There is no exposition for Prometheus, no OTel SDK,
no per-RPC counters, no histograms. Operators have to shell into the
box and `grep` the log.

## 2. What we want

1. **Health & readiness probes** — Kubernetes-style `/healthz` and
   `/readyz` returning 200/503 with a one-line body.
2. **Structured status snapshot** — `/v1/status` returning a JSON blob
   that captures the server's current picture: build/version,
   DB connection state, object-store sync state, cache fill, tenant
   list, per-tenant disk usage, recent error rate.
3. **Metrics in a standard format** — `/metrics` in Prometheus /
   OpenMetrics exposition format, scrapeable by any stack that speaks
   that protocol (Prometheus, VictoriaMetrics, Datadog Agent via the
   OpenMetrics check, New Relic Prometheus Remote Write, etc.).
4. **Per-RPC observability** — counters and latency histograms keyed
   by gRPC method, status code, tenant. The single most useful thing
   for capacity work.
5. **Tracing** — OpenTelemetry spans on every RPC so distributed
   workloads (CLI → server → object store / DB) can be inspected
   end-to-end in Jaeger/Tempo/Honeycomb/Datadog APM/etc.
6. **Audit log access** — read-only REST views of `acl_audit` for
   compliance dashboards.

We don't want to ship our own dashboard. We want the data exposed in
formats the standard stacks already render.

---

## 3. Proposed REST surface

A second listener separate from the gRPC port. Default `:8081`.

| Endpoint                           | Method | Purpose                                   | Auth |
|------------------------------------|--------|-------------------------------------------|------|
| `/healthz`                         | GET    | Liveness — process is up                  | none |
| `/readyz`                          | GET    | Readiness — DB connected, schema ok       | none |
| `/metrics`                         | GET    | Prometheus exposition (text/plain)        | none\* |
| `/v1/status`                       | GET    | JSON snapshot of server state             | bearer/role |
| `/v1/version`                      | GET    | JSON: version, git SHA, build time        | none |
| `/v1/tenants`                      | GET    | List of tenants from `public.tenants`     | system_admin |
| `/v1/tenants/{id}/usage`           | GET    | Per-tenant storage usage + file counts    | tenant member or system_admin |
| `/v1/audit/acl?since=...&limit=...`| GET    | Slice of `acl_audit`                      | system_admin |
| `/v1/audit/acl/{resource_uid}`     | GET    | Audit rows for a single resource          | MANAGE_ACL on resource or system_admin |

\* `/metrics` is typically scraped from inside a trusted network. If
the REST listener binds to a non-public interface (e.g. `127.0.0.1`
or `10.0.0.0/8` only) we leave it auth-free for low-overhead scrapes.
For public scrape, support a static bearer token via env var so
something like the Prometheus `bearer_token_file` mechanism works.

### `/v1/status` example payload

```json
{
  "version": "1.0.0",
  "git_sha": "89b238f",
  "uptime_seconds": 12345,
  "started_at": "2026-06-17T20:30:00Z",
  "database": {
    "primary_available": true,
    "using_secondary": false,
    "readonly_mode": false,
    "pool_size": 10,
    "in_use": 2
  },
  "object_store": {
    "configured": true,
    "endpoint": "http://minio.internal:9000",
    "sync_enabled": true,
    "sync_lag_seconds": 3.2,
    "last_sync_at": "2026-06-17T20:55:43Z",
    "last_sync_status": "ok"
  },
  "cache": {
    "size_bytes": 524288000,
    "max_bytes": 1073741824,
    "usage_pct": 48.8,
    "threshold_pct": 80.0
  },
  "culler": {
    "culled_files_total": 142,
    "culled_bytes_total": 1073741824
  },
  "tenants": [
    {"id": "default", "files": 4321, "bytes": 9876543210},
    {"id": "acme",    "files": 12,   "bytes": 9876}
  ]
}
```

The snapshot is intentionally cheap — every field is already
in-memory or one fast SQL query. Operators can `curl
:8081/v1/status | jq` from anywhere.

---

## 4. Metrics — Prometheus exposition

Single endpoint, text-format, scrape-cheap. Metric names follow the
[Prometheus naming conventions](https://prometheus.io/docs/practices/naming/):

```
# HELP fileengine_rpc_requests_total Total gRPC requests handled
# TYPE fileengine_rpc_requests_total counter
fileengine_rpc_requests_total{method="MakeDirectory",code="OK",tenant="default"} 4321
fileengine_rpc_requests_total{method="PutFile",code="OK",tenant="default"} 1200
fileengine_rpc_requests_total{method="PutFile",code="PERMISSION_DENIED",tenant="default"} 7

# HELP fileengine_rpc_latency_seconds Per-RPC latency histogram
# TYPE fileengine_rpc_latency_seconds histogram
fileengine_rpc_latency_seconds_bucket{method="PutFile",tenant="default",le="0.005"} 800
fileengine_rpc_latency_seconds_bucket{method="PutFile",tenant="default",le="0.01"}  950
fileengine_rpc_latency_seconds_bucket{method="PutFile",tenant="default",le="0.1"}  1190
fileengine_rpc_latency_seconds_bucket{method="PutFile",tenant="default",le="+Inf"} 1200
fileengine_rpc_latency_seconds_sum{method="PutFile",tenant="default"} 12.4
fileengine_rpc_latency_seconds_count{method="PutFile",tenant="default"} 1200

# HELP fileengine_storage_bytes Storage usage in bytes
# TYPE fileengine_storage_bytes gauge
fileengine_storage_bytes{tenant="default",state="used"} 9876543210
fileengine_storage_bytes{tenant="default",state="available"} 53687091200

# HELP fileengine_cache_usage_ratio Cache usage as fraction of max
# TYPE fileengine_cache_usage_ratio gauge
fileengine_cache_usage_ratio 0.488

# HELP fileengine_object_store_sync_lag_seconds Time since last successful S3 sync
# TYPE fileengine_object_store_sync_lag_seconds gauge
fileengine_object_store_sync_lag_seconds 3.2

# HELP fileengine_db_pool_in_use Connections currently checked out
# TYPE fileengine_db_pool_in_use gauge
fileengine_db_pool_in_use 2

# HELP fileengine_acl_changes_total ACL grant/revoke events
# TYPE fileengine_acl_changes_total counter
fileengine_acl_changes_total{action="grant"} 142
fileengine_acl_changes_total{action="revoke"} 18
fileengine_acl_changes_total{action="grant_deny"} 3
```

Why Prometheus exposition rather than something we'd invent: every
modern monitoring system speaks it.

- **Prometheus / Mimir / VictoriaMetrics / Thanos** — direct scrape.
- **OpenTelemetry Collector** — `prometheus` receiver.
- **Datadog Agent** — `OpenMetricsCheck`.
- **New Relic** — Prometheus remote-write or NRDOT collector.
- **Grafana Cloud, Honeycomb, Splunk, Dynatrace** — all support
  Prometheus scrape.
- **Nagios / Icinga** — `check_prometheus` plugin.

Cardinality discipline matters: keep labels to `method`, `code`,
`tenant`. Don't add per-file or per-user labels — those explode the
series count.

---

## 5. C++ HTTP server options

We need a small embedded HTTP server inside the existing gRPC binary.
Comparison of realistic options:

| Library         | Header-only | Build deps        | License   | Notes |
|-----------------|-------------|-------------------|-----------|-------|
| **cpp-httplib** | yes         | none (single hdr) | MIT       | The pragmatic pick. Tiny, no boost, no codegen. Used by many projects in production. Has a built-in thread pool, TLS support via OpenSSL (which we already link), keep-alive, route handlers. Recommended. |
| **Drogon**      | no          | jsoncpp, OpenSSL, optional Postgres/Redis | MIT | Full-featured async web framework. Overkill for an /metrics endpoint. |
| **Pistache**    | no          | system pkg        | Apache-2  | Decent perf, async. Smaller community than cpp-httplib. |
| **Crow**        | yes         | boost (header-only path)  | BSD-3 | Flask-like. Solid but more "framework" than we need. |
| **Microsoft cpprestsdk** | no | boost, openssl | MIT | Heavy. Microsoft has put it in maintenance mode. Avoid. |
| **Roll our own** | n/a       | n/a               | -         | Don't. We have OpenSSL but TLS, HTTP/1.1 keep-alive, chunked encoding, request parsing — too easy to get one of those wrong. |

**Recommendation: cpp-httplib.** Single `httplib.h`, zero build-system
churn, integrates with the existing OpenSSL link (we already have it
for AES-256-GCM). One C++ thread inside the existing process can
serve scrape and probe traffic without competing with gRPC's pool.

---

## 6. Metrics library

We need atomic counters, histograms with configurable buckets, and a
text-exposition serializer.

| Option                  | Notes |
|-------------------------|-------|
| **prometheus-cpp**      | Reference C++ Prometheus client. Header-only `core/` + `pull/` modules. Used in production by many services. License: MIT. Composes well with cpp-httplib. **Recommended.** |
| **OpenTelemetry SDK (cpp)** | OTel's official C++ SDK. Heavier (gRPC + protobuf already linked, but adds more). Useful if we also want OTLP traces. We can run prometheus-cpp for metrics now and add the OTel SDK later for traces. |
| **Roll our own**        | The counter is one atomic int. The histogram is N atomic ints. The exposition format is a `for` loop. ~200 lines. Tempting for v0 but loses the OpenMetrics extensions and any future maintenance. |

**Recommendation:** prometheus-cpp for metrics emission. Defer OTel
SDK to the tracing phase (§7).

---

## 7. Tracing — OpenTelemetry (optional build)

Once `/metrics` is in place, the next ROI win is distributed traces.

- Instrument every gRPC server method with a span (method name,
  tenant, user). The OTel C++ SDK ships a gRPC interceptor.
- Propagate via the standard W3C `traceparent` header (we already
  have `AuthenticationContext` on every RPC; the trace context lives
  in gRPC metadata, separate concern).
- Export via OTLP (gRPC or HTTP/protobuf) to the OTel Collector,
  which fans out to Jaeger / Tempo / Honeycomb / Datadog APM / etc.

**Build-time optional.** `opentelemetry-cpp` isn't in mainline Fedora
or Debian repos and is non-trivial to vendor, so it's a CMake option,
not a hard build dep:

```cmake
option(FILEENGINE_ENABLE_OTEL "Build with OpenTelemetry tracing support" OFF)

if(FILEENGINE_ENABLE_OTEL)
    find_package(opentelemetry-cpp REQUIRED COMPONENTS api sdk exporters_otlp_grpc)
    target_compile_definitions(fileengine_core PUBLIC FILEENGINE_OTEL=1)
    target_link_libraries(fileengine_core PUBLIC opentelemetry-cpp::api ...)
endif()
```

Source code wraps every span call in `#ifdef FILEENGINE_OTEL` (or
via a thin tracing facade that compiles to no-ops when the flag is
off). When the flag is off the binary has zero OTel link-time
footprint — the same RPM ships everywhere, with packagers flipping
the option in distros that have the SDK packaged.

Cost when enabled: linking `opentelemetry-cpp` (a few MB), running
an OTel Collector sidecar. Pay it when the team needs trace data,
not now.

---

## 8. Integration matrix

What each well-known stack consumes, and what we need to expose:

| Stack            | Needs from us                                                | Status with this plan |
|------------------|-------------------------------------------------------------|-----------------------|
| **Prometheus**   | `/metrics` scrape                                            | ✓ direct |
| **Grafana**      | data via Prometheus or Loki; dashboards                      | ✓ via Prometheus; pre-built dashboard JSON we ship as `docs/grafana-dashboard.json` |
| **VictoriaMetrics** | `/metrics` scrape                                         | ✓ direct |
| **Thanos / Cortex / Mimir** | Prometheus-style remote write                    | ✓ via vmagent or Prometheus |
| **Datadog**      | OpenMetrics check OR Agent + OTLP                            | ✓ via OpenMetrics |
| **New Relic**    | Prometheus remote-write OR OTLP                              | ✓ via remote-write |
| **AWS CloudWatch** | OTel Collector → CloudWatch exporter                       | ✓ once OTel SDK added |
| **Splunk Observability Cloud** | OTel / Prometheus remote-write             | ✓ |
| **Dynatrace**    | OneAgent + Prometheus extension                              | ✓ |
| **Honeycomb**    | OTLP traces                                                  | ✓ once OTel SDK added |
| **Jaeger / Tempo** | OTLP traces                                                | ✓ once OTel SDK added |
| **ELK / Loki**   | Structured logs                                              | ✗ need to switch ServerLogger to JSON output (small change) |
| **Nagios / Icinga / Zabbix** | active HTTP probes against `/healthz` + `check_prometheus` | ✓ |
| **Kubernetes liveness / readiness** | `/healthz` + `/readyz` HTTP                   | ✓ |
| **systemd `Type=notify`** | sd_notify("READY=1") at startup                     | Could add; minimal |

---

## 9. Recommended phased plan

### Phase A — REST listener + health + status (1–2 days)

- Add cpp-httplib (`third_party/httplib.h`), wire a second listener
  on `:8081` started from `server.cpp`.
- Implement `/healthz`, `/readyz`, `/v1/version`, `/v1/status`.
- Config: `FILEENGINE_HTTP_METRICS_ADDR=0.0.0.0`,
  `FILEENGINE_HTTP_METRICS_PORT=8081`, default-on.
- Doc + smoke test (`curl :8081/v1/status | jq`).

**Exit criteria:** kubelet-style probes work; operators can `curl
:8081/v1/status` and see DB/cache/store state.

### Phase B — Prometheus metrics (1–2 days)

- Add `prometheus-cpp` as a dependency. Wire `/metrics`.
- Per-RPC counters and latency histograms via a gRPC server
  interceptor — one place to instrument all 40+ methods.
- Gauges for cache usage, DB pool, sync lag, per-tenant storage bytes.
- Ship a starter Grafana dashboard (JSON) in `docs/dashboards/`.

**Exit criteria:** `prometheus --config.file=prometheus.yml` against
the server's `/metrics` produces every panel on the bundled dashboard.

### Phase C — Tracing (3–5 days, opt-in build)

- Gate behind `-DFILEENGINE_ENABLE_OTEL=ON`. Default OFF so the
  stock RPM/DEB built on a vanilla Fedora/Debian doesn't need
  `opentelemetry-cpp` to be present.
- When enabled: gRPC server interceptor that opens a span per
  request, attributes: `rpc.method`, `tenant`, `user`,
  `auth.system_admin`.
- Wrap `Database` and `S3Storage` calls with child spans so a
  PutFile trace shows the breakdown between metadata write, ACL
  apply, encryption, local store, and (async) S3 upload.
- Source uses a thin tracing facade (`tracing.h`) that compiles to
  no-op inline functions when `FILEENGINE_OTEL` isn't defined, so
  call sites don't need `#ifdef` clutter.

**Exit criteria (when built with the flag on):** A `PutFile` call
against an OTel Collector configured for Jaeger renders an end-to-end
trace. **Exit criteria (with the flag off):** the binary builds and
runs identically to a Phase B build, with no OTel symbols linked.

### Phase D — Structured logging (0.5 day)

- ServerLogger gains a JSON output mode (config-flagged) so Loki /
  ELK / Splunk can ingest the log file directly without grok rules.

### Phase E — Audit REST views (1 day)

- `/v1/audit/acl` endpoints, paginated. Gated by `system_admin` for
  the global view and by `MANAGE_ACL` on the resource for the
  per-resource view.

---

## 10. Decisions (resolved)

- **Bind address for `:8081`.** Default `0.0.0.0`. Production
  assumption is that the network perimeter (firewall / VPC security
  groups) restricts the port to trusted scrapers and operators —
  there is no second authentication layer to add for `/metrics`.
- **Authentication on `/metrics`.** None. Same firewall-trust
  rationale as above.
- **Cardinality of the `tenant` label.** Emit by default but
  config-gated: `FILEENGINE_METRICS_TENANT_LABEL=true|false`. Flip
  to `false` to collapse tenant series for high-tenant-count
  deployments.
- **TLS on the REST port.** Not implemented. Always assumed to be
  inside a trusted network.
- **Where do build-info / git-sha come from at runtime?** CMake
  baking a generated header (`build_info.h`) via `configure_file()`
  — the standard pattern.
- **systemd integration.** Yes, full `Type=notify` integration:
  `sd_notify("READY=1")` once the listener is up, watchdog ping
  loop, status messages. The unit file already exists; switch
  `Type=simple` → `Type=notify` and add `WatchdogSec=`.

---

## 11. Out of scope (explicitly)

- A custom dashboard UI. The Grafana ecosystem already wins this.
- Alert routing. Alertmanager / PagerDuty / Opsgenie all consume the
  Prometheus output; we don't ship rules ourselves (we ship example
  rules in `docs/`).
- Full event-bus / streaming telemetry. If that becomes a need,
  push acl_audit + file events into Kafka as a separate Phase F.

---

## 12. Locked-in defaults

- HTTP listener: cpp-httplib, ON by default
  (`FILEENGINE_HTTP_METRICS_ENABLED=true`,
  `FILEENGINE_HTTP_METRICS_ADDR=0.0.0.0`,
  `FILEENGINE_HTTP_METRICS_PORT=8081`). Firewall is the trust boundary.
- Metrics: prometheus-cpp, one global registry. Per-tenant labels on
  by default (`FILEENGINE_METRICS_TENANT_LABEL=true`); flip to
  `false` for high-tenant-count deployments.
- Tracing: **opt-in at build time** via
  `-DFILEENGINE_ENABLE_OTEL=ON`. Default OFF so the standard package
  builds on stock distros without `opentelemetry-cpp` available. When
  the flag is on, the runtime behaviour is also OFF by default and
  enables when an OTel Collector endpoint is configured
  (`FILEENGINE_OTLP_ENDPOINT=...`).
- Logging: text by default; flip to JSON with
  `FILEENGINE_LOG_FORMAT=json`.
- systemd: `Type=notify` with watchdog. `sd_notify("READY=1")` from
  the server once both the gRPC listener and the REST listener are
  bound.

---

## Appendix — references

- cpp-httplib: https://github.com/yhirose/cpp-httplib
- prometheus-cpp: https://github.com/jupp0r/prometheus-cpp
- OpenTelemetry C++: https://github.com/open-telemetry/opentelemetry-cpp
- Prometheus naming: https://prometheus.io/docs/practices/naming/
- OpenMetrics spec: https://openmetrics.io/
- Kubernetes probe semantics:
  https://kubernetes.io/docs/tasks/configure-pod-container/configure-liveness-readiness-startup-probes/
- W3C Trace Context: https://www.w3.org/TR/trace-context/
