cmake_minimum_required(VERSION 3.20)

project(oxApp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_library(oxapp_core
    src/oxapp/application.cpp
    src/oxapp/process/worker_process.cpp
    src/oxapp/process/worker_supervisor.cpp
)

add_library(oxmbs_core
    src/oxmbs/config/mbs_config.cpp
    src/oxmbs/ipc/systemv_message_queue.cpp
    src/oxmbs/ipc/ipc_queue_manager.cpp
    src/oxmbs/logging/log_level.cpp
    src/oxmbs/logging/run_logger.cpp
    src/oxmbs/runtime/system_info.cpp
)

target_include_directories(oxapp_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(oxapp_core PUBLIC cxx_std_17)

target_include_directories(oxmbs_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(oxmbs_core PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(oxapp_core PRIVATE /W4 /permissive-)
    target_compile_options(oxmbs_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(oxapp_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(oxmbs_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(oxApp
    src/oxapp/main.cpp
)

target_link_libraries(oxApp PRIVATE oxapp_core)

add_executable(oxMbsBootstrap
    src/oxmbs/main.cpp
)

target_link_libraries(oxMbsBootstrap PRIVATE oxmbs_core)

set_target_properties(oxMbsBootstrap PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(oxApp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
