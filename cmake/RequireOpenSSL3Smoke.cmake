cmake_minimum_required(VERSION 3.20)

foreach(_required IN ITEMS PIPESHELLX_BUILD_DIR PIPESHELLX_SOURCE_DIR PIPESHELLX_GENERATOR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RequireOpenSSL3Smoke.cmake requires -D${_required}=...")
    endif()
endforeach()

set(_probe_build "${PIPESHELLX_BUILD_DIR}/openssl-required-smoke")
file(REMOVE_RECURSE "${_probe_build}")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -S "${PIPESHELLX_SOURCE_DIR}"
            -B "${_probe_build}"
            -G "${PIPESHELLX_GENERATOR}"
            -DPIPESHELLX_NATIVE_TRANSPORT=ON
            -DPIPESHELLX_BUILD_TESTS=OFF
            -DPIPESHELLX_BUILD_BENCH=OFF
            -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE
    RESULT_VARIABLE _probe_result
    OUTPUT_VARIABLE _probe_stdout
    ERROR_VARIABLE _probe_stderr)
set(_probe_output "${_probe_stdout}${_probe_stderr}")
if(_probe_result EQUAL 0)
    message(FATAL_ERROR "native transport configured successfully without OpenSSL 3")
endif()
if(NOT _probe_output MATCHES "PIPESHELLX_NATIVE_TRANSPORT=ON requires OpenSSL >= 3\\.0")
    message(FATAL_ERROR "native transport failure did not explain the OpenSSL 3 requirement:\n${_probe_output}")
endif()
