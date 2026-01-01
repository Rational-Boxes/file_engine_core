#include "core/include/fileengine/s3_storage.h"
#include <iostream>
#include <vector>
#include <string>

int main() {
    // Create S3 storage instance
    fileengine::S3Storage s3_storage(
        "http://localhost:9000",  // endpoint
        "us-west-1",              // region  
        "fileservicecpp",         // bucket
        "bt4P2Mzc5plD8MePDzaa",   // access key
        "3i1M4JDiogTYxNvgO1kKoKHUkNsBrVwFxRVQ58Rc",  // secret key
        true                      // path style addressing for MinIO
    );

    // Initialize the S3 storage
    auto init_result = s3_storage.initialize();
    if (!init_result.success) {
        std::cout << "Failed to initialize S3 storage: " << init_result.error << std::endl;
        return 1;
    }

    std::cout << "S3 storage initialized successfully" << std::endl;

    // Create test data
    std::string test_content = "Test content for S3 sync functionality";
    std::vector<uint8_t> data(test_content.begin(), test_content.end());

    // Store a file
    auto store_result = s3_storage.store_file("test_file", "20251231_190000.000", data, "default");
    if (store_result.success) {
        std::cout << "File stored successfully with key: " << store_result.value << std::endl;
    } else {
        std::cout << "Failed to store file: " << store_result.error << std::endl;
        return 1;
    }

    std::cout << "S3 sync functionality test completed successfully!" << std::endl;
    return 0;
}