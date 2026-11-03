# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# Reload tests. The first group loads hand-written libraries. The second group
# uses the backend that make_default_backend returns (the environment variable HOT_BACKEND chooses it). The file gen3_missing.so does not exist on purpose.
add_executable(integration_reload_tests
    handwritten_generation_tests.cpp
    reload_migration_tests.cpp
    script_reload_tests.cpp
    script_language_tests.cpp
    handle_reload_tests.cpp
    weak_handle_tests.cpp
    cycle_handle_tests.cpp
    hook_tests.cpp
    constructor_tests.cpp
    ctor_table_tests.cpp
    script_kinds_tests.cpp
    clone_tests.cpp
    value_use_tests.cpp
    operator_struct_tests.cpp
    operator_class_tests.cpp
    operator_use_class_tests.cpp
    native_ptr_field_tests.cpp
    template_handle_tests.cpp
    using_alias_tests.cpp
    module_state_tests.cpp
    cpp_script_tests.cpp
    property_tests.cpp
    exception_tests.cpp
    collection_switch_tests.cpp
    inheritance_tests.cpp
    cross_module_tests.cpp
    interface_tests.cpp
    script_interface_tests.cpp
    runtime_polish_tests.cpp
    core_module_tests.cpp
    host_type_tests.cpp
    reload_contract_tests.cpp
    full_reload_tests.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Harness.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Object.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(integration_reload_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(integration_reload_tests PRIVATE hot_backend engine_harness GTest::gtest_main)
set_target_properties(integration_reload_tests PROPERTIES ENABLE_EXPORTS ON)
# Windows: the fixtures link the import library of this test, so they build after it.
if(NOT WIN32)
    add_dependencies(integration_reload_tests gen1 gen2 gen4_base stress_generation)
endif()
target_compile_definitions(integration_reload_tests PRIVATE
    HOT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    GEN1_SO="$<TARGET_FILE:gen1>"
    GEN2_SO="$<TARGET_FILE:gen2>"
    GEN4_BASE_SO="$<TARGET_FILE:gen4_base>"
    STRESS_SO="$<TARGET_FILE:stress_generation>"
    GEN3_MISSING_SO="${CMAKE_BINARY_DIR}/gen3_missing.so")
hot_gtest(integration_reload_tests)
