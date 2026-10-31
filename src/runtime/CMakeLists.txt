# Script runtime. It has two parts.
#   hot_runtime_core: the globals and the registry that generated code needs. An AOT
#     build links only this part (design M7). It has no loader, no migration, no dlopen.
#   hot_runtime: the core plus the state tracking, the migration and the generation loader.
add_library(hot_runtime_core STATIC threads.cpp registry.cpp errors.cpp trace.cpp)
target_include_directories(hot_runtime_core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# The format library is a build-time choice (include/hot/config.h). It has no runtime switch.
if(HOT_FORMAT_LIB STREQUAL "fmt")
    get_property(HOT_FMT_INCLUDE_DIR GLOBAL PROPERTY HOT_FMT_INCLUDE_DIR)
    if(HOT_PORTABLE)
        # A portable build holds no build path. The empty value means: the compiler finds fmt.
        set(HOT_FMT_INCLUDE_DIR "")
    endif()
    # The export holds no build path. The installed target has no HOT_FMT_INCLUDE_DIR (config.h
    # uses an empty text), so the compiler finds fmt by itself.
    target_compile_definitions(hot_runtime_core PUBLIC HOT_USE_FMT=1
        $<BUILD_INTERFACE:HOT_FMT_INCLUDE_DIR="${HOT_FMT_INCLUDE_DIR}">)
else()
    target_compile_definitions(hot_runtime_core PUBLIC HOT_USE_FMT=0)
endif()
# The runtime checks (include/hot/config.h). The define is PUBLIC, so the export of the target
# carries it: an installed host sees the setting of the SDK build.
if(NOT HOT_RUNTIME_CHECKS)
    target_compile_definitions(hot_runtime_core PUBLIC HOT_RUNTIME_CHECKS=0)
endif()
# The traceback (include/hot/config.h). The define is PUBLIC for the same reason.
if(HOT_TRACEBACK)
    target_compile_definitions(hot_runtime_core PUBLIC HOT_TRACEBACK=1)
endif()
# The C++ standard of the host (include/hot/config.h). The backends use it for the compile of
# the scripts and for the parse of the native headers. The target hotc_lib gets the same value.
hot_effective_cxx_standard(HOT_HOST_CXX_STANDARD hot_runtime_core)
target_compile_definitions(hot_runtime_core PUBLIC HOT_CXX_STANDARD=${HOT_HOST_CXX_STANDARD})
add_library(hot_runtime STATIC
    state.cpp migration.cpp loader.cpp module.cpp
    scratch.cpp paths.cpp)
# scratch.cpp reads the option as the default of keep_temp().
target_compile_definitions(hot_runtime PRIVATE HOT_KEEP_TEMP_DEFAULT=$<BOOL:${HOT_KEEP_TEMP}>)
target_link_libraries(hot_runtime PUBLIC hot_runtime_core ${CMAKE_DL_LIBS} Threads::Threads)
# paths.cpp reads these (include/hot/paths.h). The compile flags are not paths, so both modes
# have them. A portable build (HOT_PORTABLE) defines no build path. It finds the paths at run time.
target_compile_definitions(hot_runtime PRIVATE HOT_DEFAULT_CXXFLAGS="${HOT_CXXFLAGS_JOINED}")
if(HOT_PORTABLE)
    target_compile_definitions(hot_runtime PRIVATE HOT_PORTABLE=1)
else()
    target_compile_definitions(hot_runtime PRIVATE
        HOT_DEFAULT_CXX="${CMAKE_CXX_COMPILER}"
        HOT_DEFAULT_INCLUDES="${HOT_DEFAULT_INCLUDES_JOINED}"
        HOT_DEFAULT_RESOURCE_DIR="${CLANG_RESOURCE_DIR}"
        HOT_DEFAULT_WORK_DIR="${HOT_SCRATCH_DIR}/hot_work")
endif()

# AOT runtime (design M7): the core, the AOT registry and no-op object tracking.
# Do not link it together with hot_runtime.
add_library(hot_aot STATIC aot.cpp)
target_link_libraries(hot_aot PUBLIC hot_runtime_core Threads::Threads)

# ------------------------------------------------------------ Backends
# ClangFilesBackend: hotc_lib in memory, then an external compiler.
# The library is always built, because hotc and hot-ls use it. HOT_ENABLE_CLANG_FILES decides
# if hot_backend links it.
add_library(hot_clang_backend STATIC clang_files_backend.cpp)
target_link_libraries(hot_clang_backend PUBLIC hot_runtime hotc_lib)

# JitBackend: hotc_lib in memory, the Clang library compiles in memory, LLVM ORC loads.
# LLVM and Clang come from the system. CPM does not fetch them. The root file finds them.
if(HOT_JIT_ENABLED)
    add_library(hot_jit_backend STATIC jit_backend.cpp)
    # Only jit_backend.cpp includes the LLVM and Clang headers. The public header does not.
    # So these dirs are PRIVATE and the export holds no system path.
    target_include_directories(hot_jit_backend PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_include_directories(hot_jit_backend SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
    target_compile_definitions(hot_jit_backend PRIVATE ${LLVM_DEFINITIONS})
    target_link_libraries(hot_jit_backend PUBLIC hot_runtime hotc_lib clang-cpp LLVM)
endif()

# make_default_backend() returns the backend that the program chooses at run time. The target
# exists only when at least one backend is on. It defines HOT_WITH_CLANG_FILES and HOT_WITH_JIT
# for the backends that it has. The SDK headers and the tests use these defines.
if(HOT_CLANG_FILES_ENABLED OR HOT_JIT_ENABLED)
    add_library(hot_backend STATIC default_backend.cpp)
    target_link_libraries(hot_backend PUBLIC hot_runtime)
    if(HOT_PORTABLE)
        # default_backend.cpp checks the work directory of a portable host.
        target_compile_definitions(hot_backend PRIVATE HOT_PORTABLE=1)
    endif()
    if(HOT_CLANG_FILES_ENABLED)
        target_link_libraries(hot_backend PUBLIC hot_clang_backend)
        target_compile_definitions(hot_backend PUBLIC HOT_WITH_CLANG_FILES=1)
    endif()
    if(HOT_JIT_ENABLED)
        target_link_libraries(hot_backend PUBLIC hot_jit_backend)
        target_compile_definitions(hot_backend PUBLIC HOT_WITH_JIT=1)
    endif()
endif()

# Alias targets for a consumer project. They have the namespace hot::.
add_library(hot::runtime ALIAS hot_runtime)
add_library(hot::runtime_core ALIAS hot_runtime_core)
add_library(hot::aot ALIAS hot_aot)
if(TARGET hot_backend)
    add_library(hot::backend ALIAS hot_backend)
endif()
if(HOT_CLANG_FILES_ENABLED)
    add_library(hot::clang_backend ALIAS hot_clang_backend)
endif()
if(TARGET hot_jit_backend)
    add_library(hot::jit_backend ALIAS hot_jit_backend)
endif()
