# aot_demo has the C++ of a script in the binary. It links hot_aot and not hot_runtime, so it has
# no loader, no migration, no dlopen and no Clang. Actor.cpp and Util.cpp are the engine.
hot_add_scripts(TARGET aot_demo_scripts AOT SCRIPTS ${PROJECT_SOURCE_DIR}/tests/scripts/aot_demo.hot
                    INCLUDES ${PROJECT_SOURCE_DIR}/tests/engine
                    DEPENDS ${HOT_HEADERS})
add_executable(aot_demo aot_demo_main.cpp ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(aot_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aot_demo PRIVATE aot_demo_scripts)

# AOT compilation tests (S12, design M7).
#  integration_aot_parity_tests: links the loader. It runs the same demo with the loader and
#    with the AOT program aot_demo, and checks that aot_demo has no loader or Clang symbol.
#  integration_aot_registry_tests: links only hot_aot and AOT scripts (no loader).
add_executable(integration_aot_parity_tests aot_parity_tests.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Object.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(integration_aot_parity_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(integration_aot_parity_tests PRIVATE hot_backend GTest::gtest_main)
set_target_properties(integration_aot_parity_tests PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(integration_aot_parity_tests aot_demo)
target_compile_definitions(integration_aot_parity_tests PRIVATE
    HOT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    AOT_DEMO_PATH="$<TARGET_FILE:aot_demo>")
hot_gtest(integration_aot_parity_tests)

# Two scripts in one AOT library: the demo and the module variables.
hot_add_scripts(TARGET aot_registry_scripts AOT
    SCRIPTS ${PROJECT_SOURCE_DIR}/tests/scripts/aot_demo.hot ${PROJECT_SOURCE_DIR}/tests/scripts/module_v1.hot
    INCLUDES ${PROJECT_SOURCE_DIR}/tests/engine
    DEPENDS ${HOT_HEADERS})
add_executable(integration_aot_registry_tests aot_registry_tests.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(integration_aot_registry_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(integration_aot_registry_tests PRIVATE aot_registry_scripts GTest::gtest_main)
hot_gtest(integration_aot_registry_tests)
