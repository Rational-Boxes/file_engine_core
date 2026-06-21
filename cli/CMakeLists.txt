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
set(PROTO_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/../core/generated)

# Add the CLI executable
add_executable(fileengine_cli
        src/fileengine_cli.cpp
)

# Include directories
target_include_directories(fileengine_cli PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include/fileengine
    ${CMAKE_CURRENT_SOURCE_DIR}/include  # Include CLI-specific headers
    ${CMAKE_CURRENT_BINARY_DIR}/../core/generated/fileengine  # Include generated proto files from core build
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

# Installation target for CLI
include(GNUInstallDirs)
install(TARGETS fileengine_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ---------------------------------------------------------------------------
# Optional self-contained CLI for distribution (-DBUILD_STATIC_CLI=ON).
#
# The CLI only needs the generated proto code (proto_lib is a static archive)
# and its own header-only logger, so this target deliberately does NOT link
# libfileengine_core.so. It also statically links libstdc++ and libgcc, so the
# binary runs on hosts without a matching C++ runtime. Requires the static
# libstdc++ archive (Fedora: libstdc++-static).
#
# Note: gRPC, protobuf, OpenSSL and libpq remain dynamically linked because
# this system ships no static archives for them; fully static would require
# building those from source. --as-needed drops any dynamic lib the binary
# does not actually reference.
# ---------------------------------------------------------------------------
option(BUILD_STATIC_CLI "Also build a statically-linked fileengine_cli_static" OFF)

if(BUILD_STATIC_CLI)
    add_executable(fileengine_cli_static src/fileengine_cli.cpp)

    target_include_directories(fileengine_cli_static PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/include/fileengine
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/../core/generated/fileengine
        ${PROTOBUF_INCLUDE_DIRS}
        ${GRPCPP_INCLUDE_DIRS}
    )

    target_link_options(fileengine_cli_static PRIVATE
        -static-libstdc++
        -static-libgcc
        -Wl,--as-needed
    )

    target_link_libraries(fileengine_cli_static
        proto_lib        # static archive of the generated proto code
        ${GRPCPP_LIBRARIES}
        ${PROTOBUF_LIBRARIES}
        Threads::Threads
        ${CMAKE_DL_LIBS}
        ${LIBPQ_LIBRARIES}
        pthread
    )

    install(TARGETS fileengine_cli_static
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()