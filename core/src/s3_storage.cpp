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
#ifdef USE_AWS_SDK
    // Clean up AWS SDK resources if needed
    // Note: In a real application, you'd want to be careful about when to call Aws::ShutdownAPI
    // as it should only be called once when the application shuts down, not per instance
    // For this implementation, we'll just reset the client
    s3_client_.reset();
    // Do NOT call Aws::ShutdownAPI here - that should be called once at application shutdown
#endif
}

Result<void> S3Storage::initialize() {
#ifdef USE_AWS_SDK
    // Initialize AWS SDK if not already initialized
    Aws::SDKOptions options;
    Aws::InitAPI(options);

    // Create AWS credentials provider
    auto credentialsProvider = std::make_shared<Aws::Auth::SimpleAWSCredentialsProvider>(
        access_key_, secret_key_, "");

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

    // Create the S3 client with credentials provider and configuration
    s3_client_ = std::make_shared<Aws::S3::S3Client>(
        credentialsProvider,
        s3Config,
        Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
        false
    );

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
    request.SetBucket(Aws::String(bucket_));
    request.SetKey(Aws::String(key));

    // Create a string stream for the data
    auto body_stream = Aws::MakeShared<Aws::StringStream>("S3Stream", "");
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

#ifdef USE_AWS_SDK
    if (!s3_client_) {
        return Result<std::vector<uint8_t>>::err("S3 client not initialized");
    }

    // Create the GetObject request
    Aws::S3::Model::GetObjectRequest request;
    request.SetBucket(Aws::String(bucket_));
    // Use the provided storage_path which already includes tenant prefix if needed
    request.SetKey(Aws::String(storage_path));

    // Perform the request
    auto outcome = s3_client_->GetObject(request);

    if (outcome.IsSuccess()) {
        // Read the response body into a vector
        auto& body = outcome.GetResult().GetBody();
        std::ostringstream ss;
        ss << body.rdbuf();
        std::string content = ss.str();
        std::vector<uint8_t> data(content.begin(), content.end());
        return Result<std::vector<uint8_t>>::ok(data);
    } else {
        return Result<std::vector<uint8_t>>::err("Failed to download file from S3: " +
                                                outcome.GetError().GetMessage());
    }
#else
    // For non-AWS SDK builds, return an error indicating the feature is not available
    return Result<std::vector<uint8_t>>::err("AWS SDK not available - S3 storage requires USE_AWS_SDK to be defined");
#endif
}

Result<bool> S3Storage::file_exists(const std::string& storage_path, const std::string& tenant) {
    if (!initialized_) {
        return Result<bool>::err("S3 storage not initialized");
    }

#ifdef USE_AWS_SDK
    if (!s3_client_) {
        return Result<bool>::err("S3 client not initialized");
    }

    // Create the HeadObject request to check if the object exists
    Aws::S3::Model::HeadObjectRequest request;
    request.SetBucket(Aws::String(bucket_));
    request.SetKey(Aws::String(storage_path));

    // Perform the request
    auto outcome = s3_client_->HeadObject(request);

    if (outcome.IsSuccess()) {
        return Result<bool>::ok(true);
    } else {
        // If the error is because the object doesn't exist, return false
        if (outcome.GetError().GetExceptionName().find("NoSuchKey") != std::string::npos) {
            return Result<bool>::ok(false);
        } else {
            // For other errors, return the error
            return Result<bool>::err("Failed to check if file exists in S3: " +
                                    outcome.GetError().GetMessage());
        }
    }
#else
    // For non-AWS SDK builds, return an error indicating the feature is not available
    return Result<bool>::err("AWS SDK not available - S3 storage requires USE_AWS_SDK to be defined");
#endif
}

std::string S3Storage::get_storage_path(const std::string& virtual_path, const std::string& version_timestamp, const std::string& tenant) const {
    std::string key = path_to_key(virtual_path, version_timestamp);
    if (!tenant.empty()) {
        key = tenant + "/" + key;
    }
    return key;
}

Result<bool> S3Storage::bucket_exists(const std::string& tenant) {
    if (!initialized_) {
        return Result<bool>::err("S3 storage not initialized");
    }

#ifdef USE_AWS_SDK
    if (!s3_client_) {
        return Result<bool>::err("S3 client not initialized");
    }

    // For bucket existence check, we'll just check if we can list objects in the bucket
    // since HeadBucket might not be available in all S3-compatible services
    Aws::S3::Model::ListObjectsV2Request request;
    request.SetBucket(Aws::String(bucket_));
    request.SetMaxKeys(1); // Just check if we can list at least one object

    // Perform the request
    auto outcome = s3_client_->ListObjectsV2(request);

    if (outcome.IsSuccess()) {
        return Result<bool>::ok(true);
    } else {
        // If the error is because the bucket doesn't exist, return false
        if (outcome.GetError().GetExceptionName().find("NoSuchBucket") != std::string::npos) {
            return Result<bool>::ok(false);
        } else {
            // For other errors, return the error
            return Result<bool>::err("Failed to check if bucket exists in S3: " +
                                    outcome.GetError().GetMessage());
        }
    }
#else
    // For non-AWS SDK builds, return an error indicating the feature is not available
    return Result<bool>::err("AWS SDK not available - S3 storage requires USE_AWS_SDK to be defined");
#endif
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

    // With the prefix approach, tenant buckets don't need separate creation
    // The bucket already exists as configured, and we just use prefixes
    return Result<void>::ok();
}

Result<bool> S3Storage::tenant_bucket_exists(const std::string& tenant) {
    if (tenant.empty()) {
        return Result<bool>::ok(false);
    }

    if (!initialized_) {
        return Result<bool>::err("S3 storage not initialized");
    }

    // Since we're using prefixes instead of separate buckets, we consider tenant "buckets" to exist
    // as long as the main bucket exists and is accessible
    auto bucket_result = bucket_exists("");
    if (bucket_result.success) {
        return Result<bool>::ok(true);
    } else {
        return Result<bool>::err(bucket_result.error);
    }
}

Result<void> S3Storage::cleanup_tenant_bucket(const std::string& tenant) {
    if (tenant.empty()) {
        return Result<void>::err("Tenant name cannot be empty");
    }

    if (!initialized_) {
        return Result<void>::err("S3 storage not initialized");
    }

    // Since we're using prefixes and not separate buckets, we can't truly "delete" a tenant bucket
    // Instead, we'll return an error to indicate this operation is not supported
    return Result<void>::err("Cleanup of tenant storage is not supported - S3 objects are immutable");
}

Result<void> S3Storage::clear_storage(const std::string& tenant) {
    if (!initialized_) {
        return Result<void>::err("S3 storage not initialized");
    }

    // Since we're using prefixes and not separate buckets, and S3 objects are immutable,
    // we can't truly clear the storage. We'll return an error to indicate this operation is not supported
    return Result<void>::err("Clearing S3 storage is not supported - S3 objects are immutable");
}

Result<void> S3Storage::create_bucket_if_not_exists(const std::string& tenant) {
    if (!initialized_) {
        return Result<void>::err("S3 storage not initialized");
    }

    // Since all storage is in the same bucket, we just check if the main bucket exists
    auto exists_result = bucket_exists(tenant);
    if (exists_result.success && exists_result.value) {
        return Result<void>::ok();  // Bucket already exists
    } else if (exists_result.success && !exists_result.value) {
        // The bucket doesn't exist, but we can't create it in this implementation
        // since all storage is in the same configured bucket
        return Result<void>::err("Main bucket does not exist - please ensure the configured bucket exists");
    } else {
        return Result<void>::err("Failed to check bucket existence: " + exists_result.error);
    }
}

Result<void> S3Storage::delete_file(const std::string& storage_path, const std::string& tenant) {
    if (!initialized_) {
        return Result<void>::err("S3 storage not initialized");
    }

#ifdef USE_AWS_SDK
    if (!s3_client_) {
        return Result<void>::err("S3 client not initialized");
    }

    // According to specifications, items should never be removed from S3
    // This is to maintain an immutable file system with full history
    return Result<void>::err("Deleting files from S3 is not allowed - S3 objects are immutable for history preservation");
#else
    // For non-AWS SDK builds, return an error indicating the feature is not available
    return Result<void>::err("AWS SDK not available - S3 storage requires USE_AWS_SDK to be defined");
#endif
}

std::string S3Storage::path_to_key(const std::string& virtual_path, const std::string& version_timestamp) const {
    // Create a key by combining the virtual path and version timestamp
    // This creates a path like: virtual_path/version_timestamp
    std::ostringstream key_stream;
    key_stream << virtual_path << "/" << version_timestamp;
    return key_stream.str();
}

} // namespace fileengine