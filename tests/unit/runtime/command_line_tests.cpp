// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The command line helpers of the clang files backend (hot/command_line.h). They are pure, so
// the tests run on all platforms.
#include "hot/command_line.h"
#include "hot/runtime/process.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

TEST(CommandLine, QuoteLeavesPlainArgument)
{
    EXPECT_EQ(hot::windows_quote_arg("clang++"), "clang++");
    EXPECT_EQ(hot::windows_quote_arg("C:\\a\\b.cpp"), "C:\\a\\b.cpp"); // no quote: backslashes stay
    EXPECT_EQ(hot::windows_quote_arg("-DX=\\"), "-DX=\\");
}

TEST(CommandLine, QuoteAddsQuotes)
{
    EXPECT_EQ(hot::windows_quote_arg(""), "\"\"");
    EXPECT_EQ(hot::windows_quote_arg("a b"), "\"a b\"");
    EXPECT_EQ(hot::windows_quote_arg("a\tb"), "\"a\tb\"");
    EXPECT_EQ(hot::windows_quote_arg("C:\\Program Files\\x"), "\"C:\\Program Files\\x\"");
}

TEST(CommandLine, QuoteDoublesBackslashesBeforeQuote)
{
    EXPECT_EQ(hot::windows_quote_arg("a\"b"), "\"a\\\"b\"");
    EXPECT_EQ(hot::windows_quote_arg("a\\\"b"), "\"a\\\\\\\"b\"");
    // A backslash at the end goes before the closing quote: it becomes two.
    EXPECT_EQ(hot::windows_quote_arg("C:\\my dir\\"), "\"C:\\my dir\\\\\"");
}

TEST(CommandLine, JoinsArguments)
{
    EXPECT_EQ(hot::windows_command_line({}), "");
    EXPECT_EQ(hot::windows_command_line({"clang++", "-o", "a b.dll", ""}), "clang++ -o \"a b.dll\" \"\"");
}

TEST(CommandLine, MingwTriples)
{
    EXPECT_TRUE(hot::is_mingw_triple("x86_64-w64-mingw32"));
    EXPECT_TRUE(hot::is_mingw_triple("x86_64-w64-windows-gnu"));
    EXPECT_FALSE(hot::is_mingw_triple("x86_64-pc-windows-msvc"));
    EXPECT_FALSE(hot::is_mingw_triple("x86_64-pc-linux-gnu"));
    EXPECT_FALSE(hot::is_mingw_triple(""));
}

TEST(CommandLine, MingwFlagsNeedMingwTriple)
{
    EXPECT_TRUE(hot::mingw_target_flags("", "/sysroot").empty());
    EXPECT_TRUE(hot::mingw_target_flags("x86_64-pc-windows-msvc", "/sysroot").empty());
    const std::vector<std::string> expected = {"-fuse-ld=lld", "-static-libgcc", "-static-libstdc++",
        "-Wl,-Bstatic", "-lwinpthread", "-Wl,-Bdynamic"};
    EXPECT_EQ(hot::mingw_target_flags("x86_64-w64-mingw32", ""), expected);
}

TEST(CommandLine, MingwFlagsUseHighestLibstdcxxVersion)
{
    const fs::path root =
        fs::temp_directory_path() / ("hot_command_line_test_" + std::to_string(hot::detail::process_id()));
    std::error_code ec;
    fs::remove_all(root, ec);
    for (const char* v : {"9.5.0", "13.2.0", "13.10.1", "notes"})
        fs::create_directories(root / "include" / "c++" / v);
    const std::string sysroot = root.generic_string();
    const std::string base = sysroot + "/include/c++/13.10.1";
    const std::vector<std::string> expected = {"-nostdinc++", "-isystem", base, "-isystem",
        base + "/x86_64-w64-mingw32", "-isystem", base + "/backward", "-fuse-ld=lld",
        "-static-libgcc", "-static-libstdc++", "-Wl,-Bstatic", "-lwinpthread", "-Wl,-Bdynamic"};
    EXPECT_EQ(hot::mingw_target_flags("x86_64-w64-windows-gnu", sysroot), expected);
    fs::remove_all(root, ec);
}

TEST(CommandLine, MingwSysrootWithoutCxxHeadersIsAnError)
{
    const fs::path root =
        fs::temp_directory_path() / ("hot_command_line_sysroot_" + std::to_string(hot::detail::process_id()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "include");
    const std::string sysroot = root.generic_string();
    EXPECT_NE(hot::mingw_sysroot_error("x86_64-w64-mingw32", sysroot).find("has no C++ standard library headers"),
        std::string::npos);
    EXPECT_TRUE(hot::mingw_sysroot_error("x86_64-w64-mingw32", "").empty());
    EXPECT_TRUE(hot::mingw_sysroot_error("x86_64-pc-linux-gnu", sysroot).empty());
    fs::create_directories(root / "include" / "c++" / "v1");
    EXPECT_TRUE(hot::mingw_sysroot_error("x86_64-w64-mingw32", sysroot).empty()); // libc++
    fs::remove_all(root / "include" / "c++" / "v1", ec);
    fs::create_directories(root / "include" / "c++" / "13.2.0");
    EXPECT_TRUE(hot::mingw_sysroot_error("x86_64-w64-mingw32", sysroot).empty()); // libstdc++
    fs::remove_all(root, ec);
}

TEST(CommandLine, CxxDriver)
{
    EXPECT_EQ(hot::cxx_driver("clang"), "clang++");
    EXPECT_EQ(hot::cxx_driver("/opt/llvm/bin/clang"), "/opt/llvm/bin/clang++");
    EXPECT_EQ(hot::cxx_driver("C:\\llvm\\bin\\clang.exe"), "C:\\llvm\\bin\\clang++.exe");
    EXPECT_EQ(hot::cxx_driver("clang++"), "clang++");
    EXPECT_EQ(hot::cxx_driver("/usr/bin/clang-19"), "/usr/bin/clang-19");
    EXPECT_EQ(hot::cxx_driver("x86_64-w64-mingw32-g++"), "x86_64-w64-mingw32-g++");
}

// A Darwin triple has "apple", "darwin" or "macos" in it. An empty triple is not a Darwin
// triple: the caller decides for the host.
TEST(CommandLine, DarwinTriples)
{
    EXPECT_TRUE(hot::is_darwin_triple("arm64-apple-macosx13.0"));
    EXPECT_TRUE(hot::is_darwin_triple("x86_64-apple-darwin23.1.0"));
    EXPECT_TRUE(hot::is_darwin_triple("aarch64-unknown-macos"));
    EXPECT_FALSE(hot::is_darwin_triple(""));
    EXPECT_FALSE(hot::is_darwin_triple("x86_64-pc-linux-gnu"));
    EXPECT_FALSE(hot::is_darwin_triple("x86_64-w64-mingw32"));
}

// A Darwin module takes the host symbols when it loads, so the link must allow undefined symbols.
TEST(CommandLine, DarwinTargetFlags)
{
    EXPECT_EQ(hot::darwin_target_flags(true), std::vector<std::string>{"-Wl,-undefined,dynamic_lookup"});
    EXPECT_TRUE(hot::darwin_target_flags(false).empty());
}

// The JIT on macOS gets "-isysroot <sdk>" only when no flag sets a sysroot and the SDK is known.
TEST(CommandLine, DarwinSysrootFlags)
{
    const std::vector<std::string> sdk = {"-isysroot", "/sdk"};
    EXPECT_EQ(hot::darwin_sysroot_flags({"clang++", "-O1"}, "/sdk"), sdk);
    EXPECT_TRUE(hot::darwin_sysroot_flags({"clang++"}, "").empty());
    EXPECT_TRUE(hot::darwin_sysroot_flags({"clang++", "-isysroot", "/x"}, "/sdk").empty());
    EXPECT_TRUE(hot::darwin_sysroot_flags({"clang++", "-isysroot/x"}, "/sdk").empty());
    EXPECT_TRUE(hot::darwin_sysroot_flags({"clang++", "--sysroot=/x"}, "/sdk").empty());
    EXPECT_TRUE(hot::darwin_sysroot_flags({"clang++", "--sysroot", "/x"}, "/sdk").empty());
}

// A load error on macOS that comes from code signing or memory protection gets a hint with the
// entitlement. Other errors get no hint.
TEST(CommandLine, MacosEntitlementHint)
{
    const std::string dyld = "dlopen(/w/a.g1.so, 0x0006): tried: '/w/a.g1.so' (code signature in <...> "
                             "'/w/a.g1.so' not valid for use in process: mapped file has no Team ID)";
    EXPECT_NE(hot::macos_entitlement_hint(dyld).find("com.apple.security.cs.disable-library-validation"),
        std::string::npos);
    EXPECT_NE(hot::macos_entitlement_hint("cannot protect memory: Permission denied")
                  .find("com.apple.security.cs.allow-unsigned-executable-memory"),
        std::string::npos);
    EXPECT_EQ(hot::macos_entitlement_hint("Symbols not found: [ _foo ]"), "");
}

// A virtual path of the JIT gets the drive of the working directory on Windows. Clang reads "/x"
// as "D:/x" there, so the memory file system must hold the file under "D:/x".
TEST(CommandLine, JitVirtualPath)
{
    EXPECT_EQ(hot::jit_virtual_path("/hot-jit-virtual", "a.g1.e0.0.cpp", "/home/u"), "/hot-jit-virtual/a.g1.e0.0.cpp");
    EXPECT_EQ(hot::jit_virtual_path("/hot-jit-virtual", "a.cpp", "D:\\a\\hotpp"), "D:/hot-jit-virtual/a.cpp");
    EXPECT_EQ(hot::jit_virtual_path("/hot-jit-virtual", "a.cpp", "c:/w"), "c:/hot-jit-virtual/a.cpp");
    EXPECT_EQ(hot::jit_virtual_path("/hot-jit-virtual", "a.cpp", ""), "/hot-jit-virtual/a.cpp");
    EXPECT_EQ(hot::jit_virtual_path("/hot-jit-virtual", "a.cpp", "1:/x"), "/hot-jit-virtual/a.cpp");
}

// The JIT backend and the files backend take the C runtime flag from one function. An MSVC target
// gets the DLL runtime of the host. Other targets get no flag.
TEST(CommandLine, MsRuntimeLibFlags)
{
    using Args = std::vector<std::string>;
    EXPECT_EQ(hot::ms_runtime_lib_flags(true, false), Args{"-fms-runtime-lib=dll"});
    EXPECT_EQ(hot::ms_runtime_lib_flags(true, true), Args{"-fms-runtime-lib=dll_dbg"});
    EXPECT_TRUE(hot::ms_runtime_lib_flags(false, false).empty());
    EXPECT_TRUE(hot::host_ms_runtime_lib_flags("x86_64-w64-mingw32").empty());
#if defined(_MSC_VER)
    #ifdef _DEBUG
    const Args host = {"-fms-runtime-lib=dll_dbg"};
    #else
    const Args host = {"-fms-runtime-lib=dll"};
    #endif
#else
    const Args host;
#endif
    // The JIT compile has no triple. The files backend has the triple of the options.
    EXPECT_EQ(hot::host_ms_runtime_lib_flags(""), host);
    EXPECT_EQ(hot::host_ms_runtime_lib_flags("x86_64-pc-windows-msvc"), host);
}
