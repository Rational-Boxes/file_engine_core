#include "fileengine/s3_storage.h"
#include "fileengine/server_logger.h"  // Explicitly include the core logger header
#include <sstream>
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace fileengine {

// Helper function to encode data in base64
std::string S3Storage::base64_encode(const std::string& data) {
    const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    int val = 0, valb = -6;

    for (unsigned char c : data) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            result.push_back(chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) result.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (result.size() % 4) result.push_back('=');

    return result;
}


S3Storage::S3Storage(const std::string& endpoint,
                     const std::string& region,
                     const std::string& bucket,
                     const std::string& access_key,
                     const std::string& secret_key,
                     bool path_style)
    : endpoint_(endpoint), region_(region), bucket_(bucket),
      access_key_(access_key), secret_key_(secret_key),
      path_style_(path_style), initialized_(false) {
#ifdef USE_AWS_SDK
    s3_client_ = nullptr;
#endif
}

S3Storage::~S3Storage() {
    // Cleanup resources if needed
}

Result<void> S3Storage::initialize() {
#ifdef USE_AWS_SDK
    // Initialize AWS SDK if not already initialized
    Aws::SDKOptions options;
    Aws::InitAPI(options);

    // Configure the S3 client
    Aws::S3::S3ClientConfiguration s3Config;
    s3Config.endpointOverride = endpoint_;
    s3Config.scheme = Aws::Http::Scheme::HTTP;  // Use HTTP for MinIO, change to HTTPS for AWS
    if (endpoint_.find("https://") == 0) {
        s3Config.scheme = Aws::Http::Scheme::HTTPS;
    }
    s3Config.region = region_;
    s3Config.verifySSL = (s3Config.scheme == Aws::Http::Scheme::HTTPS);
    s3Config.useVirtualAddressing = !path_style_;  // Use path-style addressing if path_style_ is true

    // Create the S3 client with configuration only (credentials will be handled internally)
    s3_client_ = std::make_shared<Aws::S3::S3Client>(s3Config);

    initialized_ = true;
    return Result<void>::ok();
#else
    // For non-AWS SDK builds, we'll just mark as initialized
    // In a real implementation, this would initialize the internal S3 client
    initialized_ = true;
    return Result<void>::ok();
#endif
}

Result<std::string> S3Storage::store_file(const std::string& virtual_path, const std::string& version_timestamp,
                                          const std::vector<uint8_t>& data, const std::string& tenant) {
    if (!initialized_) {
        return Result<std::string>::err("S3 storage not initialized");
    }

    std::string key = path_to_key(virtual_path, version_timestamp);
    if (!tenant.empty()) {
        key = tenant + "/" + key;
    }

#ifdef USE_AWS_SDK
    if (!s3_client_) {
        return Result<std::string>::err("S3 client not initialized");
    }

    // Create the PutObject request
    Aws::S3::Model::PutObjectRequest request;
    request.SetBucket(bucket_);
    request.SetKey(key.c_str());

    // Create a string stream for the data
    std::shared_ptr<Aws::IOStream> body_stream = Aws::MakeShared<Aws::StringStream>("S3Stream", "");
    body_stream->write(reinterpret_cast<const char*>(data.data()), data.size());
    body_stream->flush();
    request.SetBody(body_stream);

    // Perform the request
    auto outcome = s3_client_->PutObject(request);

    if (outcome.IsSuccess()) {
        return Result<std::string>::ok(key);
    } else {
        return Result<std::string>::err("Failed to upload file to S3: " +
                                       outcome.GetError().GetMessage());
    }
#else
    // For non-AWS SDK builds, return an error indicating the feature is not available
    return Result<std::string>::err("AWS SDK not available - S3 storage requires USE_AWS_SDK to be defined");
#endif
}

Result<std::vector<uint8_t>> S3Storage::read_file(const std::string& storage_path, const std::string& tenant) {
    if (!initialized_) {
        return Result<std::vector<uint8_t>>::err("S3 storage not initialized");
    }
    
    // In a real implementation, this would download the file from S3
    // For this simplified version, we'll return an empty vector
    return Result<std::vector<uint8_t>>::ok(std::vector<uint8_t>());
}

Result<void> S3Storage::delete_file(const std::string& storage_path, const std::string& tenant) {
    if (!initialized_) {
        return Result<void>::err("S3 storage not initialized");
    }
    
    // In a real implementation, this would delete the file from S3
    return Result<void>::ok();
}

Result<bool> S3Storage::file_exists(const std::string& storage_path, const std::string& tenant) {
    if (!initialized_) {
        return Result<bool>::err("S3 storage not initialized");
    }
    
    // In a real implementation, this would check if the file exists in S3
    // For this simplified version, we'll return true
    return Result<bool>::ok(true);
}

std::string S3Storage::get_storage_path(const std::string& virtual_path, const std::string& version_timestamp, const std::string& tenant) const {
    std::string key = path_to_key(virtual_path, version_timestamp);
    if (!tenant.empty()) {
        key = tenant + "/" + key;
    }
    return key;
}

Result<void> S3Storage::create_bucket_if_not_exists(const std::string& tenant) {
    if (!initialized_) {
        return Result<void>::err("S3 storage not initialized");
    }
    
    // In a real implementation, this would create the bucket if it doesn't exist
    return Result<void>::ok();
}

Result<bool> S3Storage::bucket_exists(const std::string& tenant) {
    if (!initialized_) {
        return Result<bool>::err("S3 storage not initialized");
    }
    
    // In a real implementation, this would check if the bucket exists
    // For this simplified version, we'll return true
    return Result<bool>::ok(true);
}

bool S3Storage::is_encryption_enabled() const {
    // For this simplified implementation, we'll assume encryption is handled by S3
    return true;
}

bool S3Storage::is_initialized() const {
    return initialized_;
}

Result<void> S3Storage::create_tenant_bucket(const std::string& tenant) {
    if (tenant.empty()) {
        return Result<void>::err("Tenant name cannot be empty");
    }

    if (!initialized_) {
        return Result<void>::err("S3 storage not initialized");
    }

    // In a real implementation, this would create a tenant-specific bucket
    // For now, we'll just return success
    return Result<void>::ok();
}

Result<bool> S3Storage::tenant_bucket_exists(const std::string& tenant) {
    if (tenant.empty()) {
        return Result<bool>::ok(false);
    }

    if (!initialized_) {
        return Result<bool>::err("S3 storage not initialized");
    }

    // In a real implementation, this would check if the tenant bucket exists
    // For this simplified version, we'll return true for existing tenants
    return Result<bool>::ok(true);
}

Result<void> S3Storage::cleanup_tenant_bucket(const std::string& tenant) {
    if (tenant.empty()) {
        return Result<void>::err("Tenant name cannot be empty");
    }

    if (!initialized_) {
        return Result<void>::err("S3 storage not initialized");
    }

    // In a real implementation, this would delete the tenant-specific bucket
    return Result<void>::ok();
}

Result<void> S3Storage::clear_storage(const std::string& tenant) {
    if (tenant.empty()) {
        return Result<void>::err("Tenant cannot be empty for clear_storage operation");
    }

    if (!initialized_) {
        return Result<void>::err("S3 storage not initialized");
    }

    // In a real implementation, this would delete all objects in the tenant's bucket
    // For now, we'll just return success
    return Result<void>::ok();
}

std::string S3Storage::path_to_key(const std::string& virtual_path, const std::string& version_timestamp) const {
    // Create a key by combining the virtual path and version timestamp
    std::ostringstream key_stream;
    key_stream << virtual_path << "/" << version_timestamp;
    return key_stream.str();
}

} // namespace fileengine