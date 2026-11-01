# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# Async methods and hot::Task at run time: a real host, the backends, the AOT code and reloads.
# The tests need C++20 coroutines. The rest of the project stays C++17. The C++ standard of this
# directory is 20, so hot_add_scripts gives "--std=c++20" to hotc. The test skips the folder
# when the compiler has no <coroutine>.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
#include <coroutine>
#if !defined(__cpp_impl_coroutine)
#error no coroutines
#endif
int main() { std::coroutine_handle<> h; return h ? 1 : 0; }" HOT_HAVE_COROUTINE)
if(NOT HOT_HAVE_COROUTINE)
    message(STATUS "Hot++: the compiler has no <coroutine>; the async integration tests are off")
    return()
endif()

set(ASYNC_SCRIPTS
    ${PROJECT_SOURCE_DIR}/tests/scripts/async_host_v1.hot
    ${PROJECT_SOURCE_DIR}/tests/scripts/async_override.hot
    ${PROJECT_SOURCE_DIR}/tests/scripts/async_cpp.cpp)
# The AOT code of the scripts. The test loads it with Loader::load_aot.
hot_add_scripts(TARGET async_aot_scripts AOT SCRIPTS ${ASYNC_SCRIPTS}
    INCLUDES ${PROJECT_SOURCE_DIR}/tests/engine ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS ${HOT_HEADERS} ${CMAKE_CURRENT_SOURCE_DIR}/async_gate.h)

add_executable(integration_async_tests
    async_host_tests.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(integration_async_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(integration_async_tests PRIVATE async_aot_scripts hot_backend GTest::gtest_main)
set_target_properties(integration_async_tests PROPERTIES ENABLE_EXPORTS ON CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(integration_async_tests PRIVATE
    HOT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    ASYNC_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
hot_gtest(integration_async_tests)
