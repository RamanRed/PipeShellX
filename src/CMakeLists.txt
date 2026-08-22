find_package(Threads REQUIRED)

# Core library
add_library(pipeshellx_lib
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
