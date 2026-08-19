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
