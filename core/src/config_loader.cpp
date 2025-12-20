#include "config_loader.h"
#include <cstdlib>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace fileengine {

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

        // Remove leading/trailing whitespace
        line.erase(line.begin(), std::find_if(line.begin(), line.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
        line.erase(std::find_if(line.rbegin(), line.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), line.end());

        // Look for '=' separator
        size_t equal_pos = line.find('=');
        if (equal_pos != std::string::npos) {
            std::string key = line.substr(0, equal_pos);
            std::string value = line.substr(equal_pos + 1);

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

    // First, load from .env file if it exists
    auto env_vars = parse_env_file(filepath);

    // Apply loaded values to config (if they exist in the file)
    auto it = env_vars.find("FILEENGINE_DB_HOST");
    if (it != env_vars.end()) config.db_host = it->second;

    it = env_vars.find("FILEENGINE_DB_PORT");
    if (it != env_vars.end()) config.db_port = std::stoi(it->second);

    it = env_vars.find("FILEENGINE_DB_NAME");
    if (it != env_vars.end()) config.db_name = it->second;

    it = env_vars.find("FILEENGINE_DB_USER");
    if (it != env_vars.end()) config.db_user = it->second;

    it = env_vars.find("FILEENGINE_DB_PASSWORD");
    if (it != env_vars.end()) config.db_password = it->second;

    it = env_vars.find("FILEENGINE_STORAGE_BASE_PATH");
    if (it != env_vars.end()) config.storage_base_path = it->second;

    it = env_vars.find("FILEENGINE_ENCRYPT_DATA");
    if (it != env_vars.end()) config.encrypt_data = (it->second == "true" || it->second == "1");

    it = env_vars.find("FILEENGINE_COMPRESS_DATA");
    if (it != env_vars.end()) config.compress_data = (it->second == "true" || it->second == "1");

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
    if (it != env_vars.end()) config.s3_path_style = (it->second == "true" || it->second == "1");

    it = env_vars.find("FILEENGINE_CACHE_THRESHOLD");
    if (it != env_vars.end()) config.cache_threshold = std::stod(it->second);

    it = env_vars.find("FILEENGINE_MAX_CACHE_SIZE_MB");
    if (it != env_vars.end()) config.max_cache_size_mb = std::stoul(it->second);

    it = env_vars.find("FILEENGINE_MULTI_TENANT_ENABLED");
    if (it != env_vars.end()) config.multi_tenant_enabled = (it->second == "true" || it->second == "1");

    it = env_vars.find("FILEENGINE_ROOT_USER");
    if (it != env_vars.end()) config.root_user_enabled = (it->second == "true" || it->second == "1");

    it = env_vars.find("FILEENGINE_SYNC_ENABLED");
    if (it != env_vars.end()) config.sync_enabled = (it->second == "true" || it->second == "1");

    it = env_vars.find("FILEENGINE_SYNC_RETRY_SECONDS");
    if (it != env_vars.end()) config.sync_retry_seconds = std::stoi(it->second);

    it = env_vars.find("FILEENGINE_SYNC_ON_STARTUP");
    if (it != env_vars.end()) config.sync_on_startup = (it->second == "true" || it->second == "1");

    it = env_vars.find("FILEENGINE_SYNC_ON_DEMAND");
    if (it != env_vars.end()) config.sync_on_demand = (it->second == "true" || it->second == "1");

    it = env_vars.find("FILEENGINE_SYNC_PATTERN");
    if (it != env_vars.end()) config.sync_pattern = it->second;

    it = env_vars.find("FILEENGINE_SYNC_BIDIRECTIONAL");
    if (it != env_vars.end()) config.sync_bidirectional = (it->second == "true" || it->second == "1");

    // Logging configuration - based on the .env file
    it = env_vars.find("FILEENGINE_LOG_LEVEL");
    if (it != env_vars.end()) config.log_level = it->second;

    // Use the same file for both access and error logs, or default to main log file
    it = env_vars.find("FILEENGINE_LOG_ACCESS");
    if (it != env_vars.end()) config.log_file_path = it->second;

    // If both access and error logs are specified, we might want to handle them differently
    it = env_vars.find("FILEENGINE_LOG_ERROR");
    if (it != env_vars.end()) {
        // For now, we'll use the error log file as the main log file
        config.log_file_path = it->second;
    }

    // Default to file logging if any specific log file is set
    config.log_to_file = !config.log_file_path.empty() && config.log_file_path != "/var/log/fileengine.log";

    // Console logging can be disabled separately
    it = env_vars.find("FILEENGINE_LOG_TO_CONSOLE");
    if (it != env_vars.end()) config.log_to_console = (it->second == "true" || it->second == "1");

    it = env_vars.find("FILEENGINE_SERVER_ADDRESS");
    if (it != env_vars.end()) config.server_address = it->second;

    it = env_vars.find("FILEENGINE_SERVER_PORT");
    if (it != env_vars.end()) config.server_port = std::stoi(it->second);

    // Check for the HTTP thread pool variable as specified in the .env file
    it = env_vars.find("FILEENGINE_HTTP_THREAD_POOL");
    if (it != env_vars.end()) config.thread_pool_size = std::stoi(it->second);
    else {
        it = env_vars.find("FILEENGINE_THREAD_POOL_SIZE");
        if (it != env_vars.end()) config.thread_pool_size = std::stoi(it->second);
    }

    return config;
}

Config ConfigLoader::load_from_env() {
    Config config;

    // Load from environment variables, with defaults
    config.db_host = get_env_var("FILEENGINE_DB_HOST", config.db_host);
    config.db_port = std::stoi(get_env_var("FILEENGINE_DB_PORT", std::to_string(config.db_port)));
    config.db_name = get_env_var("FILEENGINE_DB_NAME", config.db_name);
    config.db_user = get_env_var("FILEENGINE_DB_USER", config.db_user);
    config.db_password = get_env_var("FILEENGINE_DB_PASSWORD", config.db_password);
    config.storage_base_path = get_env_var("FILEENGINE_STORAGE_BASE_PATH", config.storage_base_path);
    config.encrypt_data = (get_env_var("FILEENGINE_ENCRYPT_DATA", config.encrypt_data ? "true" : "false") == "true");
    config.compress_data = (get_env_var("FILEENGINE_COMPRESS_DATA", config.compress_data ? "true" : "false") == "true");
    config.s3_endpoint = get_env_var("FILEENGINE_S3_ENDPOINT", config.s3_endpoint);
    config.s3_region = get_env_var("FILEENGINE_S3_REGION", config.s3_region);
    config.s3_bucket = get_env_var("FILEENGINE_S3_BUCKET", config.s3_bucket);
    config.s3_access_key = get_env_var("FILEENGINE_S3_ACCESS_KEY", config.s3_access_key);
    config.s3_secret_key = get_env_var("FILEENGINE_S3_SECRET_KEY", config.s3_secret_key);
    config.s3_path_style = (get_env_var("FILEENGINE_S3_PATH_STYLE", config.s3_path_style ? "true" : "false") == "true");
    config.cache_threshold = std::stod(get_env_var("FILEENGINE_CACHE_THRESHOLD", std::to_string(config.cache_threshold)));
    config.max_cache_size_mb = std::stoul(get_env_var("FILEENGINE_MAX_CACHE_SIZE_MB", std::to_string(config.max_cache_size_mb)));
    config.multi_tenant_enabled = (get_env_var("FILEENGINE_MULTI_TENANT_ENABLED", config.multi_tenant_enabled ? "true" : "false") == "true");
    config.root_user_enabled = (get_env_var("FILEENGINE_ROOT_USER", config.root_user_enabled ? "true" : "false") == "true");
    config.server_address = get_env_var("FILEENGINE_SERVER_ADDRESS", config.server_address);
    config.server_port = std::stoi(get_env_var("FILEENGINE_SERVER_PORT", std::to_string(config.server_port)));
    config.thread_pool_size = std::stoi(get_env_var("FILEENGINE_HTTP_THREAD_POOL", std::to_string(config.thread_pool_size)));

    // Sync configuration
    config.sync_enabled = (get_env_var("FILEENGINE_SYNC_ENABLED", config.sync_enabled ? "true" : "false") == "true");
    config.sync_retry_seconds = std::stoi(get_env_var("FILEENGINE_SYNC_RETRY_SECONDS", std::to_string(config.sync_retry_seconds)));
    config.sync_on_startup = (get_env_var("FILEENGINE_SYNC_ON_STARTUP", config.sync_on_startup ? "true" : "false") == "true");
    config.sync_on_demand = (get_env_var("FILEENGINE_SYNC_ON_DEMAND", config.sync_on_demand ? "true" : "false") == "true");
    config.sync_pattern = get_env_var("FILEENGINE_SYNC_PATTERN", config.sync_pattern);
    config.sync_bidirectional = (get_env_var("FILEENGINE_SYNC_BIDIRECTIONAL", config.sync_bidirectional ? "true" : "false") == "true");

    return config;
}

Config ConfigLoader::load_from_cmd_args(int argc, char* argv[]) {
    Config config;
    int opt;
    std::string config_file_path;

    // Parse command-line options
    optind = 1;  // Reset optind to 1 to start fresh
    while ((opt = getopt(argc, argv, "c:h:p:n:u:P:d:s:e:r:b:a:k:x:T:v:m:")) != -1) {
        switch (opt) {
            case 'c':  // config file
                config_file_path = optarg;
                break;
            case 'h':
                config.db_host = optarg;
                break;
            case 'p':
                config.db_port = std::stoi(optarg);
                break;
            case 'n':
                config.db_name = optarg;
                break;
            case 'u':
                config.db_user = optarg;
                break;
            case 'P':
                config.db_password = optarg;
                break;
            case 'd':
                config.storage_base_path = optarg;
                break;
            case 's':
                config.s3_endpoint = optarg;
                break;
            case 'r':
                config.s3_region = optarg;
                break;
            case 'b':
                config.s3_bucket = optarg;
                break;
            case 'a':
                config.s3_access_key = optarg;
                break;
            case 'k':
                config.s3_secret_key = optarg;
                break;
            case 'x':
                config.server_address = optarg;
                break;
            case 'T':
                config.server_port = std::stoi(optarg);
                break;
            case 'v':
                config.cache_threshold = std::stod(optarg);
                break;
            case 'm':
                config.max_cache_size_mb = std::stoul(optarg);
                break;
            case '?':
                // Unknown option - just continue
                break;
        }
    }

    // If a config file was specified, load it (after parsing command line in a real impl)
    if (!config_file_path.empty()) {
        Config file_config = load_from_file(config_file_path);
        // Values from command line take precedence over file values
        // For this simplified implementation, we'll only apply file values if command line didn't set them
        if (config.db_host == "localhost") config.db_host = file_config.db_host;
        if (config.db_port == 5432) config.db_port = file_config.db_port;
        if (config.db_name == "fileengine") config.db_name = file_config.db_name;
        if (config.db_user == "fileengine_user") config.db_user = file_config.db_user;
        if (config.db_password == "fileengine_password") config.db_password = file_config.db_password;
        if (config.storage_base_path == "/tmp/fileengine_storage") config.storage_base_path = file_config.storage_base_path;
        if (config.s3_endpoint == "http://localhost:9000") config.s3_endpoint = file_config.s3_endpoint;
        if (config.s3_region == "us-east-1") config.s3_region = file_config.s3_region;
        if (config.s3_bucket == "fileengine") config.s3_bucket = file_config.s3_bucket;
        if (config.s3_access_key.empty()) config.s3_access_key = file_config.s3_access_key;
        if (config.s3_secret_key.empty()) config.s3_secret_key = file_config.s3_secret_key;
        if (config.server_address == "0.0.0.0") config.server_address = file_config.server_address;
        if (config.server_port == 50051) config.server_port = file_config.server_port;
    }

    // Reset optind for any other argument parsing
    optind = 1;

    return config;
}

Config ConfigLoader::load_config(int argc, char* argv[]) {
    // Start with default configuration
    Config config;

    // 1. Load from default .env file in current directory
    try {
        Config file_config = load_from_file(".env");
        // Override default config with file config
        config = file_config;  // This copies all values from file config
    } catch (...) {
        // If .env file doesn't exist or has errors, continue with defaults
        // This is expected behavior
    }

    // 2. Load from environment variables (override file config)
    Config env_config = load_from_env();
    // Only override values that are set in environment
    if (getenv("FILEENGINE_DB_HOST")) config.db_host = env_config.db_host;
    if (getenv("FILEENGINE_DB_PORT")) config.db_port = std::stoi(get_env_var("FILEENGINE_DB_PORT", std::to_string(config.db_port)));
    if (getenv("FILEENGINE_DB_NAME")) config.db_name = env_config.db_name;
    if (getenv("FILEENGINE_DB_USER")) config.db_user = env_config.db_user;
    if (getenv("FILEENGINE_DB_PASSWORD")) config.db_password = env_config.db_password;
    if (getenv("FILEENGINE_STORAGE_BASE_PATH")) config.storage_base_path = env_config.storage_base_path;
    if (getenv("FILEENGINE_ENCRYPT_DATA")) config.encrypt_data = (get_env_var("FILEENGINE_ENCRYPT_DATA", "false") == "true");
    if (getenv("FILEENGINE_COMPRESS_DATA")) config.compress_data = (get_env_var("FILEENGINE_COMPRESS_DATA", "false") == "true");
    if (getenv("FILEENGINE_S3_ENDPOINT")) config.s3_endpoint = env_config.s3_endpoint;
    if (getenv("FILEENGINE_S3_REGION")) config.s3_region = env_config.s3_region;
    if (getenv("FILEENGINE_S3_BUCKET")) config.s3_bucket = env_config.s3_bucket;
    if (getenv("FILEENGINE_S3_ACCESS_KEY")) config.s3_access_key = env_config.s3_access_key;
    if (getenv("FILEENGINE_S3_SECRET_KEY")) config.s3_secret_key = env_config.s3_secret_key;
    if (getenv("FILEENGINE_S3_PATH_STYLE")) config.s3_path_style = (get_env_var("FILEENGINE_S3_PATH_STYLE", "false") == "true");
    if (getenv("FILEENGINE_CACHE_THRESHOLD")) config.cache_threshold = std::stod(get_env_var("FILEENGINE_CACHE_THRESHOLD", std::to_string(config.cache_threshold)));
    if (getenv("FILEENGINE_MAX_CACHE_SIZE_MB")) config.max_cache_size_mb = std::stoul(get_env_var("FILEENGINE_MAX_CACHE_SIZE_MB", std::to_string(config.max_cache_size_mb)));
    if (getenv("FILEENGINE_MULTI_TENANT_ENABLED")) config.multi_tenant_enabled = (get_env_var("FILEENGINE_MULTI_TENANT_ENABLED", "true") == "true");
    if (getenv("FILEENGINE_ROOT_USER")) config.root_user_enabled = (get_env_var("FILEENGINE_ROOT_USER", config.root_user_enabled ? "true" : "false") == "true");
    // Sync configuration
    if (getenv("FILEENGINE_SYNC_ENABLED")) config.sync_enabled = (get_env_var("FILEENGINE_SYNC_ENABLED", "true") == "true");
    if (getenv("FILEENGINE_SYNC_RETRY_SECONDS")) config.sync_retry_seconds = std::stoi(get_env_var("FILEENGINE_SYNC_RETRY_SECONDS", std::to_string(config.sync_retry_seconds)));
    if (getenv("FILEENGINE_SYNC_ON_STARTUP")) config.sync_on_startup = (get_env_var("FILEENGINE_SYNC_ON_STARTUP", "true") == "true");
    if (getenv("FILEENGINE_SYNC_ON_DEMAND")) config.sync_on_demand = (get_env_var("FILEENGINE_SYNC_ON_DEMAND", "true") == "true");
    if (getenv("FILEENGINE_SYNC_PATTERN")) config.sync_pattern = get_env_var("FILEENGINE_SYNC_PATTERN", config.sync_pattern);
    if (getenv("FILEENGINE_SYNC_BIDIRECTIONAL")) config.sync_bidirectional = (get_env_var("FILEENGINE_SYNC_BIDIRECTIONAL", "true") == "true");
    if (getenv("FILEENGINE_LOG_LEVEL")) config.log_level = get_env_var("FILEENGINE_LOG_LEVEL", config.log_level);
    if (getenv("FILEENGINE_LOG_ACCESS")) config.log_file_path = get_env_var("FILEENGINE_LOG_ACCESS", config.log_file_path);
    if (getenv("FILEENGINE_LOG_ERROR")) {
        // If both access and error logs are specified, error takes precedence for the main log file
        config.log_file_path = get_env_var("FILEENGINE_LOG_ERROR", config.log_file_path);
        config.log_to_file = true;  // Automatically enable file logging if error log is specified
    }
    if (getenv("FILEENGINE_LOG_TO_CONSOLE")) config.log_to_console = (get_env_var("FILEENGINE_LOG_TO_CONSOLE", config.log_to_console ? "true" : "false") == "true");
    if (getenv("FILEENGINE_SERVER_ADDRESS")) config.server_address = env_config.server_address;
    if (getenv("FILEENGINE_SERVER_PORT")) config.server_port = std::stoi(get_env_var("FILEENGINE_SERVER_PORT", std::to_string(config.server_port)));
    if (getenv("FILEENGINE_HTTP_THREAD_POOL")) config.thread_pool_size = std::stoi(get_env_var("FILEENGINE_HTTP_THREAD_POOL", std::to_string(config.thread_pool_size)));

    // 3. Load from command-line arguments (override env and file configs)
    Config cmd_config = load_from_cmd_args(argc, argv);
    // For command-line args, we only override if they're legitimately different from defaults
    if (cmd_config.db_host != config.db_host) config.db_host = cmd_config.db_host;
    if (cmd_config.db_port != config.db_port) config.db_port = cmd_config.db_port;
    if (cmd_config.db_name != config.db_name) config.db_name = cmd_config.db_name;
    if (cmd_config.db_user != config.db_user) config.db_user = cmd_config.db_user;
    if (cmd_config.db_password != config.db_password) config.db_password = cmd_config.db_password;
    if (cmd_config.storage_base_path != config.storage_base_path) config.storage_base_path = cmd_config.storage_base_path;
    if (cmd_config.s3_endpoint != config.s3_endpoint) config.s3_endpoint = cmd_config.s3_endpoint;
    if (cmd_config.s3_region != config.s3_region) config.s3_region = cmd_config.s3_region;
    if (cmd_config.s3_bucket != config.s3_bucket) config.s3_bucket = cmd_config.s3_bucket;
    if (cmd_config.s3_access_key != config.s3_access_key) config.s3_access_key = cmd_config.s3_access_key;
    if (cmd_config.s3_secret_key != config.s3_secret_key) config.s3_secret_key = cmd_config.s3_secret_key;
    if (cmd_config.server_address != config.server_address) config.server_address = cmd_config.server_address;
    if (cmd_config.server_port != config.server_port) config.server_port = cmd_config.server_port;

    return config;
}

} // namespace fileengine