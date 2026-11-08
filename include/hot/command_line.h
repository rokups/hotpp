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

// True when the target triple is a Darwin (macOS) target: "apple", "darwin" or "macos" in the
// triple. An empty triple gives false. The caller decides for the host (__APPLE__).
bool is_darwin_triple(const std::string& target_triple);

// The extra link flags of a script module for a Darwin target. The module takes the runtime
// and the native symbols from the host when it loads. The Apple linker refuses undefined
// symbols by default, so the flags are "-Wl,-undefined,dynamic_lookup". Empty when "darwin" is false.
std::vector<std::string> darwin_target_flags(bool darwin);

// The C runtime flag of a compile for an MSVC target. The script code must use the C runtime DLL
// of the host (/MD or /MDd). The default of clang is the static runtime. The flag is
// "-fms-runtime-lib=dll", or "-fms-runtime-lib=dll_dbg" when "debug_runtime" is true. Empty when
// "msvc_target" is false.
std::vector<std::string> ms_runtime_lib_flags(bool msvc_target, bool debug_runtime);

// The C runtime flag of this host for a compile to "target_triple" (ms_runtime_lib_flags). An MSVC
// host gives the flag of its own runtime (_DEBUG gives dll_dbg). A MinGW triple and other hosts
// give no flag. The JIT backend and the files backend both use this function.
std::vector<std::string> host_ms_runtime_lib_flags(const std::string& target_triple);

// The sysroot flags of the JIT compile on macOS. The JIT has no xcrun and no clang
// configuration file, so it can miss the SDK. The flags are "-isysroot <sdk>". They are empty
// when "sdk" is empty or "args" already has -isysroot or --sysroot.
std::vector<std::string> darwin_sysroot_flags(const std::vector<std::string>& args, const std::string& sdk);

// A hint for a load error on macOS. A host with the hardened runtime refuses an ad-hoc signed
// module (library validation) and refuses executable JIT memory. The hint names the entitlement.
// Empty when the error text has no sign of these causes.
std::string macos_entitlement_hint(const std::string& error);

// The C++ driver for a compiler: "clang" becomes "clang++" and "clang.exe" becomes "clang++.exe",
// in the same directory. A MinGW link must use the C++ driver, so it links libstdc++. Other
// names do not change.
std::string cxx_driver(const std::string& compiler);

// The path of a virtual file of the JIT compile: "<dir>/<file>". "dir" starts with "/". On
// Windows, "/x" has no drive, and Clang adds the drive of the working directory ("D:/x"). Thus
// the path gets the drive of "cwd" when "cwd" starts with a drive ("D:"). The memory file system
// and the compiler then see the same absolute path. Other paths do not change.
std::string jit_virtual_path(const std::string& dir, const std::string& file, const std::string& cwd);

} // namespace hot
