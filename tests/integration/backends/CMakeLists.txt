# Backend tests: scripts that hotc compiled ahead of time, the tool hotc, and the JIT backend.
set(BACKEND_TEST_SOURCES
    precompiled_script_tests.cpp
    hotc_tool_tests.cpp
    diagnostic_mistake_tests.cpp
    debug_info_tests.cpp
    generated_name_compile_tests.cpp
    vfs_backend_tests.cpp
    backend_selection_tests.cpp)
if(HOT_JIT_ENABLED)
    list(APPEND BACKEND_TEST_SOURCES jit_backend_tests.cpp)
endif()
add_executable(integration_backend_tests ${BACKEND_TEST_SOURCES}
    ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Object.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(integration_backend_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(integration_backend_tests PRIVATE hot_backend hot_clang_backend GTest::gtest_main)
set_target_properties(integration_backend_tests PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(integration_backend_tests pre_gen1 pre_gen2 pre_native1 pre_native2 pre_handle1 pre_handle2 pre_handle3 hotc)
target_compile_definitions(integration_backend_tests PRIVATE
    HOT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    HOT_SCRATCH_DIR="${HOT_SCRATCH_DIR}/tests"
    HOTC_PATH="$<TARGET_FILE:hotc>"
    HOT_CXX="${CMAKE_CXX_COMPILER}"
    PRE_V1_SO="${CMAKE_BINARY_DIR}/hot_scripts/player_v1.g1.so"
    PRE_V2_SO="${CMAKE_BINARY_DIR}/hot_scripts/player_v2.g2.so"
    PRE_NATIVE_V1_SO="${CMAKE_BINARY_DIR}/hot_scripts/native_std_v1.g1.so"
    PRE_NATIVE_V2_SO="${CMAKE_BINARY_DIR}/hot_scripts/native_std_v2.g2.so"
    PRE_HANDLE_V1_SO="${CMAKE_BINARY_DIR}/hot_scripts/handle_v1.g1.so"
    PRE_HANDLE_V2_SO="${CMAKE_BINARY_DIR}/hot_scripts/handle_v2.g2.so"
    PRE_HANDLE_V1B_SO="${CMAKE_BINARY_DIR}/hot_scripts/handle_v1.g3.so")
if(HOT_JIT_ENABLED)
    target_compile_definitions(integration_backend_tests PRIVATE HOT_RESOURCE_DIR="${CLANG_RESOURCE_DIR}")
endif()
hot_gtest(integration_backend_tests)
