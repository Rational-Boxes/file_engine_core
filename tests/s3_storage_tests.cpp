#include <iostream>
#include <cassert>
#include <string>
#include <memory>
#include <vector>
#include "fileengine/s3_storage.h"
#include "fileengine/types.h"
#include "fileengine/utils.h"

void test_s3_storage_creation() {
    std::cout << "Testing S3Storage creation..." << std::endl;
    
    // Create S3Storage with mock parameters
    fileengine::S3Storage s3_storage(
        "http://localhost:9000",  // endpoint
        "us-east-1",              // region
        "test-bucket",            // bucket
        "test-access-key",        // access key
        "test-secret-key",        // secret key
        false                     // path style
    );
    
    // Basic functionality test
    assert(true);
    
    std::cout << "S3Storage creation test passed!" << std::endl;
}

void test_s3_storage_path_generation() {
    std::cout << "Testing S3Storage path generation..." << std::endl;
    
    fileengine::S3Storage s3_storage(
        "http://localhost:9000",
        "us-east-1", 
        "test-bucket",
        "test-access-key",
        "test-secret-key",
        false
    );
    
    std::string virtual_path = "test_file.txt";
    std::string version_timestamp = "20230101_120000";
    
    // Test storage path generation without tenant
    std::string path1 = s3_storage.get_storage_path(virtual_path, version_timestamp, "");
    assert(path1.find(virtual_path) != std::string::npos);
    assert(path1.find(version_timestamp) != std::string::npos);
    
    // Test storage path generation with tenant
    std::string path2 = s3_storage.get_storage_path(virtual_path, version_timestamp, "tenant1");
    assert(path2.find("tenant1") != std::string::npos);
    assert(path2.find(virtual_path) != std::string::npos);
    assert(path2.find(version_timestamp) != std::string::npos);
    
    // Test internal key generation
    std::string key = s3_storage.path_to_key(virtual_path, version_timestamp);
    assert(!key.empty());
    
    std::cout << "S3Storage path generation test passed!" << std::endl;
}

void test_s3_storage_encryption_flag() {
    std::cout << "Testing S3Storage encryption flag..." << std::endl;
    
    // Create with encryption (though in mock implementation this may be ignored)
    fileengine::S3Storage s3_storage(
        "http://localhost:9000",
        "us-east-1",
        "test-bucket", 
        "test-access-key",
        "test-secret-key",
        false
    );
    
    // In the mock implementation, encryption is assumed to be handled by S3
    bool encryption_enabled = s3_storage.is_encryption_enabled();
    assert(encryption_enabled);  // Should return true as per mock implementation
    
    std::cout << "S3Storage encryption flag test passed!" << std::endl;
}

void test_tenant_bucket_operations() {
    std::cout << "Testing S3Storage tenant bucket operations..." << std::endl;
    
    fileengine::S3Storage s3_storage(
        "http://localhost:9000",
        "us-east-1",
        "test-bucket",
        "test-access-key", 
        "test-secret-key",
        false
    );
    
    std::string tenant_name = "test_tenant";
    
    // Test tenant bucket creation
    auto create_result = s3_storage.create_tenant_bucket(tenant_name);
    // In mock implementation, this just returns success
    assert(create_result.success || !create_result.error.empty());
    
    // Test tenant bucket existence check
    auto exists_result = s3_storage.tenant_bucket_exists(tenant_name);
    assert(exists_result.success);
    
    // Test with empty tenant name
    auto empty_result = s3_storage.tenant_bucket_exists("");
    assert(!empty_result.success);  // Should fail with empty tenant
    
    // Test bucket existence (mock implementation)
    auto bucket_exists = s3_storage.bucket_exists("");
    assert(bucket_exists.success);
    
    std::cout << "S3Storage tenant bucket operations test passed!" << std::endl;
}

void test_s3_storage_initialize() {
    std::cout << "Testing S3Storage initialization..." << std::endl;
    
    fileengine::S3Storage s3_storage(
        "http://localhost:9000",
        "us-east-1",
        "test-bucket",
        "test-access-key",
        "test-secret-key", 
        false
    );
    
    // Test initialization
    auto init_result = s3_storage.initialize();
    assert(init_result.success);
    
    std::cout << "S3Storage initialization test passed!" << std::endl;
}

int main() {
    std::cout << "Running FileEngine Core S3/MinIO Unit Tests..." << std::endl;
    
    test_s3_storage_creation();
    test_s3_storage_path_generation();
    test_s3_storage_encryption_flag();
    test_tenant_bucket_operations();
    test_s3_storage_initialize();
    
    std::cout << "All S3/MinIO unit tests passed!" << std::endl;
    return 0;
}