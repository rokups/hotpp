# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The LLVM release for a Windows target (HOT_DOWNLOAD_LLVM, cmake/HotDownloadLlvm.cmake).
# The release has libclang.dll, libclang.lib, the clang-c headers, clang++.exe and the resource folder.

# Sets the cache value "var" to "value", with the type "type" and the text "doc". A value of the
# user wins: the function changes only a value that it set itself. The INTERNAL cache value
# _HOT_LLVM_AUTO_<var> keeps the last value of the function. Thus a new version or a new
# HOT_LLVM_DIR gives new values, also in an old build folder.
function(hot_llvm_cache var value type doc)
    if(NOT DEFINED CACHE{${var}} OR "$CACHE{${var}}" STREQUAL "$CACHE{_HOT_LLVM_AUTO_${var}}")
        set(${var} "${value}" CACHE ${type} "${doc}" FORCE)
    endif()
    set(_HOT_LLVM_AUTO_${var} "${value}" CACHE INTERNAL "The last value that Hot++ set for ${var}")
endfunction()

set(HOT_LLVM_VERSION 23.1.4)
# The name of the archive and of its unpacked folder.
set(HOT_LLVM_RELEASE_NAME "clang+llvm-${HOT_LLVM_VERSION}-x86_64-pc-windows-msvc")
hot_llvm_cache(HOT_LLVM_URL
    "https://github.com/llvm/llvm-project/releases/download/llvmorg-${HOT_LLVM_VERSION}/${HOT_LLVM_RELEASE_NAME}.tar.zst"
    STRING "The URL of the LLVM release for a Windows target")
hot_llvm_cache(HOT_LLVM_SHA256 1a3f2f998e230b5791cc4e2bfe11a1f7c07fe42c96ff44f227c9db0bedfaf091
    STRING "The SHA256 of the archive at HOT_LLVM_URL")
