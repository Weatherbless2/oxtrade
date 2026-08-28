add_executable(oxapp_smoke_test
    test_smoke.cpp
)

target_link_libraries(oxapp_smoke_test PRIVATE oxapp_core)

add_test(NAME oxapp_smoke_test COMMAND oxapp_smoke_test)

add_executable(oxmbs_config_logging_test
    test_config_logging.cpp
)

target_link_libraries(oxmbs_config_logging_test PRIVATE oxmbs_core)

add_test(NAME oxmbs_config_logging_test COMMAND oxmbs_config_logging_test)

add_executable(oxapp_worker_supervisor_test
    test_worker_supervisor.cpp
)

target_link_libraries(oxapp_worker_supervisor_test PRIVATE oxapp_core)

add_test(NAME oxapp_worker_supervisor_test COMMAND oxapp_worker_supervisor_test)

add_executable(oxmbs_systemv_ipc_test
    test_systemv_ipc.cpp
)

target_link_libraries(oxmbs_systemv_ipc_test PRIVATE oxmbs_core)

add_test(NAME oxmbs_systemv_ipc_test COMMAND oxmbs_systemv_ipc_test)

add_executable(oxmbs_task_protocol_test
    test_task_protocol.cpp
)

target_link_libraries(oxmbs_task_protocol_test PRIVATE oxmbs_core)

add_test(NAME oxmbs_task_protocol_test COMMAND oxmbs_task_protocol_test)

add_executable(oxmbs_workfunc_router_test
    test_workfunc_router.cpp
)

target_link_libraries(oxmbs_workfunc_router_test PRIVATE oxmbs_core)

add_test(NAME oxmbs_workfunc_router_test COMMAND oxmbs_workfunc_router_test)

add_executable(oxmbs_query_worker_runtime_test
    test_query_worker_runtime.cpp
)

target_link_libraries(oxmbs_query_worker_runtime_test PRIVATE oxmbs_core oxapp_core)

add_test(NAME oxmbs_query_worker_runtime_test COMMAND oxmbs_query_worker_runtime_test)

if(UNIX)
    add_library(oxmbs_lbm_test_module SHARED
        lbm_test_module.cpp
    )

    target_include_directories(oxmbs_lbm_test_module PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    target_compile_features(oxmbs_lbm_test_module PRIVATE cxx_std_17)

    add_executable(oxmbs_lbm_loader_test
        test_lbm_loader.cpp
    )

    target_link_libraries(oxmbs_lbm_loader_test PRIVATE oxmbs_core oxapp_core)
    target_compile_definitions(oxmbs_lbm_loader_test PRIVATE
        OXMBS_LBM_TEST_LIBRARY="$<TARGET_FILE:oxmbs_lbm_test_module>"
    )
    add_dependencies(oxmbs_lbm_loader_test oxmbs_lbm_test_module)  # 测试运行前先保证 mock .so 已生成。

    add_test(NAME oxmbs_lbm_loader_test COMMAND oxmbs_lbm_loader_test)

    add_executable(oxmbs_mock_lbm_end_to_end_test
        test_mock_lbm_end_to_end.cpp
    )
    target_link_libraries(oxmbs_mock_lbm_end_to_end_test PRIVATE oxmbs_core oxapp_core)
    target_compile_definitions(oxmbs_mock_lbm_end_to_end_test PRIVATE
        OXMBS_MOCK_LBM_LIBRARY="$<TARGET_FILE:oxmbs_mock_lbm>"
    )
    add_dependencies(oxmbs_mock_lbm_end_to_end_test oxmbs_mock_lbm)  # 集成测试必须加载本工单构建出的 Mock-only 动态库。
add_test(NAME oxmbs_mock_lbm_end_to_end_test COMMAND oxmbs_mock_lbm_end_to_end_test)

add_executable(oxmwh_service_test
    test_oxmwh.cpp
)

target_link_libraries(oxmwh_service_test PRIVATE oxmwh_core)

add_test(NAME oxmwh_service_test COMMAND oxmwh_service_test)

    # 性能基准不注册为 CTest：它输出分位数和吞吐，不使用容易受机器负载影响的固定耗时断言。
    add_library(oxmbs_route_latency_lbm SHARED
        performance/route_latency_lbm.cpp
    )
    target_include_directories(oxmbs_route_latency_lbm PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_compile_features(oxmbs_route_latency_lbm PRIVATE cxx_std_17)

    add_executable(oxmbs_route_latency_benchmark
        performance/benchmark_workfunc_routes.cpp
    )
    target_link_libraries(oxmbs_route_latency_benchmark PRIVATE oxmbs_core oxapp_core)
    target_compile_definitions(oxmbs_route_latency_benchmark PRIVATE
        OXMBS_ROUTE_LATENCY_LBM_LIBRARY="$<TARGET_FILE:oxmbs_route_latency_lbm>"
    )
    add_dependencies(oxmbs_route_latency_benchmark oxmbs_route_latency_lbm)
endif()
