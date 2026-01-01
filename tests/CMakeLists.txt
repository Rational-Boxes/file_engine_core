# Simple test for S3 integration
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
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated/fileengine
    ${LIBPQ_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
)