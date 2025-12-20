#pragma once

#include "types.h"
#include "IStorage.h"
#include "IObjectStore.h"
#include <string>
#include <unordered_map>
#include <list>
#include <memory>
#include <mutex>
#include <chrono>

namespace fileengine {

struct CachedFile {
    std::string path;
    std::vector<uint8_t> data;
    std::chrono::steady_clock::time_point last_accessed;
    size_t size;
    std::string tenant;
};

class CacheManager {
public:
    CacheManager(IStorage* storage, IObjectStore* object_store, double threshold = 0.80); // Default 80% threshold
    ~CacheManager();
    
    // Get a file from cache or load from storage
    Result<std::vector<uint8_t>> get_file(const std::string& storage_path, const std::string& tenant = "");
    
    // Add a file to cache
    Result<void> add_file(const std::string& storage_path, const std::vector<uint8_t>& data, const std::string& tenant = "");
    
    // Remove a file from cache
    Result<void> remove_file(const std::string& storage_path);
    
    // Check if a file is in cache
    bool is_cached(const std::string& storage_path) const;
    
    // Get cache usage statistics
    double get_cache_usage_percentage() const;
    size_t get_cache_size_bytes() const;
    size_t get_max_cache_size_bytes() const;
    
    // Set the cache threshold (0.0 to 1.0)
    void set_cache_threshold(double threshold);
    
    // Manually trigger cache cleanup based on LRU
    Result<void> cleanup_cache();
    
    // Fetch file from object store if not in cache or local storage
    Result<std::vector<uint8_t>> fetch_from_object_store_if_missing(const std::string& uid, 
                                                                   const std::string& version_timestamp, 
                                                                   const std::string& tenant = "");
    
    // Update cache access time for a file
    void update_access_time(const std::string& storage_path);
    
private:
    struct CacheEntry {
        std::list<std::string>::iterator lru_iter;  // Iterator to LRU list position
        CachedFile file;
    };
    
    IStorage* storage_;
    IObjectStore* object_store_;
    std::unordered_map<std::string, CacheEntry> cache_map_;
    std::list<std::string> lru_list_;  // For LRU eviction
    mutable std::mutex cache_mutex_;
    
    size_t current_cache_size_;
    size_t max_cache_size_bytes_;  // Max cache size in bytes (calculated based on threshold and system storage)
    double threshold_;  // Threshold for cache size as percentage of total storage
    
    // Calculate the maximum cache size based on system storage and threshold
    void calculate_max_cache_size();
    
    // Remove least recently used items until cache is below threshold
    void evict_lru_items();
    
    // Helper to get system's available storage space
    size_t get_available_system_storage() const;
};

} // namespace fileengine