// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Helpers that read the example scripts and check the diagnostics of a bad script.
#pragma once

#include "hot/backend.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace test_support
{

inline std::string read_file(const std::string& path)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline hot::ScriptSource source(const char* file)
{
    const std::string path = std::string(HOT_SOURCE_DIR) + "/tests/scripts/" + file;
    return {path, read_file(path), {}};
}

// The line number of the first line that has the text.
inline int line_with(const std::string& text, const std::string& needle)
{
    int n = 0;
    std::istringstream src(text);
    for (std::string l; std::getline(src, l);)
        if (++n, l.find(needle) != std::string::npos)
            return n;
    return 0;
}

// Each "error:" line must name the script, not a generated file.
inline void expect_errors_at_script(const std::string& text, int bad_line)
{
    const std::string expect = "bad.hot:" + std::to_string(bad_line) + ":";
    EXPECT_NE(text.find(expect), std::string::npos) << "no '" << expect << "' in:\n" << text;
    std::istringstream ls(text);
    int errors = 0;
    for (std::string l; std::getline(ls, l);)
    {
        if (l.find(" error:") == std::string::npos)
            continue;
        ++errors;
        EXPECT_NE(l.find("bad.hot:"), std::string::npos) << "error not at the script: " << l;
        EXPECT_EQ(l.find(".cpp"), std::string::npos) << l;
    }
    EXPECT_GT(errors, 0) << text;
}

} // namespace test_support
