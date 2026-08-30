cmake_minimum_required(VERSION 3.20)

foreach(_required IN ITEMS PIPESHELLX_BUILD_DIR PIPESHELLX_SOURCE_DIR PIPESHELLX_INSTALL_BINDIR
                           PIPESHELLX_INSTALL_INCLUDEDIR PIPESHELLX_INSTALL_LIBDIR
                           PIPESHELLX_INSTALL_DATADIR PIPESHELLX_GENERATOR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunInstallSmoke.cmake requires -D${_required}=...")
    endif()
endforeach()

set(_work_dir "${PIPESHELLX_BUILD_DIR}/packaging-smoke")
set(_prefix "${_work_dir}/prefix")
set(_consumer_build "${_work_dir}/consumer-build")
file(REMOVE_RECURSE "${_work_dir}")

set(_install_command "${CMAKE_COMMAND}" --install "${PIPESHELLX_BUILD_DIR}" --prefix "${_prefix}")
if(DEFINED PIPESHELLX_CONFIG AND NOT "${PIPESHELLX_CONFIG}" STREQUAL "")
    list(APPEND _install_command --config "${PIPESHELLX_CONFIG}")
endif()
execute_process(
    COMMAND ${_install_command}
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_stdout
    ERROR_VARIABLE _install_stderr)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR "cmake --install failed:\n${_install_stdout}${_install_stderr}")
endif()

file(GLOB _installed_programs RELATIVE "${_prefix}/${PIPESHELLX_INSTALL_BINDIR}"
     "${_prefix}/${PIPESHELLX_INSTALL_BINDIR}/*")
list(FIND _installed_programs pipeshellx _lowercase_program_at)
if(_lowercase_program_at EQUAL -1)
    message(FATAL_ERROR "install tree has no exactly-lowercase bin/pipeshellx: ${_installed_programs}")
endif()
set(_installed_executable "${_prefix}/${PIPESHELLX_INSTALL_BINDIR}/pipeshellx")
if(NOT EXISTS "${_installed_executable}")
    message(FATAL_ERROR "installed executable is missing: ${_installed_executable}")
endif()
execute_process(
    COMMAND "${_installed_executable}" --version
    RESULT_VARIABLE _version_result
    OUTPUT_VARIABLE _version_stdout
    ERROR_VARIABLE _version_stderr)
if(NOT _version_result EQUAL 0 OR NOT _version_stdout MATCHES "0\\.6\\.0")
    message(FATAL_ERROR
            "installed pipeshellx --version failed (${_version_result}):\n${_version_stdout}${_version_stderr}")
endif()

set(_package_dir "${_prefix}/${PIPESHELLX_INSTALL_LIBDIR}/cmake/pipeshellx")
foreach(_package_file IN ITEMS pipeshellxConfig.cmake pipeshellxConfigVersion.cmake pipeshellxTargets.cmake)
    if(NOT EXISTS "${_package_dir}/${_package_file}")
        message(FATAL_ERROR "installed CMake package is missing: ${_package_dir}/${_package_file}")
    endif()
endforeach()

file(GLOB _target_files "${_package_dir}/pipeshellxTargets*.cmake")
set(_target_contents "")
foreach(_target_file IN LISTS _target_files)
    file(READ "${_target_file}" _contents)
    string(APPEND _target_contents "${_contents}")
endforeach()
foreach(_forbidden IN ITEMS "${PIPESHELLX_SOURCE_DIR}" "${PIPESHELLX_BUILD_DIR}" "pipeshellx_warnings")
    string(FIND "${_target_contents}" "${_forbidden}" _forbidden_at)
    if(NOT _forbidden_at EQUAL -1)
        message(FATAL_ERROR "installed target export leaks '${_forbidden}'")
    endif()
endforeach()

set(_doc_dir "${_prefix}/${PIPESHELLX_INSTALL_DATADIR}/doc/pipeshellx")
foreach(_doc_file IN ITEMS LICENSE NOTICE THIRD_PARTY_NOTICES.md README.md CHANGELOG.md)
    if(NOT EXISTS "${_doc_dir}/${_doc_file}")
        message(FATAL_ERROR "install tree is missing documentation file: ${_doc_dir}/${_doc_file}")
    endif()
endforeach()

set(_include_dir "${_prefix}/${PIPESHELLX_INSTALL_INCLUDEDIR}")
if(NOT EXISTS "${_include_dir}/psx/pipeline/planner.hpp")
    message(FATAL_ERROR "install tree is missing supported public headers")
endif()
foreach(_internal_header IN ITEMS cli_options.hpp client_config.hpp client_manager.hpp
                                  command_executor.hpp logger.hpp process_manager.hpp ssh_auth.hpp
                                  ssh_transport.hpp terminal_client.hpp psx/cli/run_command.hpp
                                  psx/os/backend.hpp)
    if(EXISTS "${_include_dir}/${_internal_header}")
        message(FATAL_ERROR "install tree exposes internal header: ${_internal_header}")
    endif()
endforeach()

set(_configure_command
    "${CMAKE_COMMAND}"
    -S "${PIPESHELLX_SOURCE_DIR}/tests/packaging/downstream"
    -B "${_consumer_build}"
    -G "${PIPESHELLX_GENERATOR}"
    "-DCMAKE_PREFIX_PATH:PATH=${_prefix}"
    -DCMAKE_BUILD_TYPE=Release)
execute_process(
    COMMAND ${_configure_command}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_stdout
    ERROR_VARIABLE _configure_stderr)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR "downstream configure failed:\n${_configure_stdout}${_configure_stderr}")
endif()

set(_build_command "${CMAKE_COMMAND}" --build "${_consumer_build}")
if(DEFINED PIPESHELLX_CONFIG AND NOT "${PIPESHELLX_CONFIG}" STREQUAL "")
    list(APPEND _build_command --config "${PIPESHELLX_CONFIG}")
endif()
execute_process(
    COMMAND ${_build_command}
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_stdout
    ERROR_VARIABLE _build_stderr)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR "downstream build failed:\n${_build_stdout}${_build_stderr}")
endif()

set(_consumer_executable "${_consumer_build}/bin/pipeshellx_package_consumer")
if(DEFINED PIPESHELLX_CONFIG AND NOT "${PIPESHELLX_CONFIG}" STREQUAL "" AND
   EXISTS "${_consumer_build}/bin/${PIPESHELLX_CONFIG}/pipeshellx_package_consumer")
    set(_consumer_executable "${_consumer_build}/bin/${PIPESHELLX_CONFIG}/pipeshellx_package_consumer")
endif()
execute_process(COMMAND "${_consumer_executable}" RESULT_VARIABLE _consumer_result)
if(NOT _consumer_result EQUAL 0)
    message(FATAL_ERROR "installed pipeshellx::lib consumer exited ${_consumer_result}")
endif()
