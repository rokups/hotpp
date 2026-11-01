# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The translator library hotc_lib. It needs libclang (C API) from the system.
# The tool hotc (src/tools) uses it. The root file sets CLANG_RESOURCE_DIR.
# First choice: the target libclang of the Clang CMake package (a JIT build has it already).
# The package must have the same major version as the clang that sets CLANG_RESOURCE_DIR. Else
# the library and the resource folder do not match, and the fallback below is used.
# The resource folder is <prefix>/lib/clang/<major>, and the package is in <prefix>/lib/cmake/clang.
set(_hot_res_major "")
if(CLANG_RESOURCE_DIR)
    get_filename_component(_hot_res_name "${CLANG_RESOURCE_DIR}" NAME)
    if(_hot_res_name MATCHES "^[0-9]+")
        string(REGEX MATCH "^[0-9]+" _hot_res_major "${_hot_res_name}")
    endif()
endif()
# HOT_LIBCLANG_NO_PACKAGE (cmake/HotDownloadLlvm.cmake): the LLVM release has no usable package.
if(NOT TARGET libclang AND NOT HOT_LIBCLANG_NO_PACKAGE)
    # LLVMConfig.cmake probes C headers, so the C language must be on for the search.
    get_property(_hot_languages GLOBAL PROPERTY ENABLED_LANGUAGES)
    if(NOT "C" IN_LIST _hot_languages)
        enable_language(C)
    endif()
    set(_hot_clang_hints "")
    if(CLANG_RESOURCE_DIR)
        set(_hot_clang_hints HINTS "${CLANG_RESOURCE_DIR}/../../cmake/clang")
    endif()
    find_package(Clang CONFIG QUIET ${_hot_clang_hints})
endif()
set(_hot_use_package OFF)
if(TARGET libclang)
    set(_hot_use_package ON)
    if(_hot_res_major AND Clang_VERSION)
        string(REGEX MATCH "^[0-9]+" _hot_pkg_major "${Clang_VERSION}")
        if(NOT _hot_pkg_major STREQUAL _hot_res_major)
            message(STATUS "libclang: the Clang package is version ${Clang_VERSION}, but the compiler has "
                "resource folder ${_hot_res_major}. The package is not used.")
            set(_hot_use_package OFF)
        endif()
    endif()
endif()
if(_hot_use_package)
    set(HOT_LIBCLANG_TARGET libclang)
    set(HOT_LIBCLANG_FROM_PACKAGE ON CACHE INTERNAL "The build links the libclang target")
    get_target_property(_hot_libclang_file libclang LOCATION)
    find_path(CLANG_C_INCLUDE_DIR clang-c/Index.h HINTS ${CLANG_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} REQUIRED)
    message(STATUS "libclang: target libclang of the Clang package (${_hot_libclang_file})")
else()
    # Fallback: no usable Clang package. Search the library and the header. A cache value (a
    # preset, the command line or cmake/HotDownloadLlvm.cmake) wins over the search.
    find_library(CLANG_C_LIBRARY NAMES clang libclang REQUIRED)
    find_path(CLANG_C_INCLUDE_DIR clang-c/Index.h REQUIRED)
    set(HOT_LIBCLANG_TARGET ${CLANG_C_LIBRARY})
    set(HOT_LIBCLANG_FROM_PACKAGE OFF CACHE INTERNAL "The build links the libclang target")
    set(_hot_libclang_file "${CLANG_C_LIBRARY}")
    message(STATUS "libclang: ${CLANG_C_LIBRARY}")
endif()
# The major version of libclang in this build. HotConfig.cmake compares it with the package of the consumer.
if(_hot_use_package AND Clang_VERSION)
    string(REGEX MATCH "^[0-9]+" _hot_build_major "${Clang_VERSION}")
else()
    set(_hot_build_major "${_hot_res_major}")
endif()
set(HOT_LIBCLANG_VERSION_MAJOR "${_hot_build_major}" CACHE INTERNAL "The major version of libclang in this build")
# The root file reads these for the SDK package config (HotConfig.cmake).
set(HOT_LIBCLANG_FILE "${_hot_libclang_file}" CACHE INTERNAL "The libclang file of this build")
add_library(hotc_lib STATIC hotc_lib.cpp vfs.cpp file_time.cpp emit.cpp lower.cpp types.cpp check.cpp diag_rewrite.cpp
    parser/script_grammar.cpp parser/cpp_frontend.cpp
    parser/expression_grammar.cpp parser/statement_grammar.cpp)
target_include_directories(hotc_lib PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(hotc_lib PRIVATE ${CLANG_C_INCLUDE_DIR})
# The export holds no path of libclang. HotConfig.cmake finds the library for the consumer.
target_link_libraries(hotc_lib PUBLIC $<BUILD_INTERFACE:${HOT_LIBCLANG_TARGET}>)
target_link_libraries(hotc_lib PRIVATE $<BUILD_INTERFACE:foonathan::lexy::core>)
# hot_runtime has hot::mingw_target_flags (command_line.cpp) for the parse of a MinGW target.
target_link_libraries(hotc_lib PUBLIC hot_runtime)
# The translator builds with the standard of the project (default C++17). The parse of the native
# headers uses the standard of the host (HOT_CXX_STANDARD, include/hot/config.h).
hot_effective_cxx_standard(HOT_HOST_CXX_STANDARD hot_runtime_core)
target_compile_definitions(hotc_lib PRIVATE HOT_CXX_STANDARD=${HOT_HOST_CXX_STANDARD})
if(HOT_PORTABLE)
    # A portable build holds no build path. The empty value means: the caller or libclang decides.
    target_compile_definitions(hotc_lib PRIVATE HOT_RESOURCE_DIR="")
else()
    target_compile_definitions(hotc_lib PRIVATE HOT_RESOURCE_DIR="${CLANG_RESOURCE_DIR}")
    # The target and the sysroot of the parse: the same as for the script modules. An empty value
    # adds no flag (the default of libclang).
    target_compile_definitions(hotc_lib PRIVATE HOT_PARSE_TARGET="${HOT_HOTC_TARGET}"
        HOT_PARSE_SYSROOT="${HOT_SCRIPT_SYSROOT}")
endif()
add_library(hot::hotc_lib ALIAS hotc_lib)

# The DLL of libclang (Windows, HOT_LIBCLANG_DLL from cmake/HotDownloadLlvm.cmake). The loader finds it
# beside the executable, so the build copies it to the folder of hotc.exe, hot-ls.exe and the tests.
# All of them link hotc_lib, so the copy occurs before their link.
if(HOT_LIBCLANG_DLL)
    add_custom_target(hot_libclang_dll
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:hotc>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${HOT_LIBCLANG_DLL}" "$<TARGET_FILE_DIR:hotc>"
        COMMENT "Copy libclang.dll to the folder of the executables"
        VERBATIM)
    add_dependencies(hotc_lib hot_libclang_dll)
    if(HOT_INSTALL)
        install(FILES "${HOT_LIBCLANG_DLL}" DESTINATION ${CMAKE_INSTALL_BINDIR})
        # The compiler headers of the release (stddef.h and others). The parse needs them. The
        # SDK has them in lib/clang/<major>/include. libclang.dll in bin/ finds this folder itself,
        # and HotConfig.cmake gives it to hotc. Only the headers go in: the libraries are not needed.
        if(HOT_CLANG_RESOURCE_DIR)
            get_filename_component(_hot_res_name "${HOT_CLANG_RESOURCE_DIR}" NAME)
            install(DIRECTORY "${HOT_CLANG_RESOURCE_DIR}/include"
                DESTINATION ${CMAKE_INSTALL_LIBDIR}/clang/${_hot_res_name})
        endif()
    endif()
endif()
