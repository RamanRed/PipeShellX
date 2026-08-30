cmake_minimum_required(VERSION 3.20)
project(pipeshellx_package_consumer LANGUAGES CXX)

find_package(pipeshellx 0.6 CONFIG REQUIRED)
if(NOT TARGET pipeshellx::lib)
    message(FATAL_ERROR "pipeshellx package did not define pipeshellx::lib")
endif()

add_executable(pipeshellx_package_consumer main.cpp)
set_target_properties(pipeshellx_package_consumer PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_link_libraries(pipeshellx_package_consumer PRIVATE pipeshellx::lib)
