// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Experiment F2 (design 4.2): common mistakes in a script. The compile fails. The error text
// speaks script names and points to the script line.
//
// Each case is a pair of files in tests/golden/diagnostics: NN_name.hot (or NN_name.cpp for a
// plain C++ script) and NN_name.expected.
// The test loads the script with the backend that HOT_BACKEND selected. The text of the error
// must equal the expected file. The source directory shows as "<src>". The last line of the
// error ("the compile of ... failed") is not part of the expected text.
// To change a golden file, set HOT_UPDATE_GOLDEN: the test writes the error text of the run
// (without the "the compile of" line) and still compares. Look at the diff before you commit.
#include "hot/backend.h"
#include "support/script_files.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

namespace fs = std::filesystem;

const std::string kDir = std::string(HOT_SOURCE_DIR) + "/tests/golden/diagnostics/";

std::vector<std::string> case_names()
{
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(kDir))
        if (e.path().extension() == ".expected")
            names.push_back(e.path().stem().string());
    std::sort(names.begin(), names.end());
    return names;
}

// The error text without the lines that only one backend prints ("the compile of ..." and
// "N error generated.") and with "<src>" for the source dir.
std::string normalize(std::string text)
{
    for (size_t p = text.find(HOT_SOURCE_DIR); p != std::string::npos; p = text.find(HOT_SOURCE_DIR, p))
        text.replace(p, std::string(HOT_SOURCE_DIR).size(), "<src>");
#ifdef _WIN32
    // clang joins an include dir and a header name with "\": "<src>/tests/engine\Actor.h".
    for (size_t p = text.find("<src>/"); p != std::string::npos; p = text.find("<src>/", p + 1))
        for (size_t i = p; i < text.size() && text[i] != ':' && text[i] != '\n'; ++i)
            if (text[i] == '\\')
                text[i] = '/';
    // The MSVC STL spells the default arguments of std::string. The golden file has the short form.
    const std::string long_string = "basic_string<char, char_traits<char>, allocator<char>>";
    for (size_t p = text.find(long_string); p != std::string::npos; p = text.find(long_string, p))
        text.replace(p, long_string.size(), "basic_string<char>");
#endif
    std::istringstream in(text);
    std::string out;
    for (std::string l; std::getline(in, l);)
        if (l.rfind("the compile of ", 0) != 0 && l.find(" generated.") == std::string::npos)
            out += l + "\n";
    return out;
}

class DiagnosticMistakes : public ::testing::TestWithParam<std::string>
{
};

TEST_P(DiagnosticMistakes, ErrorSpeaksScriptNames)
{
    const std::string name = GetParam();
    // A case is a .cpp script when NN_name.cpp exists, else a .hot script.
    // A relative name: the error text starts with it.
    const bool is_cpp = fs::exists(kDir + name + ".cpp");
    const std::string file = name + (is_cpp ? ".cpp" : ".hot");
    const hot::ScriptSource source{file, test_support::read_file(kDir + file), {}};
    ASSERT_FALSE(source.text.empty());
    auto backend = hot::make_default_backend();
    const hot::LoadResult r = backend->load(source, 1);
    ASSERT_FALSE(r) << "the script must not compile";
    const std::string actual = normalize(r.error);
    if (std::getenv("HOT_UPDATE_GOLDEN"))
    {
        std::string text = r.error;
        for (size_t p = text.find(HOT_SOURCE_DIR); p != std::string::npos; p = text.find(HOT_SOURCE_DIR, p))
            text.replace(p, std::string(HOT_SOURCE_DIR).size(), "<src>");
        std::istringstream in(text);
        std::string out;
        for (std::string l; std::getline(in, l);)
            if (l.rfind("the compile of ", 0) != 0)
                out += l + "\n";
        std::ofstream(kDir + name + ".expected") << out;
    }
    EXPECT_EQ(actual, normalize(test_support::read_file(kDir + name + ".expected")));
    // No generated name is left in the lines that name the script.
    // A script that uses "__" in a name of its own can see that name in the error.
    const bool script_has_dunder = source.text.find("__") != std::string::npos;
    // The script name of a .cpp case ends with ".cpp", so only the generated file name ".g1.cpp"
    // is forbidden for it.
    for (const char* generated : {"_Shell", "State_g", "__", "self->", "hot::deref", "state_of", is_cpp ? ".g1.cpp" : ".cpp"})
    {
        if (script_has_dunder && std::string(generated) == "__")
            continue;
        EXPECT_EQ(actual.find(generated), std::string::npos) << generated << " in:\n" << actual;
    }
}

INSTANTIATE_TEST_SUITE_P(Mistakes, DiagnosticMistakes, ::testing::ValuesIn(case_names()),
    [](const ::testing::TestParamInfo<std::string>& i)
{
    std::string s = i.param;
    std::replace_if(s.begin(), s.end(), [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }, '_');
    return s;
});

// The table has at least ten different mistakes (design F2).
TEST(DiagnosticMistakesTable, HasTenCases)
{
    EXPECT_GE(case_names().size(), 10u);
}

} // namespace
