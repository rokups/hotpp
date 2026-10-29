# The example programs. The engine comes first: other examples use its sources.
add_subdirectory(engine)
add_subdirectory(imgui)
add_subdirectory(scripts)
add_subdirectory(ship)

# bench_backends times the compile and the load of the example scripts.
add_executable(bench_backends bench_backends.cpp engine/Actor.cpp engine/Util.cpp)
set_target_properties(bench_backends PROPERTIES ENABLE_EXPORTS ON)
target_link_libraries(bench_backends PRIVATE spp_backend)
if(SPP_BACKEND STREQUAL "JIT")
    target_link_libraries(bench_backends PRIVATE spp_jit_backend)
    target_compile_definitions(bench_backends PRIVATE BENCH_JIT=1)
else()
    target_link_libraries(bench_backends PRIVATE spp_clang_backend)
    target_compile_definitions(bench_backends PRIVATE BENCH_CLANG_FILES=1)
endif()
target_compile_definitions(bench_backends PRIVATE
    BENCH_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    BENCH_CXX="${CMAKE_CXX_COMPILER}"
    BENCH_INCLUDES="${CMAKE_SOURCE_DIR}/include|${CMAKE_SOURCE_DIR}/example/engine"
    BENCH_RESOURCE_DIR="${CLANG_RESOURCE_DIR}"
    BENCH_WORK_DIR="${SPP_SCRATCH_DIR}/bench")
