# The samples of Hot++ for consumer projects. Each sample is a small project that uses Hot++ as a library.
add_subdirectory(aot)
add_subdirectory(aot-reload)
add_subdirectory(runtime-compile)
add_subdirectory(precompiled-library)
add_subdirectory(hot-reload)
add_subdirectory(clang-files-backend)
add_subdirectory(jit-backend)
add_subdirectory(imgui)

# Option: build the sample aot as a separate project, in a new build directory. The test checks
# that the sample works without the build settings of the Hot++ source tree.
if(HOT_SAMPLES_STANDALONE_TEST)
    set(dir ${CMAKE_BINARY_DIR}/samples_standalone/aot)
    add_test(NAME samples_standalone_aot
        COMMAND ${CMAKE_CTEST_COMMAND} --build-and-test ${CMAKE_CURRENT_SOURCE_DIR}/aot ${dir}
            --build-generator ${CMAKE_GENERATOR} --build-config $<CONFIG> --build-noclean
            --build-options -DHOT_ROOT=${PROJECT_SOURCE_DIR} -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                -DHOT_BACKEND=${HOT_BACKEND} -DCMAKE_BUILD_TYPE=$<CONFIG>
            --test-command ${CMAKE_CTEST_COMMAND} --test-dir ${dir} -C $<CONFIG> --output-on-failure)
    set_tests_properties(samples_standalone_aot PROPERTIES TIMEOUT 3600 LABELS standalone)
endif()
