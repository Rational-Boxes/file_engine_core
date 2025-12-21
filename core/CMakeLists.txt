cmake_minimum_required(VERSION 3.15)
project(file_engine_core_core VERSION 1.0.0)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Find packages
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# Find gRPC
find_package(PkgConfig REQUIRED)
if(PkgConfig_FOUND)
    pkg_check_modules(GRPCPP REQUIRED grpc++)
    pkg_check_modules(LIBPQ REQUIRED libpq)
endif()

# Find Protobuf
find_package(Protobuf REQUIRED)
if(NOT PROTOBUF_FOUND)
    message(FATAL_ERROR "Protobuf is required but not found")
endif()

# Find uuid library
find_package(PkgConfig REQUIRED)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(UUID REQUIRED uuid)
endif()

# Define proto files
set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../proto)
set(PROTO_FILE ${PROTO_DIR}/fileservice.proto)
set(PROTO_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)

# Generate gRPC code from proto
add_custom_command(
    OUTPUT ${PROTO_OUTPUT_DIR}/fileengine/fileservice.pb.cc ${PROTO_OUTPUT_DIR}/fileengine/fileservice.pb.h
           ${PROTO_OUTPUT_DIR}/fileengine/fileservice.grpc.pb.cc ${PROTO_OUTPUT_DIR}/fileengine/fileservice.grpc.pb.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_OUTPUT_DIR}/fileengine
    COMMAND protoc --proto_path=${PROTO_DIR} --cpp_out=${PROTO_OUTPUT_DIR}/fileengine ${PROTO_FILE}
    COMMAND protoc --proto_path=${PROTO_DIR} --grpc_out=${PROTO_OUTPUT_DIR}/fileengine --plugin=protoc-gen-grpc=`which grpc_cpp_plugin` ${PROTO_FILE}
    DEPENDS ${PROTO_FILE}
    COMMENT "Generating gRPC code from proto"
)

# Create a custom target for generated files
add_custom_target(generated_files DEPENDS
    ${PROTO_OUTPUT_DIR}/fileengine/fileservice.pb.cc
    ${PROTO_OUTPUT_DIR}/fileengine/fileservice.pb.h
    ${PROTO_OUTPUT_DIR}/fileengine/fileservice.grpc.pb.cc
    ${PROTO_OUTPUT_DIR}/fileengine/fileservice.grpc.pb.h)

# Add proto library
add_library(proto_lib
    ${PROTO_OUTPUT_DIR}/fileengine/fileservice.pb.cc
    ${PROTO_OUTPUT_DIR}/fileengine/fileservice.grpc.pb.cc
)

target_include_directories(proto_lib PUBLIC
    ${PROTO_OUTPUT_DIR}
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
)

target_link_libraries(proto_lib
    ${PROTOBUF_LIBRARIES}
    ${GRPCPP_LIBRARIES}
)

# Add core library
add_library(fileengine_core SHARED
    src/database.cpp
    src/connection_pool.cpp
    src/storage.cpp
    src/s3_storage.cpp
    src/filesystem.cpp
    src/tenant_manager.cpp
    src/cache_manager.cpp
    src/acl_manager.cpp
    src/utils.cpp
    src/config_loader.cpp
    src/grpc_service.cpp  # Add gRPC service implementation
    src/object_store_sync.cpp  # Add missing source file
    src/storage_tracker.cpp    # Add missing source file
    src/file_culler.cpp        # Add missing file culler source file
    src/logger.cpp             # Add logger source file
    src/query_builder.cpp      # Add query builder source file
)

# Include directories
target_include_directories(fileengine_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/fileengine
    ${LIBPQ_INCLUDE_DIRS}
    ${CMAKE_CURRENT_BINARY_DIR}/generated/fileengine # Include the generated fileengine directory
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(fileengine_core
    ${LIBPQ_LIBRARIES}
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${GRPCPP_LIBRARIES}
    proto_lib
    ${UUID_LIBRARIES}  # Add uuid library
    ${CMAKE_THREAD_LIBS_INIT}  # Properly link thread libraries
)

# Compiler definitions
target_compile_definitions(fileengine_core PRIVATE
    _GNU_SOURCE
)

# Add executable
add_executable(fileengine_server
    src/server.cpp
)

target_link_libraries(fileengine_server
    fileengine_core
    proto_lib
    ${GRPCPP_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    ${UUID_LIBRARIES}  # Add uuid library
    gpr  # Link gpr library for gRPC core
    grpc++_reflection  # Add reflection library
    pthread
)

add_dependencies(fileengine_core generated_files)
add_dependencies(fileengine_server generated_files)