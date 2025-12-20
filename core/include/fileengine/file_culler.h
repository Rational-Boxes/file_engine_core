#pragma once

#include "types.h"
#include "IStorage.h"
#include "IObjectStore.h"
#include "storage_tracker.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>

namespace fileengine {

struct CullingConfig {
    double threshold_percentage;  // Percentage of storage at which to start culling (e.g., 0.8 for 80%)
    int min_age_days;            // Minimum age of files to consider for culling
    int keep_count;              // Minimum number of versions to keep
    bool enabled;                // Whether culling is enabled
    std::string strategy;        // "lru" (least recently used) or "lfu" (least frequently used)
};

class FileCuller {
public:
    FileCuller(IStorage* storage, IObjectStore* object_store, StorageTracker* storage_tracker);
    
    // Configure culling parameters
    void configure(const CullingConfig& config);
    
    // Start automatic culling (runs in background thread)
    void start_automatic_culling();
    
    // Stop automatic culling
    void stop_automatic_culling();
    
    // Perform a manual culling operation
    Result<void> perform_culling();
    
    // Get culling statistics
    size_t get_culled_file_count() const;
    size_t get_culled_byte_count() const;
    
    // Check if a file should be culled based on current configuration
    bool should_cull_file(const std::string& file_path, const std::string& tenant = "") const;
    
    // Update culling configuration
    void update_config(const CullingConfig& config);
    
    // Get current culling configuration
    CullingConfig get_config() const;

    ~FileCuller();

private:
    IStorage* storage_;
    IObjectStore* object_store_;
    StorageTracker* storage_tracker_;
    CullingConfig config_;

    std::thread culling_thread_;
    bool running_;
    mutable std::mutex config_mutex_;
    std::atomic<size_t> culled_file_count_;
    std::atomic<size_t> culled_byte_count_;

    // The main culling loop
    void culling_loop();

    // Internal culling method
    Result<void> cull_low_priority_files();

    // Get candidate files for culling based on strategy
    std::vector<std::string> get_culling_candidates(int limit = 100, const std::string& tenant = "") const;

    // Verify file exists in object store before culling
    Result<bool> verify_file_in_object_store(const std::string& file_path, const std::string& tenant = "") const;

    // Check if culling should be triggered based on current usage
    bool should_trigger_culling() const;
};

} // namespace fileengine