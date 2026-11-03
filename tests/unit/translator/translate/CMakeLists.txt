# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The tests of hotc::translate for each language feature.
add_executable(unit_translator_translate_tests
    abstract_translation_tests.cpp
    access_translation_tests.cpp
    alias_translation_tests.cpp
    collection_switch_translation_tests.cpp
    builtin_type_translation_tests.cpp
    constructor_translation_tests.cpp
    convenience_translation_tests.cpp
    borrowed_storage_translation_tests.cpp
    native_pointer_field_translation_tests.cpp
    default_handle_translation_tests.cpp
    safe_call_translation_tests.cpp
    default_value_translation_tests.cpp
    handle_translation_tests.cpp
    handle_override_translation_tests.cpp
    weak_handle_translation_tests.cpp
    value_use_translation_tests.cpp
    script_attribute_translation_tests.cpp
    import_translation_tests.cpp
    script_import_translation_tests.cpp
    inherited_handle_translation_tests.cpp
    lambda_translation_tests.cpp
    generated_name_translation_tests.cpp
    module_decl_translation_tests.cpp
    override_translation_tests.cpp
    inheritance_translation_tests.cpp
    interface_translation_tests.cpp
    script_interface_translation_tests.cpp
    reload_hook_translation_tests.cpp
    reload_contract_translation_tests.cpp
    prelude_translation_tests.cpp
    module_attrs_translation_tests.cpp
    property_translation_tests.cpp
    reference_translation_tests.cpp
    scope_translation_tests.cpp
    structured_binding_translation_tests.cpp
    script_kinds_translation_tests.cpp
    stub_check_translation_tests.cpp
    string_interpolation_translation_tests.cpp
    template_base_translation_tests.cpp
    namespaced_base_translation_tests.cpp
    typed_access_translation_tests.cpp
    source_map_translation_tests.cpp)
target_link_libraries(unit_translator_translate_tests PRIVATE hotc_lib GTest::gtest_main)
target_include_directories(unit_translator_translate_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/translator ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(unit_translator_translate_tests PRIVATE HOT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
hot_gtest(unit_translator_translate_tests)
