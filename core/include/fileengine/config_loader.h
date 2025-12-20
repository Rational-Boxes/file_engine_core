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

    // Security configuration
    bool root_user_enabled = false;

    // Sync configuration
    bool sync_enabled = true;
    int sync_retry_seconds = 60;
    bool sync_on_startup = true;
    bool sync_on_demand = true;
    std::string sync_pattern = "all";
    bool sync_bidirectional = true;

    // Logging configuration
    std::string log_level = "INFO";
    std::string log_file_path = "/var/log/fileengine.log";
    bool log_to_console = true;
    bool log_to_file = false;
    size_t log_rotation_size_mb = 10;
    int log_retention_days = 7;
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