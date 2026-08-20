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
