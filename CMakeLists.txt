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
    src/oxmbs/ipc/task_protocol.cpp
    src/oxmbs/ipc/task_correlation.cpp
    src/oxmbs/routing/message_route_policy.cpp
    src/oxmbs/routing/workfunc_router.cpp
    src/oxmbs/worker/query_worker_runtime.cpp
    src/oxmbs/worker/mock_query_service.cpp
    src/oxmbs/worker/reply_collector.cpp
    src/oxmbs/logging/log_level.cpp
    src/oxmbs/logging/run_logger.cpp
    src/oxmbs/runtime/system_info.cpp
    src/oxmbs/lbm/lbm_abi.cpp
    src/oxmbs/lbm/funclist_registry.cpp
    src/oxmbs/lbm/lbm_module_loader.cpp
    src/oxmbs/lbm/lbm_task_dispatcher.cpp
    src/oxmbs/mock/mock_backend_protocol.cpp
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

add_library(oxmwh_core
    src/oxmwh/async_market_publisher.cpp
    src/oxmwh/market_event.cpp
    src/oxmwh/market_wire.cpp
    src/oxmwh/oxmwh_service.cpp
    src/oxmwh/mock_market_source.cpp
)

target_include_directories(oxmwh_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(oxmwh_core PUBLIC cxx_std_17)

# LBM 在 Linux/WSL 通过 dlopen/dlsym 延迟加载；CMake 负责在需要的平台传递 libdl。
target_link_libraries(oxmbs_core PUBLIC ${CMAKE_DL_LIBS})

if(MSVC)
    target_compile_options(oxapp_core PRIVATE /W4 /permissive-)
    target_compile_options(oxmbs_core PRIVATE /W4 /permissive-)
    target_compile_options(oxmwh_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(oxapp_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(oxmbs_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(oxmwh_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(oxApp
    src/oxapp/main.cpp
)

target_link_libraries(oxApp PRIVATE oxapp_core)

add_executable(oxMbsBootstrap
    src/oxmbs/main.cpp
)

target_link_libraries(oxMbsBootstrap PRIVATE oxmbs_core oxapp_core)

if(UNIX)
    # 仅 #24 本地联调使用：动态库保留 #8 的真实 dlopen/dlsym 边界，不承担 #11-#13 正式业务职责。
    add_library(oxmbs_mock_lbm SHARED
        src/oxmbs/mock/mock_lbm.cpp
        src/oxmbs/mock/mock_backend_protocol.cpp  # Mock .so 独立解析其临时路由，不能依赖主进程静态库导出符号。
    )
    target_include_directories(oxmbs_mock_lbm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(oxmbs_mock_lbm PRIVATE cxx_std_17)

    add_executable(oxMbsMockBackend
        src/oxmbs/mock/mock_backend_main.cpp
        src/oxmbs/mock/mock_gateway.cpp
    )
    target_link_libraries(oxMbsMockBackend PRIVATE oxmbs_core oxapp_core)
    target_compile_definitions(oxMbsMockBackend PRIVATE
        OXMBS_MOCK_LBM_LIBRARY="$<TARGET_FILE:oxmbs_mock_lbm>"
    )
    add_dependencies(oxMbsMockBackend oxmbs_mock_lbm)  # 后端启动前必须已有可由生成 funclist 引用的 Mock-only .so。
endif()

set_target_properties(oxMbsBootstrap PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(TARGET oxMbsMockBackend)
    set_target_properties(oxMbsMockBackend PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

set_target_properties(oxApp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
