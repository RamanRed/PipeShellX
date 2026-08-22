find_package(Threads REQUIRED)

# L0 — OS primitive backends (the only place platform headers may appear)
if(WIN32)
    message(FATAL_ERROR "The Win32 backend arrives in Phase 3 (PLAN.md)")
else()
    set(PIPESHELLX_STREAM_SOURCES
        stream/bounded_buffer.cpp
        stream/credit_window.cpp
        stream/line_framer.cpp
        stream/stream.cpp
    )
    set(PIPESHELLX_INVENTORY_SOURCES
        inventory/inventory.cpp
    )
    set(PIPESHELLX_SINK_SOURCES
        sink/group_sink.cpp
        sink/json_sink.cpp
        sink/stream_sink.cpp
    )
    set(PIPESHELLX_OS_SOURCES
        os/posix/child_exit.cpp
        os/posix/console.cpp
        os/posix/error.cpp
        os/posix/handle.cpp
        os/posix/io.cpp
        os/posix/paths.cpp
        os/posix/pipe.cpp
        os/posix/poller.cpp
        os/posix/poller_poll.cpp
        os/posix/process.cpp
        os/posix/signal.cpp
        os/posix/signal_source.cpp
        os/posix/system.cpp
    )
    if(APPLE OR CMAKE_SYSTEM_NAME MATCHES "BSD|DragonFly")
        list(APPEND PIPESHELLX_OS_SOURCES
            os/posix/child_exit_kqueue.cpp
            os/posix/poller_kqueue.cpp
            os/posix/signal_source_kqueue.cpp)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND PIPESHELLX_OS_SOURCES
            os/posix/child_exit_pidfd.cpp
            os/posix/poller_epoll.cpp
            os/posix/signal_source_signalfd.cpp)
    endif()
endif()

# Core library
add_library(pipeshellx_lib
    ${PIPESHELLX_OS_SOURCES}
    ${PIPESHELLX_STREAM_SOURCES}
    ${PIPESHELLX_SINK_SOURCES}
    ${PIPESHELLX_INVENTORY_SOURCES}
    runtime/reactor.cpp
    cli_options.cpp
    client_config.cpp
    client_manager.cpp
    command_executor.cpp
    logger.cpp
    process_manager.cpp
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
