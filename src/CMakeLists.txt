find_package(Threads REQUIRED)

# L0 — OS primitive backends (the only place platform headers may appear)
if(WIN32)
    message(FATAL_ERROR "The Win32 backend arrives in Phase 3 (PLAN.md)")
else()
    set(PIPESHELLX_OS_SOURCES
        os/posix/error.cpp
        os/posix/handle.cpp
        os/posix/io.cpp
        os/posix/pipe.cpp
        os/posix/process.cpp
        os/posix/signal.cpp
    )
endif()

# Core library
add_library(pipeshellx_lib
    ${PIPESHELLX_OS_SOURCES}
    cli_options.cpp
    client_config.cpp
    client_manager.cpp
    command_executor.cpp
    ipc_engine.cpp
    logger.cpp
    process_manager.cpp
    session_manager.cpp
    ssh_auth.cpp
)
add_library(pipeshellx::lib ALIAS pipeshellx_lib)

target_include_directories(pipeshellx_lib PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(pipeshellx_lib
    PUBLIC Threads::Threads
    PRIVATE pipeshellx_warnings
)
target_compile_definitions(pipeshellx_lib PUBLIC
    PIPESHELLX_VERSION="${PROJECT_VERSION}"
)

# Interactive executable
add_executable(pipeshellx_app
    main.cpp
    terminal_client.cpp
)
set_target_properties(pipeshellx_app PROPERTIES OUTPUT_NAME PipeShellX)
target_link_libraries(pipeshellx_app PRIVATE pipeshellx_lib pipeshellx_warnings)

# Install rules
install(TARGETS pipeshellx_app pipeshellx_lib
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
