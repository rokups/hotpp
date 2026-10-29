# spp_add_scripts: compile scripts ahead of time with the tool sppc.
#
# The function also calls spp_script_host with the name TARGET. The host has the scripts,
# the INCLUDES and the FLAGS. The language server uses it for completion.
#
#   spp_add_scripts(TARGET name SCRIPTS a.spp b.spp GEN n
#                   [INCLUDES dir...] [FLAGS flag...] [DEPENDS file...]
#                   [OUT_DIR dir] [AFTER target])
#
# The function only runs sppc. It makes one custom target "name". The target
# makes the library OUT_DIR/<script name>.g<n>.so for each script. OUT_DIR
# defaults to ${CMAKE_BINARY_DIR}/spp_scripts. Load such a library with
# spp::module_from_library. The variable SPP_SCRIPTS_<TARGET>_LIBRARIES gets the paths.
#
# The generations of one class share a slot file in OUT_DIR. Build generation 2
# after generation 1: give AFTER the target of generation 1.
function(spp_add_scripts)
    cmake_parse_arguments(A "" "TARGET;GEN;OUT_DIR;AFTER" "SCRIPTS;INCLUDES;FLAGS;DEPENDS" ${ARGN})
    if(NOT A_TARGET OR NOT A_SCRIPTS OR NOT A_GEN)
        message(FATAL_ERROR "spp_add_scripts needs TARGET, SCRIPTS and GEN")
    endif()
    if(NOT A_OUT_DIR)
        set(A_OUT_DIR ${CMAKE_BINARY_DIR}/spp_scripts)
    endif()
    set(scripts "")
    set(outputs "")
    foreach(s ${A_SCRIPTS})
        get_filename_component(abs ${s} ABSOLUTE)
        get_filename_component(stem ${s} NAME_WLE)
        list(APPEND scripts ${abs})
        list(APPEND outputs ${A_OUT_DIR}/${stem}.g${A_GEN}.so)
    endforeach()
    set(args "")
    foreach(i ${A_INCLUDES})
        list(APPEND args -I ${i})
    endforeach()
    foreach(f ${A_FLAGS})
        list(APPEND args --cxxflag ${f})
    endforeach()
    add_custom_command(
        OUTPUT ${outputs}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${A_OUT_DIR}
        COMMAND sppc ${scripts} --gen ${A_GEN} --out-dir ${A_OUT_DIR}
                --cxx ${CMAKE_CXX_COMPILER} ${args}
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
                    INCLUDES ${A_INCLUDES} FLAGS ${A_FLAGS})
endfunction()

# spp_add_shipped_scripts: build scripts into the program (shipping mode, design M7).
#
#   spp_add_shipped_scripts(TARGET name SCRIPTS a.spp b.spp
#                           [INCLUDES dir...] [PRELUDES file...] [FLAGS sppc-flag...]
#                           [DEPENDS file...] [OUT_DIR dir])
#
# The function runs "sppc --ship" and makes the static library "name" from the generated
# C++. The library defines spp::shipped_table (include/spp/shipped.h). It links spp_ship
# (PUBLIC), so a program that links "name" has no loader, no reload and no Clang. INCLUDES
# are the include dirs of the translator and of the compile. OUT_DIR defaults to
# ${CMAKE_BINARY_DIR}/spp_ship/<name>.
# The function also calls spp_script_host with the name TARGET (scripts and INCLUDES).
function(spp_add_shipped_scripts)
    cmake_parse_arguments(A "" "TARGET;OUT_DIR" "SCRIPTS;INCLUDES;PRELUDES;FLAGS;DEPENDS" ${ARGN})
    if(NOT A_TARGET OR NOT A_SCRIPTS)
        message(FATAL_ERROR "spp_add_shipped_scripts needs TARGET and SCRIPTS")
    endif()
    if(NOT A_OUT_DIR)
        set(A_OUT_DIR ${CMAKE_BINARY_DIR}/spp_ship/${A_TARGET})
    endif()
    set(scripts "")
    set(sources "")
    foreach(s ${A_SCRIPTS})
        get_filename_component(abs ${s} ABSOLUTE)
        get_filename_component(stem ${s} NAME_WLE)
        list(APPEND scripts ${abs})
        list(APPEND sources ${A_OUT_DIR}/${stem}.ship.cpp)
    endforeach()
    set(registry ${A_OUT_DIR}/spp_shipped_registry.cpp)
    set(args "")
    foreach(i ${A_INCLUDES})
        list(APPEND args -I ${i})
    endforeach()
    foreach(p ${A_PRELUDES})
        get_filename_component(abs ${p} ABSOLUTE)
        list(APPEND args --prelude ${abs})
    endforeach()
    add_custom_command(
        OUTPUT ${sources} ${registry}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${A_OUT_DIR}
        COMMAND sppc --ship ${scripts} --out-dir ${A_OUT_DIR} ${args} ${A_FLAGS}
        DEPENDS sppc ${scripts} ${A_PRELUDES} ${A_DEPENDS}
        COMMENT "sppc --ship: ${A_SCRIPTS}"
        VERBATIM)
    add_library(${A_TARGET} STATIC ${sources} ${registry})
    target_link_libraries(${A_TARGET} PUBLIC spp_ship)
    # The generated code has parameters that a script does not use.
    target_compile_options(${A_TARGET} PRIVATE -Wno-unused-parameter)
    target_include_directories(${A_TARGET} PUBLIC ${A_INCLUDES})
    # The language server gets a script host with the same scripts and includes.
    spp_script_host(NAME ${A_TARGET} SCRIPTS ${scripts} INCLUDES ${A_INCLUDES})
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
# always the first include dir. INCLUDES and FLAGS come after the lists of the target.
# A UTILITY target has no such lists, so the function skips them.
#
# A define or an option that has a double quote or a backslash does not go into the
# file. A generator expression cannot escape such a character.
set(SPP_HOSTS_DIR "${CMAKE_SOURCE_DIR}/.cache/spp/hosts" CACHE PATH
    "Directory for the script host files that spp-ls reads")

# Make a text safe inside a JSON string and inside a generator expression.
function(_spp_host_escape out text)
    string(REPLACE "\\" "\\\\" text "${text}")
    string(REPLACE "\"" "\\\"" text "${text}")
    string(REPLACE ">" "$<ANGLE-R>" text "${text}")
    string(REPLACE "," "$<COMMA>" text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

# Make a JSON array from a list of paths or strings. The list is static.
function(_spp_host_static_array out)
    set(items "")
    foreach(i ${ARGN})
        _spp_host_escape(e "${i}")
        list(APPEND items "${e}")
    endforeach()
    set(${out} "${items}" PARENT_SCOPE)
endfunction()

# Make the JSON array text from a generator expression list. An empty list gives [].
function(_spp_host_json_array out list)
    set(l "$<FILTER:${list},EXCLUDE,^$>")
    set(${out} "[$<$<BOOL:${l}>:\"$<JOIN:${l},\"$<COMMA>\">\">]" PARENT_SCOPE)
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

    # Make absolute paths.
    set(dirs "")
    foreach(d ${A_SCRIPT_DIRS})
        get_filename_component(abs ${d} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
        list(APPEND dirs ${abs})
    endforeach()
    set(scripts "")
    foreach(s ${A_SCRIPTS})
        get_filename_component(abs ${s} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
        list(APPEND scripts ${abs})
    endforeach()
    set(includes "${CMAKE_SOURCE_DIR}/include")
    foreach(i ${A_INCLUDES})
        get_filename_component(abs ${i} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
        list(APPEND includes ${abs})
    endforeach()
    _spp_host_static_array(dirs "${dirs}")
    _spp_host_static_array(scripts "${scripts}")
    _spp_host_static_array(extra_includes ${includes})
    _spp_host_static_array(extra_flags ${A_FLAGS})

    # A define or an option with a quote or a backslash is not in the file.
    set(bad [=["|\\]=])
    set(target_includes "")
    set(target_defines "")
    set(target_flags "")
    if(use_target)
        set(t ${A_TARGET})
        set(target_includes
            "$<FILTER:$<TARGET_PROPERTY:${t},INCLUDE_DIRECTORIES>,EXCLUDE,${bad}>")
        set(target_defines
            "$<FILTER:$<TARGET_PROPERTY:${t},COMPILE_DEFINITIONS>,EXCLUDE,${bad}>")
        set(target_flags
            "$<FILTER:$<TARGET_PROPERTY:${t},COMPILE_OPTIONS>,EXCLUDE,${bad}>")
    endif()
    # The project include dir comes first, then the target dirs, then INCLUDES.
    list(GET extra_includes 0 first_include)
    list(REMOVE_AT extra_includes 0)
    _spp_host_json_array(j_includes
        "$<REMOVE_DUPLICATES:${first_include};${target_includes};${extra_includes}>")
    _spp_host_json_array(j_defines "$<REMOVE_DUPLICATES:${target_defines}>")
    _spp_host_json_array(j_flags "$<REMOVE_DUPLICATES:${target_flags};${extra_flags}>")
    _spp_host_json_array(j_dirs "${dirs}")
    _spp_host_json_array(j_scripts "${scripts}")

    _spp_host_escape(e_name "${A_NAME}")
    _spp_host_escape(e_bin "${CMAKE_BINARY_DIR}")
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
    string(APPEND json "  \"compiler\": \"${e_cxx}\",\n")
    string(APPEND json "  \"std\": \"c++20\",\n")
    string(APPEND json "  \"resourceDir\": \"${e_res}\",\n")
    string(APPEND json "  \"scriptDirs\": ${j_dirs},\n")
    string(APPEND json "  \"scripts\": ${j_scripts},\n")
    string(APPEND json "  \"includeDirs\": ${j_includes},\n")
    string(APPEND json "  \"defines\": ${j_defines},\n")
    string(APPEND json "  \"flags\": ${j_flags}\n")
    string(APPEND json "}\n")
    file(GENERATE OUTPUT "${SPP_HOSTS_DIR}/${A_NAME}.json" CONTENT "${json}")
endfunction()
