# hot_add_scripts: compile scripts with the tool hotc. There are two modes.
#
#   hot_add_scripts(TARGET name SCRIPTS a.hot b.hot
#                   [INCLUDES dir...] [FLAGS cxxflag...] [DEPENDS file...]
#                   [OUT_DIR dir] [AFTER target] [HOTC_ARGS arg...])
#   hot_add_scripts(TARGET name SCRIPTS a.hot b.hot AOT
#                   [INCLUDES dir...] [PRELUDES file...] [FLAGS hotc-flag...]
#                   [DEPENDS file...] [OUT_DIR dir] [HOTC_ARGS arg...])
#
# The function also calls hot_script_host with the name TARGET. The host has the scripts
# and the INCLUDES. In the default mode it has the FLAGS too. The language server uses
# the host for completion.
#
# Default mode (no AOT): the function only runs hotc. It makes one custom target "name".
# The target makes the library OUT_DIR/<script name>.g<n>.so for each script. OUT_DIR
# defaults to ${CMAKE_BINARY_DIR}/hot_scripts. Load such a library with
# hot::module_from_library. The variable HOT_SCRIPTS_<TARGET>_LIBRARIES gets the paths.
# HOTC_ARGS are extra arguments for hotc, in both modes. The generation is 1; the option
# --gen n in HOTC_ARGS sets another one (the tests build later generations this way).
# FLAGS are compile flags (--cxxflag). The generations of one class
# share a slot file in OUT_DIR. Build generation 2 after generation 1: give AFTER the
# target of generation 1.
#
# In both modes, the hotc call always has the include dir of Hot++ (HOT_INCLUDE_DIR) as the
# first -I. INCLUDES come after it.
#
# AOT mode (design M7): the function runs "hotc --aot" and makes the static library "name"
# from the generated C++. The library defines hot::aot_table (include/hot/aot.h). It links
# hot_aot (PUBLIC), so a program that links "name" has no loader, no reload and no Clang.
# INCLUDES are the include dirs of the translator and of the compile. FLAGS are flags
# for hotc. OUT_DIR defaults to ${CMAKE_BINARY_DIR}/hot_aot/<name>. AOT mode has no AFTER
# and always uses generation 1. PRELUDES is only for AOT mode.
#
# String interpolation "${...}": the C++ standard picks the format library at build time
# (cmake/HotFormat.cmake). The standard comes from hot_effective_cxx_standard: the property
# CXX_STANDARD of the target, else CMAKE_CXX_STANDARD, else the Hot++ default 20, else 17.
# A standard less than 20 selects fmt. C++20 and later select std::format, unless the CMake
# option HOT_USE_FMT is ON. With fmt, the function gets fmt (HOT_FMT_PROVIDER: an installed
# package, else CPM.cmake, by default), passes --format=fmt to hotc and gives the compile the fmt include directory.
# In AOT mode the library "name" links the target hot_fmt (fmt). There is no runtime switch.
# The include dir of Hot++. It is relative to this file, so it works in a consumer project.
# It is an internal cache variable, so a project that uses add_subdirectory sees it in every scope.
get_filename_component(_hot_include_dir "${CMAKE_CURRENT_LIST_DIR}/../include" ABSOLUTE)
set(HOT_INCLUDE_DIR "${_hot_include_dir}" CACHE INTERNAL "The include dir of Hot++")

# Sets the variable named by "out" to the Clang resource directory of the build compiler.
# It uses CLANG_RESOURCE_DIR if set. Else it asks CMAKE_CXX_COMPILER (Clang only). Empty if unknown.
function(_hot_resource_dir out)
    set(dir "${CLANG_RESOURCE_DIR}")
    if(NOT dir AND NOT MSVC)
        execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
                        OUTPUT_VARIABLE dir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    set(${out} "${dir}" PARENT_SCOPE)
endfunction()

function(hot_add_scripts)
    cmake_parse_arguments(A "AOT" "TARGET;OUT_DIR;AFTER" "SCRIPTS;INCLUDES;PRELUDES;FLAGS;DEPENDS;HOTC_ARGS" ${ARGN})
    if(A_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "hot_add_scripts: unknown arguments: ${A_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT A_TARGET OR NOT A_SCRIPTS)
        message(FATAL_ERROR "hot_add_scripts needs TARGET and SCRIPTS")
    endif()
    if(A_AOT)
        if(DEFINED A_AFTER)
            message(FATAL_ERROR "hot_add_scripts: AFTER does not work with AOT (target ${A_TARGET})")
        endif()
    else()
        set(A_GEN 1)
        list(FIND A_HOTC_ARGS --gen gen_at)
        if(NOT gen_at EQUAL -1)
            math(EXPR gen_at "${gen_at} + 1")
            list(GET A_HOTC_ARGS ${gen_at} A_GEN)
        endif()
        if(A_PRELUDES)
            message(FATAL_ERROR "hot_add_scripts: PRELUDES works only with AOT (target ${A_TARGET})")
        endif()
    endif()

    # The output name comes from the stem of the script. Two scripts with the same stem clash.
    set(seen_stems "")
    set(seen_scripts "")
    foreach(s ${A_SCRIPTS})
        get_filename_component(stem ${s} NAME_WLE)
        list(FIND seen_stems "${stem}" seen_at)
        if(NOT seen_at EQUAL -1)
            list(GET seen_scripts ${seen_at} seen_script)
            message(FATAL_ERROR "hot_add_scripts: the scripts '${seen_script}' and '${s}' have the same stem '${stem}' (target ${A_TARGET})")
        endif()
        list(APPEND seen_stems "${stem}")
        list(APPEND seen_scripts "${s}")
    endforeach()

    # The format library of "${...}".
    hot_effective_cxx_standard(std_num "")
    hot_format_library(format_lib ${std_num})
    set(fmt_flags "")
    set(fmt_includes "")
    if(format_lib STREQUAL "fmt")
        hot_provide_fmt()
        get_property(fmt_dir GLOBAL PROPERTY HOT_FMT_INCLUDE_DIR)
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
    # The include dir of Hot++ comes first. A caller does not need to pass it in INCLUDES.
    set(args -I ${HOT_INCLUDE_DIR})
    # hotc at build time takes the resource directory of the build compiler. It does not search
    # PATH. This option is build-time only. It does not go into a host binary.
    _hot_resource_dir(res_dir)
    if(res_dir)
        list(APPEND args --resource-dir ${res_dir})
    endif()
    foreach(i ${A_INCLUDES})
        list(APPEND args -I ${i})
    endforeach()

    set(format_args --std=c++${std_num})
    if(format_lib STREQUAL "fmt")
        list(APPEND format_args --format=fmt)
    endif()

    if(A_AOT)
        if(NOT A_OUT_DIR)
            set(A_OUT_DIR ${CMAKE_BINARY_DIR}/hot_aot/${A_TARGET})
        endif()
        set(sources "")
        foreach(s ${A_SCRIPTS})
            get_filename_component(stem ${s} NAME_WLE)
            list(APPEND sources ${A_OUT_DIR}/${stem}.aot.cpp)
        endforeach()
        set(registry ${A_OUT_DIR}/hot_aot_registry.cpp)
        foreach(p ${A_PRELUDES})
            get_filename_component(abs ${p} ABSOLUTE)
            list(APPEND args --prelude ${abs})
        endforeach()
        add_custom_command(
            OUTPUT ${sources} ${registry}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${A_OUT_DIR}
            COMMAND hotc --aot ${scripts} --out-dir ${A_OUT_DIR} ${args} ${A_FLAGS} ${format_args} ${A_HOTC_ARGS}
            DEPENDS hotc ${scripts} ${A_PRELUDES} ${A_DEPENDS}
            COMMENT "hotc --aot: ${A_SCRIPTS}"
            VERBATIM)
        add_library(${A_TARGET} STATIC ${sources} ${registry})
        target_link_libraries(${A_TARGET} PUBLIC hot_aot)
        # The generated code uses the format library that the effective standard selects (std::format
        # from C++20). The library gets this standard as a minimum. A higher standard of the
        # consumer stays, and a consumer that links the library gets the minimum too.
        target_compile_features(${A_TARGET} PUBLIC cxx_std_${std_num})
        if(format_lib STREQUAL "fmt")
            target_link_libraries(${A_TARGET} PUBLIC hot_fmt)
        endif()
        # The generated code has parameters that a script does not use.
        target_compile_options(${A_TARGET} PRIVATE -Wno-unused-parameter)
        target_include_directories(${A_TARGET} PUBLIC ${A_INCLUDES})
        # The language server gets a script host with the same scripts and includes.
        hot_script_host(NAME ${A_TARGET} SCRIPTS ${scripts} INCLUDES ${A_INCLUDES} ${fmt_includes})
        return()
    endif()

    if(NOT A_OUT_DIR)
        set(A_OUT_DIR ${CMAKE_BINARY_DIR}/hot_scripts)
    endif()
    set(outputs "")
    foreach(s ${A_SCRIPTS})
        get_filename_component(stem ${s} NAME_WLE)
        list(APPEND outputs ${A_OUT_DIR}/${stem}.g${A_GEN}.so)
    endforeach()
    # The backend of hotc adds the fmt include directory and the define to the compile.
    set(all_flags ${A_FLAGS} ${fmt_flags})
    foreach(f ${A_FLAGS})
        list(APPEND args --cxxflag ${f})
    endforeach()
    list(APPEND args ${format_args})
    add_custom_command(
        OUTPUT ${outputs}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${A_OUT_DIR}
        COMMAND hotc ${scripts} --out-dir ${A_OUT_DIR}
                --cxx ${CMAKE_CXX_COMPILER} ${args} --gen ${A_GEN} ${A_HOTC_ARGS}
        DEPENDS hotc ${scripts} ${A_DEPENDS}
        COMMENT "hotc: generation ${A_GEN} of ${A_SCRIPTS}"
        VERBATIM)
    add_custom_target(${A_TARGET} DEPENDS ${outputs})
    if(A_AFTER)
        add_dependencies(${A_TARGET} ${A_AFTER})
    endif()
    set(HOT_SCRIPTS_${A_TARGET}_LIBRARIES ${outputs} PARENT_SCOPE)
    # The language server gets a script host with the same scripts, includes and flags.
    hot_script_host(NAME ${A_TARGET} SCRIPTS ${scripts}
                    INCLUDES ${A_INCLUDES} FLAGS ${all_flags})
endfunction()

# hot_add_host: set up the scripts of one host program. The target must exist before the call.
#
#   hot_add_host(TARGET t [MODE AOT|HOT_RELOAD|PRECOMPILED] SCRIPTS a.hot b.hot
#                [INCLUDES dir...] [DEPENDS file...] [PRELUDES file...] [FLAGS flag...]
#                [HOTC_ARGS arg...])
#
# MODE defaults to HOT_RELOAD. The options have the meaning that they have in hot_add_scripts.
#   AOT:         hot_add_scripts makes the static library ${t}_scripts. The target t links it.
#   PRECOMPILED: hot_add_scripts makes the script libraries with the target ${t}_scripts. The
#                target t links hot::runtime, exports its symbols and depends on ${t}_scripts.
#                The libraries go to ${CMAKE_BINARY_DIR}/hot_scripts/${t}, so two hosts do not clash.
#                The variable HOT_SCRIPTS_${t}_LIBRARIES gets the library paths.
#   HOT_RELOAD:  the target t links hot::backend and exports its symbols. A development build
#                gets the define HOT_SCRIPT_DIR (the directory of the first script). A portable
#                build (HOT_PORTABLE) holds no build path. It copies the include dir of Hot++ to
#                <directory of t>/hot/include after the build, where default_paths() finds it.
# In every mode, INCLUDES are include dirs of the target t.
function(hot_add_host)
    cmake_parse_arguments(H "" "TARGET;MODE" "SCRIPTS;INCLUDES;DEPENDS;PRELUDES;FLAGS;HOTC_ARGS" ${ARGN})
    if(H_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "hot_add_host: unknown arguments: ${H_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT H_TARGET OR NOT H_SCRIPTS)
        message(FATAL_ERROR "hot_add_host needs TARGET and SCRIPTS")
    endif()
    if(NOT TARGET ${H_TARGET})
        message(FATAL_ERROR "hot_add_host: the target ${H_TARGET} does not exist")
    endif()
    if(NOT H_MODE)
        set(H_MODE HOT_RELOAD)
    endif()
    if(NOT H_MODE STREQUAL "AOT" AND NOT H_MODE STREQUAL "HOT_RELOAD" AND NOT H_MODE STREQUAL "PRECOMPILED")
        message(FATAL_ERROR "hot_add_host: MODE must be AOT, HOT_RELOAD or PRECOMPILED. It is: ${H_MODE}")
    endif()
    set(t ${H_TARGET})
    set(common_args SCRIPTS ${H_SCRIPTS})
    if(H_INCLUDES)
        list(APPEND common_args INCLUDES ${H_INCLUDES})
    endif()
    if(H_DEPENDS)
        list(APPEND common_args DEPENDS ${H_DEPENDS})
    endif()
    if(H_FLAGS)
        list(APPEND common_args FLAGS ${H_FLAGS})
    endif()
    if(H_HOTC_ARGS)
        list(APPEND common_args HOTC_ARGS ${H_HOTC_ARGS})
    endif()

    if(H_MODE STREQUAL "AOT")
        set(prelude_args "")
        if(H_PRELUDES)
            set(prelude_args PRELUDES ${H_PRELUDES})
        endif()
        hot_add_scripts(TARGET ${t}_scripts AOT ${common_args} ${prelude_args})
        target_link_libraries(${t} PRIVATE ${t}_scripts)
    elseif(H_MODE STREQUAL "PRECOMPILED")
        if(H_PRELUDES)
            message(FATAL_ERROR "hot_add_host: PRELUDES works only with MODE AOT (target ${t})")
        endif()
        hot_add_scripts(TARGET ${t}_scripts OUT_DIR ${CMAKE_BINARY_DIR}/hot_scripts/${t} ${common_args})
        target_link_libraries(${t} PRIVATE hot::runtime)
        set_target_properties(${t} PROPERTIES ENABLE_EXPORTS ON)
        add_dependencies(${t} ${t}_scripts)
        set(HOT_SCRIPTS_${t}_LIBRARIES ${HOT_SCRIPTS_${t}_scripts_LIBRARIES} PARENT_SCOPE)
    else()
        if(H_PRELUDES)
            message(FATAL_ERROR "hot_add_host: PRELUDES works only with MODE AOT (target ${t})")
        endif()
        target_link_libraries(${t} PRIVATE hot::backend)
        set_target_properties(${t} PROPERTIES ENABLE_EXPORTS ON)
        hot_script_host(NAME ${t} TARGET ${t} ${common_args})
        if(HOT_PORTABLE)
            add_custom_command(TARGET ${t} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory ${HOT_INCLUDE_DIR} $<TARGET_FILE_DIR:${t}>/hot/include
                COMMENT "Copy the Hot++ include directory next to ${t}"
                VERBATIM)
        else()
            list(GET H_SCRIPTS 0 first_script)
            get_filename_component(first_abs ${first_script} ABSOLUTE)
            get_filename_component(script_dir ${first_abs} DIRECTORY)
            target_compile_definitions(${t} PRIVATE HOT_SCRIPT_DIR="${script_dir}")
        endif()
    endif()
    target_include_directories(${t} PRIVATE ${H_INCLUDES})
endfunction()

# hot_script_host: describe a script host for the language server hot-ls.
#
#   hot_script_host(NAME name [TARGET target] [SCRIPT_DIRS dir...] [SCRIPTS file...]
#                   [INCLUDES dir...] [FLAGS flag...])
#
# The function writes the JSON file ${HOT_HOSTS_DIR}/<name>.json when CMake generates
# the build system. It does not build anything. The language server reads the file.
# The cache variable HOT_HOSTS_DIR defaults to ${CMAKE_SOURCE_DIR}/.cache/hot/hosts.
#
# The include dirs, defines and options of TARGET are in the file. They include the
# usage requirements of the linked libraries. The include dir of Hot++ (HOT_INCLUDE_DIR) is
# always the first include dir. Then come the include dirs of the target, then INCLUDES. FLAGS
# come after the options of the target. A UTILITY target has no such lists, so the function
# skips them. A relative path in SCRIPT_DIRS, SCRIPTS or INCLUDES is relative to the current
# source directory.
#
# The defines, the options and the include dirs of the target can have any character that a
# path or a flag can have, such as a quote or a backslash. A generator expression cannot
# escape such a character for JSON. So the function writes them to three side files, one entry
# on each line: ${HOT_HOSTS_DIR}/<name>.defines, <name>.options and <name>.includes. The JSON
# file names them in "definesFile", "optionsFile" and "includesFile". A line needs no escape.
# The .includes file has the raw value of the property INCLUDE_DIRECTORIES of the target. A
# relative entry is relative to the source directory of the target. The JSON file has this
# directory in "sourceDir", and hot-ls resolves the relative entries with it.
# The explicit INCLUDES, FLAGS, SCRIPT_DIRS and SCRIPTS go into the JSON file with a full JSON
# escape. They must be plain values: a generator expression in them has no effect.
#
# Two limits remain. Both come from the line format and from the CMake lists:
#  - A path with a newline cannot be written in a line. hot-ls reads it as two paths.
#  - A path with a semicolon is a CMake list separator. CMake splits such a path in two
#    entries in INCLUDES and in the property, and the two parts are not real paths.
set(HOT_HOSTS_DIR "${CMAKE_SOURCE_DIR}/.cache/hot/hosts" CACHE PATH
    "Directory for the script host files that hot-ls reads")

# Make a text safe inside a JSON string. Also make it safe for file(GENERATE CONTENT): a "$"
# would start a generator expression, so the function writes it as "$<1:$>".
function(_hot_host_escape out text)
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
function(_hot_host_json_array out)
    set(text "")
    set(sep "")
    foreach(i IN LISTS ARGN)
        _hot_host_escape(e "${i}")
        string(APPEND text "${sep}\"${e}\"")
        set(sep ",")
    endforeach()
    set(${out} "[${text}]" PARENT_SCOPE)
endfunction()

function(hot_script_host)
    cmake_parse_arguments(A "" "NAME;TARGET" "SCRIPT_DIRS;SCRIPTS;INCLUDES;FLAGS" ${ARGN})
    if(NOT A_NAME OR (NOT A_SCRIPT_DIRS AND NOT A_SCRIPTS))
        message(FATAL_ERROR "hot_script_host needs NAME and SCRIPT_DIRS or SCRIPTS")
    endif()
    set(use_target FALSE)
    if(A_TARGET)
        if(NOT TARGET ${A_TARGET})
            message(FATAL_ERROR "hot_script_host: the target ${A_TARGET} does not exist")
        endif()
        get_target_property(type ${A_TARGET} TYPE)
        if(NOT type STREQUAL "UTILITY")
            set(use_target TRUE)
        endif()
    endif()

    # The array texts. The paths stay as they are. A relative path is relative to sourceDir,
    # and hot-ls resolves it.
    set(includes "${HOT_INCLUDE_DIR}")
    list(APPEND includes ${A_INCLUDES})
    _hot_host_json_array(j_dirs ${A_SCRIPT_DIRS})
    _hot_host_json_array(j_scripts ${A_SCRIPTS})
    _hot_host_json_array(j_includes ${includes})
    _hot_host_json_array(j_flags ${A_FLAGS})

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
        file(GENERATE OUTPUT "${HOT_HOSTS_DIR}/${A_NAME}.defines" CONTENT "${side_defines}")
        file(GENERATE OUTPUT "${HOT_HOSTS_DIR}/${A_NAME}.options" CONTENT "${side_options}")
        file(GENERATE OUTPUT "${HOT_HOSTS_DIR}/${A_NAME}.includes" CONTENT "${side_includes}")
    endif()

    _hot_host_escape(e_name "${A_NAME}")
    _hot_host_escape(e_bin "${CMAKE_BINARY_DIR}")
    _hot_host_escape(e_src "${CMAKE_CURRENT_SOURCE_DIR}")
    _hot_host_escape(e_cxx "${CMAKE_CXX_COMPILER}")
    set(res "")
    if(DEFINED CLANG_RESOURCE_DIR)
        set(res "${CLANG_RESOURCE_DIR}")
    endif()
    _hot_host_escape(e_res "${res}")

    set(json "{\n")
    string(APPEND json "  \"version\": 1,\n")
    string(APPEND json "  \"name\": \"${e_name}\",\n")
    string(APPEND json "  \"binaryDir\": \"${e_bin}\",\n")
    string(APPEND json "  \"sourceDir\": \"${e_src}\",\n")
    string(APPEND json "  \"compiler\": \"${e_cxx}\",\n")
    # The language standard of the scripts. The generated code needs C++17 only, so the scripts
    # use the standard of the consuming target. Without a target, the effective standard
    # (hot_effective_cxx_standard) applies, as in the build of the scripts.
    hot_effective_cxx_standard(std_num "")
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
    file(GENERATE OUTPUT "${HOT_HOSTS_DIR}/${A_NAME}.json" CONTENT "${json}")
endfunction()
