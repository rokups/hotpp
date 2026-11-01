# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# aot_demo has the C++ of a script in the binary. It links hot_aot and not hot_runtime, so it has
# no loader, no migration, no dlopen and no Clang. Actor.cpp and Util.cpp are the engine.
hot_add_scripts(TARGET aot_demo_scripts AOT SCRIPTS ${PROJECT_SOURCE_DIR}/tests/scripts/aot_demo.hot
                    INCLUDES ${PROJECT_SOURCE_DIR}/tests/engine
                    DEPENDS ${HOT_HEADERS})
add_executable(aot_demo aot_demo_main.cpp ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(aot_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aot_demo PRIVATE aot_demo_scripts hot::aot)

# AOT compilation tests (S12, design M7).
#  integration_aot_parity_tests: links the loader. It runs the same demo with the loader and
#    with the AOT program aot_demo, and checks that aot_demo has no loader or Clang symbol.
#  integration_aot_registry_tests: links only hot_aot and AOT scripts (no loader).
add_executable(integration_aot_parity_tests aot_parity_tests.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Object.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(integration_aot_parity_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
# aot_demo_scripts links only the core: Loader::load_aot takes the table (no hot_aot here).
target_link_libraries(integration_aot_parity_tests PRIVATE aot_demo_scripts hot_backend GTest::gtest_main)
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
target_link_libraries(integration_aot_registry_tests PRIVATE aot_registry_scripts hot::aot GTest::gtest_main)
hot_gtest(integration_aot_registry_tests)

# Loader::load_aot and Backend::seed_slots: the loader, a backend and the AOT scripts in one
# program. The scripts library links only the core of the runtime.
add_executable(integration_aot_load_tests aot_load_tests.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(integration_aot_load_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(integration_aot_load_tests PRIVATE aot_registry_scripts hot_backend hot_clang_backend GTest::gtest_main)
set_target_properties(integration_aot_load_tests PROPERTIES ENABLE_EXPORTS ON)
target_compile_definitions(integration_aot_load_tests PRIVATE HOT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
hot_gtest(integration_aot_load_tests)

# Override and revert of one AOT unit at run time: two units, the loader and a backend.
hot_add_scripts(TARGET aot_override_scripts AOT
    SCRIPTS ${PROJECT_SOURCE_DIR}/tests/scripts/override_a_v1.hot ${PROJECT_SOURCE_DIR}/tests/scripts/override_b_v1.hot
    INCLUDES ${PROJECT_SOURCE_DIR}/tests/engine
    DEPENDS ${HOT_HEADERS})
add_executable(integration_aot_override_tests aot_override_tests.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(integration_aot_override_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(integration_aot_override_tests PRIVATE aot_override_scripts hot_backend GTest::gtest_main)
set_target_properties(integration_aot_override_tests PROPERTIES ENABLE_EXPORTS ON)
target_compile_definitions(integration_aot_override_tests PRIVATE HOT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
hot_gtest(integration_aot_override_tests)

# Optimized AOT (MODE AOT OPTIMIZED): the same scripts, built with "hotc --aot-optimized". The
# optimized libraries have no slots, so the loader cannot override or reload them.
# aot_demo_opt runs the demo like aot_demo. The parity test compares the two outputs.
hot_add_scripts(TARGET aot_demo_opt_scripts AOT OPTIMIZED SCRIPTS ${PROJECT_SOURCE_DIR}/tests/scripts/aot_demo.hot
                    INCLUDES ${PROJECT_SOURCE_DIR}/tests/engine
                    DEPENDS ${HOT_HEADERS})
add_executable(aot_demo_opt aot_demo_main.cpp ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(aot_demo_opt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aot_demo_opt PRIVATE aot_demo_opt_scripts hot::aot)
add_dependencies(integration_aot_parity_tests aot_demo_opt)
target_compile_definitions(integration_aot_parity_tests PRIVATE AOT_DEMO_OPT_PATH="$<TARGET_FILE:aot_demo_opt>")

# The registry tests and the load tests with the optimized library of two scripts.
hot_add_scripts(TARGET aot_registry_opt_scripts AOT OPTIMIZED
    SCRIPTS ${PROJECT_SOURCE_DIR}/tests/scripts/aot_demo.hot ${PROJECT_SOURCE_DIR}/tests/scripts/module_v1.hot
    INCLUDES ${PROJECT_SOURCE_DIR}/tests/engine
    DEPENDS ${HOT_HEADERS})
add_executable(integration_aot_registry_opt_tests aot_registry_tests.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(integration_aot_registry_opt_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(integration_aot_registry_opt_tests PRIVATE AOT_EXPECT_OPTIMIZED=1)
target_link_libraries(integration_aot_registry_opt_tests PRIVATE aot_registry_opt_scripts hot::aot GTest::gtest_main)
hot_gtest(integration_aot_registry_opt_tests)

add_executable(integration_aot_opt_load_tests aot_opt_load_tests.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(integration_aot_opt_load_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(integration_aot_opt_load_tests PRIVATE aot_registry_opt_scripts hot_backend GTest::gtest_main)
set_target_properties(integration_aot_opt_load_tests PROPERTIES ENABLE_EXPORTS ON)
hot_gtest(integration_aot_opt_load_tests)

# The cover script has the cases that the demo lacks: a noexcept override, a const virtual that
# the shell forwards, an interface base, and a script virtual through a base handle. The parity
# test builds it as plain AOT (aot_cover) and as optimized AOT (aot_cover_opt) and compares the output.
foreach(variant "" _opt)
    set(opt_keyword "")
    if(variant STREQUAL "_opt")
        set(opt_keyword OPTIMIZED)
    endif()
    hot_add_scripts(TARGET aot_cover${variant}_scripts AOT ${opt_keyword}
        SCRIPTS ${PROJECT_SOURCE_DIR}/tests/scripts/aot_cover.hot ${PROJECT_SOURCE_DIR}/tests/scripts/aot_cover_cpp.cpp
        ${PROJECT_SOURCE_DIR}/tests/scripts/aot_opt_fields.hot
        INCLUDES ${PROJECT_SOURCE_DIR}/tests/engine
        DEPENDS ${HOT_HEADERS})
    add_executable(aot_cover${variant} aot_cover_main.cpp ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
        ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
    target_include_directories(aot_cover${variant} PRIVATE ${PROJECT_SOURCE_DIR}/tests/engine)
    target_link_libraries(aot_cover${variant} PRIVATE aot_cover${variant}_scripts hot::aot)
    add_dependencies(integration_aot_parity_tests aot_cover${variant})
endforeach()
target_compile_definitions(integration_aot_parity_tests PRIVATE
    AOT_COVER_PATH="$<TARGET_FILE:aot_cover>"
    AOT_COVER_OPT_PATH="$<TARGET_FILE:aot_cover_opt>")
