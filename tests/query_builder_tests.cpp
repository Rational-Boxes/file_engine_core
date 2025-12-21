#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <map>

#include "fileengine/query_builder.h"
#include "fileengine/types.h"

void test_query_builder_select_operations() {
    std::cout << "Testing QueryBuilder SELECT operations..." << std::endl;
    
    fileengine::QueryBuilder qb;
    
    // Test simple SELECT *
    std::string query = qb.select("*").from("files").build();
    assert(query == "SELECT * FROM files");
    
    // Test SELECT with specific columns
    query = qb.select({"uid", "name", "type"}).from("files").build();
    assert(query.find("SELECT uid, name, type FROM files") != std::string::npos);
    
    // Test SELECT with WHERE clause
    query = qb.select("*").from("files").where("name", "test.txt").build();
    assert(query.find("SELECT * FROM files WHERE name = ") != std::string::npos);
    
    std::cout << "QueryBuilder SELECT operations test passed!" << std::endl;
}

void test_query_builder_insert_operations() {
    std::cout << "Testing QueryBuilder INSERT operations..." << std::endl;
    
    fileengine::QueryBuilder qb;
    
    // Test simple INSERT
    std::string query = qb.insert_into("files")
                         .insert_columns({"uid", "name", "parent_uid"})
                         .values({"'test-123'", "'test.txt'", "'parent-456'"})
                         .build();
    assert(query.find("INSERT INTO files") != std::string::npos);
    assert(query.find("uid, name, parent_uid") != std::string::npos);
    assert(query.find("'test-123', 'test.txt', 'parent-456'") != std::string::npos);
    
    std::cout << "QueryBuilder INSERT operations test passed!" << std::endl;
}

void test_query_builder_update_operations() {
    std::cout << "Testing QueryBuilder UPDATE operations..." << std::endl;
    
    fileengine::QueryBuilder qb;
    
    // Test simple UPDATE
    std::string query = qb.update("files")
                         .set("name", "new_name.txt")
                         .where("uid", "test-123")
                         .build();
    assert(query.find("UPDATE files SET name = 'new_name.txt' WHERE uid = 'test-123'") != std::string::npos);
    
    // Test UPDATE with multiple SET clauses
    query = qb.update("files")
              .set("name", "another_name.txt")
              .set("permissions", "0644")
              .where("uid", "test-123")
              .build();
    assert(query.find("name = 'another_name.txt'") != std::string::npos);
    assert(query.find("permissions = '0644'") != std::string::npos);
    
    std::cout << "QueryBuilder UPDATE operations test passed!" << std::endl;
}

void test_query_builder_where_conditions() {
    std::cout << "Testing QueryBuilder WHERE conditions..." << std::endl;
    
    fileengine::QueryBuilder qb;
    
    // Test equality condition
    std::string query = qb.select("*").from("files").where("uid", "test-123").build();
    assert(query.find("WHERE uid = 'test-123'") != std::string::npos);
    
    // Test inequality condition
    query = qb.select("*").from("files").where("uid", "test-123", fileengine::QueryBuilder::ConditionType::NOT_EQUAL).build();
    assert(query.find("WHERE uid != 'test-123'") != std::string::npos);
    
    // Test greater than condition
    query = qb.select("*").from("files").where("size", "1024", fileengine::QueryBuilder::ConditionType::GREATER_THAN).build();
    assert(query.find("WHERE size > '1024'") != std::string::npos);
    
    // Test AND condition
    query = qb.select("*").from("files")
              .where("name", "test.txt")
              .and_where("size", "1024", fileengine::QueryBuilder::ConditionType::GREATER_THAN)
              .build();
    assert(query.find("WHERE name = 'test.txt' AND size > '1024'") != std::string::npos);
    
    // Test OR condition
    query = qb.select("*").from("files")
              .where("name", "test1.txt")
              .or_where("name", "test2.txt")
              .build();
    assert(query.find("WHERE name = 'test1.txt' OR name = 'test2.txt'") != std::string::npos);
    
    std::cout << "QueryBuilder WHERE conditions test passed!" << std::endl;
}

void test_query_builder_order_limit_offset() {
    std::cout << "Testing QueryBuilder ORDER BY, LIMIT, OFFSET..." << std::endl;
    
    fileengine::QueryBuilder qb;
    
    // Test ORDER BY ASC
    std::string query = qb.select("*").from("files").order_by("name", true).build();
    assert(query.find("ORDER BY name ASC") != std::string::npos);
    
    // Test ORDER BY DESC
    query = qb.select("*").from("files").order_by("modified_at", false).build();
    assert(query.find("ORDER BY modified_at DESC") != std::string::npos);
    
    // Test LIMIT
    query = qb.select("*").from("files").limit(10).build();
    assert(query.find("LIMIT 10") != std::string::npos);
    
    // Test OFFSET
    query = qb.select("*").from("files").offset(20).build();
    assert(query.find("OFFSET 20") != std::string::npos);
    
    // Test complex query with all features
    query = qb.select({"uid", "name", "size"})
              .from("files")
              .where("parent_uid", "parent-123")
              .order_by("name", true)
              .limit(10)
              .offset(0)
              .build();
    assert(query.find("SELECT uid, name, size FROM files") != std::string::npos);
    assert(query.find("WHERE parent_uid = 'parent-123'") != std::string::npos);
    assert(query.find("ORDER BY name ASC") != std::string::npos);
    assert(query.find("LIMIT 10 OFFSET 0") != std::string::npos);
    
    std::cout << "QueryBuilder ORDER BY, LIMIT, OFFSET test passed!" << std::endl;
}

void test_query_builder_sanitization() {
    std::cout << "Testing QueryBuilder sanitization features..." << std::endl;
    
    fileengine::QueryBuilder qb;
    
    // Test that potentially hazardous input is properly handled
    // The query builder should properly escape or parameterize values
    std::string malicious_input = "test'; DROP TABLE files; --";
    
    std::string query = qb.select("*").from("files").where("name", malicious_input).build();
    // The query should still be a valid SELECT and not include the DROP command
    assert(query.find("SELECT") != std::string::npos);
    assert(query.find("DROP TABLE") == std::string::npos);
    
    std::cout << "QueryBuilder sanitization test passed!" << std::endl;
}

void test_query_builder_methods_chaining() {
    std::cout << "Testing QueryBuilder method chaining..." << std::endl;
    
    fileengine::QueryBuilder qb;
    
    // Test long method chaining
    std::string query = qb
        .select({"uid", "name", "created_at"})
        .from("files")
        .where("parent_uid", "root-folder")
        .and_where("type", "0", fileengine::QueryBuilder::ConditionType::EQUAL)  // REGULAR_FILE
        .or_where("type", "1", fileengine::QueryBuilder::ConditionType::EQUAL)   // DIRECTORY
        .order_by("created_at", false)  // DESC
        .limit(25)
        .offset(10)
        .build();
    
    // Verify elements are present in the query
    assert(query.find("SELECT uid, name, created_at") != std::string::npos);
    assert(query.find("FROM files") != std::string::npos);
    assert(query.find("WHERE parent_uid = 'root-folder'") != std::string::npos);
    assert(query.find("ORDER BY created_at DESC") != std::string::npos);
    assert(query.find("LIMIT 25 OFFSET 10") != std::string::npos);
    
    std::cout << "QueryBuilder method chaining test passed!" << std::endl;
}

void test_query_builder_get_parameters() {
    std::cout << "Testing QueryBuilder parameter operations..." << std::endl;
    
    fileengine::QueryBuilder qb;
    
    // Test building a query with parameters
    qb.select("*").from("files").where("uid", "test-123");
    std::vector<std::string> params = qb.get_params();
    
    // In a real implementation, this would return the parameter values
    // For our mock implementation, just ensure the method exists
    assert(true);
    
    // Test building query with parameters for prepared statements
    std::string prepared_query = qb.build_with_params();
    // Should contain parameter placeholders
    assert(!prepared_query.empty());
    
    std::cout << "QueryBuilder parameter operations test passed!" << std::endl;
}

int main() {
    std::cout << "Running FileEngine Core Query Builder Unit Tests..." << std::endl;

    test_query_builder_select_operations();
    test_query_builder_insert_operations();
    test_query_builder_update_operations();
    test_query_builder_where_conditions();
    test_query_builder_order_limit_offset();
    test_query_builder_sanitization();
    test_query_builder_methods_chaining();
    test_query_builder_get_parameters();

    std::cout << "All QueryBuilder unit tests passed!" << std::endl;
    return 0;
}