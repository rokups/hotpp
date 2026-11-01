# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# A plain Python test of the pure helpers of the lldb formatters. lldb is not needed.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME PythonLldbFormatters
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test_hot_formatters.py)
else()
    message(STATUS "python3 not found: the test of the lldb formatters is off")
endif()
