# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# libclang and clang++ from an LLVM release for a Windows target. The root file includes this
# file when HOT_LLVM_DIR or HOT_DOWNLOAD_LLVM is set, after CPM and before the resource folder.
# HOT_LLVM_DIR: the folder of an unpacked release. Then no download occurs.
# HOT_DOWNLOAD_LLVM: CPM downloads HOT_LLVM_URL (cmake/HotLlvmVersion.cmake).
# The file sets these cache values. A value from a preset or from the command line wins:
#   CLANG_C_INCLUDE_DIR, CLANG_C_LIBRARY, HOT_CLANG_RESOURCE_DIR, HOT_LIBCLANG_DLL.
# It also sets HOT_SCRIPT_COMPILER to the clang++.exe of the release when it has the default value.
# A MinGW build sets HOT_LIBCLANG_NO_PACKAGE (no Clang CMake package) and turns the JIT off.
# A cross build runs hotc.exe and the tests with CMAKE_CROSSCOMPILING_EMULATOR (wine).

if(NOT WIN32)
    message(FATAL_ERROR "HOT_LLVM_DIR and HOT_DOWNLOAD_LLVM need a Windows target. The release "
        "has Windows binaries only. Remove the two values for this build.")
endif()

if(HOT_LLVM_DIR)
    set(_hot_llvm_dir "${HOT_LLVM_DIR}")
else()
    CPMAddPackage(
        NAME hot_llvm_release
        URL "${HOT_LLVM_URL}"
        URL_HASH SHA256=${HOT_LLVM_SHA256}
        DOWNLOAD_ONLY YES
    )
    set(_hot_llvm_dir "${hot_llvm_release_SOURCE_DIR}")
endif()

# The resource folder is lib/clang/<major>.
file(GLOB _hot_llvm_res LIST_DIRECTORIES true "${_hot_llvm_dir}/lib/clang/[0-9]*")
list(LENGTH _hot_llvm_res _hot_llvm_res_count)
set(_hot_llvm_missing "")
foreach(_hot_llvm_file include/clang-c/Index.h lib/libclang.lib bin/libclang.dll bin/clang++.exe)
    if(NOT EXISTS "${_hot_llvm_dir}/${_hot_llvm_file}")
        list(APPEND _hot_llvm_missing "${_hot_llvm_file}")
    endif()
endforeach()
if(NOT _hot_llvm_res_count EQUAL 1)
    list(APPEND _hot_llvm_missing "lib/clang/<major> (exactly one folder)")
endif()
if(_hot_llvm_missing)
    list(JOIN _hot_llvm_missing ", " _hot_llvm_missing_text)
    message(FATAL_ERROR "The LLVM release at ${_hot_llvm_dir} is not complete. These items are "
        "missing: ${_hot_llvm_missing_text}. Set HOT_LLVM_DIR to an unpacked "
        "clang+llvm-<version>-x86_64-pc-windows-msvc release, or set HOT_DOWNLOAD_LLVM=ON.")
endif()

set(CLANG_C_INCLUDE_DIR "${_hot_llvm_dir}/include" CACHE PATH "The folder that has clang-c/Index.h")
set(CLANG_C_LIBRARY "${_hot_llvm_dir}/lib/libclang.lib" CACHE FILEPATH "The libclang import library")
set(HOT_CLANG_RESOURCE_DIR "${_hot_llvm_res}" CACHE PATH "The Clang resource directory")
set(HOT_LIBCLANG_DLL "${_hot_llvm_dir}/bin/libclang.dll" CACHE FILEPATH
    "The libclang DLL. The build copies it beside hotc.exe, hot-ls.exe and the tests")
if(HOT_SCRIPT_COMPILER STREQUAL CMAKE_CXX_COMPILER)
    set(HOT_SCRIPT_COMPILER "${_hot_llvm_dir}/bin/clang++.exe" CACHE STRING
        "The compiler of the script modules at run time" FORCE)
endif()
if(MINGW)
    # The release has no CMake package of Clang and no LLVM libraries for the JIT.
    set(HOT_LIBCLANG_NO_PACKAGE ON)
    set(HOT_ENABLE_JIT OFF)
endif()
message(STATUS "LLVM release: ${_hot_llvm_dir}")
