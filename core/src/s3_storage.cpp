// Copyright (C) 2026 James Hickman
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <mutex>
#include "fileengine/s3_storage.h"
#include "fileengine/transfer_tracker.h"
#include "fileengine/server_logger.h"  // Explicitly include the core logger header
#include <sstream>
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <filesystem>

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
    // Order matters: the client holds SDK resources, so it has to go first. The
    // guard is released second, and only the LAST one released actually shuts
    // the SDK down (AwsSdkGuard above) — so this is safe with any number of
    // S3Storage instances, which is why ShutdownAPI is not called here directly.
    s3_client_.reset();
    sdk_guard_.reset();
#endif
}

#ifdef USE_AWS_SDK
namespace {

// AWS SDK process lifecycle.
//
// Three things were wrong here, and together they crashed the process on exit
// with a jump to address 0:
//
//   1. `Aws::SDKOptions` was a STACK LOCAL passed to InitAPI and destroyed on
//      the next line. The struct carries std::function hooks (logging and HTTP
//      client factories); the SDK keeps hold of them, and calling one after its
//      target is gone is precisely a jump to address 0. The options must outlive
//      the SDK, and the SAME object must be handed to ShutdownAPI.
//   2. InitAPI ran on every initialize() call rather than once per process.
//   3. ShutdownAPI was never called at all, so whatever survived was torn down
//      by the C++ runtime in an undefined order.
//
// This owns all three. The guard is reference-counted and handed to every
// S3Storage, so the SDK is initialized on first use and shut down exactly once,
// when the last user is destroyed — which is necessarily BEFORE static
// destruction, and after every S3 client has already gone.
class AwsSdkGuard {
public:
    static std::shared_ptr<AwsSdkGuard> acquire() {
        static std::mutex mu;
        static std::weak_ptr<AwsSdkGuard> existing;
        std::lock_guard<std::mutex> lock(mu);
        if (auto live = existing.lock()) {
            return live;   // already initialized; join the existing lifetime
        }
        std::shared_ptr<AwsSdkGuard> fresh(new AwsSdkGuard());
        existing = fresh;
        return fresh;
    }

    ~AwsSdkGuard() {
        Aws::ShutdownAPI(options_);
    }

    AwsSdkGuard(const AwsSdkGuard&) = delete;
    AwsSdkGuard& operator=(const AwsSdkGuard&) = delete;

private:
    AwsSdkGuard() {
        Aws::InitAPI(options_);
    }

    // A member, not a local: the SDK holds references into it for its whole
    // lifetime, and ShutdownAPI must receive the same object InitAPI got.
    Aws::SDKOptions options_;
};

} // namespace
#endif

Result<void> S3Storage::initialize() {
#ifdef USE_AWS_SDK
    // Initialize the SDK once per process and hold it up for as long as this
    // object could use it. See AwsSdkGuard above.
    sdk_guard_ = AwsSdkGuard::acquire();

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

Result<std::string> S3Storage::store_file_from_path(const std::string& virtual_path,
                                                    const std::string& version_timestamp,
                                                    const std::string& local_path,
                                                    const std::string& tenant) {
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

    // S3 multipart requires parts of at least 5 MiB (except the last). Stream the
    // local file in PART_SIZE slices so only one part is held in memory at a time.
    constexpr std::streamsize PART_SIZE = 8 * 1024 * 1024;  // 8 MiB
    std::error_code ec;
    std::uintmax_t file_size = std::filesystem::file_size(local_path, ec);
    if (ec) {
        return Result<std::string>::err("Cannot stat local file: " + local_path);
    }
    // Small files: a single PutObject (the default path) is cheaper than multipart.
    if (file_size <= static_cast<std::uintmax_t>(PART_SIZE)) {
        return IObjectStore::store_file_from_path(virtual_path, version_timestamp, local_path, tenant);
    }

    Aws::S3::Model::CreateMultipartUploadRequest create_req;
    create_req.SetBucket(Aws::String(bucket_));
    create_req.SetKey(Aws::String(key));
    auto created = s3_client_->CreateMultipartUpload(create_req);
    if (!created.IsSuccess()) {
        return Result<std::string>::err("Failed to start S3 multipart upload: " +
                                        created.GetError().GetMessage());
    }
    const Aws::String upload_id = created.GetResult().GetUploadId();

    // From here until the upload is COMMITTED, every exit must abort — otherwise
    // the parts already uploaded stay in the bucket: billed, invisible to a
    // normal listing, referenced by nothing, and indistinguishable from an
    // upload still in flight.
    //
    // A scope guard rather than an abort call on each error path. Hand-placed
    // aborts were already one short (the CompleteMultipartUpload failure
    // returned without one, stranding a whole object's worth of parts at the
    // point where the most data is at stake), and any early return or throw
    // added later would have had the same problem. This way cleanup is the
    // default and committing is the explicit act.
    struct AbortGuard {
        Aws::S3::S3Client* client;
        Aws::String bucket, key, upload_id;
        bool committed = false;
        // Where it gave up, and how much crossed the wire first. Both are only
        // meaningful on the failure path; on success the destructor returns
        // early and the caller has already recorded the completion.
        TransferTracker::Stage stage = TransferTracker::Stage::OpenFailed;
        std::uint64_t bytes_sent = 0;
        ~AbortGuard() {
            if (committed || !client) return;
            // A big transfer that did not arrive. Invisible everywhere else —
            // no object, no reference, nothing in a normal bucket listing — so
            // this counter is the only place it is ever seen.
            TransferTracker::instance().record_aborted(stage, bytes_sent);

            // The counters say a big upload failed; this says WHICH one, so a
            // user reporting "my large file didn't upload" can be matched to an
            // actual event rather than a rate on a graph. The key carries
            // tenant, uid and version, which is everything needed to find it.
            const char* where = (stage == TransferTracker::Stage::OpenFailed)     ? "open"
                              : (stage == TransferTracker::Stage::PartFailed)     ? "transfer"
                                                                                  : "commit";
            SERVER_LOG_WARN("S3Storage::multipart",
                            std::string("aborting multipart upload at ") + where +
                            " — key=" + std::string(key.c_str()) +
                            " bytes_transferred=" + std::to_string(bytes_sent) +
                            " upload_id=" + std::string(upload_id.c_str()));

            Aws::S3::Model::AbortMultipartUploadRequest req;
            req.SetBucket(bucket);
            req.SetKey(key);
            req.SetUploadId(upload_id);
            // Best-effort: we are already on a failure path and have nothing
            // better to do if the abort itself fails. That residue is what the
            // bucket's AbortIncompleteMultipartUpload lifecycle rule reaps —
            // which is why that rule is not optional housekeeping, and why a
            // rising abort_failed count is worth an alert of its own.
            auto aborted = client->AbortMultipartUpload(req);
            if (!aborted.IsSuccess()) {
                TransferTracker::instance().record_abort_failed();
                // Louder than the abort itself: this one leaves parts behind.
                // They are billed, absent from a normal listing, and stay until
                // the bucket's AbortIncompleteMultipartUpload rule reaps them —
                // so the upload_id is the only handle anyone has on them.
                SERVER_LOG_ERROR("S3Storage::multipart",
                                 std::string("abort FAILED — parts are stranded until the "
                                             "bucket lifecycle rule reaps them. key=") +
                                 std::string(key.c_str()) +
                                 " upload_id=" + std::string(upload_id.c_str()) +
                                 " reason=" + aborted.GetError().GetMessage().c_str());
            }
        }
    } abort_guard{s3_client_.get(), Aws::String(bucket_), Aws::String(key), upload_id};

    std::ifstream in(local_path, std::ios::binary);
    if (!in.is_open()) {
        return Result<std::string>::err("Cannot open local file for multipart upload: " + local_path);
    }

    // The file opened, so any failure from here is a transfer failure.
    abort_guard.stage = TransferTracker::Stage::PartFailed;

    Aws::S3::Model::CompletedMultipartUpload completed;
    std::vector<char> buf(static_cast<size_t>(PART_SIZE));
    int part_number = 1;
    std::string err_msg;
    bool ok = true;

    while (in) {
        in.read(buf.data(), PART_SIZE);
        std::streamsize n = in.gcount();
        if (n <= 0) break;

        auto body = Aws::MakeShared<Aws::StringStream>("S3Part", "");
        body->write(buf.data(), n);
        body->flush();

        Aws::S3::Model::UploadPartRequest part_req;
        part_req.SetBucket(Aws::String(bucket_));
        part_req.SetKey(Aws::String(key));
        part_req.SetUploadId(upload_id);
        part_req.SetPartNumber(part_number);
        part_req.SetContentLength(static_cast<long long>(n));
        part_req.SetBody(body);

        auto part_res = s3_client_->UploadPart(part_req);
        if (!part_res.IsSuccess()) {
            ok = false;
            err_msg = part_res.GetError().GetMessage();
            break;
        }
        abort_guard.bytes_sent += static_cast<std::uint64_t>(n);
        Aws::S3::Model::CompletedPart cp;
        cp.SetPartNumber(part_number);
        cp.SetETag(part_res.GetResult().GetETag());
        completed.AddParts(cp);
        ++part_number;
    }

    if (!ok) {
        return Result<std::string>::err("S3 multipart upload failed: " + err_msg);
    }

    // Every part landed; only the commit remains. Distinguished because it is
    // the cruellest failure — the whole transfer succeeded and still produced
    // nothing — and because it points at the store rather than the link.
    abort_guard.stage = TransferTracker::Stage::CompleteFailed;

    Aws::S3::Model::CompleteMultipartUploadRequest complete_req;
    complete_req.SetBucket(Aws::String(bucket_));
    complete_req.SetKey(Aws::String(key));
    complete_req.SetUploadId(upload_id);
    complete_req.SetMultipartUpload(completed);
    auto complete_res = s3_client_->CompleteMultipartUpload(complete_req);
    if (!complete_res.IsSuccess()) {
        // Falls through to the guard, which aborts. This is the path that
        // previously leaked, and the worst one to leak on: every part has
        // uploaded by now, so it strands the whole object.
        return Result<std::string>::err("Failed to complete S3 multipart upload: " +
                                        complete_res.GetError().GetMessage());
    }

    // Committed: the key is now visible and the parts belong to it.
    abort_guard.committed = true;
    TransferTracker::instance().record_completed(abort_guard.bytes_sent);
    return Result<std::string>::ok(key);
#else
    return Result<std::string>::err("AWS SDK not available - S3 storage requires USE_AWS_SDK to be defined");
#endif
}

// Bytes pulled from the object store per read. Matches the local storage read
// size so a restore moves in the same rhythm as a local stream.
static constexpr size_t kObjectReadChunkBytes = 256 * 1024;

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

Result<void> S3Storage::read_file_stream(
        const std::string& storage_path,
        const std::function<bool(const uint8_t*, size_t)>& on_chunk,
        const std::string& tenant) {
    (void)tenant;
    if (!initialized_) {
        return Result<void>::err("S3 storage not initialized");
    }

#ifdef USE_AWS_SDK
    if (!s3_client_) {
        return Result<void>::err("S3 client not initialized");
    }

    Aws::S3::Model::GetObjectRequest request;
    request.SetBucket(Aws::String(bucket_));
    request.SetKey(Aws::String(storage_path));

    auto outcome = s3_client_->GetObject(request);
    if (!outcome.IsSuccess()) {
        return Result<void>::err("Failed to download file from S3: " +
                                 outcome.GetError().GetMessage());
    }

    // GetBody() is an IOStream, so the payload can be pulled through in pieces.
    // read_file() instead slurps it into an ostringstream and then copies that
    // into a vector — two full copies of the object before the caller sees
    // anything, which for a restored file is the whole point of avoiding.
    auto& body = outcome.GetResult().GetBody();
    std::vector<char> buf(kObjectReadChunkBytes);
    while (body.good()) {
        body.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize n = body.gcount();
        if (n <= 0) break;
        if (!on_chunk(reinterpret_cast<const uint8_t*>(buf.data()),
                      static_cast<size_t>(n))) {
            return Result<void>::ok();      // caller aborted; not an error
        }
    }
    return Result<void>::ok();
#else
    (void)storage_path; (void)on_chunk;
    return Result<void>::err("AWS SDK not available - S3 storage requires USE_AWS_SDK to be defined");
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

    // Deletion IS supported, and has to be.
    //
    // This used to be a hard-coded refusal, on the reasoning that S3 objects are
    // immutable for history preservation. That is right as a POLICY for the
    // normal write path — the core never rewrites a version's object, and the
    // version cull deliberately leaves bucket copies alone — but it cannot be a
    // CAPABILITY limit, because erasure has to reach the durable copy. With this
    // refusing, an erasure destroyed the database rows and the local bytes and
    // left the content sitting in the bucket, while the platform reported the
    // file erased. That is the false compliance claim §5.4.4 warns is the worst
    // thing to be wrong about here.
    //
    // Callers that must not delete simply do not call this. Erasure does, and
    // treats a failure as fatal rather than best-effort.
    Aws::S3::Model::DeleteObjectRequest request;
    request.SetBucket(Aws::String(bucket_));
    request.SetKey(Aws::String(storage_path));

    auto outcome = s3_client_->DeleteObject(request);
    if (outcome.IsSuccess()) {
        return Result<void>::ok();
    }
    // S3 DeleteObject is idempotent: deleting a key that is not there succeeds.
    // A NoSuchKey response therefore means somebody else already removed it,
    // which is the outcome we wanted — not a failure to report.
    const auto& err = outcome.GetError();
    if (err.GetExceptionName().find("NoSuchKey") != std::string::npos ||
        err.GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND) {
        return Result<void>::ok();
    }
    return Result<void>::err("Failed to delete object from S3: " +
                             std::string(err.GetExceptionName().c_str()) + " - " +
                             std::string(err.GetMessage().c_str()));
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