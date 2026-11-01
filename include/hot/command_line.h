// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Helpers for the command line of the external compiler (the clang files backend). They are pure
// functions on all platforms, so the tests run them on Linux too.
#pragma once

#include <string>
#include <vector>

namespace hot
{

// One argument in the quoting of the Microsoft C runtime (CommandLineToArgvW). An argument with
// a space, a tab, a new line or a quote gets quotes. The backslashes before a quote and before
// the closing quote become two. An empty argument becomes "".
std::string windows_quote_arg(const std::string& arg);

// The command line for CreateProcess: the quoted arguments with one space between them.
std::string windows_command_line(const std::vector<std::string>& argv);

// True when the target triple is a MinGW target ("mingw" or "windows-gnu" in the triple).
bool is_mingw_triple(const std::string& target_triple);

// The extra compile and link flags for a MinGW target. Empty when the triple is not a MinGW
// target. The flags are "-nostdinc++", an "-isystem" for each libstdc++ header directory of the
// sysroot (<sysroot>/include/c++/<version>, its <arch>-w64-mingw32 and backward subdirectories)
// and "-fuse-ld=lld -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic"
// (a static MinGW runtime, as in the host).
// The <arch> is the first part of the triple, for example x86_64. The
// version is the highest directory under <sysroot>/include/c++. With no sysroot or no version
// directory, the flags have no "-isystem". A directory <sysroot>/../lib/gcc/<arch>-w64-mingw32/
// <version> (the static libgcc) adds "-L" with the highest version.
std::vector<std::string> mingw_target_flags(const std::string& target_triple, const std::string& sysroot);

// The error text for a MinGW target with a sysroot that has no C++ standard library headers.
// The headers are in "<sysroot>/include/c++/<version>" (libstdc++) or in "<sysroot>/include/c++/v1"
// (libc++, the llvm-mingw layout). Without them the compile uses the C++ headers of the clang
// installation. These do not agree with the host, so the callers stop with this text. Empty
// when the triple is not MinGW, the sysroot is empty, or the headers are there. For the libc++
// layout, mingw_target_flags gives no "-isystem".
std::string mingw_sysroot_error(const std::string& target_triple, const std::string& sysroot);

// The C++ driver for a compiler: "clang" becomes "clang++" and "clang.exe" becomes "clang++.exe",
// in the same directory. A MinGW link must use the C++ driver, so it links libstdc++. Other
// names do not change.
std::string cxx_driver(const std::string& compiler);

} // namespace hot
