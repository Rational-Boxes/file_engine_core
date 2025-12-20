cmake_minimum_required(VERSION 3.15)
project(file_engine_core_tests)

# Compiler settings
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(Threads REQUIRED)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../cli/include
)

# Add test executables
add_executable(basic_tests
    basic_tests.cpp
)

add_executable(database_tests
    database_tests.cpp
)

add_executable(storage_tests
    storage_tests.cpp
)

add_executable(s3_storage_tests
    s3_storage_tests.cpp
)

add_executable(tenant_tests
    tenant_tests.cpp
)

add_executable(acl_tests
    acl_tests.cpp
)

add_executable(cache_tests
    cache_tests.cpp
)

add_executable(filesystem_tests
    filesystem_tests.cpp
)

add_executable(query_builder_tests
    query_builder_tests.cpp
)

add_executable(storage_tracker_tests
    storage_tracker_tests.cpp
)

add_executable(file_culler_tests
    file_culler_tests.cpp
)

add_executable(object_store_sync_tests
    object_store_sync_tests.cpp
)

# Link libraries for all tests
# Set the library path to find the core library built in the core directory
set(FILEENGINE_CORE_LIBRARY_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../core/build)

target_link_libraries(basic_tests
    ${FILEENGINE_CORE_LIBRARY_PATH}/libfileengine_core.a
    ${FILEENGINE_CORE_LIBRARY_PATH}/libproto_lib.a
    ${LIBPQ_LIBRARIES}
    ${UUID_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    ${GRPCPP_LIBRARIES}
    gpr
    grpc++_reflection
    Threads::Threads
    ${CMAKE_DL_LIBS}
    pthread
    dl
)

target_link_libraries(database_tests
    ${FILEENGINE_CORE_LIBRARY_PATH}/libfileengine_core.a
    ${FILEENGINE_CORE_LIBRARY_PATH}/libproto_lib.a
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${LIBPQ_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    ${GRPCPP_LIBRARIES}
    ${UUID_LIBRARIES}
    gpr
    grpc++_reflection
    pthread
)

target_link_libraries(storage_tests
    ${FILEENGINE_CORE_LIBRARY_PATH}/libfileengine_core.a
    ${FILEENGINE_CORE_LIBRARY_PATH}/libproto_lib.a
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${LIBPQ_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    ${GRPCPP_LIBRARIES}
    ${UUID_LIBRARIES}
    gpr
    grpc++_reflection
    pthread
)

target_link_libraries(s3_storage_tests
    ${FILEENGINE_CORE_LIBRARY_PATH}/libfileengine_core.a
    ${FILEENGINE_CORE_LIBRARY_PATH}/libproto_lib.a
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${LIBPQ_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    ${GRPCPP_LIBRARIES}
    ${UUID_LIBRARIES}
    gpr
    grpc++_reflection
    pthread
)

target_link_libraries(tenant_tests
    ${FILEENGINE_CORE_LIBRARY_PATH}/libfileengine_core.a
    ${FILEENGINE_CORE_LIBRARY_PATH}/libproto_lib.a
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${LIBPQ_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    ${GRPCPP_LIBRARIES}
    ${UUID_LIBRARIES}
    gpr
    grpc++_reflection
    pthread
)

target_link_libraries(acl_tests
    ${FILEENGINE_CORE_LIBRARY_PATH}/libfileengine_core.a
    ${FILEENGINE_CORE_LIBRARY_PATH}/libproto_lib.a
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${LIBPQ_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    ${GRPCPP_LIBRARIES}
    ${UUID_LIBRARIES}
    gpr
    grpc++_reflection
    pthread
)

target_link_libraries(cache_tests
    ${FILEENGINE_CORE_LIBRARY_PATH}/libfileengine_core.a
    ${FILEENGINE_CORE_LIBRARY_PATH}/libproto_lib.a
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${LIBPQ_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    ${GRPCPP_LIBRARIES}
    ${UUID_LIBRARIES}
    gpr
    grpc++_reflection
    pthread
)

target_link_libraries(filesystem_tests
    ${FILEENGINE_CORE_LIBRARY_PATH}/libfileengine_core.a
    ${FILEENGINE_CORE_LIBRARY_PATH}/libproto_lib.a
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${LIBPQ_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    ${GRPCPP_LIBRARIES}
    ${UUID_LIBRARIES}
    gpr
    grpc++_reflection
    pthread
)

target_link_libraries(query_builder_tests
    ${FILEENGINE_CORE_LIBRARY_PATH}/libfileengine_core.a
    ${FILEENGINE_CORE_LIBRARY_PATH}/libproto_lib.a
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${LIBPQ_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    ${GRPCPP_LIBRARIES}
    ${UUID_LIBRARIES}
    gpr
    grpc++_reflection
    pthread
)

target_link_libraries(storage_tracker_tests
    ${FILEENGINE_CORE_LIBRARY_PATH}/libfileengine_core.a
    ${FILEENGINE_CORE_LIBRARY_PATH}/libproto_lib.a
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${LIBPQ_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    ${GRPCPP_LIBRARIES}
    ${UUID_LIBRARIES}
    gpr
    grpc++_reflection
    pthread
)

target_link_libraries(file_culler_tests
    ${FILEENGINE_CORE_LIBRARY_PATH}/libfileengine_core.a
    ${FILEENGINE_CORE_LIBRARY_PATH}/libproto_lib.a
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${LIBPQ_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    ${GRPCPP_LIBRARIES}
    ${UUID_LIBRARIES}
    gpr
    grpc++_reflection
    pthread
)

target_link_libraries(object_store_sync_tests
    ${FILEENGINE_CORE_LIBRARY_PATH}/libfileengine_core.a
    ${FILEENGINE_CORE_LIBRARY_PATH}/libproto_lib.a
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${LIBPQ_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    ${GRPCPP_LIBRARIES}
    ${UUID_LIBRARIES}
    gpr
    grpc++_reflection
    pthread
)

# Compiler definitions
set_target_properties(
    basic_tests
    database_tests
    storage_tests
    s3_storage_tests
    tenant_tests
    acl_tests
    cache_tests
    filesystem_tests
    query_builder_tests
    storage_tracker_tests
    file_culler_tests
    object_store_sync_tests
    PROPERTIES
    COMPILE_DEFINITIONS "_GNU_SOURCE"
)