# Closures that outlive a generation, and the unload of a retired generation.
add_executable(integration_closure_tests
    closure_tests.cpp
    generation_unload_tests.cpp
    ${CMAKE_SOURCE_DIR}/example/engine/Actor.cpp
    ${CMAKE_SOURCE_DIR}/example/engine/Util.cpp)
target_include_directories(integration_closure_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(integration_closure_tests PRIVATE spp_backend GTest::gtest_main)
set_target_properties(integration_closure_tests PROPERTIES ENABLE_EXPORTS ON)
target_compile_definitions(integration_closure_tests PRIVATE SPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
gtest_discover_tests(integration_closure_tests)
