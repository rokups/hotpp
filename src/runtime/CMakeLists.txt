# Script runtime. It has two parts.
#   hot_runtime_core: the globals and the registry that generated code needs. An AOT
#     build links only this part (design M7). It has no loader, no migration, no dlopen.
#   hot_runtime: the core plus the state tracking, the migration and the generation loader.
add_library(hot_runtime_core STATIC threads.cpp registry.cpp)
target_include_directories(hot_runtime_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
# The format library is a build-time choice (include/hot/config.h). It has no runtime switch.
if(HOT_FORMAT_LIB STREQUAL "fmt")
    get_property(HOT_FMT_INCLUDE_DIR GLOBAL PROPERTY HOT_FMT_INCLUDE_DIR)
    target_compile_definitions(hot_runtime_core PUBLIC HOT_USE_FMT=1 HOT_FMT_INCLUDE_DIR="${HOT_FMT_INCLUDE_DIR}")
else()
    target_compile_definitions(hot_runtime_core PUBLIC HOT_USE_FMT=0)
endif()
# The C++ standard of the host (include/hot/config.h). The backends use it for the compile of
# the scripts and for the parse of the native headers. The target hotc_lib gets the same value.
hot_effective_cxx_standard(HOT_HOST_CXX_STANDARD hot_runtime_core)
target_compile_definitions(hot_runtime_core PUBLIC HOT_CXX_STANDARD=${HOT_HOST_CXX_STANDARD})
add_library(hot_runtime STATIC
    state.cpp migration.cpp loader.cpp module.cpp
    scratch.cpp)
# scratch.cpp reads the option as the default of keep_temp().
target_compile_definitions(hot_runtime PRIVATE HOT_KEEP_TEMP_DEFAULT=$<BOOL:${HOT_KEEP_TEMP}>)
target_link_libraries(hot_runtime PUBLIC hot_runtime_core ${CMAKE_DL_LIBS} Threads::Threads)

# AOT runtime (design M7): the core, the AOT registry and no-op object tracking.
# Do not link it together with hot_runtime.
add_library(hot_aot STATIC aot.cpp)
target_link_libraries(hot_aot PUBLIC hot_runtime_core Threads::Threads)

# ------------------------------------------------------------ Backends
# ClangFilesBackend: hotc_lib in memory, then an external compiler.
add_library(hot_clang_backend STATIC clang_files_backend.cpp)
target_link_libraries(hot_clang_backend PUBLIC hot_runtime hotc_lib)

# JitBackend: hotc_lib in memory, the Clang library compiles in memory, LLVM ORC loads.
# LLVM and Clang come from the system. CPM does not fetch them. The root file finds them.
if(HOT_BACKEND STREQUAL "JIT")
    add_library(hot_jit_backend STATIC jit_backend.cpp)
    target_include_directories(hot_jit_backend PUBLIC ${PROJECT_SOURCE_DIR}/include)
    target_include_directories(hot_jit_backend SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
    target_compile_definitions(hot_jit_backend PRIVATE ${LLVM_DEFINITIONS})
    target_link_libraries(hot_jit_backend PUBLIC hot_runtime hotc_lib clang-cpp LLVM)
endif()

# make_default_backend() returns the backend that HOT_BACKEND selected.
add_library(hot_backend STATIC default_backend.cpp)
target_link_libraries(hot_backend PUBLIC hot_runtime)
if(HOT_BACKEND STREQUAL "CLANG_FILES")
    target_link_libraries(hot_backend PUBLIC hot_clang_backend)
    target_compile_definitions(hot_backend PRIVATE
        HOT_BACKEND_CLANG_FILES=1
        HOT_DEFAULT_CXX="${CMAKE_CXX_COMPILER}"
        HOT_DEFAULT_CXXFLAGS="${HOT_CXXFLAGS_JOINED}"
        HOT_DEFAULT_INCLUDES="${HOT_DEFAULT_INCLUDES_JOINED}"
        HOT_DEFAULT_RESOURCE_DIR="${CLANG_RESOURCE_DIR}"
        HOT_DEFAULT_WORK_DIR="${HOT_SCRATCH_DIR}/hot_work")
elseif(HOT_BACKEND STREQUAL "JIT")
    target_link_libraries(hot_backend PUBLIC hot_jit_backend)
    target_compile_definitions(hot_backend PRIVATE
        HOT_BACKEND_JIT=1
        HOT_DEFAULT_CXX="${CMAKE_CXX_COMPILER}"
        HOT_DEFAULT_CXXFLAGS="${HOT_CXXFLAGS_JOINED}"
        HOT_DEFAULT_INCLUDES="${HOT_DEFAULT_INCLUDES_JOINED}"
        HOT_DEFAULT_RESOURCE_DIR="${CLANG_RESOURCE_DIR}")
endif()
