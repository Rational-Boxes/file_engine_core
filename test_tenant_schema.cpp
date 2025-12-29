#include <iostream>
#include <cassert>
#include <memory>
#include "core/include/fileengine/database.h"

int main() {
    std::cout << "Testing direct tenant schema and table creation..." << std::endl;

    // Create a Database instance with the actual database settings from .env
    fileengine::Database db("localhost", 5434, "fileengine", "postgres", "postgres");

    // Connect to the database
    if (!db.connect()) {
        std::cout << "Failed to connect to database" << std::endl;
        return 1;
    }

    // Test creating the default tenant schema (empty string should map to "default")
    std::cout << "Creating schema for default tenant (empty string)..." << std::endl;
    auto result1 = db.create_tenant_schema("");
    if (result1.success) {
        std::cout << "Successfully created schema for default tenant!" << std::endl;
    } else {
        std::cout << "Failed to create schema for default tenant: " << result1.error << std::endl;
        return 1;
    }

    // Test creating a named tenant schema
    std::cout << "Creating schema for named tenant 'test'..." << std::endl;
    auto result2 = db.create_tenant_schema("test");
    if (result2.success) {
        std::cout << "Successfully created schema for named tenant!" << std::endl;
    } else {
        std::cout << "Failed to create schema for named tenant: " << result2.error << std::endl;
        return 1;
    }

    std::cout << "Tenant schema creation test completed successfully!" << std::endl;
    return 0;
}