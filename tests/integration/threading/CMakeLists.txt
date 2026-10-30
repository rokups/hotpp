# Worker threads, a barrier and a reload from a third thread.
add_executable(integration_threading_tests
    parallel_tick_tests.cpp
    entry_counter_tests.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(integration_threading_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(integration_threading_tests PRIVATE hot_backend GTest::gtest_main)
set_target_properties(integration_threading_tests PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(integration_threading_tests gen4_base)
target_compile_definitions(integration_threading_tests PRIVATE
    GEN4_BASE_SO="$<TARGET_FILE:gen4_base>"
    GEN3_MISSING_SO="${CMAKE_BINARY_DIR}/gen3_missing.so")
hot_gtest(integration_threading_tests)
