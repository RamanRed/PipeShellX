cmake_minimum_required(VERSION 3.20)
project(pipeshellx VERSION 0.6.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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
option(PIPESHELLX_NATIVE_TRANSPORT "Build the native mTLS backplane (requires OpenSSL >= 3.0)" ON)
option(PIPESHELLX_STATIC_OPENSSL
       "Prefer static OpenSSL libraries (recommended for self-contained release builds)" OFF)
if(CMAKE_DISABLE_FIND_PACKAGE_OpenSSL AND PIPESHELLX_NATIVE_TRANSPORT)
  message(FATAL_ERROR
    "PIPESHELLX_NATIVE_TRANSPORT=ON requires OpenSSL >= 3.0; "
    "install OpenSSL 3 or configure -DPIPESHELLX_NATIVE_TRANSPORT=OFF")
endif()
if(PIPESHELLX_NATIVE_TRANSPORT)
  if(PIPESHELLX_STATIC_OPENSSL)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
  endif()
  find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)
  message(STATUS "pipeshellx: native transport enabled (OpenSSL ${OPENSSL_VERSION})")
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
set(PIPESHELLX_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/pipeshellx"
    CACHE PATH "Install directory for the pipeshellx CMake package")

# ---------------------------------------------------------------------------
# Warnings: an INTERFACE target linked by every first-party target, so that
# third-party code pulled in via FetchContent is not subject to -Werror.
# ---------------------------------------------------------------------------
add_library(pipeshellx_warnings INTERFACE)
target_compile_options(pipeshellx_warnings INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>
  # Omitted aggregate members are intentionally value-initialized throughout
  # the C++20 designated initializers; GNU and upstream Clang diagnose those
  # under -Wextra even when every omitted member has a safe default.
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-missing-field-initializers>
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

# ---------------------------------------------------------------------------
# Installable CMake package
# ---------------------------------------------------------------------------
install(DIRECTORY include/psx/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/psx
    COMPONENT Development
    FILES_MATCHING
        PATTERN "*.hpp"
        PATTERN "cli" EXCLUDE
        PATTERN "backend.hpp" EXCLUDE)

configure_package_config_file(
    cmake/pipeshellxConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/pipeshellxConfig.cmake"
    INSTALL_DESTINATION "${PIPESHELLX_INSTALL_CMAKEDIR}")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/pipeshellxConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/pipeshellxConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/pipeshellxConfigVersion.cmake"
    DESTINATION "${PIPESHELLX_INSTALL_CMAKEDIR}"
    COMPONENT Development)
install(FILES LICENSE NOTICE README.md CHANGELOG.md
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/pipeshellx"
    COMPONENT Runtime)
