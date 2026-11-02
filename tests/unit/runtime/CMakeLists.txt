# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# Unit tests of the runtime header. They need no backend and no script.
add_executable(unit_runtime_tests
    checked_ptr_tests.cpp
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
    new_shared_tests.cpp
    clone_state_tests.cpp
    abstract_called_tests.cpp
    command_line_tests.cpp
    task_tests.cpp
    scheduler_tests.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Object.cpp)
target_link_libraries(unit_runtime_tests PRIVATE hot_runtime engine_harness GTest::gtest_main)
target_include_directories(unit_runtime_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
# The task tests need C++20 coroutines. The rest of the project stays C++17.
set_target_properties(unit_runtime_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
hot_gtest(unit_runtime_tests)
