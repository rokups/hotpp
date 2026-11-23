# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# hot_c_string(<out> <value>)
#
# It puts the value in <out> with each backslash and each quote escaped. Use <out> inside the
# quotes of a C string macro: target_compile_definitions(t PRIVATE NAME="${out}").
function(hot_c_string out value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    set(${out} "${value}" PARENT_SCOPE)
endfunction()
