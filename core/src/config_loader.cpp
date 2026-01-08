#include "config_loader.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <map>
#include <memory>
#include <cstdlib>
#include <unistd.h>
#include <algorithm>
#include <cctype>

namespace fileengine {

// Loads FILEENGINE_LOG_LEVEL and validates conventional log-levels for the logging subsystem

std::string ConfigLoader::get_env_var(const std::string& var, const std::string& default_val) {
    const char* env_val = std::getenv(var.c_str());
    return env_val ? std::string(env_val) : default_val;
}

std::map<std::string, std::string> ConfigLoader::parse_env_file(const std::string& filepath) {
    std::map<std::string, std::string> env_vars;
    std::ifstream file(filepath);

    if (!file.is_open()) {
        return env_vars;  // Return empty map if file can't be opened
    }

    std::string line;
    while (std::getline(file, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        // Find the first '=' to split key=value
        size_t pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);

            // Trim whitespace
            key.erase(0, key.find_first_not_of(" \t\r\n"));
            key.erase(key.find_last_not_of(" \t\r\n") + 1);
            value.erase(0, value.find_first_not_of(" \t\r\n"));
            value.erase(value.find_last_not_of(" \t\r\n") + 1);

            // Remove quotes if present
            if (value.length() >= 2 && 
                ((value.front() == '"' && value.back() == '"') || 
                 (value.front() == '\'' && value.back() == '\''))) {
                value = value.substr(1, value.length() - 2);
            }

            env_vars[key] = value;
        }
    }

    return env_vars;
}

Config ConfigLoader::load_from_file(const std::string& filepath) {
    Config config;
    auto env_vars = parse_env_file(filepath);
    auto it = env_vars.end();

    // Database configuration
    it = env_vars.find("FILEENGINE_PG_HOST");
    if (it != env_vars.end()) config.db_host = it->second;
    
    it = env_vars.find("FILEENGINE_PG_PORT");
    if (it != env_vars.end()) config.db_port = std::stoi(it->second);
    
    it = env_vars.find("FILEENGINE_PG_DATABASE");
    if (it != env_vars.end()) config.db_name = it->second;
    
    it = env_vars.find("FILEENGINE_PG_USER");
    if (it != env_vars.end()) config.db_user = it->second;
    
    it = env_vars.find("FILEENGINE_PG_PASSWORD");
    if (it != env_vars.end()) config.db_password = it->second;

    // Storage configuration
    it = env_vars.find("FILEENGINE_STORAGE_BASE");
    if (it != env_vars.end()) config.storage_base_path = it->second;

    // Compression and encryption settings
    it = env_vars.find("FILEENGINE_ENCRYPT_DATA");
    if (it != env_vars.end()) config.encrypt_data = (it->second == "true" || it->second == "TRUE" || it->second == "1");

    it = env_vars.find("FILEENGINE_COMPRESS_DATA");
    if (it != env_vars.end()) config.compress_data = (it->second == "true" || it->second == "TRUE" || it->second == "1");

    it = env_vars.find("AT_REST_KEY");
    if (it != env_vars.end()) config.encryption_key = it->second;

    // S3/MinIO configuration
    it = env_vars.find("FILEENGINE_S3_ENDPOINT");
    if (it != env_vars.end()) config.s3_endpoint = it->second;
    
    it = env_vars.find("FILEENGINE_S3_REGION");
    if (it != env_vars.end()) config.s3_region = it->second;
    
    it = env_vars.find("FILEENGINE_S3_BUCKET");
    if (it != env_vars.end()) config.s3_bucket = it->second;
    
    it = env_vars.find("FILEENGINE_S3_ACCESS_KEY");
    if (it != env_vars.end()) config.s3_access_key = it->second;
    
    it = env_vars.find("FILEENGINE_S3_SECRET_KEY");
    if (it != env_vars.end()) config.s3_secret_key = it->second;
    
    it = env_vars.find("FILEENGINE_S3_PATH_STYLE");
    if (it != env_vars.end()) config.s3_path_style = (it->second == "true" || it->second == "TRUE" || it->second == "1");

    // Cache configuration
    it = env_vars.find("FILEENGINE_CACHE_THRESHOLD");
    if (it != env_vars.end()) config.cache_threshold = std::stod(it->second);
    
    it = env_vars.find("FILEENGINE_MAX_CACHE_SIZE_MB");
    if (it != env_vars.end()) config.max_cache_size_mb = std::stoul(it->second);
    
    it = env_vars.find("FILEENGINE_MULTI_TENANT_ENABLED");
    if (it != env_vars.end()) config.multi_tenant_enabled = (it->second == "true" || it->second == "1");

    // Server configuration - Changed to match .env file
    it = env_vars.find("FILEENGINE_GRPC_HOST");
    if (it != env_vars.end()) config.server_address = it->second;

    it = env_vars.find("FILEENGINE_GRPC_PORT");
    if (it != env_vars.end()) config.server_port = std::stoi(it->second);
    
    it = env_vars.find("FILEENGINE_HTTP_THREAD_POOL");
    if (it != env_vars.end()) config.thread_pool_size = std::stoi(it->second);

    // Security configuration
    it = env_vars.find("FILEENGINE_ROOT_USER");
    if (it != env_vars.end()) config.root_user_enabled = (it->second == "true" || it->second == "1");

    // Sync configuration
    it = env_vars.find("FILEENGINE_S3_SYNC_SUPPORT");
    if (it != env_vars.end()) config.sync_enabled = (it->second == "true" || it->second == "minio" || it->second == "s3");
    
    it = env_vars.find("FILEENGINE_S3_RETRY_SECONDS");
    if (it != env_vars.end()) config.sync_retry_seconds = std::stoi(it->second);

    // Logging configuration
    if (env_vars.count("FILEENGINE_LOG_LEVEL")) config.log_level = env_vars.at("FILEENGINE_LOG_LEVEL");
    if (env_vars.count("FILEENGINE_LOG_FILE_PATH")) config.log_file_path = env_vars.at("FILEENGINE_LOG_FILE_PATH");
    if (env_vars.count("FILEENGINE_LOG_TO_CONSOLE")) config.log_to_console = (env_vars.at("FILEENGINE_LOG_TO_CONSOLE") == "true");
    if (env_vars.count("FILEENGINE_LOG_TO_FILE")) config.log_to_file = (env_vars.at("FILEENGINE_LOG_TO_FILE") == "true");
    if (env_vars.count("FILEENGINE_LOG_ROTATION_SIZE_MB")) config.log_rotation_size_mb = std::stoul(env_vars.at("FILEENGINE_LOG_ROTATION_SIZE_MB"));
    if (env_vars.count("FILEENGINE_LOG_RETENTION_DAYS")) config.log_retention_days = std::stoi(env_vars.at("FILEENGINE_LOG_RETENTION_DAYS"));

    return config;
}

Config ConfigLoader::load_from_env() {
    Config config;

    // Load from environment variables - use special indicator for unset values
    std::string env_value;

    env_value = get_env_var("FILEENGINE_PG_HOST", ""); // Empty string means not set
    if (!env_value.empty()) config.db_host = env_value;

    env_value = get_env_var("FILEENGINE_PG_PORT", "");
    if (!env_value.empty()) config.db_port = std::stoi(env_value);

    env_value = get_env_var("FILEENGINE_PG_DATABASE", "");
    if (!env_value.empty()) config.db_name = env_value;

    env_value = get_env_var("FILEENGINE_PG_USER", "");
    if (!env_value.empty()) config.db_user = env_value;

    env_value = get_env_var("FILEENGINE_PG_PASSWORD", "");
    if (!env_value.empty()) config.db_password = env_value;

    env_value = get_env_var("FILEENGINE_STORAGE_BASE", "");
    if (!env_value.empty()) config.storage_base_path = env_value;

    env_value = get_env_var("FILEENGINE_ENCRYPT_DATA", "");
    if (!env_value.empty()) config.encrypt_data = (env_value == "true" || env_value == "TRUE" || env_value == "1");

    env_value = get_env_var("FILEENGINE_COMPRESS_DATA", "");
    if (!env_value.empty()) config.compress_data = (env_value == "true" || env_value == "TRUE" || env_value == "1");

    env_value = get_env_var("AT_REST_KEY", "");
    if (!env_value.empty()) config.encryption_key = env_value;

    env_value = get_env_var("FILEENGINE_S3_ENDPOINT", "");
    if (!env_value.empty()) config.s3_endpoint = env_value;

    env_value = get_env_var("FILEENGINE_S3_REGION", "");
    if (!env_value.empty()) config.s3_region = env_value;

    env_value = get_env_var("FILEENGINE_S3_BUCKET", "");
    if (!env_value.empty()) config.s3_bucket = env_value;

    env_value = get_env_var("FILEENGINE_S3_ACCESS_KEY", "");
    if (!env_value.empty()) config.s3_access_key = env_value;

    env_value = get_env_var("FILEENGINE_S3_SECRET_KEY", "");
    if (!env_value.empty()) config.s3_secret_key = env_value;

    env_value = get_env_var("FILEENGINE_S3_PATH_STYLE", "");
    if (!env_value.empty()) config.s3_path_style = (env_value == "true" || env_value == "TRUE" || env_value == "1");

    env_value = get_env_var("FILEENGINE_CACHE_THRESHOLD", "");
    if (!env_value.empty()) config.cache_threshold = std::stod(env_value);

    env_value = get_env_var("FILEENGINE_MAX_CACHE_SIZE_MB", "");
    if (!env_value.empty()) config.max_cache_size_mb = std::stoul(env_value);

    env_value = get_env_var("FILEENGINE_MULTI_TENANT_ENABLED", "");
    if (!env_value.empty()) config.multi_tenant_enabled = (env_value == "true" || env_value == "1");

    // Server configuration - Changed to match .env file
    env_value = get_env_var("FILEENGINE_GRPC_HOST", "");
    if (!env_value.empty()) config.server_address = env_value;

    env_value = get_env_var("FILEENGINE_GRPC_PORT", "");
    if (!env_value.empty()) config.server_port = std::stoi(env_value);

    env_value = get_env_var("FILEENGINE_HTTP_THREAD_POOL", "");
    if (!env_value.empty()) config.thread_pool_size = std::stoi(env_value);

    // Security configuration
    env_value = get_env_var("FILEENGINE_ROOT_USER", "");
    if (!env_value.empty()) config.root_user_enabled = (env_value == "true" || env_value == "1");

    // Sync configuration
    env_value = get_env_var("FILEENGINE_S3_SYNC_SUPPORT", "");
    if (!env_value.empty()) config.sync_enabled = (env_value == "true" || env_value == "minio" || env_value == "s3");

    env_value = get_env_var("FILEENGINE_S3_RETRY_SECONDS", "");
    if (!env_value.empty()) config.sync_retry_seconds = std::stoi(env_value);

    env_value = get_env_var("FILEENGINE_S3_SYNC_ON_STARTUP", "");
    if (!env_value.empty()) config.sync_on_startup = (env_value == "true");

    env_value = get_env_var("FILEENGINE_S3_SYNC_ON_DEMAND", "");
    if (!env_value.empty()) config.sync_on_demand = (env_value == "true");

    env_value = get_env_var("FILEENGINE_S3_SYNC_PATTERN", "");
    if (!env_value.empty()) config.sync_pattern = env_value;

    env_value = get_env_var("FILEENGINE_S3_SYNC_BIDIRECTIONAL", "");
    if (!env_value.empty()) config.sync_bidirectional = (env_value == "true");

    // Logging configuration
    env_value = get_env_var("FILEENGINE_LOG_LEVEL", "");
    if (!env_value.empty()) config.log_level = env_value;

    env_value = get_env_var("FILEENGINE_LOG_FILE_PATH", "");
    if (!env_value.empty()) config.log_file_path = env_value;

    env_value = get_env_var("FILEENGINE_LOG_TO_CONSOLE", "");
    if (!env_value.empty()) config.log_to_console = (env_value == "true");

    env_value = get_env_var("FILEENGINE_LOG_TO_FILE", "");
    if (!env_value.empty()) config.log_to_file = (env_value == "true");

    env_value = get_env_var("FILEENGINE_LOG_ROTATION_SIZE_MB", "");
    if (!env_value.empty()) config.log_rotation_size_mb = std::stoul(env_value);

    env_value = get_env_var("FILEENGINE_LOG_RETENTION_DAYS", "");
    if (!env_value.empty()) config.log_retention_days = std::stoi(env_value);

    return config;
}

Config ConfigLoader::load_from_cmd_args(int argc, char* argv[]) {
    Config config;

    // Set all values to designated "unset" state initially
    config.db_host.clear();
    config.db_port = -1;  // Use -1 as indicator for unset integer values
    config.db_name.clear();
    config.db_user.clear();
    config.db_password.clear();
    config.storage_base_path.clear();
    config.s3_endpoint.clear();
    config.s3_region.clear();
    config.s3_bucket.clear();
    config.s3_access_key.clear();
    config.s3_secret_key.clear();
    config.server_address.clear();
    config.server_port = -1;
    config.thread_pool_size = -1;

    // Parse command-line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            // Config file is handled separately in load_config
            ++i;
        } else if (arg == "--db-host" && i + 1 < argc) {
            config.db_host = argv[++i];
        } else if (arg == "--db-port" && i + 1 < argc) {
            config.db_port = std::stoi(argv[++i]);
        } else if (arg == "--db-name" && i + 1 < argc) {
            config.db_name = argv[++i];
        } else if (arg == "--db-user" && i + 1 < argc) {
            config.db_user = argv[++i];
        } else if (arg == "--db-password" && i + 1 < argc) {
            config.db_password = argv[++i];
        } else if (arg == "--storage-path" && i + 1 < argc) {
            config.storage_base_path = argv[++i];
        } else if (arg == "--s3-endpoint" && i + 1 < argc) {
            config.s3_endpoint = argv[++i];
        } else if (arg == "--s3-region" && i + 1 < argc) {
            config.s3_region = argv[++i];
        } else if (arg == "--s3-bucket" && i + 1 < argc) {
            config.s3_bucket = argv[++i];
        } else if (arg == "--s3-access-key" && i + 1 < argc) {
            config.s3_access_key = argv[++i];
        } else if (arg == "--s3-secret-key" && i + 1 < argc) {
            config.s3_secret_key = argv[++i];
        } else if (arg == "--listen-addr" && i + 1 < argc) {
            config.server_address = argv[++i];
        } else if (arg == "--listen-port" && i + 1 < argc) {
            config.server_port = std::stoi(argv[++i]);
        } else if (arg == "--thread-pool-size" && i + 1 < argc) {
            config.thread_pool_size = std::stoi(argv[++i]);
        }
    }

    return config;
}

Config ConfigLoader::load_config(int argc, char* argv[]) {
    // 1. Load from file (lowest priority)
    std::string config_file = ".env";
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_file = argv[++i];
        }
    }
    Config config = load_from_file(config_file.c_str());

    // 2. Load from environment (medium priority)
    Config env_config = load_from_env();
    if (!env_config.db_host.empty() && env_config.db_host != "localhost") config.db_host = env_config.db_host;
    if (env_config.db_port != 5432) config.db_port = env_config.db_port;
    if (!env_config.db_name.empty() && env_config.db_name != "fileengine") config.db_name = env_config.db_name;
    if (!env_config.db_user.empty() && env_config.db_user != "fileengine_user") config.db_user = env_config.db_user;
    if (!env_config.db_password.empty() && env_config.db_password != "fileengine_password") config.db_password = env_config.db_password;
    if (!env_config.storage_base_path.empty() && env_config.storage_base_path != "/tmp/fileengine_storage") config.storage_base_path = env_config.storage_base_path;
    if (env_config.encrypt_data) config.encrypt_data = env_config.encrypt_data;
    if (env_config.compress_data) config.compress_data = env_config.compress_data;
    if (!env_config.encryption_key.empty()) config.encryption_key = env_config.encryption_key;
    if (!env_config.s3_endpoint.empty() && env_config.s3_endpoint != "http://localhost:9000") config.s3_endpoint = env_config.s3_endpoint;
    if (!env_config.s3_region.empty() && env_config.s3_region != "us-east-1") config.s3_region = env_config.s3_region;
    if (!env_config.s3_bucket.empty() && env_config.s3_bucket != "fileengine") config.s3_bucket = env_config.s3_bucket;
    if (!env_config.s3_access_key.empty() && env_config.s3_access_key != "minioadmin") config.s3_access_key = env_config.s3_access_key;
    if (!env_config.s3_secret_key.empty() && env_config.s3_secret_key != "minioadmin") config.s3_secret_key = env_config.s3_secret_key;
    if (!env_config.s3_path_style) config.s3_path_style = env_config.s3_path_style;
    if (env_config.cache_threshold != 0.8) config.cache_threshold = env_config.cache_threshold;
    if (env_config.max_cache_size_mb != 1024) config.max_cache_size_mb = env_config.max_cache_size_mb;
    if (!env_config.multi_tenant_enabled) config.multi_tenant_enabled = env_config.multi_tenant_enabled;
    if (!env_config.server_address.empty() && env_config.server_address != "0.0.0.0") config.server_address = env_config.server_address;
    if (env_config.server_port != 50051) config.server_port = env_config.server_port;
    if (env_config.thread_pool_size != 10) config.thread_pool_size = env_config.thread_pool_size;
    if (env_config.root_user_enabled) config.root_user_enabled = env_config.root_user_enabled;
    if (!env_config.sync_enabled) config.sync_enabled = env_config.sync_enabled;
    if (env_config.sync_retry_seconds != 60) config.sync_retry_seconds = env_config.sync_retry_seconds;
    if (!env_config.sync_on_startup) config.sync_on_startup = env_config.sync_on_startup;
    if (!env_config.sync_on_demand) config.sync_on_demand = env_config.sync_on_demand;
    if (!env_config.sync_pattern.empty() && env_config.sync_pattern != "all") config.sync_pattern = env_config.sync_pattern;
    if (!env_config.sync_bidirectional) config.sync_bidirectional = env_config.sync_bidirectional;
    if (!env_config.secondary_db_host.empty()) config.secondary_db_host = env_config.secondary_db_host;
    if (env_config.secondary_db_port != 5432) config.secondary_db_port = env_config.secondary_db_port;
    if (!env_config.secondary_db_name.empty() && env_config.secondary_db_name != "fileengine_local") config.secondary_db_name = env_config.secondary_db_name;
    if (!env_config.secondary_db_user.empty() && env_config.secondary_db_user != "fileengine_user") config.secondary_db_user = env_config.secondary_db_user;
    if (!env_config.secondary_db_password.empty() && env_config.secondary_db_password != "fileengine_password") config.secondary_db_password = env_config.secondary_db_password;
    if (env_config.log_level != "INFO") config.log_level = env_config.log_level;
    if (env_config.log_file_path != "/tmp/fileengine.log") config.log_file_path = env_config.log_file_path;
    if (!env_config.log_to_console) config.log_to_console = env_config.log_to_console;
    if (env_config.log_to_file) config.log_to_file = env_config.log_to_file;
    if (env_config.log_rotation_size_mb != 10) config.log_rotation_size_mb = env_config.log_rotation_size_mb;
    if (env_config.log_retention_days != 7) config.log_retention_days = env_config.log_retention_days;

    // 3. Load from command-line arguments (highest priority)
    Config cmd_config = load_from_cmd_args(argc, argv);
    if (!cmd_config.db_host.empty()) config.db_host = cmd_config.db_host;
    if (cmd_config.db_port != -1) config.db_port = cmd_config.db_port;
    if (!cmd_config.db_name.empty()) config.db_name = cmd_config.db_name;
    if (!cmd_config.db_user.empty()) config.db_user = cmd_config.db_user;
    if (!cmd_config.db_password.empty()) config.db_password = cmd_config.db_password;
    if (!cmd_config.storage_base_path.empty()) config.storage_base_path = cmd_config.storage_base_path;
    if (!cmd_config.s3_endpoint.empty()) config.s3_endpoint = cmd_config.s3_endpoint;
    if (!cmd_config.s3_region.empty()) config.s3_region = cmd_config.s3_region;
    if (!cmd_config.s3_bucket.empty()) config.s3_bucket = cmd_config.s3_bucket;
    if (!cmd_config.s3_access_key.empty()) config.s3_access_key = cmd_config.s3_access_key;
    if (!cmd_config.s3_secret_key.empty()) config.s3_secret_key = cmd_config.s3_secret_key;
    if (!cmd_config.server_address.empty()) config.server_address = cmd_config.server_address;
    if (cmd_config.server_port != -1) config.server_port = cmd_config.server_port;
    if (cmd_config.thread_pool_size != -1) config.thread_pool_size = cmd_config.thread_pool_size;

    return config;
}

} // namespace fileengine