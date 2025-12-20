#include "fileengine/storage_tracker.h"
#include <sys/statvfs.h>
#include <filesystem>
#include <algorithm>

namespace fileengine {

StorageTracker::StorageTracker(const std::string& base_path) : base_path_(base_path) {
    overall_usage_ = get_filesystem_stats();
    update_usage_stats();
}

StorageUsage StorageTracker::get_current_usage() {
    std::lock_guard<std::mutex> lock(usage_mutex_);
    return overall_usage_;
}

StorageUsage StorageTracker::get_tenant_usage(const std::string& tenant) {
    std::lock_guard<std::mutex> lock(usage_mutex_);
    
    auto it = tenant_usage_map_.find(tenant);
    if (it != tenant_usage_map_.end()) {
        return it->second;
    }
    
    // If tenant not found, return empty usage
    StorageUsage empty_usage = {};
    return empty_usage;
}

void StorageTracker::record_file_creation(const std::string& file_path, size_t size, const std::string& tenant) {
    std::lock_guard<std::mutex> lock(usage_mutex_);
    
    FileUsage file_usage;
    file_usage.file_path = file_path;
    file_usage.size_bytes = size;
    file_usage.last_accessed = std::chrono::steady_clock::now();
    file_usage.last_modified = std::chrono::steady_clock::now();
    file_usage.tenant = tenant;
    file_usage.access_count = 1;
    
    file_usage_map_[file_path] = file_usage;
    
    // Update overall and tenant usage
    overall_usage_.used_space_bytes += size;
    update_tenant_usage(tenant);
}

void StorageTracker::record_file_access(const std::string& file_path, const std::string& tenant) {
    std::lock_guard<std::mutex> lock(usage_mutex_);
    
    auto it = file_usage_map_.find(file_path);
    if (it != file_usage_map_.end()) {
        it->second.last_accessed = std::chrono::steady_clock::now();
        it->second.access_count++;
    }
}

void StorageTracker::record_file_modification(const std::string& file_path, size_t new_size, const std::string& tenant) {
    std::lock_guard<std::mutex> lock(usage_mutex_);
    
    auto it = file_usage_map_.find(file_path);
    if (it != file_usage_map_.end()) {
        // Update size difference
        size_t old_size = it->second.size_bytes;
        overall_usage_.used_space_bytes = (overall_usage_.used_space_bytes - old_size) + new_size;
        
        it->second.size_bytes = new_size;
        it->second.last_modified = std::chrono::steady_clock::now();
        it->second.last_accessed = std::chrono::steady_clock::now();
        it->second.access_count++;
    } else {
        // File not in tracking, add it
        record_file_creation(file_path, new_size, tenant);
    }
    
    update_tenant_usage(tenant);
}

void StorageTracker::record_file_deletion(const std::string& file_path, const std::string& tenant) {
    std::lock_guard<std::mutex> lock(usage_mutex_);
    
    auto it = file_usage_map_.find(file_path);
    if (it != file_usage_map_.end()) {
        // Remove from usage
        overall_usage_.used_space_bytes -= it->second.size_bytes;
        
        // Remove from tenant usage
        if (!it->second.tenant.empty()) {
            auto tenant_it = tenant_usage_map_.find(it->second.tenant);
            if (tenant_it != tenant_usage_map_.end()) {
                tenant_it->second.used_space_bytes -= it->second.size_bytes;
                tenant_it->second.usage_percentage = 
                    static_cast<double>(tenant_it->second.used_space_bytes) / 
                    static_cast<double>(tenant_it->second.total_space_bytes) * 100.0;
            }
        }
        
        file_usage_map_.erase(it);
    }
}

void StorageTracker::update_usage_stats() {
    std::lock_guard<std::mutex> lock(usage_mutex_);
    
    // Get current filesystem stats
    overall_usage_ = get_filesystem_stats();
    
    // Calculate usage for all files we're tracking
    size_t total_tracked_usage = 0;
    std::map<std::string, size_t> tenant_sizes;
    
    for (const auto& [path, usage] : file_usage_map_) {
        total_tracked_usage += usage.size_bytes;
        tenant_sizes[usage.tenant] += usage.size_bytes;
    }
    
    overall_usage_.used_space_bytes = total_tracked_usage;
    overall_usage_.usage_percentage = 
        static_cast<double>(total_tracked_usage) / 
        static_cast<double>(overall_usage_.total_space_bytes) * 100.0;
    
    // Update tenant usage stats
    for (const auto& [tenant, size] : tenant_sizes) {
        auto& tenant_usage = tenant_usage_map_[tenant];
        tenant_usage.total_space_bytes = overall_usage_.total_space_bytes; // Same filesystem
        tenant_usage.used_space_bytes = size;
        tenant_usage.available_space_bytes = overall_usage_.available_space_bytes;
        tenant_usage.usage_percentage = 
            static_cast<double>(size) / 
            static_cast<double>(overall_usage_.total_space_bytes) * 100.0;
        tenant_usage.last_updated = std::chrono::steady_clock::now();
    }
}

std::vector<FileUsage> StorageTracker::get_most_accessed_files(int limit, const std::string& tenant) {
    std::lock_guard<std::mutex> lock(usage_mutex_);
    
    std::vector<FileUsage> files;
    for (const auto& [path, usage] : file_usage_map_) {
        if (tenant.empty() || usage.tenant == tenant) {
            files.push_back(usage);
        }
    }
    
    // Sort by access count (descending)
    std::sort(files.begin(), files.end(), 
              [](const FileUsage& a, const FileUsage& b) {
                  return a.access_count > b.access_count;
              });
    
    if (files.size() > static_cast<size_t>(limit)) {
        files.resize(limit);
    }
    
    return files;
}

std::vector<FileUsage> StorageTracker::get_least_accessed_files(int limit, const std::string& tenant) {
    std::lock_guard<std::mutex> lock(usage_mutex_);
    
    std::vector<FileUsage> files;
    for (const auto& [path, usage] : file_usage_map_) {
        if (tenant.empty() || usage.tenant == tenant) {
            files.push_back(usage);
        }
    }
    
    // Sort by access count (ascending)
    std::sort(files.begin(), files.end(), 
              [](const FileUsage& a, const FileUsage& b) {
                  return a.access_count < b.access_count;
              });
    
    if (files.size() > static_cast<size_t>(limit)) {
        files.resize(limit);
    }
    
    return files;
}

std::vector<FileUsage> StorageTracker::get_largest_files(int limit, const std::string& tenant) {
    std::lock_guard<std::mutex> lock(usage_mutex_);
    
    std::vector<FileUsage> files;
    for (const auto& [path, usage] : file_usage_map_) {
        if (tenant.empty() || usage.tenant == tenant) {
            files.push_back(usage);
        }
    }
    
    // Sort by size (descending)
    std::sort(files.begin(), files.end(), 
              [](const FileUsage& a, const FileUsage& b) {
                  return a.size_bytes > b.size_bytes;
              });
    
    if (files.size() > static_cast<size_t>(limit)) {
        files.resize(limit);
    }
    
    return files;
}

std::map<std::string, StorageUsage> StorageTracker::get_tenant_storage_report() {
    std::lock_guard<std::mutex> lock(usage_mutex_);
    return tenant_usage_map_;
}

StorageUsage StorageTracker::get_overall_storage_report() {
    std::lock_guard<std::mutex> lock(usage_mutex_);
    return overall_usage_;
}

StorageUsage StorageTracker::get_filesystem_stats() const {
    struct statvfs fs_info;
    
    if (statvfs(base_path_.c_str(), &fs_info) == 0) {
        StorageUsage usage;
        usage.total_space_bytes = static_cast<size_t>(fs_info.f_blocks) * fs_info.f_frsize;
        usage.available_space_bytes = static_cast<size_t>(fs_info.f_bavail) * fs_info.f_frsize;
        usage.used_space_bytes = usage.total_space_bytes - usage.available_space_bytes;
        usage.usage_percentage = 
            static_cast<double>(usage.used_space_bytes) / 
            static_cast<double>(usage.total_space_bytes) * 100.0;
        usage.last_updated = std::chrono::steady_clock::now();
        
        return usage;
    }
    
    // Return empty if unable to get stats
    StorageUsage empty_usage = {};
    return empty_usage;
}

size_t StorageTracker::calculate_directory_usage(const std::string& dir_path) const {
    size_t total_size = 0;
    
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_path)) {
            if (entry.is_regular_file()) {
                total_size += entry.file_size();
            }
        }
    } catch (const std::exception& e) {
        // Handle error as appropriate
    }
    
    return total_size;
}

void StorageTracker::update_tenant_usage(const std::string& tenant) {
    if (tenant.empty()) return;
    
    auto& tenant_usage = tenant_usage_map_[tenant];
    tenant_usage.total_space_bytes = overall_usage_.total_space_bytes;
    tenant_usage.available_space_bytes = overall_usage_.available_space_bytes;
    tenant_usage.last_updated = std::chrono::steady_clock::now();
    
    // Recalculate tenant usage
    size_t tenant_used = 0;
    for (const auto& [path, usage] : file_usage_map_) {
        if (usage.tenant == tenant) {
            tenant_used += usage.size_bytes;
        }
    }
    
    tenant_usage.used_space_bytes = tenant_used;
    tenant_usage.usage_percentage = 
        static_cast<double>(tenant_used) / 
        static_cast<double>(tenant_usage.total_space_bytes) * 100.0;
}

} // namespace fileengine