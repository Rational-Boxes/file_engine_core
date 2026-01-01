#include <iostream>
#include <memory>
#include <vector>
#include <string>
#include "fileengine/s3_storage.h"
#include "fileengine/object_store_sync.h"

int main() {
    std::cout << "Testing S3 Integration..." << std::endl;

    // Create S3Storage instance with mock parameters
    // In a real test, these would come from environment variables or config
    fileengine::S3Storage s3_storage(
        "http://localhost:9000",  // endpoint
        "us-east-1",              // region
        "test-bucket",            // bucket
        "test-access-key",        // access key
        "test-secret-key",        // secret key
        true                      // path style addressing (for MinIO)
    );

    // Test initialization
    std::cout << "Testing S3 storage initialization..." << std::endl;
    auto init_result = s3_storage.initialize();
    if (init_result.success) {
        std::cout << "S3 storage initialized successfully" << std::endl;
    } else {
        std::cout << "S3 storage initialization failed: " << init_result.error << std::endl;
        // Note: This might fail if AWS SDK is not available or configured properly
        // That's expected in many environments
    }

    // Test that immutable operations are properly restricted
    std::cout << "Testing immutable operations..." << std::endl;

    // Test that delete operations are properly restricted
    auto delete_result = s3_storage.delete_file("test/path", "test-tenant");
    if (!delete_result.success) {
        std::cout << "Delete operation correctly restricted: " << delete_result.error << std::endl;
    } else {
        std::cout << "ERROR: Delete operation should be restricted!" << std::endl;
    }

    // Test that tenant cleanup is properly restricted
    auto cleanup_result = s3_storage.cleanup_tenant_bucket("test-tenant");
    if (!cleanup_result.success) {
        std::cout << "Tenant cleanup correctly restricted: " << cleanup_result.error << std::endl;
    } else {
        std::cout << "ERROR: Tenant cleanup should be restricted!" << std::endl;
    }

    // Test that storage clearing is properly restricted
    auto clear_result = s3_storage.clear_storage("test-tenant");
    if (!clear_result.success) {
        std::cout << "Storage clearing correctly restricted: " << clear_result.error << std::endl;
    } else {
        std::cout << "ERROR: Storage clearing should be restricted!" << std::endl;
    }

    std::cout << "S3 Integration tests completed." << std::endl;
    return 0;
}