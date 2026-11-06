// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A helper for the tests: it gives the command of an LLVM tool for run_process.
#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace test_support
{

// The command of an LLVM tool for run_process. On Windows, the tool is in HOT_LLVM_TOOLS_DIR when
// the folder is known and has the tool. Else the command is the name with ".exe", and PATH finds
// it. On other systems, the command is the name, and PATH finds it.
inline std::string llvm_tool(const std::string& name)
{
#ifdef _WIN32
    #ifdef HOT_LLVM_TOOLS_DIR
    const std::filesystem::path in_dir = std::filesystem::path(HOT_LLVM_TOOLS_DIR) / (name + ".exe");
    std::error_code ec;
    if (std::filesystem::exists(in_dir, ec))
        return in_dir.string();
    #endif
    return name + ".exe";
#else
    return name;
#endif
}

} // namespace test_support
