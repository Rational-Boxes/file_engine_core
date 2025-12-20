#include <iostream>
#include <cassert>
#include <string>
#include <memory>
#include "fileengine/types.h"
#include "fileengine/utils.h"
#include "fileengine/database.h"
#include "fileengine/storage.h"
#include "fileengine/s3_storage.h"
#include "fileengine/tenant_manager.h"
#include "fileengine/filesystem.h"

void test_uuid_generation() {
    std::cout << "Testing UUID generation..." << std::endl;
    std::string uuid1 = fileengine::Utils::generate_uuid();
    std::string uuid2 = fileengine::Utils::generate_uuid();
    
    assert(!uuid1.empty());
    assert(!uuid2.empty());
    assert(uuid1 != uuid2); // Should be different
    
    std::cout << "UUID generation test passed!" << std::endl;
}

void test_timestamp_generation() {
    std::cout << "Testing timestamp generation..." << std::endl;
    std::string ts1 = fileengine::Utils::get_timestamp_string();
    std::string ts2 = fileengine::Utils::get_timestamp_string();
    
    assert(!ts1.empty());
    assert(!ts2.empty());
    // Note: timestamps could be the same if generated very quickly
    
    std::cout << "Timestamp generation test passed!" << std::endl;
}

void test_filesystem_basics() {
    std::cout << "Testing filesystem basic operations..." << std::endl;

    // This is a basic test to ensure the filesystem classes can be instantiated
    // A full test would require a running database and storage backend
    // First create a database instance to share with tenant manager
    auto database = std::make_shared<fileengine::Database>("localhost", 5432, "fileengine_test",
                                                          "testuser", "testpass", 5);

    fileengine::TenantConfig config;
    config.storage_base_path = "/tmp/fileengine_test";
    config.s3_endpoint = "http://localhost:9000";
    config.s3_region = "us-east-1";
    config.s3_bucket = "fileengine-test";
    config.s3_access_key = "minioadmin";
    config.s3_secret_key = "minioadmin";
    config.s3_path_style = true;
    config.encrypt_data = false;
    config.compress_data = false;

    auto tenant_manager = std::make_shared<fileengine::TenantManager>(database, config);  // Pass shared database
    auto filesystem = std::make_shared<fileengine::FileSystem>(tenant_manager);

    // Just ensure they can be created without crashing
    assert(tenant_manager != nullptr);
    assert(filesystem != nullptr);

    std::cout << "Filesystem basic operations test passed!" << std::endl;
}

void test_result_type() {
    std::cout << "Testing Result type..." << std::endl;
    
    // Test successful result
    fileengine::Result<std::string> success = fileengine::Result<std::string>::ok("test value");
    assert(success.success);
    assert(success.value == "test value");
    assert(success.error.empty());
    
    // Test error result
    fileengine::Result<std::string> error = fileengine::Result<std::string>::err("test error");
    assert(!error.success);
    assert(error.value.empty());
    assert(error.error == "test error");
    
    // Test void result
    fileengine::Result<void> void_success = fileengine::Result<void>::ok();
    assert(void_success.success);
    assert(void_success.error.empty());
    
    fileengine::Result<void> void_error = fileengine::Result<void>::err("test error");
    assert(!void_error.success);
    assert(void_error.error == "test error");
    
    std::cout << "Result type test passed!" << std::endl;
}

int main() {
    std::cout << "Running FileEngine Core Tests..." << std::endl;
    
    test_uuid_generation();
    test_timestamp_generation();
    test_filesystem_basics();
    test_result_type();
    
    std::cout << "All tests passed!" << std::endl;
    return 0;
}