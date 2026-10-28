# Unit tests of the runtime header. They need no backend and no script.
add_executable(unit_runtime_tests
    deref_tests.cpp
    migration_tests.cpp
    module_state_tests.cpp
    intrusive_handle_tests.cpp
    ref_arg_tests.cpp
    shell_tests.cpp)
target_link_libraries(unit_runtime_tests PRIVATE spp_runtime GTest::gtest_main)
gtest_discover_tests(unit_runtime_tests)
