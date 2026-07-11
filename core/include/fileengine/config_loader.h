#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

#include <string>
#include <map>

namespace fileengine {

struct Config {
    // Database configuration
    std::string db_host = "localhost";
    int db_port = 5432;
    std::string db_name = "fileengine";
    std::string db_user = "fileengine_user";
    std::string db_password = "fileengine_password";
    
    // Storage configuration
    std::string storage_base_path = "/tmp/fileengine_storage";
    bool encrypt_data = false;
    bool compress_data = false;
    std::string encryption_key;  // Added for encryption support
    
    // S3/MinIO configuration
    std::string s3_endpoint = "http://localhost:9000";
    std::string s3_region = "us-east-1";
    std::string s3_bucket = "fileengine";
    std::string s3_access_key = "minioadmin";
    std::string s3_secret_key = "minioadmin";
    bool s3_path_style = true;
    
    // Cache configuration
    double cache_threshold = 0.8;  // 80% threshold
    size_t max_cache_size_mb = 1024;  // 1GB max cache
    
    // Tenant configuration
    bool multi_tenant_enabled = true;
    
    // Server configuration
    std::string server_address = "0.0.0.0";
    int server_port = 50051;
    int thread_pool_size = 10;

    // Monitoring REST listener (Phase A — health, readiness, /v1/status,
    // /v1/version, /metrics in Phase B). The trust boundary is the network
    // perimeter; no in-process auth or TLS on this port.
    bool        http_metrics_enabled = true;
    std::string http_metrics_addr = "0.0.0.0";
    int         http_metrics_port = 8081;
    bool        metrics_tenant_label = true;  // emit tenant label on metrics

    // Security configuration
    bool root_user_enabled = false;
    // When true, apply_default_acls grants OTHER->READ on every new resource,
    // making files/directories world-readable by default. Off by default so
    // freshly created resources are private to the creator.
    bool default_world_readable = false;

    // Sync configuration
    bool sync_enabled = true;
    int sync_retry_seconds = 60;
    bool sync_on_startup = true;
    bool sync_on_demand = true;
    std::string sync_pattern = "all";
    bool sync_bidirectional = true;

    // Secondary/local database for read-only operations when primary is unavailable
    std::string secondary_db_host;
    int secondary_db_port = 5432;
    std::string secondary_db_name = "fileengine_local";
    std::string secondary_db_user = "fileengine_user";
    std::string secondary_db_password = "fileengine_password";

    // Logging configuration
    std::string log_level = "INFO";
    std::string log_file_path = "/tmp/fileengine.log";
    bool log_to_console = true;
    bool log_to_file = false;
    size_t log_rotation_size_mb = 10;
    int log_retention_days = 7;

    // Event queueing (optional; see design_documents/redis_event_queueing_plan.md
    // and convert_search_ai/design_documents/EVENT_CONTRACT.md). Disabled by
    // default — existing deployments are unaffected until opted in. The Redis
    // connection honors the REDDIS_* keys already present in .env.
    bool        events_enabled = false;
    std::string events_redis_host = "localhost";
    int         events_redis_port = 6379;
    std::string events_redis_password;
    int         events_redis_db = 0;
    // Single stream shared by all tenants; the tenant is carried as an event
    // field and consumers are multi-tenant aware (EVENT_CONTRACT.md §5).
    std::string events_stream = "fileengine:events";
    long long   events_stream_maxlen = 100000;
    size_t      events_outbox_capacity = 10000;

    // Durable audit emission (usage_logging_and_auditing.md §5). Separate,
    // durable pipeline from the fail-open event stream above; shares the same
    // Redis broker (events_redis_*) but a different stream, drained by the
    // audit_service writer. WAL provides spool-ahead durability (§6).
    bool        audit_enabled = false;
    std::string audit_stream = "fileengine:audit";
    long long   audit_stream_maxlen = 1000000;
    std::string audit_wal_path = "audit.wal";
    // Access-log throughput valve (§6/§13). Default full-fidelity; "sample:N"
    // emits 1-in-N successful reads; "count[:K]" emits an aggregate every K.
    // Denied accesses are ALWAYS recorded in full regardless of the mode.
    std::string audit_access_mode = "full";
};

class ConfigLoader {
public:
    static Config load_config(int argc, char* argv[]);
    
private:
    static Config load_from_env();
    static Config load_from_file(const std::string& filepath);
    static Config load_from_cmd_args(int argc, char* argv[]);
    static std::map<std::string, std::string> parse_env_file(const std::string& filepath);
    static std::string get_env_var(const std::string& var, const std::string& default_val);
};

} // namespace fileengine

#endif // CONFIG_LOADER_H