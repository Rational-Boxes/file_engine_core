#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <signal.h>
#include <csignal>
#include <cstdlib>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>

#include "fileengine/filesystem.h"
#include "fileengine/database.h"
#include "fileengine/storage.h"
#include "fileengine/s3_storage.h"
#include "fileengine/tenant_manager.h"
#include "fileengine/acl_manager.h"
#include "fileengine/cache_manager.h"
#include "fileengine/utils.h"
#include "fileengine/object_store_sync.h"
#include "fileengine/grpc_service.h"
#include "fileengine/config_loader.h"
#include "fileengine/storage_tracker.h"
#include "fileengine/file_culler.h"
#include "fileengine/server_logger.h"

// Generated gRPC files are included here
#include "fileservice.grpc.pb.h"

namespace fileengine {

// Global variable for signal handling
volatile sig_atomic_t signal_received = 0;

void signal_handler(int signal) {
    signal_received = signal;
}

} // namespace fileengine

int main(int argc, char** argv) {
    std::cout << "Starting FileEngine gRPC Server..." << std::endl;

    // Load configuration from environment or config file
    fileengine::Config config = fileengine::ConfigLoader::load_config(argc, argv);

    // Initialize logging system
    // fileengine::ServerLogger::getInstance().initialize(
    //     config.log_level,
    //     config.log_file_path,
    //     config.log_to_console,
    //     config.log_to_file,
    //     config.log_rotation_size_mb,
    //     config.log_retention_days
    // );
    //
    // fileengine::ServerLogger::getInstance().info("Server", "Logger initialized with level: " + config.log_level);

    std::cout << "Config loaded:" << std::endl;
    std::cout << "  DB Host: " << config.db_host << std::endl;
    std::cout << "  DB Port: " << config.db_port << std::endl;
    std::cout << "  DB Name: " << config.db_name << std::endl;
    std::cout << "  Storage Path: " << config.storage_base_path << std::endl;
    std::cout << "  S3 Endpoint: " << config.s3_endpoint << std::endl;
    std::cout << "  Log Level: " << config.log_level << std::endl;
    std::cout << "  Log File: " << config.log_file_path << std::endl;
    std::cout << "  Log to Console: " << (config.log_to_console ? "true" : "false") << std::endl;

    // Connect to the database
    std::cout << "Connecting to database..." << std::endl;
    auto database = std::make_shared<fileengine::Database>(config.db_host, config.db_port, config.db_name,
                                                         config.db_user, config.db_password,
                                                         config.thread_pool_size); // Use configured thread pool size
    if (!database->connect()) {
        std::cerr << "Failed to connect to database" << std::endl;
        return -1;
    }

    std::cout << "Database connected successfully." << std::endl;

    // Configure secondary/local database if specified in the config
    if (!config.secondary_db_host.empty()) {
        database->configure_secondary_connection(config.secondary_db_host, config.secondary_db_port,
                                                 config.secondary_db_name, config.secondary_db_user,
                                                 config.secondary_db_password);
        std::cout << "Secondary database configured for failover." << std::endl;
    }

    // Start monitoring to detect database connection failures and attempt reconnection
    database->start_connection_monitoring();
    std::cout << "Database connection monitoring started." << std::endl;

    // Create the core system tables if not already created
    std::cout << "Ensuring database schema exists..." << std::endl;
    auto schema_result = database->create_schema();
    if (!schema_result.success) {
        std::cerr << "Failed to create database schema: " << schema_result.error << std::endl;
        return -1;
    }

    std::cout << "Database schema verified." << std::endl;

    // Initialize S3 storage if configured
    std::cout << "Initializing object store..." << std::endl;
    auto s3_storage = std::make_unique<fileengine::S3Storage>(config.s3_endpoint, config.s3_region, config.s3_bucket,
                                                              config.s3_access_key, config.s3_secret_key,
                                                              !config.s3_path_style); // path_style flag is inverted in constructor

    auto s3_init_result = s3_storage->initialize();
    if (!s3_init_result.success) {
        std::cerr << "Failed to initialize S3 storage: " << s3_init_result.error << std::endl;
        // Continue anyway, as local storage can work without S3
    } else {
        std::cout << "S3 storage initialized successfully." << std::endl;
    }

    // Initialize storage tracker
    auto storage_tracker = std::make_unique<fileengine::StorageTracker>(config.storage_base_path);

    // Initialize storage
    std::cout << "Initializing local storage..." << std::endl;
    auto storage = std::make_unique<fileengine::Storage>(config.storage_base_path, config.encrypt_data, config.compress_data);

    // Initialize tenant manager
    std::cout << "Initializing tenant manager..." << std::endl;
    fileengine::TenantConfig tenant_config;
    tenant_config.db_host = config.db_host;
    tenant_config.db_port = config.db_port;
    tenant_config.db_name = config.db_name;
    tenant_config.db_user = config.db_user;
    tenant_config.db_password = config.db_password;
    tenant_config.storage_base_path = config.storage_base_path;
    tenant_config.s3_endpoint = config.s3_endpoint;
    tenant_config.s3_region = config.s3_region;
    tenant_config.s3_bucket = config.s3_bucket;
    tenant_config.s3_access_key = config.s3_access_key;
    tenant_config.s3_secret_key = config.s3_secret_key;
    tenant_config.s3_path_style = !config.s3_path_style; // path_style flag is inverted in constructor
    tenant_config.encrypt_data = config.encrypt_data;
    tenant_config.compress_data = config.compress_data;

    auto tenant_manager = std::make_shared<fileengine::TenantManager>(tenant_config, database, storage_tracker.get());

    // Initialize ACL manager
    auto acl_manager = std::make_shared<fileengine::AclManager>(database);

    // Initialize cache manager
    auto cache_manager = std::make_unique<fileengine::CacheManager>(storage.get(), s3_storage.get(), config.cache_threshold);

    // Initialize filesystem
    std::cout << "Initializing filesystem..." << std::endl;
    auto filesystem = std::make_shared<fileengine::FileSystem>(tenant_manager);
    filesystem->set_acl_manager(acl_manager);

    // Initialize file culling system
    std::cout << "Initializing file culling system..." << std::endl;
    auto file_culler = std::make_unique<fileengine::FileCuller>(storage.get(), s3_storage.get(), storage_tracker.get());

    // Set the file culler for cache management (create a separate instance for the filesystem)
    filesystem->set_file_culler(std::make_unique<fileengine::FileCuller>(storage.get(), s3_storage.get(), storage_tracker.get()));

    // Initialize the default tenant to ensure root directory exists
    std::cout << "Initializing default tenant..." << std::endl;
    bool default_tenant_init = tenant_manager->initialize_tenant("default");
    if (default_tenant_init) {
        std::cout << "Default tenant initialized successfully." << std::endl;
    } else {
        std::cerr << "Warning: Failed to initialize default tenant" << std::endl;
    }

    // Configure and initialize object store sync
    std::cout << "Initializing object store sync..." << std::endl;
    fileengine::SyncConfig sync_config;
    sync_config.enabled = config.sync_enabled;
    sync_config.retry_seconds = config.sync_retry_seconds;
    sync_config.sync_on_startup = config.sync_on_startup;
    sync_config.sync_on_demand = config.sync_on_demand;
    sync_config.sync_pattern = config.sync_pattern;
    sync_config.bidirectional = config.sync_bidirectional;

    auto object_store_sync = std::make_unique<fileengine::ObjectStoreSync>(database, storage.get(), s3_storage.get());
    object_store_sync->configure(sync_config);

    // Start the sync service if S3 is available
    if (s3_init_result.success) {
        auto sync_result = object_store_sync->start_sync_service();
        if (!sync_result.success) {
            std::cerr << "Failed to start object store sync: " << sync_result.error << std::endl;
            // Continue anyway, as sync is not critical for basic operation
        } else {
            std::cout << "Object store sync initialized and started." << std::endl;
        }
    } else {
        std::cout << "Object store sync not started (S3 not available)" << std::endl;
    }

    // Start the file culling system
    file_culler->start_automatic_culling();
    std::cout << "File culling system initialized and started." << std::endl;

    // Create gRPC service
    std::cout << "Initializing gRPC service..." << std::endl;
    fileengine::GRPCFileService service(filesystem, tenant_manager, acl_manager, std::move(storage_tracker));

    std::string server_address = config.server_address + ":" + std::to_string(config.server_port);
    std::cout << "Attempting to bind gRPC server to " << server_address << std::endl;

    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();

    // Set the number of threads based on configuration (the config loader should handle the environment variable)
    int num_threads = config.thread_pool_size; // Use the configuration value
    std::cout << "Setting gRPC server to use " << num_threads << " threads" << std::endl;

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    // Set thread pool options based on the configuration
    builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::NUM_CQS, num_threads);
    builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::MIN_POLLERS, num_threads);
    builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::MAX_POLLERS, num_threads);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        std::cerr << "Failed to start gRPC server" << std::endl;
        return -1;
    }

    std::cout << "gRPC Server listening on " << server_address << " with " << num_threads << " threads" << std::endl;

    // Set up signal handling for graceful shutdown
    std::signal(SIGINT, fileengine::signal_handler);
    std::signal(SIGTERM, fileengine::signal_handler);

    // Wait for signal to shutdown
    while (fileengine::signal_received == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\nShutting down gRPC server..." << std::endl;
    server->Shutdown();

    // Stop services in reverse order
    file_culler->stop_automatic_culling();
    if (s3_init_result.success) {
        object_store_sync->stop_sync_service();
    }

    // Cleanup in reverse order
    object_store_sync.reset();
    file_culler.reset();
    cache_manager.reset();
    storage_tracker.reset();
    filesystem.reset();
    acl_manager.reset();
    tenant_manager.reset();
    s3_storage.reset();
    storage.reset();
    database.reset();

    std::cout << "gRPC server shut down completed." << std::endl;

    return 0;
}