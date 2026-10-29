// Experiment F2 (design 4.2): common mistakes in a script. The compile fails. The error text
// speaks script names and points to the script line.
//
// Each case is a pair of files in tests/golden/diagnostics: NN_name.spp and NN_name.expected.
// The test loads the script with the backend that SPP_BACKEND selected. The text of the error
// must equal the expected file. The source directory shows as "<src>". The last line of the
// error ("the compile of ... failed") is not part of the expected text.
// To change a golden file, run sppc on the script, look at the result and copy it.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "spp/backend.h"
#include "support/script_files.h"

namespace {

namespace fs = std::filesystem;

const std::string kDir = std::string(SPP_SOURCE_DIR) + "/tests/golden/diagnostics/";

std::vector<std::string> case_names()
{
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(kDir))
        if (e.path().extension() == ".expected") names.push_back(e.path().stem().string());
    std::sort(names.begin(), names.end());
    return names;
}

// The error text without the lines that only one backend prints ("the compile of ..." and
// "N error generated.") and with "<src>" for the source dir.
std::string normalize(std::string text)
{
    for (size_t p = text.find(SPP_SOURCE_DIR); p != std::string::npos; p = text.find(SPP_SOURCE_DIR, p))
        text.replace(p, std::string(SPP_SOURCE_DIR).size(), "<src>");
    std::istringstream in(text);
    std::string out;
    for (std::string l; std::getline(in, l);)
        if (l.rfind("the compile of ", 0) != 0 && l.find(" generated.") == std::string::npos) out += l + "\n";
    return out;
}

class DiagnosticMistakes : public ::testing::TestWithParam<std::string> {};

TEST_P(DiagnosticMistakes, ErrorSpeaksScriptNames)
{
    const std::string name = GetParam();
    // A relative name: the error text starts with it.
    const spp::ScriptSource source{name + ".spp", test_support::read_file(kDir + name + ".spp"), {}};
    ASSERT_FALSE(source.text.empty());
    auto backend = spp::make_default_backend();
    const spp::LoadResult r = backend->load(source, 1);
    ASSERT_FALSE(r) << "the script must not compile";
    const std::string actual = normalize(r.error);
    EXPECT_EQ(actual, normalize(test_support::read_file(kDir + name + ".expected")));
    // No generated name is left in the lines that name the script.
    for (const char* generated : {"_Shell", "State_g", "__", "self->", "spp::deref", ".cpp"})
        EXPECT_EQ(actual.find(generated), std::string::npos) << generated << " in:\n" << actual;
}

INSTANTIATE_TEST_SUITE_P(Mistakes, DiagnosticMistakes, ::testing::ValuesIn(case_names()),
                         [](const ::testing::TestParamInfo<std::string>& i) {
                             std::string s = i.param;
                             std::replace_if(s.begin(), s.end(), [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }, '_');
                             return s;
                         });

// The table has at least ten different mistakes (design F2).
TEST(DiagnosticMistakesTable, HasTenCases) { EXPECT_GE(case_names().size(), 10u); }

}  // namespace
