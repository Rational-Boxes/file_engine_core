#include <iostream>
#include <vector>
#include <string>
#include <filesystem>

int main() {
    std::string base_path = "/home/telendry/temp/fileengine";
    std::string tenant = "default";  // empty would also be "default"
    
    std::string search_path = base_path;
    if (!tenant.empty()) {
        search_path += "/" + tenant;
    }
    
    std::cout << "Searching in path: " << search_path << std::endl;
    
    std::vector<std::string> paths;
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(search_path)) {
            if (entry.is_regular_file()) {
                paths.push_back(entry.path().string());
                std::cout << "Found file: " << entry.path().string() << std::endl;
                
                // Only show first 5 files to avoid too much output
                if (paths.size() >= 5) {
                    std::cout << "... and more files" << std::endl;
                    break;
                }
            }
        }
    } catch (const std::exception& ex) {
        std::cout << "Error: " << ex.what() << std::endl;
        return 1;
    }
    
    std::cout << "Total files found: " << paths.size() << std::endl;
    
    // Now let's test the parsing logic
    if (!paths.empty()) {
        std::string path = paths[0];
        std::cout << "\nTesting path parsing for: " << path << std::endl;
        
        size_t tenant_pos = path.find(tenant.empty() ? "default" : tenant);
        if (tenant_pos != std::string::npos) {
            std::cout << "Found tenant in path at position: " << tenant_pos << std::endl;
            
            size_t last_slash = path.find_last_of('/');
            if (last_slash != std::string::npos) {
                std::string version_timestamp = path.substr(last_slash + 1);
                std::cout << "Version timestamp: " << version_timestamp << std::endl;
                
                size_t second_last_slash = path.substr(0, last_slash).find_last_of('/');
                if (second_last_slash != std::string::npos) {
                    std::string uid = path.substr(second_last_slash + 1, last_slash - second_last_slash - 1);
                    std::cout << "Extracted UID: " << uid << std::endl;
                }
            }
        } else {
            std::cout << "Tenant not found in path!" << std::endl;
        }
    }
    
    return 0;
}