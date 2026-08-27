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

# Leak-proof deleted-reachability unit test (AclManager + mock DB; no live DB).
add_executable(test_deleted_reachability test_deleted_reachability.cpp)
target_link_libraries(test_deleted_reachability
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
    target_link_libraries(test_deleted_reachability ${AWSSDK_LINK_LIBRARIES})
endif()
target_include_directories(test_deleted_reachability PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
    ${CMAKE_CURRENT_BINARY_DIR}/../core/generated/fileengine
    ${LIBPQ_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
)

# File culler data-loss regression (mock IStorage/IObjectStore; no live DB).
# Proves culling fails closed: a local payload is never deleted unless the
# object store confirms a durable copy.
# Header-only tracker: no core link needed, just threads. Keeping it standalone
# means the counters stay testable without dragging in gRPC and libpq.
add_executable(transfer_tracker_tests transfer_tracker_tests.cpp)
target_link_libraries(transfer_tracker_tests Threads::Threads)
target_include_directories(transfer_tracker_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
)
add_test(NAME transfer_tracker_tests COMMAND transfer_tracker_tests)
# Pure counters, no database, no object store, no network — it belongs in the
# suite a developer can run anywhere.
set_tests_properties(transfer_tracker_tests PROPERTIES LABELS "unit")

# Drives a real multipart upload into a failure so the abort guard actually
# runs. Needs an object store; skips cleanly without one. Labelled "integration"
# so `ctest -L unit` stays infrastructure-free.
add_executable(multipart_abort_integration multipart_abort_integration.cpp)
target_link_libraries(multipart_abort_integration
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
    target_link_libraries(multipart_abort_integration ${AWSSDK_LINK_LIBRARIES})
endif()
target_include_directories(multipart_abort_integration PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
    ${CMAKE_CURRENT_BINARY_DIR}/../core/generated/fileengine
    ${LIBPQ_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
)
add_test(NAME multipart_abort_integration COMMAND multipart_abort_integration)
set_tests_properties(multipart_abort_integration PROPERTIES LABELS "integration")

add_executable(file_culler_tests file_culler_tests.cpp)
target_link_libraries(file_culler_tests
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
    target_link_libraries(file_culler_tests ${AWSSDK_LINK_LIBRARIES})
endif()
target_include_directories(file_culler_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
    ${CMAKE_CURRENT_BINARY_DIR}/../core/generated/fileengine
    ${LIBPQ_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
)

# Version-selection regression (FileSystem::newest_version; no live DB).
add_executable(test_version_selection test_version_selection.cpp)
target_link_libraries(test_version_selection
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
    target_link_libraries(test_version_selection ${AWSSDK_LINK_LIBRARIES})
endif()
target_include_directories(test_version_selection PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
    ${CMAKE_CURRENT_BINARY_DIR}/../core/generated/fileengine
    ${LIBPQ_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
)

# ACL security semantics unit test (AclManager + mock DB; no live DB).
add_executable(test_security_acl test_security_acl.cpp)
target_link_libraries(test_security_acl
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
    target_link_libraries(test_security_acl ${AWSSDK_LINK_LIBRARIES})
endif()
target_include_directories(test_security_acl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
    ${CMAKE_CURRENT_BINARY_DIR}/../core/generated/fileengine
    ${LIBPQ_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
)

# Connection-pool lifecycle: proves a returned connection wakes a waiting caller.
# Needs a live Postgres (FE_TEST_PG_*); skips cleanly when there isn't one.
add_executable(test_pool_lifecycle test_pool_lifecycle.cpp)
target_include_directories(test_pool_lifecycle PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
)
target_link_libraries(test_pool_lifecycle
    fileengine_core
    ${LIBPQ_LIBRARIES}
)
add_test(NAME pool_lifecycle COMMAND test_pool_lifecycle)
# Needs a database; skips cleanly without one, so it is safe in any environment.
set_tests_properties(pool_lifecycle PROPERTIES LABELS "lifecycle" TIMEOUT 120)

# Production-shaped stress: real uploads through the HTTP bridge while sampling
# the core's monitoring endpoint, asserting every request completed, every
# database connection came back, and the workers returned to waiting.
#
# Labelled `stress` because it needs the whole stack up
# (scripts/start_backend_services.sh) rather than just a database. Run the
# always-safe tests with:  ctest -L lifecycle
# and this one with:       ctest -L stress
add_test(NAME thread_lifecycle_stress
         COMMAND ${CMAKE_COMMAND} -E env
                 python3 ${CMAKE_CURRENT_SOURCE_DIR}/thread_lifecycle_probe.py
                 --files 200 --concurrency 32)
set_tests_properties(thread_lifecycle_stress PROPERTIES LABELS "stress;lifecycle" TIMEOUT 600 SKIP_RETURN_CODE 77)


# ---------------------------------------------------------------------------
# ctest registration
#
# These executables all existed and none were registered, so `ctest` ran nothing
# and every one of them was only ever run by hand — which is how a mock that no
# longer compiled, and a test that segfaults on exit, both went unnoticed.
#
# Labels select by what a test NEEDS, so a developer with no infrastructure can
# still run a meaningful suite:
#
#   ctest -L unit           no infrastructure; mocks only. Safe anywhere.
#   ctest -L lifecycle      needs a database (skips cleanly without one)
#   ctest -L stress         needs the whole stack up
#   ctest                   everything registered
# ---------------------------------------------------------------------------

# Pure unit tests — mocks and in-process logic, no database, no object store.
foreach(t
    test_acl_group_role_permissions
    test_role_based_access_scenarios
    test_comprehensive_acl_roles
    test_acl_rbac_comprehensive
    test_security_acl
    test_deleted_reachability
    test_connection_router
    test_version_selection
    file_culler_tests
    crypto_stream_tests)
  add_test(NAME ${t} COMMAND ${t})
  set_tests_properties(${t} PROPERTIES LABELS "unit" TIMEOUT 120)
endforeach()

# S3 integration. Was DISABLED for segfaulting on exit; the cause turned out to
# be an ODR violation rather than anything in the test — USE_AWS_SDK was set with
# directory-scoped add_compile_definitions(), so the library compiled S3Storage
# WITH its AWS members and the tests compiled the same header WITHOUT them. The
# test allocated the smaller object and library code wrote past the end of it.
# The define is now PUBLIC on the fileengine_core target, so every consumer of
# the header agrees on the layout.
#
# Needs an S3 endpoint reachable at localhost:9000 (MinIO in dev).
add_test(NAME test_s3_integration COMMAND test_s3_integration)
set_tests_properties(test_s3_integration PROPERTIES
    LABELS "integration" TIMEOUT 120)

# Broader stress than thread_lifecycle_stress: this one hunts crashes and
# soft-locks (restart detection, monitoring answerable under load, descriptor
# and thread growth) rather than the request/connection lifecycle specifically.
# Same harness the bridges use, pointed at the core.
add_test(NAME load_stress
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/load_stress.py
                 --target core --requests 200 --concurrency 32)
set_tests_properties(load_stress PROPERTIES LABELS "stress" TIMEOUT 600 SKIP_RETURN_CODE 77)

# Service credentials and capability classification. The load-bearing test here
# enumerates the compiled service descriptor and fails if any RPC belongs to no
# capability — so a newly added method cannot ship unclassified, and central
# enforcement means coverage rather than someone's diligence.
add_executable(service_credential_tests service_credential_tests.cpp)
target_include_directories(service_credential_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
    ${CMAKE_CURRENT_BINARY_DIR}/../core/generated/fileengine
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS})
target_link_libraries(service_credential_tests
    fileengine_core proto_lib ${LIBPQ_LIBRARIES} ${GRPCPP_LIBRARIES} ${PROTOBUF_LIBRARIES})
add_test(NAME service_credential_tests COMMAND service_credential_tests)
set_tests_properties(service_credential_tests PROPERTIES LABELS "unit" TIMEOUT 60)

# The logger's security-relevant behaviour: a SECURITY entry no configuration
# can silence, names that never reach the log in clear, and rotation/retention
# that actually run. No infrastructure — it writes to a scratch directory.
add_executable(logger_tests logger_tests.cpp)
target_include_directories(logger_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include)
target_link_libraries(logger_tests fileengine_core ${LIBPQ_LIBRARIES})
add_test(NAME logger_tests COMMAND logger_tests)
set_tests_properties(logger_tests PROPERTIES LABELS "unit" TIMEOUT 120)

# ---------------------------------------------------------------------------
# The guaranteed accountability record (PROPOSAL_accountability_record.md)
# ---------------------------------------------------------------------------

# Pure unit tests over the canonical byte form, the chain hash and the closed
# detail schema. No infrastructure. These pin a CROSS-REPO contract:
# audit_service re-derives the same canonical form and hash in Python to verify
# every record it reads, so a silent change here would not produce a subtle bug,
# it would fail every chain verification in the platform.
add_executable(accountability_tests accountability_tests.cpp)
target_include_directories(accountability_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include)
target_link_libraries(accountability_tests fileengine_core ${LIBPQ_LIBRARIES})
add_test(NAME accountability_tests COMMAND accountability_tests)
set_tests_properties(accountability_tests PROPERTIES LABELS "unit" TIMEOUT 60)

# The guarantees that only a real database can demonstrate: atomicity with the
# operation, fail-closed refusal, a gap-free commit-ordered seq under
# concurrency, a strictly monotonic ts across a backwards clock step, and the
# cursor read. Skips cleanly (exit 77) without Postgres.
add_executable(accountability_live_tests accountability_live_tests.cpp)
target_include_directories(accountability_live_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include)
target_link_libraries(accountability_live_tests
    fileengine_core ${LIBPQ_LIBRARIES} Threads::Threads)
add_test(NAME accountability_live_tests COMMAND accountability_live_tests)
set_tests_properties(accountability_live_tests PROPERTIES
    LABELS "lifecycle" TIMEOUT 300 SKIP_RETURN_CODE 77)

# Configuration precedence: environment > .env > .conf > built-in default.
# Uses a temporary working directory, so the repository's own .env is untouched.
add_executable(test_config_precedence test_config_precedence.cpp)
target_include_directories(test_config_precedence PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/include)
target_link_libraries(test_config_precedence fileengine_core ${LIBPQ_LIBRARIES})
add_test(NAME test_config_precedence COMMAND test_config_precedence)
set_tests_properties(test_config_precedence PROPERTIES LABELS "unit" TIMEOUT 60)
