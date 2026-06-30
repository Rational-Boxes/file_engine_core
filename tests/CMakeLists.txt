# Define test executables
add_executable(test_acl_group_role_permissions test_acl_group_role_permissions.cpp)

# Link libraries for the ACL test
target_link_libraries(test_acl_group_role_permissions
    fileengine_core
    ${LIBPQ_LIBRARIES}
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${GRPCPP_LIBRARIES}
    proto_lib
    ${UUID_LIBRARIES}
    ${CURL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)

if(AWSSDK_FOUND)
    target_link_libraries(test_acl_group_role_permissions ${AWSSDK_LINK_LIBRARIES})
endif()

target_include_directories(test_acl_group_role_permissions PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
    ${CMAKE_CURRENT_BINARY_DIR}/../core/generated/fileengine
    ${LIBPQ_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
)

# Role-based access scenarios test
add_executable(test_role_based_access_scenarios test_role_based_access_scenarios.cpp)

target_link_libraries(test_role_based_access_scenarios
    fileengine_core
    ${LIBPQ_LIBRARIES}
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${GRPCPP_LIBRARIES}
    proto_lib
    ${UUID_LIBRARIES}
    ${CURL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)

if(AWSSDK_FOUND)
    target_link_libraries(test_role_based_access_scenarios ${AWSSDK_LINK_LIBRARIES})
endif()

target_include_directories(test_role_based_access_scenarios PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
    ${CMAKE_CURRENT_BINARY_DIR}/../core/generated/fileengine
    ${LIBPQ_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
)

# Comprehensive ACL and role tests
add_executable(test_comprehensive_acl_roles test_comprehensive_acl_roles.cpp)

target_link_libraries(test_comprehensive_acl_roles
    fileengine_core
    ${LIBPQ_LIBRARIES}
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${GRPCPP_LIBRARIES}
    proto_lib
    ${UUID_LIBRARIES}
    ${CURL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)

if(AWSSDK_FOUND)
    target_link_libraries(test_comprehensive_acl_roles ${AWSSDK_LINK_LIBRARIES})
endif()

target_include_directories(test_comprehensive_acl_roles PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
    ${CMAKE_CURRENT_BINARY_DIR}/../core/generated/fileengine
    ${LIBPQ_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
)

# Comprehensive ACL and RBAC test
add_executable(test_acl_rbac_comprehensive test_acl_rbac_comprehensive.cpp)

target_link_libraries(test_acl_rbac_comprehensive
    fileengine_core
    ${LIBPQ_LIBRARIES}
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${GRPCPP_LIBRARIES}
    proto_lib
    ${UUID_LIBRARIES}
    ${CURL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)

if(AWSSDK_FOUND)
    target_link_libraries(test_acl_rbac_comprehensive ${AWSSDK_LINK_LIBRARIES})
endif()

target_include_directories(test_acl_rbac_comprehensive PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
    ${CMAKE_CURRENT_BINARY_DIR}/../core/generated/fileengine
    ${LIBPQ_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
)

# Include the existing S3 integration test
add_executable(test_s3_integration test_s3_integration.cpp)

target_link_libraries(test_s3_integration
    fileengine_core
    ${LIBPQ_LIBRARIES}
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${GRPCPP_LIBRARIES}
    proto_lib
    ${UUID_LIBRARIES}
    ${CURL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)

if(AWSSDK_FOUND)
    target_link_libraries(test_s3_integration ${AWSSDK_LINK_LIBRARIES})
endif()

target_include_directories(test_s3_integration PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
    ${CMAKE_CURRENT_BINARY_DIR}/../core/generated/fileengine
    ${LIBPQ_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
)
# Streaming crypto primitives (CompressStream/EncryptStream + decrypt/decompress)
add_executable(crypto_stream_tests crypto_stream_tests.cpp)
target_link_libraries(crypto_stream_tests
    fileengine_core
    ${LIBPQ_LIBRARIES}
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${GRPCPP_LIBRARIES}
    proto_lib
    ${UUID_LIBRARIES}
    ${CURL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)

if(AWSSDK_FOUND)
    target_link_libraries(crypto_stream_tests ${AWSSDK_LINK_LIBRARIES})
endif()

target_include_directories(crypto_stream_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
    ${CMAKE_CURRENT_BINARY_DIR}/../core/generated/fileengine
    ${LIBPQ_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
)

# Read/write connection-routing failover unit test (header-only; no core link).
add_executable(test_connection_router test_connection_router.cpp)
target_include_directories(test_connection_router PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
)
