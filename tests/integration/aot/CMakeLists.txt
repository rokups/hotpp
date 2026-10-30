# aot_demo has the C++ of a script in the binary. It links spp_aot and not spp_runtime, so it has
# no loader, no migration, no dlopen and no Clang. Actor.cpp and Util.cpp are the engine.
spp_add_scripts(TARGET aot_demo_scripts AOT SCRIPTS ${PROJECT_SOURCE_DIR}/tests/scripts/aot_demo.spp
                    INCLUDES ${PROJECT_SOURCE_DIR}/tests/engine
                    DEPENDS ${SPP_HEADERS})
add_executable(aot_demo aot_demo_main.cpp ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(aot_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aot_demo PRIVATE aot_demo_scripts)

# AOT compilation tests (S12, design M7).
#  integration_aot_parity_tests: links the loader. It runs the same demo with the loader and
#    with the AOT program aot_demo, and checks that aot_demo has no loader or Clang symbol.
#  integration_aot_registry_tests: links only spp_aot and AOT scripts (no loader).
add_executable(integration_aot_parity_tests aot_parity_tests.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Object.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(integration_aot_parity_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(integration_aot_parity_tests PRIVATE spp_backend GTest::gtest_main)
set_target_properties(integration_aot_parity_tests PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(integration_aot_parity_tests aot_demo)
target_compile_definitions(integration_aot_parity_tests PRIVATE
    SPP_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    AOT_DEMO_PATH="$<TARGET_FILE:aot_demo>")
spp_gtest(integration_aot_parity_tests)

# Two scripts in one AOT library: the demo and the module variables.
spp_add_scripts(TARGET aot_registry_scripts AOT
    SCRIPTS ${PROJECT_SOURCE_DIR}/tests/scripts/aot_demo.spp ${PROJECT_SOURCE_DIR}/tests/scripts/module_v1.spp
    INCLUDES ${PROJECT_SOURCE_DIR}/tests/engine
    DEPENDS ${SPP_HEADERS})
add_executable(integration_aot_registry_tests aot_registry_tests.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(integration_aot_registry_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(integration_aot_registry_tests PRIVATE aot_registry_scripts GTest::gtest_main)
spp_gtest(integration_aot_registry_tests)
