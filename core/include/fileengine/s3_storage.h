#pragma once

#include "types.h"
#include "IObjectStore.h"
#include <string>
#include <vector>
#include <memory>

#ifdef USE_AWS_SDK
#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/client/ClientConfiguration.h>
#endif

namespace fileengine {

class S3Storage : public IObjectStore {
public:
    S3Storage(const std::string& endpoint,
              const std::string& region,
              const std::string& bucket,
              const std::string& access_key,
              const std::string& secret_key,
              bool path_style = false);
    ~S3Storage();

    // Initialize AWS SDK and S3 client
    Result<void> initialize() override;

    // Check if object store is initialized
    bool is_initialized() const override;

    // File storage operations (automatically compress and encrypt)
    Result<std::string> store_file(const std::string& virtual_path, const std::string& version_timestamp,
                                   const std::vector<uint8_t>& data, const std::string& tenant = "") override;
    Result<std::vector<uint8_t>> read_file(const std::string& storage_path, const std::string& tenant = "") override;
    Result<void> delete_file(const std::string& storage_path, const std::string& tenant = "") override;
    Result<bool> file_exists(const std::string& storage_path, const std::string& tenant = "") override;

    // Get storage path for a virtual file (same format as local storage)
    std::string get_storage_path(const std::string& virtual_path, const std::string& version_timestamp, const std::string& tenant = "") const override;

    // Bucket operations
    Result<void> create_bucket_if_not_exists(const std::string& tenant = "") override;
    Result<bool> bucket_exists(const std::string& tenant = "") override;

    // Check if encryption is enabled
    bool is_encryption_enabled() const override;

    // Tenant management operations
    Result<void> create_tenant_bucket(const std::string& tenant) override;
    Result<bool> tenant_bucket_exists(const std::string& tenant) override;
    Result<void> cleanup_tenant_bucket(const std::string& tenant) override;
    Result<void> clear_storage(const std::string& tenant = "") override;

private:
    std::string endpoint_;
    std::string region_;
    std::string bucket_;
    std::string access_key_;
    std::string secret_key_;
    bool path_style_;
    bool initialized_;

#ifdef USE_AWS_SDK
    std::shared_ptr<Aws::S3::S3Client> s3_client_;
#endif

    // Helper to generate storage key from path
    std::string path_to_key(const std::string& virtual_path, const std::string& version_timestamp) const;

    // Helper function for base64 encoding
    std::string base64_encode(const std::string& data);

#ifndef USE_AWS_SDK
    // Helper functions for AWS Signature Version 4 (only needed if not using AWS SDK)
    std::string aws_sign_v4(const std::string& method, const std::string& path,
                            const std::string& query, const std::string& headers,
                            const std::string& payload_hash, const std::string& timestamp) const;
    std::string sha256_hash(const std::string& str) const;
    std::string hmac_sha256_hash(const std::string& key, const std::string& msg) const;
    std::string get_current_iso8601_time() const;
    std::string get_date_from_iso8601(const std::string& iso8601_time) const;
#endif
};

} // namespace fileengine