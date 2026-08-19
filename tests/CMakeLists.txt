add_executable(oxapp_smoke_test
    test_smoke.cpp
)

target_link_libraries(oxapp_smoke_test PRIVATE oxapp_core)

add_test(NAME oxapp_smoke_test COMMAND oxapp_smoke_test)

add_executable(oxapp_worker_supervisor_test
    test_worker_supervisor.cpp
)

target_link_libraries(oxapp_worker_supervisor_test PRIVATE oxapp_core)

add_test(NAME oxapp_worker_supervisor_test COMMAND oxapp_worker_supervisor_test)
