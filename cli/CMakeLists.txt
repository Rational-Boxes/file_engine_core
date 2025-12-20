cmake_minimum_required(VERSION 3.15)
project(file_engine_core_cli VERSION 1.0.0)

# Add CLI executable
add_executable(fileengine_cli
    src/main.cpp
)

# Include directories
target_include_directories(fileengine_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
)

# Link libraries
target_link_libraries(fileengine_cli 
    fileengine_core
    Threads::Threads
)

# Compiler definitions
target_compile_definitions(fileengine_cli PRIVATE
    _GNU_SOURCE
)