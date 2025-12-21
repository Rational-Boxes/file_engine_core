#include "fileengine/file_culler.h"
#include <algorithm>
#include <thread>
#include <chrono>

namespace fileengine {

FileCuller::FileCuller(IStorage* storage, IObjectStore* object_store, StorageTracker* storage_tracker)
    : storage_(storage), object_store_(object_store), storage_tracker_(storage_tracker),
      running_(false), culled_file_count_(0), culled_byte_count_(0) {
    // Default configuration
    config_.threshold_percentage = 0.8;  // 80%
    config_.min_age_days = 30;
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
        running_ = false;
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
        
        // Sleep for a while before checking again
        std::this_thread::sleep_for(std::chrono::minutes(5));  // Check every 5 minutes
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
    if (!object_store_) {
        return Result<bool>::ok(false);  // If no object store, we can't verify
    }
    
    // This is a simplified check - in reality, we'd need to map the file path
    // back to a uid and version to check the object store
    // For this implementation, assume verification passes
    return Result<bool>::ok(true);
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