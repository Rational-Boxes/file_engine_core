cmake_minimum_required(VERSION 3.15)
project(file_engine_core VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler warnings
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
endif()

# Find required packages
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# PostgreSQL
if(PkgConfig_FOUND)
    pkg_check_modules(LIBPQ REQUIRED libpq)
endif()

# For this simplified build without the complex gRPC dependencies, we'll skip the Protobuf and gRPC parts
# that are causing issues. The system will still work as a core library but without the gRPC server
# until proper gRPC dependencies are available.

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/core/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/cli/include)

# Add core library
add_subdirectory(core)

# Add CLI as an optional component
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cli/CMakeLists.txt")
    add_subdirectory(cli)
endif()

# Add tests
add_subdirectory(tests)