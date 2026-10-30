// Helpers for the tests that call hotc::translate. CMake defines HOT_SOURCE_DIR.
#pragma once

#include "hot/hotc_lib.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace translator_test
{

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

inline std::string read_file(const std::string& path)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline void write_file(const fs::path& path, const std::string& text)
{
    std::ofstream(path) << text;
}

// A new empty directory for the headers of one test.
class TempDir
{
public:
    TempDir()
    {
        static int n = 0;
        path_ =
            fs::temp_directory_path() / ("hotc_session_test_" + std::to_string(::getpid()) + "_" + std::to_string(n++));
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

inline hotc::TranslateOptions engine_options(const char* script_file, int generation)
{
    const std::string root = HOT_SOURCE_DIR;
    hotc::TranslateOptions o;
    o.script_name = root + "/tests/scripts/" + script_file;
    o.script_text = read_file(o.script_name);
    o.generation = generation;
    o.include_dirs = {root + "/tests/engine", root + "/include"};
    return o;
}

inline hotc::TranslateOptions temp_options(const TempDir& dir, const std::string& script_text)
{
    hotc::TranslateOptions o;
    o.script_name = "t.hot";
    o.script_text = script_text;
    o.generation = 1;
    o.include_dirs = {dir.path().string()};
    return o;
}

inline const char* kHeaderV1 =
    "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n"
    "class Panel { public: virtual ~Panel() {} virtual void Layout(int w) {} };\n";

inline std::string widget_script(const std::string& method, const std::string& mods = "")
{
    return "using \"w.h\";\nclass Foo : Widget {\n    " + mods + "void " + method + " { }\n}\n";
}

// Count the "error:" lines in the diagnostics text.
inline int count_errors(const std::string& text)
{
    int n = 0;
    for (size_t p = text.find("error:"); p != std::string::npos; p = text.find("error:", p + 1))
        ++n;
    return n;
}

inline hotc::TranslateResult translate_text(const std::string& script)
{
    hotc::TranslateOptions o;
    o.script_name = "t.hot";
    o.script_text = script;
    o.generation = 1;
    return hotc::translate(o);
}

// The error text for a script text that needs no header.
inline std::string diag_of(const std::string& script)
{
    return translate_text(script).diagnostics;
}

inline std::string lambda_script(const std::string& body)
{
    return "using \"w.h\";\nclass Foo : Widget {\n    int n = 0;\n    void run(int* p) {\n" + body + "\n    }\n}\n";
}

} // namespace translator_test
