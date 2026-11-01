# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# libclang and clang++ from an LLVM release for a Windows target. The root file includes this
# file when HOT_LLVM_DIR or HOT_DOWNLOAD_LLVM is set, after CPM and before the resource folder.
# HOT_LLVM_DIR: the folder of an unpacked release. Then no download occurs. The root file sets
# it from HOT_LLVM_RELEASES_DIR and the version when it is empty.
# HOT_DOWNLOAD_LLVM: CPM downloads HOT_LLVM_URL (cmake/HotLlvmVersion.cmake).
# The file sets these cache values with hot_llvm_cache. A value from a preset or from the command
# line wins. A new HOT_LLVM_DIR changes the values that the file set before:
#   CLANG_C_INCLUDE_DIR, CLANG_C_LIBRARY, HOT_CLANG_RESOURCE_DIR, HOT_LIBCLANG_DLL.
# It also sets HOT_SCRIPT_COMPILER to the clang++.exe of the release when it has the default value
# (CMAKE_CXX_COMPILER) or a value of the file.
# A MinGW build sets HOT_LIBCLANG_NO_PACKAGE (no Clang CMake package) and turns the JIT off.
# A cross build runs hotc.exe and the tests with CMAKE_CROSSCOMPILING_EMULATOR (wine).

if(NOT WIN32)
    message(FATAL_ERROR "HOT_LLVM_DIR and HOT_DOWNLOAD_LLVM need a Windows target. The release "
        "has Windows binaries only. Remove the two values for this build.")
endif()

if(HOT_LLVM_DIR)
    set(_hot_llvm_dir "${HOT_LLVM_DIR}")
else()
    # The URL and the SHA256 go together. A URL of the user needs a SHA256 of the user.
    if(NOT HOT_LLVM_URL STREQUAL _HOT_LLVM_AUTO_HOT_LLVM_URL
            AND HOT_LLVM_SHA256 STREQUAL _HOT_LLVM_AUTO_HOT_LLVM_SHA256)
        message(FATAL_ERROR "HOT_LLVM_URL is not the URL of the LLVM release ${HOT_LLVM_VERSION}. "
            "Set HOT_LLVM_SHA256 to the SHA256 of the archive at ${HOT_LLVM_URL}.")
    endif()
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

hot_llvm_cache(CLANG_C_INCLUDE_DIR "${_hot_llvm_dir}/include" PATH "The folder that has clang-c/Index.h")
hot_llvm_cache(CLANG_C_LIBRARY "${_hot_llvm_dir}/lib/libclang.lib" FILEPATH "The libclang import library")
hot_llvm_cache(HOT_CLANG_RESOURCE_DIR "${_hot_llvm_res}" PATH "The Clang resource directory")
hot_llvm_cache(HOT_LIBCLANG_DLL "${_hot_llvm_dir}/bin/libclang.dll" FILEPATH
    "The libclang DLL. The build copies it beside hotc.exe, hot-ls.exe and the tests")
# The default value of the root file is not a value of the user.
if(HOT_SCRIPT_COMPILER STREQUAL CMAKE_CXX_COMPILER)
    set(_HOT_LLVM_AUTO_HOT_SCRIPT_COMPILER "${HOT_SCRIPT_COMPILER}" CACHE INTERNAL
        "The last value that Hot++ set for HOT_SCRIPT_COMPILER")
endif()
hot_llvm_cache(HOT_SCRIPT_COMPILER "${_hot_llvm_dir}/bin/clang++.exe" STRING
    "The compiler of the script modules at run time")
# The build links libclang.lib directly. The CMake package of the release is not used.
set(HOT_LIBCLANG_NO_PACKAGE ON)
if(MINGW)
    # The release has no LLVM libraries for a MinGW JIT.
    string(TOUPPER "$CACHE{HOT_ENABLE_JIT}" _hot_llvm_jit)
    if(_hot_llvm_jit STREQUAL "ON")
        message(WARNING "HOT_ENABLE_JIT=ON is ignored. The LLVM release has no LLVM libraries for "
            "the JIT, so a MinGW build has the clang files backend only.")
    endif()
    set(HOT_ENABLE_JIT OFF)
endif()
message(STATUS "LLVM release: ${_hot_llvm_dir}")
