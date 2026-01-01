#include "core/include/fileengine/s3_storage.h"
#include "core/include/fileengine/object_store_sync.h"
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>

int main() {
    std::cout << "Testing S3 functionality..." << std::endl;

    // Create S3 storage instance
    fileengine::S3Storage s3_storage(
        "http://localhost:9000",  // endpoint - adjust as needed for your setup
        "us-west-1",              // region
        "fileservicecpp",         // bucket
        "YOUR_ACCESS_KEY",        // access key - replace with actual key
        "YOUR_SECRET_KEY",        // secret key - replace with actual key
        true                      // path style addressing for MinIO
    );

    // Initialize the S3 storage
    auto init_result = s3_storage.initialize();
    if (!init_result.success) {
        std::cout << "Failed to initialize S3 storage: " << init_result.error << std::endl;
        return 1;
    }

    std::cout << "S3 storage initialized successfully" << std::endl;

    // Test storing a file
    std::string test_content = "Test content for S3 functionality verification";
    std::vector<uint8_t> data(test_content.begin(), test_content.end());

    auto store_result = s3_storage.store_file("test_file", "20251231_190000.000", data, "default");
    if (store_result.success) {
        std::cout << "File stored successfully with key: " << store_result.value << std::endl;
    } else {
        std::cout << "Failed to store file: " << store_result.error << std::endl;
        return 1;
    }

    // Test checking if file exists
    auto exists_result = s3_storage.file_exists(store_result.value, "default");
    if (exists_result.success && exists_result.value) {
        std::cout << "File exists in S3: " << store_result.value << std::endl;
    } else if (exists_result.success && !exists_result.value) {
        std::cout << "File does not exist in S3: " << store_result.value << std::endl;
    } else {
        std::cout << "Failed to check if file exists: " << exists_result.error << std::endl;
        return 1;
    }

    // Test reading the file back
    auto read_result = s3_storage.read_file(store_result.value, "default");
    if (read_result.success) {
        std::string content(read_result.value.begin(), read_result.value.end());
        std::cout << "File read successfully. Content: " << content << std::endl;
        
        if (content == test_content) {
            std::cout << "Content matches original!" << std::endl;
        } else {
            std::cout << "Content does not match original!" << std::endl;
            return 1;
        }
    } else {
        std::cout << "Failed to read file: " << read_result.error << std::endl;
        return 1;
    }

    std::cout << "S3 functionality test completed successfully!" << std::endl;
    return 0;
}