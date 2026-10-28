# Shipping mode tests (S12, design M7).
#  integration_ship_parity_tests: links the loader. It runs the same demo with the loader and
#    with the shipped program ship_demo, and checks that ship_demo has no loader or Clang symbol.
#  integration_ship_registry_tests: links only spp_ship and shipped scripts (no loader).
add_executable(integration_ship_parity_tests ship_parity_tests.cpp
    ${CMAKE_SOURCE_DIR}/example/engine/Actor.cpp
    ${CMAKE_SOURCE_DIR}/example/engine/Object.cpp
    ${CMAKE_SOURCE_DIR}/example/engine/Util.cpp)
target_include_directories(integration_ship_parity_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests ${CMAKE_SOURCE_DIR}/example/ship)
target_link_libraries(integration_ship_parity_tests PRIVATE spp_backend GTest::gtest_main)
set_target_properties(integration_ship_parity_tests PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(integration_ship_parity_tests ship_demo)
target_compile_definitions(integration_ship_parity_tests PRIVATE
    SPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    SHIP_DEMO_PATH="$<TARGET_FILE:ship_demo>")
gtest_discover_tests(integration_ship_parity_tests)

# Two scripts in one shipped library: the demo and the module variables.
spp_add_shipped_scripts(TARGET ship_registry_scripts
    SCRIPTS ${CMAKE_SOURCE_DIR}/example/scripts/ship_demo.spp ${CMAKE_SOURCE_DIR}/example/scripts/module_v1.spp
    INCLUDES ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine
    DEPENDS ${SPP_HEADERS})
add_executable(integration_ship_registry_tests ship_registry_tests.cpp
    ${CMAKE_SOURCE_DIR}/example/engine/Actor.cpp
    ${CMAKE_SOURCE_DIR}/example/engine/Util.cpp)
target_include_directories(integration_ship_registry_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests ${CMAKE_SOURCE_DIR}/example/ship)
target_link_libraries(integration_ship_registry_tests PRIVATE ship_registry_scripts GTest::gtest_main)
gtest_discover_tests(integration_ship_registry_tests)
