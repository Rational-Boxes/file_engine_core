#include "fileengine/file_culler.h"
#include <algorithm>
#include <thread>
#include <chrono>
#include <filesystem>

namespace fileengine {

FileCuller::FileCuller(IStorage* storage, IObjectStore* object_store, StorageTracker* storage_tracker)
    : storage_(storage), object_store_(object_store), storage_tracker_(storage_tracker),
      running_(false), culled_file_count_(0), culled_byte_count_(0) {
    // Default configuration
    config_.threshold_percentage = 0.8;  // 80%
    config_.min_age_days = 0;  // Changed from 30 to 0 for testing purposes
    config_.keep_count = 1;
    config_.enabled = true;
    config_.strategy = "lru";  // least recently used
}

void FileCuller::configure(const CullingConfig& config) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    config_ = config;
}

void FileCuller::start_automatic_culling() {
    if (running_) {
        return;  // Already running
    }
    
    running_ = true;
    culling_thread_ = std::thread(&FileCuller::culling_loop, this);
}

void FileCuller::stop_automatic_culling() {
    if (running_) {
        {
            // Set the flag under the wait mutex so the worker cannot miss the
            // wakeup between checking running_ and blocking on the CV.
            std::lock_guard<std::mutex> wait_lock(cull_wait_mutex_);
            running_ = false;
        }
        cull_wait_cv_.notify_all();
        if (culling_thread_.joinable()) {
            culling_thread_.join();
        }
    }
}

Result<void> FileCuller::perform_culling() {
    if (!config_.enabled) {
        return Result<void>::ok();
    }

    // Check if we should trigger culling based on current usage
    if (!should_trigger_culling()) {
        return Result<void>::ok();
    }

    auto result = cull_low_priority_files();
    return result;
}

Result<void> FileCuller::perform_culling_for_space(size_t required_bytes) {
    if (!config_.enabled) {
        return Result<void>::err("Culling is not enabled");
    }

    // Get current usage to determine how much space we have
    if (!storage_tracker_) {
        return Result<void>::err("Storage tracker not available");
    }

    auto current_usage = storage_tracker_->get_current_usage();
    size_t available_space = current_usage.available_space_bytes;

    // If we already have enough space, no need to cull
    if (available_space >= required_bytes) {
        return Result<void>::ok();
    }

    // Calculate how much space we need to free up
    size_t space_needed = required_bytes - available_space;

    // Get files to cull based on current strategy
    auto candidates = get_culling_candidates(100);  // Get up to 100 candidates

    size_t freed_space = 0;
    for (const auto& file_path : candidates) {
        if (freed_space >= space_needed) {
            break;  // We've freed enough space
        }

        // Verify the file exists in object store before culling
        auto verify_result = verify_file_in_object_store(file_path);
        if (verify_result.success && verify_result.value) {
            // Get file size to know how much space we'll free
            std::error_code ec;
            size_t file_size = std::filesystem::file_size(file_path, ec);
            if (!ec) {
                // Remove from local storage
                if (storage_) {
                    auto delete_result = storage_->delete_file(file_path);
                    if (delete_result.success) {
                        // Update counters
                        culled_file_count_++;
                        culled_byte_count_ += file_size;

                        // Update storage tracker
                        if (storage_tracker_) {
                            storage_tracker_->record_file_deletion(file_path);
                        }

                        freed_space += file_size;
                    }
                }
            }
        }
    }

    if (freed_space < space_needed) {
        return Result<void>::err("Could not free enough space. Requested: " +
                                std::to_string(required_bytes) +
                                " bytes, freed: " + std::to_string(freed_space) + " bytes");
    }

    return Result<void>::ok();
}

size_t FileCuller::get_culled_file_count() const {
    return culled_file_count_.load();
}

size_t FileCuller::get_culled_byte_count() const {
    return culled_byte_count_.load();
}

bool FileCuller::should_cull_file(const std::string& file_path, const std::string& tenant) const {
    // For now, return true if we should cull based on configuration
    // A more sophisticated implementation would check file properties
    return true;
}

void FileCuller::update_config(const CullingConfig& config) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    config_ = config;
}

CullingConfig FileCuller::get_config() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_;
}

void FileCuller::culling_loop() {
    while (running_) {
        // Check if we should perform culling
        if (should_trigger_culling()) {
            perform_culling();
        }
        
        // Sleep before the next pass, but wake immediately on stop so shutdown
        // isn't blocked for up to 5 minutes waiting out this interval.
        std::unique_lock<std::mutex> wait_lock(cull_wait_mutex_);
        cull_wait_cv_.wait_for(wait_lock, std::chrono::minutes(5),
                               [this] { return !running_.load(); });
    }
}

Result<void> FileCuller::cull_low_priority_files() {
    // Get files to cull based on current strategy
    auto candidates = get_culling_candidates(50);  // Cull up to 50 files at a time
    
    for (const auto& file_path : candidates) {
        // Verify the file exists in object store before culling
        auto verify_result = verify_file_in_object_store(file_path);
        if (verify_result.success && verify_result.value) {
            // Remove from local storage
            if (storage_) {
                auto delete_result = storage_->delete_file(file_path);
                if (delete_result.success) {
                    // Update counters
                    culled_file_count_++;
                    
                    // Update storage tracker
                    if (storage_tracker_) {
                        storage_tracker_->record_file_deletion(file_path);
                    }
                }
            }
        }
    }
    
    return Result<void>::ok();
}

std::vector<std::string> FileCuller::get_culling_candidates(int limit, const std::string& tenant) const {
    std::vector<std::string> candidates;
    
    if (storage_tracker_) {
        if (config_.strategy == "lru") {
            // Get least recently accessed files
            auto lru_files = storage_tracker_->get_least_accessed_files(limit, tenant);
            for (const auto& file_usage : lru_files) {
                candidates.push_back(file_usage.file_path);
            }
        } else if (config_.strategy == "lfu") {
            // Get least frequently accessed files
            auto lfu_files = storage_tracker_->get_least_accessed_files(limit, tenant);
            for (const auto& file_usage : lfu_files) {
                candidates.push_back(file_usage.file_path);
            }
        }
    }
    
    return candidates;
}

Result<bool> FileCuller::verify_file_in_object_store(const std::string& file_path, const std::string& tenant) const {
    // Culling deletes the LOCAL copy of a payload. That is only safe when the
    // payload is durably present in the object store; otherwise we would be
    // destroying the sole copy of the data, violating the never-destroy-data
    // invariant. This check therefore FAILS CLOSED: unless we can positively
    // confirm the object exists in the store, we refuse to cull (return false).
    if (!object_store_) {
        return Result<bool>::ok(false);  // no backend to verify against -> never cull
    }

    // Local layout (Storage::get_storage_path):
    //   <base>/[<tenant>/]<l1>/<l2>/<l3>/<uid>/<version_timestamp>
    // Recover (uid, version) from the trailing two path components.
    std::filesystem::path p(file_path);
    std::string version = p.filename().string();
    std::filesystem::path uid_dir = p.parent_path();
    std::string uid = uid_dir.filename().string();
    if (version.empty() || uid.empty()) {
        return Result<bool>::ok(false);  // unparseable path -> never cull
    }

    // Candidates are gathered host-wide, so the caller often has no tenant.
    // Recover it from the path: five levels up from the version file
    // (<tenant>/<l1>/<l2>/<l3>/<uid>/<version>). A wrong guess only makes the
    // existence check miss, which still fails closed (no cull).
    std::string effective_tenant = tenant;
    if (effective_tenant.empty()) {
        std::filesystem::path t =
            uid_dir.parent_path().parent_path().parent_path().parent_path();
        if (!t.empty()) {
            effective_tenant = t.filename().string();
        }
    }

    std::string object_key = object_store_->get_storage_path(uid, version, effective_tenant);
    auto exists = object_store_->file_exists(object_key, effective_tenant);
    if (!exists.success) {
        return Result<bool>::ok(false);  // could not verify (store error) -> never cull
    }
    return Result<bool>::ok(exists.value);
}

bool FileCuller::should_trigger_culling() const {
    if (!config_.enabled || !storage_tracker_) {
        return false;
    }
    
    auto usage = storage_tracker_->get_current_usage();
    return usage.usage_percentage >= (config_.threshold_percentage * 100.0);
}

FileCuller::~FileCuller() {
    stop_automatic_culling();
}

} // namespace fileengine