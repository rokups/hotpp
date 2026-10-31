# bench_backends times the compile and the load of the test scripts.
# The benchmark uses the test engine (tests/engine) and the test scripts.
add_executable(bench_backends bench_backends.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
set_target_properties(bench_backends PROPERTIES ENABLE_EXPORTS ON)
target_include_directories(bench_backends PRIVATE ${PROJECT_SOURCE_DIR}/tests/engine)
target_link_libraries(bench_backends PRIVATE hot_backend)
# The bench times one backend: the JIT backend only when the build has no clang files backend.
if(HOT_JIT_ENABLED AND NOT HOT_CLANG_FILES_ENABLED)
    target_link_libraries(bench_backends PRIVATE hot_jit_backend)
    target_compile_definitions(bench_backends PRIVATE BENCH_JIT=1)
else()
    target_link_libraries(bench_backends PRIVATE hot_clang_backend)
    target_compile_definitions(bench_backends PRIVATE BENCH_CLANG_FILES=1)
endif()
target_compile_definitions(bench_backends PRIVATE
    BENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    BENCH_CXX="${CMAKE_CXX_COMPILER}"
    BENCH_INCLUDES="${PROJECT_SOURCE_DIR}/include|${PROJECT_SOURCE_DIR}/tests/engine"
    BENCH_RESOURCE_DIR="${CLANG_RESOURCE_DIR}"
    BENCH_WORK_DIR="${HOT_SCRATCH_DIR}/bench")
