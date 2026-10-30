# Script runtime. It has two parts.
#   spp_runtime_core: the globals and the registry that generated code needs. An AOT
#     build links only this part (design M7). It has no loader, no migration, no dlopen.
#   spp_runtime: the core plus the state tracking, the migration and the generation loader.
add_library(spp_runtime_core STATIC threads.cpp registry.cpp)
target_include_directories(spp_runtime_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
# The format library is a build-time choice (include/hot/config.h). It has no runtime switch.
if(SPP_FORMAT_LIB STREQUAL "fmt")
    get_property(SPP_FMT_INCLUDE_DIR GLOBAL PROPERTY SPP_FMT_INCLUDE_DIR)
    target_compile_definitions(spp_runtime_core PUBLIC HOT_USE_FMT=1 HOT_FMT_INCLUDE_DIR="${SPP_FMT_INCLUDE_DIR}")
else()
    target_compile_definitions(spp_runtime_core PUBLIC HOT_USE_FMT=0)
endif()
# The C++ standard of the host (include/hot/config.h). The backends use it for the compile of
# the scripts and for the parse of the native headers. The target sppc_lib gets the same value.
spp_effective_cxx_standard(SPP_HOST_CXX_STANDARD spp_runtime_core)
target_compile_definitions(spp_runtime_core PUBLIC HOT_CXX_STANDARD=${SPP_HOST_CXX_STANDARD})
add_library(spp_runtime STATIC
    state.cpp migration.cpp loader.cpp module.cpp
    scratch.cpp)
# scratch.cpp reads the option as the default of keep_temp().
target_compile_definitions(spp_runtime PRIVATE SPP_KEEP_TEMP_DEFAULT=$<BOOL:${SPP_KEEP_TEMP}>)
target_link_libraries(spp_runtime PUBLIC spp_runtime_core ${CMAKE_DL_LIBS} Threads::Threads)

# AOT runtime (design M7): the core, the AOT registry and no-op object tracking.
# Do not link it together with spp_runtime.
add_library(spp_aot STATIC aot.cpp)
target_link_libraries(spp_aot PUBLIC spp_runtime_core Threads::Threads)

# ------------------------------------------------------------ Backends
# ClangFilesBackend: sppc_lib in memory, then an external compiler.
add_library(spp_clang_backend STATIC clang_files_backend.cpp)
target_link_libraries(spp_clang_backend PUBLIC spp_runtime sppc_lib)

# JitBackend: sppc_lib in memory, the Clang library compiles in memory, LLVM ORC loads.
# LLVM and Clang come from the system. CPM does not fetch them. The root file finds them.
if(SPP_BACKEND STREQUAL "JIT")
    add_library(spp_jit_backend STATIC jit_backend.cpp)
    target_include_directories(spp_jit_backend PUBLIC ${PROJECT_SOURCE_DIR}/include)
    target_include_directories(spp_jit_backend SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
    target_compile_definitions(spp_jit_backend PRIVATE ${LLVM_DEFINITIONS})
    target_link_libraries(spp_jit_backend PUBLIC spp_runtime sppc_lib clang-cpp LLVM)
endif()

# make_default_backend() returns the backend that SPP_BACKEND selected.
add_library(spp_backend STATIC default_backend.cpp)
target_link_libraries(spp_backend PUBLIC spp_runtime)
if(SPP_BACKEND STREQUAL "CLANG_FILES")
    target_link_libraries(spp_backend PUBLIC spp_clang_backend)
    target_compile_definitions(spp_backend PRIVATE
        SPP_BACKEND_CLANG_FILES=1
        SPP_DEFAULT_CXX="${CMAKE_CXX_COMPILER}"
        SPP_DEFAULT_CXXFLAGS="${SPP_CXXFLAGS_JOINED}"
        SPP_DEFAULT_INCLUDES="${SPP_DEFAULT_INCLUDES_JOINED}"
        SPP_DEFAULT_RESOURCE_DIR="${CLANG_RESOURCE_DIR}"
        SPP_DEFAULT_WORK_DIR="${SPP_SCRATCH_DIR}/spp_work")
elseif(SPP_BACKEND STREQUAL "JIT")
    target_link_libraries(spp_backend PUBLIC spp_jit_backend)
    target_compile_definitions(spp_backend PRIVATE
        SPP_BACKEND_JIT=1
        SPP_DEFAULT_CXX="${CMAKE_CXX_COMPILER}"
        SPP_DEFAULT_CXXFLAGS="${SPP_CXXFLAGS_JOINED}"
        SPP_DEFAULT_INCLUDES="${SPP_DEFAULT_INCLUDES_JOINED}"
        SPP_DEFAULT_RESOURCE_DIR="${CLANG_RESOURCE_DIR}")
endif()
