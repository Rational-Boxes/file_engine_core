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

#pragma once

#include "types.h"
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <mutex>

namespace fileengine {

struct StorageUsage {
    size_t total_space_bytes;
    size_t used_space_bytes;
    size_t available_space_bytes;
    double usage_percentage;
    std::chrono::steady_clock::time_point last_updated;
};

struct FileUsage {
    std::string file_path;
    size_t size_bytes;
    std::chrono::steady_clock::time_point last_accessed;
    std::chrono::steady_clock::time_point last_modified;
    std::string tenant;
    int access_count;
};

class StorageTracker {
public:
    StorageTracker(const std::string& base_path);
    
    // Get current storage usage for the host
    StorageUsage get_current_usage();
    
    // Get storage usage for a specific tenant
    StorageUsage get_tenant_usage(const std::string& tenant);
    
    // Track file operations
    void record_file_creation(const std::string& file_path, size_t size, const std::string& tenant = "");
    void record_file_access(const std::string& file_path, const std::string& tenant = "");
    void record_file_modification(const std::string& file_path, size_t new_size, const std::string& tenant = "");
    void record_file_deletion(const std::string& file_path, const std::string& tenant = "");
    
    // Update overall usage statistics
    void update_usage_stats();
    
    // Get most accessed files
    std::vector<FileUsage> get_most_accessed_files(int limit = 10, const std::string& tenant = "");
    
    // Get least accessed files
    std::vector<FileUsage> get_least_accessed_files(int limit = 10, const std::string& tenant = "");
    
    // Get largest files
    std::vector<FileUsage> get_largest_files(int limit = 10, const std::string& tenant = "");
    
    // Get storage report
    std::map<std::string, StorageUsage> get_tenant_storage_report();
    
    // Get overall storage report
    StorageUsage get_overall_storage_report();

    // Live status of the node/filesystem the files are cached on (fresh statvfs).
    // This is host-level (NOT tenant-scoped) — the disk the store lives on.
    StorageUsage get_node_usage() const;

private:
    std::string base_path_;
    std::map<std::string, FileUsage> file_usage_map_;  // path -> FileUsage
    std::map<std::string, StorageUsage> tenant_usage_map_;  // tenant -> StorageUsage
    StorageUsage overall_usage_;
    mutable std::mutex usage_mutex_;
    
    // Helper to get filesystem stats
    StorageUsage get_filesystem_stats() const;
    
    // Helper to scan directory and calculate usage
    size_t calculate_directory_usage(const std::string& dir_path) const;
    
    // Helper to update tenant usage
    void update_tenant_usage(const std::string& tenant);

private:
    // Helper to initialize from existing files on startup
    void initialize_from_existing_files();
};

} // namespace fileengine