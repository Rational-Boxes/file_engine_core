#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>

namespace fileengine {

// File types
enum class FileType {
    REGULAR_FILE,
    DIRECTORY,
    SYMLINK
};

// File metadata - updated to use UUID instead of path for file identification
struct FileInfo {
    std::string uid;              // UUID of the file/directory (primary identifier)
    std::string path;             // Path for backward compatibility or path-based operations
    std::string name;
    std::string parent_uid;       // UUID of the parent directory
    FileType type;
    int64_t size;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point modified_at;
    std::string version;          // Version as timestamp string instead of integer
    std::string owner;
    int permissions;
};

// Directory entry - updated to use UUID for identification
struct DirectoryEntry {
    std::string uid;              // UUID of the entry
    std::string name;
    FileType type;
    int64_t size;
    int64_t created_at;           // Unix timestamp for creation time
    int64_t modified_at;          // Unix timestamp for modification time
    int32_t version_count;        // Number of versions for files
};

// Result types
template<typename T>
struct Result {
    bool success;
    T value;
    std::string error;

    static Result<T> ok(const T& val) {
        return Result<T>{true, val, ""};
    }

    static Result<T> err(const std::string& error_msg) {
        return Result<T>{false, T{}, error_msg};
    }
};

// Specialization for void
template<>
struct Result<void> {
    bool success;
    std::string error;

    static Result<void> ok() {
        return Result<void>{true, ""};
    }

    static Result<void> err(const std::string& error_msg) {
        return Result<void>{false, error_msg};
    }
};

} // namespace fileengine