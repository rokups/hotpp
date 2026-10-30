# spp_add_scripts: compile scripts with the tool sppc. There are two modes.
#
#   spp_add_scripts(TARGET name SCRIPTS a.spp b.spp
#                   [INCLUDES dir...] [FLAGS cxxflag...] [DEPENDS file...]
#                   [OUT_DIR dir] [AFTER target])
#   spp_add_scripts(TARGET name SCRIPTS a.spp b.spp AOT
#                   [INCLUDES dir...] [PRELUDES file...] [FLAGS sppc-flag...]
#                   [DEPENDS file...] [OUT_DIR dir])
#
# The function also calls spp_script_host with the name TARGET. The host has the scripts
# and the INCLUDES. In the default mode it has the FLAGS too. The language server uses
# the host for completion.
#
# Default mode (no AOT): the function only runs sppc. It makes one custom target "name".
# The target makes the library OUT_DIR/<script name>.g<n>.so for each script. OUT_DIR
# defaults to ${CMAKE_BINARY_DIR}/spp_scripts. Load such a library with
# spp::module_from_library. The variable SPP_SCRIPTS_<TARGET>_LIBRARIES gets the paths.
# SPPC_ARGS are extra arguments for sppc, in both modes. The generation is 1; the option
# --gen n in SPPC_ARGS sets another one (the tests build later generations this way).
# FLAGS are compile flags (--cxxflag). The generations of one class
# share a slot file in OUT_DIR. Build generation 2 after generation 1: give AFTER the
# target of generation 1.
#
# AOT mode (design M7): the function runs "sppc --aot" and makes the static library "name"
# from the generated C++. The library defines spp::aot_table (include/spp/aot.h). It links
# spp_aot (PUBLIC), so a program that links "name" has no loader, no reload and no Clang.
# INCLUDES are the include dirs of the translator and of the compile. FLAGS are flags
# for sppc. OUT_DIR defaults to ${CMAKE_BINARY_DIR}/spp_aot/<name>. AOT mode has no GEN
# and no AFTER. PRELUDES is only for AOT mode.
#
# String interpolation "${...}": the C++ standard picks the format library at build time
# (cmake/SppFormat.cmake). The standard is CMAKE_CXX_STANDARD, else 17.
# A standard less than 20 selects fmt. C++20 and later select std::format, unless the CMake
# option SPP_USE_FMT is ON. With fmt, the function gets fmt (SPP_FMT_PROVIDER: an installed
# package, else CPM.cmake, by default), passes --format=fmt to sppc and gives the compile the fmt include directory.
# In AOT mode the library "name" links the target spp_fmt (fmt). There is no runtime switch.
function(spp_add_scripts)
    cmake_parse_arguments(A "AOT" "TARGET;OUT_DIR;AFTER" "SCRIPTS;INCLUDES;PRELUDES;FLAGS;DEPENDS;SPPC_ARGS" ${ARGN})
    if(A_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "spp_add_scripts: unknown arguments: ${A_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT A_TARGET OR NOT A_SCRIPTS)
        message(FATAL_ERROR "spp_add_scripts needs TARGET and SCRIPTS")
    endif()
    if(A_AOT)
        if(DEFINED A_AFTER)
            message(FATAL_ERROR "spp_add_scripts: AFTER does not work with AOT (target ${A_TARGET})")
        endif()
    else()
        set(A_GEN 1)
        list(FIND A_SPPC_ARGS --gen gen_at)
        if(NOT gen_at EQUAL -1)
            math(EXPR gen_at "${gen_at} + 1")
            list(GET A_SPPC_ARGS ${gen_at} A_GEN)
        endif()
        if(A_PRELUDES)
            message(FATAL_ERROR "spp_add_scripts: PRELUDES works only with AOT (target ${A_TARGET})")
        endif()
    endif()

    # The format library of "${...}".
    spp_effective_cxx_standard(std_num "")
    spp_format_library(format_lib ${std_num})
    set(fmt_flags "")
    set(fmt_includes "")
    if(format_lib STREQUAL "fmt")
        spp_provide_fmt()
        get_property(fmt_dir GLOBAL PROPERTY SPP_FMT_INCLUDE_DIR)
        set(fmt_flags -DFMT_HEADER_ONLY)
        if(fmt_dir)
            list(APPEND fmt_flags -I${fmt_dir})
            set(fmt_includes ${fmt_dir})
        endif()
    endif()

    set(scripts "")
    foreach(s ${A_SCRIPTS})
        get_filename_component(abs ${s} ABSOLUTE)
        list(APPEND scripts ${abs})
    endforeach()
    set(args "")
    foreach(i ${A_INCLUDES})
        list(APPEND args -I ${i})
    endforeach()

    set(format_args --std=c++${std_num})
    if(format_lib STREQUAL "fmt")
        list(APPEND format_args --format=fmt)
    endif()

    if(A_AOT)
        if(NOT A_OUT_DIR)
            set(A_OUT_DIR ${CMAKE_BINARY_DIR}/spp_aot/${A_TARGET})
        endif()
        set(sources "")
        foreach(s ${A_SCRIPTS})
            get_filename_component(stem ${s} NAME_WLE)
            list(APPEND sources ${A_OUT_DIR}/${stem}.aot.cpp)
        endforeach()
        set(registry ${A_OUT_DIR}/spp_aot_registry.cpp)
        foreach(p ${A_PRELUDES})
            get_filename_component(abs ${p} ABSOLUTE)
            list(APPEND args --prelude ${abs})
        endforeach()
        add_custom_command(
            OUTPUT ${sources} ${registry}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${A_OUT_DIR}
            COMMAND sppc --aot ${scripts} --out-dir ${A_OUT_DIR} ${args} ${A_FLAGS} ${format_args} ${A_SPPC_ARGS}
            DEPENDS sppc ${scripts} ${A_PRELUDES} ${A_DEPENDS}
            COMMENT "sppc --aot: ${A_SCRIPTS}"
            VERBATIM)
        add_library(${A_TARGET} STATIC ${sources} ${registry})
        target_link_libraries(${A_TARGET} PUBLIC spp_aot)
        if(format_lib STREQUAL "fmt")
            target_link_libraries(${A_TARGET} PUBLIC spp_fmt)
        endif()
        # The generated code has parameters that a script does not use.
        target_compile_options(${A_TARGET} PRIVATE -Wno-unused-parameter)
        target_include_directories(${A_TARGET} PUBLIC ${A_INCLUDES})
        # The language server gets a script host with the same scripts and includes.
        spp_script_host(NAME ${A_TARGET} SCRIPTS ${scripts} INCLUDES ${A_INCLUDES} ${fmt_includes})
        return()
    endif()

    if(NOT A_OUT_DIR)
        set(A_OUT_DIR ${CMAKE_BINARY_DIR}/spp_scripts)
    endif()
    set(outputs "")
    foreach(s ${A_SCRIPTS})
        get_filename_component(stem ${s} NAME_WLE)
        list(APPEND outputs ${A_OUT_DIR}/${stem}.g${A_GEN}.so)
    endforeach()
    # The backend of sppc adds the fmt include directory and the define to the compile.
    set(all_flags ${A_FLAGS} ${fmt_flags})
    foreach(f ${A_FLAGS})
        list(APPEND args --cxxflag ${f})
    endforeach()
    list(APPEND args ${format_args})
    add_custom_command(
        OUTPUT ${outputs}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${A_OUT_DIR}
        COMMAND sppc ${scripts} --out-dir ${A_OUT_DIR}
                --cxx ${CMAKE_CXX_COMPILER} ${args} --gen ${A_GEN} ${A_SPPC_ARGS}
        DEPENDS sppc ${scripts} ${A_DEPENDS}
        COMMENT "sppc: generation ${A_GEN} of ${A_SCRIPTS}"
        VERBATIM)
    add_custom_target(${A_TARGET} DEPENDS ${outputs})
    if(A_AFTER)
        add_dependencies(${A_TARGET} ${A_AFTER})
    endif()
    set(SPP_SCRIPTS_${A_TARGET}_LIBRARIES ${outputs} PARENT_SCOPE)
    # The language server gets a script host with the same scripts, includes and flags.
    spp_script_host(NAME ${A_TARGET} SCRIPTS ${scripts}
                    INCLUDES ${A_INCLUDES} FLAGS ${all_flags})
endfunction()

# spp_script_host: describe a script host for the language server spp-ls.
#
#   spp_script_host(NAME name [TARGET target] [SCRIPT_DIRS dir...] [SCRIPTS file...]
#                   [INCLUDES dir...] [FLAGS flag...])
#
# The function writes the JSON file ${SPP_HOSTS_DIR}/<name>.json when CMake generates
# the build system. It does not build anything. The language server reads the file.
# The cache variable SPP_HOSTS_DIR defaults to ${CMAKE_SOURCE_DIR}/.cache/spp/hosts.
#
# The include dirs, defines and options of TARGET are in the file. They include the
# usage requirements of the linked libraries. The dir ${CMAKE_SOURCE_DIR}/include is
# always the first include dir. Then come the include dirs of the target, then INCLUDES. FLAGS
# come after the options of the target. A UTILITY target has no such lists, so the function
# skips them. A relative path in SCRIPT_DIRS, SCRIPTS or INCLUDES is relative to the current
# source directory.
#
# The defines, the options and the include dirs of the target can have any character that a
# path or a flag can have, such as a quote or a backslash. A generator expression cannot
# escape such a character for JSON. So the function writes them to three side files, one entry
# on each line: ${SPP_HOSTS_DIR}/<name>.defines, <name>.options and <name>.includes. The JSON
# file names them in "definesFile", "optionsFile" and "includesFile". A line needs no escape.
# The .includes file has the raw value of the property INCLUDE_DIRECTORIES of the target. A
# relative entry is relative to the source directory of the target. The JSON file has this
# directory in "sourceDir", and spp-ls resolves the relative entries with it.
# The explicit INCLUDES, FLAGS, SCRIPT_DIRS and SCRIPTS go into the JSON file with a full JSON
# escape. They must be plain values: a generator expression in them has no effect.
#
# Two limits remain. Both come from the line format and from the CMake lists:
#  - A path with a newline cannot be written in a line. spp-ls reads it as two paths.
#  - A path with a semicolon is a CMake list separator. CMake splits such a path in two
#    entries in INCLUDES and in the property, and the two parts are not real paths.
set(SPP_HOSTS_DIR "${CMAKE_SOURCE_DIR}/.cache/spp/hosts" CACHE PATH
    "Directory for the script host files that spp-ls reads")

# Make a text safe inside a JSON string. Also make it safe for file(GENERATE CONTENT): a "$"
# would start a generator expression, so the function writes it as "$<1:$>".
function(_spp_host_escape out text)
    string(REPLACE "\\" "\\\\" text "${text}")
    string(REPLACE "\"" "\\\"" text "${text}")
    string(REPLACE "\n" "\\n" text "${text}")
    string(REPLACE "\r" "\\r" text "${text}")
    string(REPLACE "\t" "\\t" text "${text}")
    # The other control characters (1 to 31) become \u00XX.
    set(hex "0123456789abcdef")
    foreach(n RANGE 1 31)
        string(ASCII ${n} ch)
        string(FIND "${text}" "${ch}" pos)
        if(NOT pos EQUAL -1)
            math(EXPR hi "${n} / 16")
            math(EXPR lo "${n} % 16")
            string(SUBSTRING "${hex}" ${hi} 1 h)
            string(SUBSTRING "${hex}" ${lo} 1 l)
            string(REPLACE "${ch}" "\\u00${h}${l}" text "${text}")
        endif()
    endforeach()
    string(REPLACE "$" "$<1:$>" text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

# Make the JSON array text from a list of strings. An empty list gives [].
function(_spp_host_json_array out)
    set(text "")
    set(sep "")
    foreach(i IN LISTS ARGN)
        _spp_host_escape(e "${i}")
        string(APPEND text "${sep}\"${e}\"")
        set(sep ",")
    endforeach()
    set(${out} "[${text}]" PARENT_SCOPE)
endfunction()

function(spp_script_host)
    cmake_parse_arguments(A "" "NAME;TARGET" "SCRIPT_DIRS;SCRIPTS;INCLUDES;FLAGS" ${ARGN})
    if(NOT A_NAME OR (NOT A_SCRIPT_DIRS AND NOT A_SCRIPTS))
        message(FATAL_ERROR "spp_script_host needs NAME and SCRIPT_DIRS or SCRIPTS")
    endif()
    set(use_target FALSE)
    if(A_TARGET)
        if(NOT TARGET ${A_TARGET})
            message(FATAL_ERROR "spp_script_host: the target ${A_TARGET} does not exist")
        endif()
        get_target_property(type ${A_TARGET} TYPE)
        if(NOT type STREQUAL "UTILITY")
            set(use_target TRUE)
        endif()
    endif()

    # The array texts. The paths stay as they are. A relative path is relative to sourceDir,
    # and spp-ls resolves it.
    set(includes "${CMAKE_SOURCE_DIR}/include")
    list(APPEND includes ${A_INCLUDES})
    _spp_host_json_array(j_dirs ${A_SCRIPT_DIRS})
    _spp_host_json_array(j_scripts ${A_SCRIPTS})
    _spp_host_json_array(j_includes ${includes})
    _spp_host_json_array(j_flags ${A_FLAGS})

    if(use_target)
        set(t ${A_TARGET})
        # One entry on each line. A literal newline joins the entries.
        set(nl "\n")
        set(side_defines
            "$<JOIN:$<REMOVE_DUPLICATES:$<FILTER:$<TARGET_PROPERTY:${t},COMPILE_DEFINITIONS>,EXCLUDE,^$>>,${nl}>${nl}")
        set(side_options
            "$<JOIN:$<REMOVE_DUPLICATES:$<FILTER:$<TARGET_PROPERTY:${t},COMPILE_OPTIONS>,EXCLUDE,^$>>,${nl}>${nl}")
        set(side_includes
            "$<JOIN:$<FILTER:$<TARGET_PROPERTY:${t},INCLUDE_DIRECTORIES>,EXCLUDE,^$>,${nl}>${nl}")
        file(GENERATE OUTPUT "${SPP_HOSTS_DIR}/${A_NAME}.defines" CONTENT "${side_defines}")
        file(GENERATE OUTPUT "${SPP_HOSTS_DIR}/${A_NAME}.options" CONTENT "${side_options}")
        file(GENERATE OUTPUT "${SPP_HOSTS_DIR}/${A_NAME}.includes" CONTENT "${side_includes}")
    endif()

    _spp_host_escape(e_name "${A_NAME}")
    _spp_host_escape(e_bin "${CMAKE_BINARY_DIR}")
    _spp_host_escape(e_src "${CMAKE_CURRENT_SOURCE_DIR}")
    _spp_host_escape(e_cxx "${CMAKE_CXX_COMPILER}")
    set(res "")
    if(DEFINED CLANG_RESOURCE_DIR)
        set(res "${CLANG_RESOURCE_DIR}")
    endif()
    _spp_host_escape(e_res "${res}")

    set(json "{\n")
    string(APPEND json "  \"version\": 1,\n")
    string(APPEND json "  \"name\": \"${e_name}\",\n")
    string(APPEND json "  \"binaryDir\": \"${e_bin}\",\n")
    string(APPEND json "  \"sourceDir\": \"${e_src}\",\n")
    string(APPEND json "  \"compiler\": \"${e_cxx}\",\n")
    # The language standard of the scripts. The generated code needs C++17 only, so the scripts
    # use the standard of the consuming target. Without a target, CMAKE_CXX_STANDARD or 17 applies.
    set(std_num 17)
    if(DEFINED CMAKE_CXX_STANDARD AND NOT CMAKE_CXX_STANDARD STREQUAL "")
        set(std_num ${CMAKE_CXX_STANDARD})
    endif()
    if(use_target)
        set(std_num "$<IF:$<BOOL:$<TARGET_PROPERTY:${A_TARGET},CXX_STANDARD>>,$<TARGET_PROPERTY:${A_TARGET},CXX_STANDARD>,${std_num}>")
    endif()
    string(APPEND json "  \"std\": \"c++${std_num}\",\n")
    string(APPEND json "  \"resourceDir\": \"${e_res}\",\n")
    string(APPEND json "  \"scriptDirs\": ${j_dirs},\n")
    string(APPEND json "  \"scripts\": ${j_scripts},\n")
    string(APPEND json "  \"includeDirs\": ${j_includes},\n")
    string(APPEND json "  \"defines\": [],\n")
    if(use_target)
        string(APPEND json "  \"definesFile\": \"${e_name}.defines\",\n")
        string(APPEND json "  \"optionsFile\": \"${e_name}.options\",\n")
        string(APPEND json "  \"includesFile\": \"${e_name}.includes\",\n")
    endif()
    string(APPEND json "  \"flags\": ${j_flags}\n")
    string(APPEND json "}\n")
    file(GENERATE OUTPUT "${SPP_HOSTS_DIR}/${A_NAME}.json" CONTENT "${json}")
endfunction()
