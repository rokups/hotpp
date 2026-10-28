# spp_add_scripts: compile scripts ahead of time with the tool sppc.
#
#   spp_add_scripts(TARGET name SCRIPTS a.script b.script GEN n
#                   [INCLUDES dir...] [FLAGS flag...] [DEPENDS file...]
#                   [OUT_DIR dir] [AFTER target])
#
# The function only runs sppc. It makes one custom target "name". The target
# makes the library OUT_DIR/<script name>.g<n>.so for each script. OUT_DIR
# defaults to ${CMAKE_BINARY_DIR}/spp_scripts. Load such a library with
# script::module_from_library. The variable SPP_SCRIPTS_<TARGET>_LIBRARIES gets the paths.
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
endfunction()
