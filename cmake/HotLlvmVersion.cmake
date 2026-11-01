# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The LLVM release for a Windows target (HOT_DOWNLOAD_LLVM, cmake/HotDownloadLlvm.cmake).
# The release has libclang.dll, libclang.lib, the clang-c headers, clang++.exe and the resource folder.
set(HOT_LLVM_VERSION 23.1.4)
set(HOT_LLVM_URL
    "https://github.com/llvm/llvm-project/releases/download/llvmorg-${HOT_LLVM_VERSION}/clang+llvm-${HOT_LLVM_VERSION}-x86_64-pc-windows-msvc.tar.zst"
    CACHE STRING "The URL of the LLVM release for a Windows target")
set(HOT_LLVM_SHA256 1a3f2f998e230b5791cc4e2bfe11a1f7c07fe42c96ff44f227c9db0bedfaf091
    CACHE STRING "The SHA256 of the archive at HOT_LLVM_URL")
