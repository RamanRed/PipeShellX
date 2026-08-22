cmake_minimum_required(VERSION 3.20)
project(PipeShellX VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PIPESHELLX_BUILD_TESTS  "Build the GoogleTest suite"                        ON)
option(PIPESHELLX_BUILD_BENCH  "Build the benchmark harness (bench/)"               ON)
option(PIPESHELLX_WERROR       "Treat warnings as errors"                           ON)
option(PIPESHELLX_SYSTEM_GTEST "Use an installed GTest instead of FetchContent"     OFF)
option(PIPESHELLX_NATIVE_TRANSPORT "Build the native mTLS backplane (needs OpenSSL)"  ON)
if(PIPESHELLX_NATIVE_TRANSPORT)
    find_package(OpenSSL)
endif()
set(PIPESHELLX_SANITIZE "" CACHE STRING
    "Comma-separated sanitizers to enable (e.g. address,undefined or thread)")

# Build type default
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ---------------------------------------------------------------------------
# Warnings: an INTERFACE target linked by every first-party target, so that
# third-party code pulled in via FetchContent is not subject to -Werror.
# ---------------------------------------------------------------------------
add_library(pipeshellx_warnings INTERFACE)
target_compile_options(pipeshellx_warnings INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>
  $<$<AND:$<BOOL:${PIPESHELLX_WERROR}>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-Werror>
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
  $<$<AND:$<BOOL:${PIPESHELLX_WERROR}>,$<CXX_COMPILER_ID:MSVC>>:/WX>
)

# ---------------------------------------------------------------------------
# Sanitizers (applied globally so that tests and dependencies agree)
# ---------------------------------------------------------------------------
if(PIPESHELLX_SANITIZE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "PIPESHELLX_SANITIZE needs GCC or Clang (got ${CMAKE_CXX_COMPILER_ID})")
  endif()
  # Accept both "address,undefined" and the CMake list form "address;undefined".
  string(REPLACE ";" "," _pipeshellx_sanitizers "${PIPESHELLX_SANITIZE}")
  message(STATUS "Sanitizers enabled: ${_pipeshellx_sanitizers}")
  add_compile_options(-fsanitize=${_pipeshellx_sanitizers} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${_pipeshellx_sanitizers})
endif()

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(PIPESHELLX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(PIPESHELLX_BUILD_BENCH)
  add_subdirectory(bench)
endif()

# Install headers
install(DIRECTORY include/ DESTINATION include FILES_MATCHING PATTERN "*.hpp")
