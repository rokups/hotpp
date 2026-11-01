# bench_backends measures the run-time cost of every backend of the build, with no rebuild.
# One program tests the clang files backend, the JIT backend (both when the build has them) and
# the AOT scripts. A native C++ twin of each script is the reference.
#
# The program links hot_backend, which links the backends of the build. The scripts in scripts/
# are also built as AOT code in the program (hot_add_host MODE AOT_RELOAD): that mode links the AOT
# library and the backend in one executable, and the program loads the AOT classes with
# Loader::load_aot. The AOT library and the native twin get -O2, so they match the -O2 that the
# program passes to the compile of the scripts at run time.
#
# Each case is a .hot script and, when C++ can write the case, a .cpp script (cpp_<case>.cpp). The
# .cpp scripts are plain C++ files that hotc translates (the libclang frontend). Both kinds are built
# in the same way. A .cpp script has module functions and static methods, which a .hot script does
# not have (module_fn.cpp). A .cpp script has no script class handles, so the .cpp twins of the cases
# with handles use raw pointers to script objects. The reload case and the reload chain also run
# as .cpp scripts (their text is in bench_backends.cpp).
#
# The load section uses the test engine (tests/engine) and the test scripts.
file(GLOB BENCH_SCRIPTS ${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.hot ${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.cpp)
list(SORT BENCH_SCRIPTS)
add_executable(bench_backends bench_backends.cpp BenchEngine.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(bench_backends PRIVATE ${PROJECT_SOURCE_DIR}/tests/engine ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bench_backends PRIVATE hot_backend)
hot_add_host(TARGET bench_backends MODE AOT_RELOAD SCRIPTS ${BENCH_SCRIPTS}
             INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS BenchEngine.h)
if(NOT MSVC)
    target_compile_options(bench_backends PRIVATE -O2)
    target_compile_options(bench_backends_scripts PRIVATE -O2)
endif()
target_compile_definitions(bench_backends PRIVATE
    BENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    BENCH_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scripts"
    BENCH_CXX="${CMAKE_CXX_COMPILER}"
    BENCH_INCLUDES="${PROJECT_SOURCE_DIR}/include|${PROJECT_SOURCE_DIR}/tests/engine|${CMAKE_CURRENT_SOURCE_DIR}"
    BENCH_RESOURCE_DIR="${CLANG_RESOURCE_DIR}"
    BENCH_WORK_DIR="${HOT_SCRATCH_DIR}/bench")

# bench_backends_opt: the same sources, with the scripts as optimized AOT code (hotc --aot-optimized).
# One program cannot hold two builds of the same scripts (one hot::aot_table), so this is a second
# program. It tests only the optimized code ("aot-opt"). The loader refuses to reload that code.
add_executable(bench_backends_opt bench_backends.cpp BenchEngine.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(bench_backends_opt PRIVATE ${PROJECT_SOURCE_DIR}/tests/engine ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bench_backends_opt PRIVATE hot_backend)
hot_add_scripts(TARGET bench_backends_opt_scripts AOT OPTIMIZED SCRIPTS ${BENCH_SCRIPTS}
                INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS BenchEngine.h)
target_link_libraries(bench_backends_opt PRIVATE bench_backends_opt_scripts)
set_target_properties(bench_backends_opt PROPERTIES ENABLE_EXPORTS ON)
target_include_directories(bench_backends_opt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT MSVC)
    target_compile_options(bench_backends_opt PRIVATE -O2)
    target_compile_options(bench_backends_opt_scripts PRIVATE -O2)
endif()
target_compile_definitions(bench_backends_opt PRIVATE BENCH_AOT_OPT=1
    BENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    BENCH_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scripts"
    BENCH_CXX="${CMAKE_CXX_COMPILER}"
    BENCH_INCLUDES="${PROJECT_SOURCE_DIR}/include|${PROJECT_SOURCE_DIR}/tests/engine|${CMAKE_CURRENT_SOURCE_DIR}"
    BENCH_RESOURCE_DIR="${CLANG_RESOURCE_DIR}"
    BENCH_WORK_DIR="${HOT_SCRATCH_DIR}/bench_opt")
