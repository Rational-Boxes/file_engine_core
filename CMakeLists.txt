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

# Set default installation prefix to /opt/file_engine_core
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/opt/file_engine_core")
endif()

# Create a post-install script to set up symlinks to standard locations (requires root)
install(CODE "
    # Create symbolic links to standard locations if running with sufficient privileges
    execute_process(
        COMMAND \${CMAKE_COMMAND} -E make_directory /usr/local/bin
        RESULT_VARIABLE MKDIR_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(MKDIR_RESULT EQUAL 0 OR MKDIR_RESULT EQUAL \"\")
        execute_process(
            COMMAND \${CMAKE_COMMAND} -E create_symlink ${CMAKE_INSTALL_PREFIX}/bin/fileengine_server /usr/local/bin/fileengine_server
            RESULT_VARIABLE LINK_RESULT1
            OUTPUT_QUIET
            ERROR_QUIET
        )
        execute_process(
            COMMAND \${CMAKE_COMMAND} -E create_symlink ${CMAKE_INSTALL_PREFIX}/bin/fileengine_cli /usr/local/bin/fileengine_cli
            RESULT_VARIABLE LINK_RESULT2
            OUTPUT_QUIET
            ERROR_QUIET
        )
    endif()

    execute_process(
        COMMAND \${CMAKE_COMMAND} -E make_directory /usr/local/lib
        RESULT_VARIABLE MKDIR_RESULT2
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(MKDIR_RESULT2 EQUAL 0 OR MKDIR_RESULT2 EQUAL \"\")
        execute_process(
            COMMAND \${CMAKE_COMMAND} -E create_symlink ${CMAKE_INSTALL_PREFIX}/lib/libfileengine_core.so /usr/local/lib/libfileengine_core.so
            RESULT_VARIABLE LINK_RESULT3
            OUTPUT_QUIET
            ERROR_QUIET
        )
    endif()

    execute_process(
        COMMAND \${CMAKE_COMMAND} -E make_directory /usr/local/include
        RESULT_VARIABLE MKDIR_RESULT3
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(MKDIR_RESULT3 EQUAL 0 OR MKDIR_RESULT3 EQUAL \"\")
        execute_process(
            COMMAND \${CMAKE_COMMAND} -E create_symlink ${CMAKE_INSTALL_PREFIX}/include/fileengine /usr/local/include/fileengine
            RESULT_VARIABLE LINK_RESULT4
            OUTPUT_QUIET
            ERROR_QUIET
        )
    endif()
")

# Install systemd service file
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/fileengine.service
    DESTINATION /etc/systemd/system/
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
)