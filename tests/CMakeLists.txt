# GoogleTest is always available so the suite can never be silently skipped:
# use an installed copy when there is one (works offline), otherwise fetch it.
# PIPESHELLX_SYSTEM_GTEST=ON requires the installed copy and never fetches.
if(PIPESHELLX_SYSTEM_GTEST)
    find_package(GTest REQUIRED)
else()
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.17.0
            GIT_SHALLOW    TRUE
        )
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
endif()

set(PIPESHELLX_TLS_TEST_SOURCES "")
if(PIPESHELLX_NATIVE_TRANSPORT AND OpenSSL_FOUND)
    set(PIPESHELLX_TLS_TEST_SOURCES unit/os/test_tls.cpp unit/transport/test_tls_stream.cpp
        unit/transport/test_native_transport.cpp unit/ca/test_certificate_authority.cpp
        unit/cli/test_ca_command.cpp
    unit/cli/test_diff_command.cpp
    unit/cli/test_pipe_command.cpp unit/cli/test_node_command.cpp)
endif()

add_executable(pipeshellx_tests
    unit/os/test_child_exit.cpp
    unit/os/test_handle.cpp
    unit/os/test_pipe.cpp
    unit/os/test_poller.cpp
    unit/os/test_process.cpp
    unit/os/test_result.cpp
    unit/os/test_signal_source.cpp
    unit/os/test_socket.cpp
    ${PIPESHELLX_TLS_TEST_SOURCES}
    unit/os/test_system.cpp
    unit/stream/test_bounded_buffer.cpp
    unit/stream/test_credit_window.cpp
    unit/stream/test_stream.cpp
    unit/sink/test_sinks.cpp
    unit/inventory/test_inventory.cpp
    unit/sink/test_consensus.cpp
    unit/pipeline/test_planner.cpp
    unit/pipeline/test_parser.cpp
    unit/pipeline/test_pipeline_yaml.cpp
    unit/pipeline/test_local_runner.cpp
    unit/policy/test_policy.cpp
    unit/cli/test_run_command.cpp
    unit/cli/test_ping_command.cpp
    unit/stream/test_line_framer.cpp
    unit/stream/test_spool_buffer.cpp
    unit/runtime/test_reactor.cpp
    unit/runtime/test_backoff.cpp
    unit/audit/test_audit_log.cpp
    unit/transport/test_frame_codec.cpp
    unit/transport/test_open_request.cpp
    unit/transport/test_control_payloads.cpp
    unit/transport/test_session.cpp
    unit/transport/test_session_faults.cpp
    unit/transport/test_scale.cpp
    unit/transport/test_node_fencing.cpp
    test_cli_options.cpp
    test_client_config.cpp
    test_command_executor.cpp
    test_logger.cpp
    test_process_manager.cpp
    test_process_manager_golden.cpp
    test_main.cpp
    test_ssh_auth.cpp
    test_ssh_transport.cpp
)

target_include_directories(pipeshellx_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(pipeshellx_tests PRIVATE
    pipeshellx_lib
    pipeshellx_warnings
    GTest::gtest
)

include(GoogleTest)
gtest_discover_tests(pipeshellx_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    PROPERTIES TIMEOUT 120
)

# Phase 1 exit criterion "poll backend removable without behaviour change":
# the behavioural suites must pass on the portable poll backend too.
add_test(NAME golden_on_poll_backend
    COMMAND pipeshellx_tests --gtest_filter=Golden*:ProcessManagerTest.*:CommandExecutorTest.*
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(golden_on_poll_backend PROPERTIES ENVIRONMENT "PIPESHELLX_POLLER=poll" TIMEOUT 300)
