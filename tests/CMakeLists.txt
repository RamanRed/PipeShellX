# GoogleTest is always available: fetched at configure time unless the user
# explicitly opts into a system installation. The suite can never be skipped.
if(PIPESHELLX_SYSTEM_GTEST)
    find_package(GTest REQUIRED)
else()
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

add_executable(pipeshellx_tests
    unit/os/test_child_exit.cpp
    unit/os/test_handle.cpp
    unit/os/test_pipe.cpp
    unit/os/test_poller.cpp
    unit/os/test_process.cpp
    unit/os/test_result.cpp
    unit/os/test_signal_source.cpp
    unit/runtime/test_reactor.cpp
    test_cli_options.cpp
    test_client_config.cpp
    test_command_executor.cpp
    test_logger.cpp
    test_process_manager.cpp
    test_process_manager_golden.cpp
    test_ssh_auth.cpp
)

target_link_libraries(pipeshellx_tests PRIVATE
    pipeshellx_lib
    pipeshellx_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(pipeshellx_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    PROPERTIES TIMEOUT 60
)
