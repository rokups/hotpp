# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# Link helper for a shared library of script code that is not made by hotc.
#
#   hot_module_link_options(<var> [SYSTEM <name>])
#   hot_allow_undefined_symbols(<target>)
#
# Such a library has undefined hot:: symbols. The host (hot_runtime) provides them when it
# loads the library. An ELF linker accepts this. The Mach-O linker does not: it needs
# "-Wl,-undefined,dynamic_lookup". Other systems need no option.
# hot_module_link_options sets <var> to the options for the system. SYSTEM defaults to
# CMAKE_SYSTEM_NAME. hot_allow_undefined_symbols adds the options to the target.
# hotc uses the same flags for the modules that it builds (src/runtime/command_line.cpp).
function(hot_module_link_options var)
    cmake_parse_arguments(PARSE_ARGV 1 A "" "SYSTEM" "")
    if(NOT A_SYSTEM)
        set(A_SYSTEM "${CMAKE_SYSTEM_NAME}")
    endif()
    set(options "")
    if(A_SYSTEM STREQUAL "Darwin")
        set(options "LINKER:-undefined,dynamic_lookup")
    endif()
    set(${var} "${options}" PARENT_SCOPE)
endfunction()

function(hot_allow_undefined_symbols target)
    hot_module_link_options(_hot_options)
    if(_hot_options)
        target_link_options(${target} PRIVATE ${_hot_options})
    endif()
endfunction()
