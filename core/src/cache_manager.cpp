#include "fileengine/cache_manager.h"
#include <sys/statvfs.h>
#include <algorithm>

// TODO: Look out for race conditions in this operation, implement a semephore because culling down to the storage limit nees to be a critical section of code

namespace fileengine {

CacheManager::CacheManager(IStorage* storage, IObjectStore* object_store, double threshold)
    : storage_(storage), object_store_(object_store), current_cache_size_(0), 
      max_cache_size_bytes_(0), threshold_(threshold) {
    calculate_max_cache_size();
}

CacheManager::~CacheManager() {
    // Cleanup cache
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_map_.clear();
    lru_list_.clear();
}

Result<std::vector<uint8_t>> CacheManager::get_file(const std::string& storage_path, const std::string& tenant) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    auto it = cache_map_.find(storage_path);
    if (it != cache_map_.end()) {
        // Update LRU position
        lru_list_.erase(it->second.lru_iter);
        lru_list_.push_front(storage_path);
        it->second.lru_iter = lru_list_.begin();
        
        // Update access time
        it->second.file.last_accessed = std::chrono::steady_clock::now();
        
        return Result<std::vector<uint8_t>>::ok(it->second.file.data);
    }
    
    // File not in cache, try to load from storage
    if (storage_) {
        auto storage_result = storage_->read_file(storage_path, tenant);
        if (storage_result.success) {
            // Add to cache
            add_file(storage_path, storage_result.value, tenant);
            return storage_result;
        }
    }
    
    // If not in storage, try object store
    if (object_store_) {
        auto object_store_result = object_store_->read_file(storage_path, tenant);
        if (object_store_result.success) {
            // Add to cache
            add_file(storage_path, object_store_result.value, tenant);
            return object_store_result;
        }
    }
    
    return Result<std::vector<uint8_t>>::err("File not found in cache, storage, or object store: " + storage_path);
}

Result<void> CacheManager::add_file(const std::string& storage_path, const std::vector<uint8_t>& data, const std::string& tenant) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    size_t data_size = data.size();
    
    // Check if we need to evict items to make space
    while (current_cache_size_ + data_size > max_cache_size_bytes_ * threshold_ && !lru_list_.empty()) {
        // Remove least recently used item
        std::string lru_path = lru_list_.back();
        lru_list_.pop_back();
        
        auto lru_it = cache_map_.find(lru_path);
        if (lru_it != cache_map_.end()) {
            current_cache_size_ -= lru_it->second.file.size;
            cache_map_.erase(lru_it);
        }
    }
    
    // Check if we still have space
    if (current_cache_size_ + data_size <= max_cache_size_bytes_) {
        // Add new item
        auto& entry = cache_map_[storage_path];
        entry.file.path = storage_path;
        entry.file.data = data;
        entry.file.size = data_size;
        entry.file.tenant = tenant;
        entry.file.last_accessed = std::chrono::steady_clock::now();
        
        lru_list_.push_front(storage_path);
        entry.lru_iter = lru_list_.begin();
        
        current_cache_size_ += data_size;
        
        return Result<void>::ok();
    }
    
    // If still no space, evict more items (this can happen in race conditions)
    evict_lru_items();
    
    if (current_cache_size_ + data_size <= max_cache_size_bytes_) {
        // Try to add again
        auto& entry = cache_map_[storage_path];
        entry.file.path = storage_path;
        entry.file.data = data;
        entry.file.size = data_size;
        entry.file.tenant = tenant;
        entry.file.last_accessed = std::chrono::steady_clock::now();
        
        lru_list_.push_front(storage_path);
        entry.lru_iter = lru_list_.begin();
        
        current_cache_size_ += data_size;
        
        return Result<void>::ok();
    }
    
    return Result<void>::err("Not enough space in cache even after eviction");
}

Result<void> CacheManager::remove_file(const std::string& storage_path) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    auto it = cache_map_.find(storage_path);
    if (it == cache_map_.end()) {
        return Result<void>::ok();  // File not in cache, nothing to remove
    }
    
    // Update size
    current_cache_size_ -= it->second.file.size;
    
    // Remove from LRU list
    lru_list_.erase(it->second.lru_iter);
    
    // Remove from cache map
    cache_map_.erase(it);
    
    return Result<void>::ok();
}

bool CacheManager::is_cached(const std::string& storage_path) const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    return cache_map_.find(storage_path) != cache_map_.end();
}

double CacheManager::get_cache_usage_percentage() const {
    if (max_cache_size_bytes_ == 0) return 0.0;
    return static_cast<double>(current_cache_size_) / static_cast<double>(max_cache_size_bytes_);
}

size_t CacheManager::get_cache_size_bytes() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    return current_cache_size_;
}

size_t CacheManager::get_max_cache_size_bytes() const {
    return max_cache_size_bytes_;
}

void CacheManager::set_cache_threshold(double threshold) {
    if (threshold >= 0.0 && threshold <= 1.0) {
        threshold_ = threshold;
        // Trigger cleanup if we're over the new threshold
        if (get_cache_usage_percentage() > threshold_) {
            cleanup_cache();
        }
    }
}

Result<void> CacheManager::cleanup_cache() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    // Evict LRU items until we're below the threshold
    evict_lru_items();
    
    return Result<void>::ok();
}

Result<std::vector<uint8_t>> CacheManager::fetch_from_object_store_if_missing(const std::string& uid, 
                                                                              const std::string& version_timestamp, 
                                                                              const std::string& tenant) {
    // Generate the storage path
    std::string storage_path;
    if (storage_) {
        storage_path = storage_->get_storage_path(uid, version_timestamp, tenant);
    } else {
        // Create a dummy path based on uid and version
        storage_path = uid + "/" + version_timestamp;
    }
    
    // First check if we have it in cache
    auto cache_result = get_file(storage_path, tenant);
    if (cache_result.success) {
        return cache_result;
    }
    
    // If not in cache, try to get from object store
    if (object_store_) {
        auto obj_store_path = object_store_->get_storage_path(uid, version_timestamp, tenant);
        auto obj_result = object_store_->read_file(obj_store_path, tenant);
        
        if (obj_result.success) {
            // Add to cache and return
            add_file(storage_path, obj_result.value, tenant);
            return obj_result;
        }
    }
    
    return Result<std::vector<uint8_t>>::err("File not found in cache or object store");
}

void CacheManager::update_access_time(const std::string& storage_path) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    auto it = cache_map_.find(storage_path);
    if (it != cache_map_.end()) {
        // Update LRU position
        lru_list_.erase(it->second.lru_iter);
        lru_list_.push_front(storage_path);
        it->second.lru_iter = lru_list_.begin();
        
        // Update access time
        it->second.file.last_accessed = std::chrono::steady_clock::now();
    }
}

void CacheManager::calculate_max_cache_size() {
    max_cache_size_bytes_ = get_available_system_storage();
}

size_t CacheManager::get_available_system_storage() const {
    struct statvfs fs_info;
    
    // Use a default path, or get from configuration in a real implementation
    const char* path = "/tmp";  // This would be configurable in a real system
    if (statvfs(path, &fs_info) == 0) {
        // Return total space (would be configurable in reality)
        return static_cast<size_t>(fs_info.f_blocks) * fs_info.f_frsize;
    }
    
    // Default to 1 GB if we can't determine
    return 1024 * 1024 * 1024; // 1 GB
}

void CacheManager::evict_lru_items() {
    // Evict items until we're below the threshold
    while (!lru_list_.empty() && 
           current_cache_size_ > max_cache_size_bytes_ * threshold_) {
        
        std::string lru_path = lru_list_.back();
        lru_list_.pop_back();
        
        auto lru_it = cache_map_.find(lru_path);
        if (lru_it != cache_map_.end()) {
            current_cache_size_ -= lru_it->second.file.size;
            cache_map_.erase(lru_it);
        }
    }
}

} // namespace fileengine