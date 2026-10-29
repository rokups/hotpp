# AOT compilation tests (S12, design M7).
#  integration_aot_parity_tests: links the loader. It runs the same demo with the loader and
#    with the AOT program aot_demo, and checks that aot_demo has no loader or Clang symbol.
#  integration_aot_registry_tests: links only spp_aot and AOT scripts (no loader).
add_executable(integration_aot_parity_tests aot_parity_tests.cpp
    ${CMAKE_SOURCE_DIR}/example/engine/Actor.cpp
    ${CMAKE_SOURCE_DIR}/example/engine/Object.cpp
    ${CMAKE_SOURCE_DIR}/example/engine/Util.cpp)
target_include_directories(integration_aot_parity_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests ${CMAKE_SOURCE_DIR}/example/aot)
target_link_libraries(integration_aot_parity_tests PRIVATE spp_backend GTest::gtest_main)
set_target_properties(integration_aot_parity_tests PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(integration_aot_parity_tests aot_demo)
target_compile_definitions(integration_aot_parity_tests PRIVATE
    SPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    AOT_DEMO_PATH="$<TARGET_FILE:aot_demo>")
spp_gtest(integration_aot_parity_tests)

# Two scripts in one AOT library: the demo and the module variables.
spp_add_aot_scripts(TARGET aot_registry_scripts
    SCRIPTS ${CMAKE_SOURCE_DIR}/example/scripts/aot_demo.spp ${CMAKE_SOURCE_DIR}/example/scripts/module_v1.spp
    INCLUDES ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine
    DEPENDS ${SPP_HEADERS})
add_executable(integration_aot_registry_tests aot_registry_tests.cpp
    ${CMAKE_SOURCE_DIR}/example/engine/Actor.cpp
    ${CMAKE_SOURCE_DIR}/example/engine/Util.cpp)
target_include_directories(integration_aot_registry_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests ${CMAKE_SOURCE_DIR}/example/aot)
target_link_libraries(integration_aot_registry_tests PRIVATE aot_registry_scripts GTest::gtest_main)
spp_gtest(integration_aot_registry_tests)
