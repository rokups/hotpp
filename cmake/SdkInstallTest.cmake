# The test of the installed SDK. Run it with "cmake -P". It does these steps:
#   1. cmake --install <BUILD_DIR> --prefix <WORK_DIR>/prefix, then move the prefix to
#      <WORK_DIR>/prefix_moved
#   2. configure samples/aot against the prefix (HOT_ROOT is empty, so find_package(Hot) runs)
#   3. build the sample and run its tests.
# Variables (-D): BUILD_DIR, SAMPLE_DIR, WORK_DIR, CXX_COMPILER, CXX_FLAGS, GENERATOR, CONFIG.
foreach(v BUILD_DIR SAMPLE_DIR WORK_DIR CXX_COMPILER GENERATOR)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "SdkInstallTest.cmake needs -D${v}=...")
    endif()
endforeach()

# Run a command. Stop with an error when it fails.
function(_run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "Failed (${rc}): ${ARGN}")
    endif()
endfunction()

set(prefix ${WORK_DIR}/prefix)
set(moved_prefix ${WORK_DIR}/prefix_moved)
set(sample_build ${WORK_DIR}/sample_aot)
file(REMOVE_RECURSE ${prefix} ${moved_prefix} ${sample_build})

set(config_args "")
if(CONFIG)
    set(config_args --config ${CONFIG})
endif()
_run(${CMAKE_COMMAND} --install ${BUILD_DIR} --prefix ${prefix} ${config_args})
# Move the prefix. The SDK must work from the new place (it is relocatable).
file(RENAME ${prefix} ${moved_prefix})
set(prefix ${moved_prefix})
_run(${CMAKE_COMMAND} -S ${SAMPLE_DIR} -B ${sample_build} -G ${GENERATOR}
     -DHOT_ROOT= -DCMAKE_PREFIX_PATH=${prefix}
     -DCMAKE_CXX_COMPILER=${CXX_COMPILER} "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}"
     -DCMAKE_BUILD_TYPE=${CONFIG})
_run(${CMAKE_COMMAND} --build ${sample_build} ${config_args})
set(ctest_config "")
if(CONFIG)
    set(ctest_config -C ${CONFIG})
endif()
_run(${CMAKE_CTEST_COMMAND} --test-dir ${sample_build} ${ctest_config} --output-on-failure)
