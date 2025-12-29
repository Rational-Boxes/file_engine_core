cmake_minimum_required(VERSION 3.15)
project(fileengine_cli VERSION 1.0.0)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find packages
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# Find gRPC and Protobuf
find_package(PkgConfig REQUIRED)
if(PkgConfig_FOUND)
    pkg_check_modules(GRPCPP REQUIRED grpc++)
    pkg_check_modules(LIBPQ REQUIRED libpq)
endif()

find_package(Protobuf REQUIRED)
if(NOT PROTOBUF_FOUND)
    message(FATAL_ERROR "Protobuf is required but not found")
endif()

# Ensure the proto files are generated first
set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../proto)
set(PROTO_FILE ${PROTO_DIR}/fileservice.proto)
set(PROTO_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/../core/build/generated)

# Add the CLI executable
add_executable(fileengine_cli
        src/fileengine_cli.cpp
)

# Include directories
target_include_directories(fileengine_cli PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include/fileengine
    ${CMAKE_CURRENT_SOURCE_DIR}/include  # Include CLI-specific headers
    ${CMAKE_CURRENT_BINARY_DIR}/../core/build/generated/fileengine  # Include generated proto files from core build
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
)

# Link libraries - use the core library and proto library
target_link_libraries(fileengine_cli
    fileengine_core  # Link with the main core library
    proto_lib        # Link with the generated proto library
    ${GRPCPP_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${LIBPQ_LIBRARIES}
    pthread
)