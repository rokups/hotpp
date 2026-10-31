# Unit tests of the runtime header. They need no backend and no script.
add_executable(unit_runtime_tests
    coalesce_tests.cpp
    deref_tests.cpp
    runtime_checks_tests.cpp
    migration_tests.cpp
    loader_reset_tests.cpp
    loader_unit_tests.cpp
    module_state_tests.cpp
    intrusive_handle_tests.cpp
    weak_handle_tests.cpp
    shell_tests.cpp
    log_text_tests.cpp
    file_watcher_tests.cpp
    entry_counter_tests.cpp
    scratch_tests.cpp
    paths_tests.cpp
    trace_tests.cpp
    new_shared_tests.cpp)
target_link_libraries(unit_runtime_tests PRIVATE hot_runtime engine_harness GTest::gtest_main)
target_include_directories(unit_runtime_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
hot_gtest(unit_runtime_tests)
