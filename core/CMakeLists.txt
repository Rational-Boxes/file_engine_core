cmake_minimum_required(VERSION 3.15)
project(file_engine_core_core VERSION 1.0.0)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ----- Build-time options ----------------------------------------------------
option(FILEENGINE_ENABLE_OTEL "Build with OpenTelemetry tracing support" OFF)
option(FILEENGINE_ENABLE_REST "Build with the REST monitoring listener" ON)

# ----- systemd integration (sd_notify) ---------------------------------------
# libsystemd is small and ubiquitous; if pkg-config can't find it (e.g.
# cross-compile, alpine) we degrade gracefully with stub macros in server.cpp.
pkg_check_modules(SYSTEMD libsystemd)
if(SYSTEMD_FOUND)
    message(STATUS "libsystemd found — sd_notify integration enabled")
endif()

# ----- Build-info header (configure_file substitution) -----------------------
set(FILEENGINE_VERSION "${PROJECT_VERSION}")

# git SHA at configure time; falls back to "unknown" when not in a repo.
find_program(GIT_EXECUTABLE git)
set(FILEENGINE_GIT_SHA "unknown")
if(GIT_EXECUTABLE AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_SOURCE_DIR} rev-parse --short HEAD
        OUTPUT_VARIABLE _git_sha
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_git_sha)
        set(FILEENGINE_GIT_SHA "${_git_sha}")
    endif()
endif()

string(TIMESTAMP FILEENGINE_BUILD_TIMESTAMP "%Y-%m-%dT%H:%M:%SZ" UTC)

if(FILEENGINE_ENABLE_OTEL)
    set(FILEENGINE_OTEL_BOOL "true")
else()
    set(FILEENGINE_OTEL_BOOL "false")
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/build_info.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/fileengine/build_info.h
    @ONLY
)

# Find packages
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# Find gRPC
find_package(PkgConfig REQUIRED)
if(PkgConfig_FOUND)
    pkg_check_modules(GRPCPP REQUIRED grpc++)
    pkg_check_modules(LIBPQ REQUIRED libpq)
endif()

# Find zlib
find_package(PkgConfig REQUIRED)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(ZLIB REQUIRED zlib)
endif()

# Find OpenSSL
find_package(OpenSSL REQUIRED)

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

# Find curl library
find_package(PkgConfig REQUIRED)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(CURL REQUIRED libcurl)
endif()

# Find AWS SDK
find_package(AWSSDK QUIET COMPONENTS s3 core)
if(AWSSDK_FOUND)
    message(STATUS "AWS SDK found, enabling S3 support")
    add_compile_definitions(USE_AWS_SDK=1)
else()
    message(WARNING "AWS SDK not found, S3 functionality will be limited")
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
    src/connection_pool_manager.cpp
    src/storage.cpp
    src/s3_storage.cpp
    src/filesystem.cpp
    src/tenant_manager.cpp
    src/cache_manager.cpp
    src/acl_manager.cpp
    src/role_manager.cpp       # Add role manager source file
    src/utils.cpp
    src/config_loader.cpp
    src/grpc_service.cpp  # Add gRPC service implementation
    src/object_store_sync.cpp  # Add missing source file
    src/storage_tracker.cpp    # Add missing source file
    src/file_culler.cpp        # Add missing file culler source file
    # src/logger.cpp             # Temporarily exclude logger source file due to interface mismatch
    src/query_builder.cpp      # Add query builder source file
    src/server_logger.cpp      # Add server logger source file
    src/crypto_utils.cpp       # Add crypto utilities source file
    src/rest_server.cpp        # Monitoring HTTP listener (Phase A)
    src/event.cpp              # File-activity event model + JSON envelope
    src/event_sink.cpp         # Async bounded-outbox sink base
    src/event_sink_factory.cpp # Builds the configured sink (or none)
)

# Include directories
target_include_directories(fileengine_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/fileengine
    ${LIBPQ_INCLUDE_DIRS}
    ${CMAKE_CURRENT_BINARY_DIR}/generated/fileengine # Include the generated fileengine directory
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/third_party  # cpp-httplib + nlohmann/json (vendored)
)

# Link libraries
target_link_libraries(fileengine_core
    ${LIBPQ_LIBRARIES}
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${GRPCPP_LIBRARIES}
    proto_lib
    ${UUID_LIBRARIES}  # Add uuid library
    ${CURL_LIBRARIES}  # Add curl library
    ${ZLIB_LIBRARIES}  # Add zlib library for compression
    OpenSSL::Crypto    # Add OpenSSL library for encryption
    ${CMAKE_THREAD_LIBS_INIT}  # Properly link thread libraries
)
if(AWSSDK_FOUND)
    target_link_libraries(fileengine_core ${AWSSDK_LINK_LIBRARIES})
endif()

# Compiler definitions
target_compile_definitions(fileengine_core PRIVATE
    _GNU_SOURCE
)

# Optional Redis-based file-activity event emission (off by default; requires
# hiredis-devel). When unavailable the event subsystem still compiles, but the
# factory returns no sink so nothing is published.
option(FILEENGINE_ENABLE_EVENTS "Build with Redis event emission (requires hiredis-devel)" OFF)
if(FILEENGINE_ENABLE_EVENTS)
    find_library(HIREDIS_LIB hiredis)
    find_path(HIREDIS_INC hiredis/hiredis.h)
    if(HIREDIS_LIB AND HIREDIS_INC)
        target_sources(fileengine_core PRIVATE src/redis_event_sink.cpp)
        target_include_directories(fileengine_core PRIVATE ${HIREDIS_INC})
        target_link_libraries(fileengine_core ${HIREDIS_LIB})
        target_compile_definitions(fileengine_core PRIVATE FILEENGINE_HAS_EVENTS=1)
        message(STATUS "FileEngine event emission ENABLED (hiredis: ${HIREDIS_LIB})")
    else()
        message(WARNING "FILEENGINE_ENABLE_EVENTS=ON but hiredis not found; "
                        "event emission compiled out (install hiredis-devel)")
    endif()
endif()

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

if(SYSTEMD_FOUND)
    target_compile_definitions(fileengine_server PRIVATE FILEENGINE_HAS_SYSTEMD=1)
    target_include_directories(fileengine_server PRIVATE ${SYSTEMD_INCLUDE_DIRS})
    target_link_libraries(fileengine_server ${SYSTEMD_LIBRARIES})
endif()

add_dependencies(fileengine_core generated_files)
add_dependencies(fileengine_server generated_files)

# Installation targets
include(GNUInstallDirs)

install(TARGETS fileengine_core
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(TARGETS fileengine_server
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install header files
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

# Install generated files
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

# Install a default configuration file to /etc/fileengine
install(CODE "
    # Create /etc/fileengine directory if it doesn't exist
    execute_process(
        COMMAND \${CMAKE_COMMAND} -E make_directory /etc/fileengine
        RESULT_VARIABLE MKDIR_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
    )

    # Create a default configuration file if one doesn't already exist
    if(MKDIR_RESULT EQUAL 0 OR MKDIR_RESULT EQUAL \"\")
        # Check if the config file already exists
        execute_process(
            COMMAND sh -c \"test -f /etc/fileengine/core.conf\"
            RESULT_VARIABLE CONFIG_EXISTS
            OUTPUT_QUIET
            ERROR_QUIET
        )

        if(NOT CONFIG_EXISTS EQUAL 0)
            # Create a default configuration file
            execute_process(
                COMMAND sh -c \"echo '# FileEngine Core Service Configuration
# This file is created during installation

# Database Configuration
FILEENGINE_PG_HOST=localhost
FILEENGINE_PG_PORT=5432
FILEENGINE_PG_DATABASE=fileengine
FILEENGINE_PG_USER=fileengine_user
FILEENGINE_PG_PASSWORD=fileengine_password

# Storage Configuration
FILEENGINE_STORAGE_BASE=/var/lib/fileengine/storage

# Encryption and Compression Settings
FILEENGINE_ENCRYPT_DATA=false
FILEENGINE_COMPRESS_DATA=false
AT_REST_KEY=

# S3/MinIO Configuration
FILEENGINE_S3_ENDPOINT=http://localhost:9000
FILEENGINE_S3_REGION=us-east-1
FILEENGINE_S3_BUCKET=fileengine
FILEENGINE_S3_ACCESS_KEY=minioadmin
FILEENGINE_S3_SECRET_KEY=minioadmin
FILEENGINE_S3_PATH_STYLE=true

# Cache Configuration
FILEENGINE_CACHE_THRESHOLD=0.8
FILEENGINE_MAX_CACHE_SIZE_MB=1024

# Server Configuration
FILEENGINE_GRPC_HOST=0.0.0.0
FILEENGINE_GRPC_PORT=50051
FILEENGINE_HTTP_THREAD_POOL=10

# Security Configuration
FILEENGINE_ROOT_USER=false

# Sync Configuration
FILEENGINE_S3_SYNC_SUPPORT=true
FILEENGINE_S3_RETRY_SECONDS=60
FILEENGINE_S3_SYNC_ON_STARTUP=true
FILEENGINE_S3_SYNC_ON_DEMAND=true
FILEENGINE_S3_SYNC_PATTERN=all
FILEENGINE_S3_SYNC_BIDIRECTIONAL=true

# Logging Configuration
FILEENGINE_LOG_LEVEL=INFO
FILEENGINE_LOG_FILE_PATH=/var/log/fileengine.log
FILEENGINE_LOG_TO_CONSOLE=true
FILEENGINE_LOG_TO_FILE=false
FILEENGINE_LOG_ROTATION_SIZE_MB=10
FILEENGINE_LOG_RETENTION_DAYS=7
' > /etc/fileengine/core.conf\"
            )
        endif()
    endif()
")