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