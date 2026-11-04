# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# A plain Python test of the pure helpers of the lldb formatters. lldb is not needed.
# A second test runs a real lldb session (see below). A third test checks the natvis file.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME PythonLldbFormatters
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test_hot_formatters.py)
else()
    message(STATUS "python3 not found: the test of the lldb formatters is off")
endif()

# The natvis file: well formed XML, and the types that it names exist in the headers.
if(Python3_Interpreter_FOUND)
    add_test(NAME PythonNatvis
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test_natvis.py)
endif()

# A real lldb session. The program lldb_target is an AOT build of lldb_target/target.hot.
# The tests need lldb, python3 and the Hot++ AOT targets, and not MSVC (the program has GCC and
# Clang flags). The scripts return 77 (ctest shows "skipped") when lldb does not work here.
# The program is not part of "all": the fixture test PythonLldbTarget builds it.
find_program(HOT_LLDB lldb)
find_program(HOT_LLDB_DAP NAMES lldb-dap lldb-vscode)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "python3 not found: the lldb session tests are off")
elseif(NOT HOT_LLDB)
    message(STATUS "lldb not found: the lldb session tests are off")
elseif(MSVC OR NOT TARGET hotc OR NOT TARGET hot::aot)
    message(STATUS "The Hot++ AOT targets are missing, or the compiler is MSVC: the lldb session tests are off")
else()
    add_subdirectory(lldb_target)
    add_test(NAME PythonLldbTarget
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target lldb_target --config $<CONFIG>)
    set_tests_properties(PythonLldbTarget PROPERTIES FIXTURES_SETUP lldb_target TIMEOUT 600)
    add_test(NAME PythonLldbSession
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test_lldb_session.py
                     $<TARGET_FILE:lldb_target> ${PROJECT_SOURCE_DIR}/tools/lldb/hot_formatters.py
                     ${CMAKE_CURRENT_SOURCE_DIR}/lldb_target/target.hot ${HOT_LLDB})
    set_tests_properties(PythonLldbSession PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120
                         FIXTURES_REQUIRED lldb_target)
    # The debug adapter lldb-dap: a breakpoint in the .hot file must stop and name the .hot file.
    if(HOT_LLDB_DAP)
        add_test(NAME PythonLldbDap
                 COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test_lldb_dap.py
                         $<TARGET_FILE:lldb_target> ${CMAKE_CURRENT_SOURCE_DIR}/lldb_target/target.hot
                         ${HOT_LLDB_DAP})
        set_tests_properties(PythonLldbDap PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120
                             FIXTURES_REQUIRED lldb_target)
    endif()
endif()
