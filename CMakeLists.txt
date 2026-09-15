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
    src/oxmbs/market/market_wire_broadcaster.cpp
    src/oxmbs/auth/auth_service.cpp
    src/oxmbs/service/qmt_service_router.cpp
    src/oxmbs/service/qmt_repository_adapter.cpp
)

if(UNIX)
    # HP-Socket 不作为“系统已安装包”猜测，必须由构建调用方明确给出已核验的版本根目录。
    find_package(PostgreSQL REQUIRED)
    set(HPSOCKET_ROOT "/root/projects/quantfabric-study/QuantFabric/XAPI/HP-Socket/5.8.2"
        CACHE PATH "HP-Socket 5.8.2 root directory")
    if(NOT EXISTS "${HPSOCKET_ROOT}/include/HPSocket4C.h" OR
       NOT EXISTS "${HPSOCKET_ROOT}/lib/libhpsocket4c.so")
        message(FATAL_ERROR
            "HP-Socket 5.8.2 was not found under HPSOCKET_ROOT='${HPSOCKET_ROOT}'. "
            "Pass -DHPSOCKET_ROOT=/absolute/path/to/HP-Socket/5.8.2.")
    endif()

    add_library(hpsocket4c SHARED IMPORTED GLOBAL)
    set_target_properties(hpsocket4c PROPERTIES
        IMPORTED_LOCATION "${HPSOCKET_ROOT}/lib/libhpsocket4c.so"
        INTERFACE_INCLUDE_DIRECTORIES "${HPSOCKET_ROOT}/include"
    )
    target_sources(oxmbs_core PRIVATE
        src/oxmbs/network/pack_message.cpp
        src/oxmbs/network/hp_socket_server.cpp
        src/oxmbs/network/hp_socket_bridge.cpp
        src/oxmbs/storage/postgres_pool.cpp
        src/oxmbs/storage/qmt_repository.cpp
    )
    target_link_libraries(oxmbs_core PUBLIC hpsocket4c PostgreSQL::PostgreSQL)
    set_property(TARGET oxmbs_core APPEND PROPERTY
        BUILD_RPATH "${HPSOCKET_ROOT}/lib")
endif()

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
    add_library(oxmbs_lbm_qmt SHARED
        src/oxmbs/lbm/lbm_qmt_module.cpp
        src/oxmbs/ipc/task_protocol.cpp
        src/oxmbs/service/qmt_service_router.cpp
    )
    target_include_directories(oxmbs_lbm_qmt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(oxmbs_lbm_qmt PRIVATE cxx_std_17)
    target_compile_options(oxmbs_lbm_qmt PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(oxmbs_lbm_qmt PRIVATE PostgreSQL::PostgreSQL)  # 模块独立编译也必须显式获得 QmtRepository 端口所需的 libpq 头文件与链接边界。
    set_target_properties(oxmbs_lbm_qmt PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lbm")  # 部署时复制到 funclist.xml 相对 lbm/ 目录，XML 不绑定构建路径。

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
