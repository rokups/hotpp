// Golden-output tests. The files in tests/golden/expected/ hold the generated C++ and the slot
// file of each example script. A later parser port must give the same bytes.
// Set SPP_UPDATE_GOLDEN to write the files and pass.
// CMake defines SPP_SOURCE_DIR.
#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace translator_test;

namespace
{

// The expected files are in tests/golden/expected/. SPP_UPDATE_GOLDEN writes them.
void check_golden(const std::string& name, const std::string& text)
{
    const fs::path path = fs::path(SPP_SOURCE_DIR) / "tests" / "golden" / "expected" / name;
    if (std::getenv("SPP_UPDATE_GOLDEN") != nullptr)
    {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
        return;
    }
    ASSERT_TRUE(fs::exists(path)) << "missing golden file: " << path;
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    EXPECT_EQ(ss.str(), text) << "golden file differs: " << name;
}

// Translates "<base>_v1" as generation 1 and "<base>_v2" as generation 2.
// The slot input of generation 2 is the slot output of generation 1.
void golden_pair(const std::string& base)
{
    std::string slots;
    for (int gen = 1; gen <= 2; ++gen)
    {
        const std::string file = base + "_v" + std::to_string(gen) + ".spp";
        sppc::TranslateOptions o = engine_options(file.c_str(), gen);
        o.script_name = file; // the file name only, so the output has no absolute path
        o.slots_in = slots;
        const sppc::TranslateResult r = sppc::translate(o);
        ASSERT_TRUE(r.ok) << r.diagnostics;
        const std::string stem = base + "_v" + std::to_string(gen) + ".g" + std::to_string(gen);
        check_golden(stem + ".cpp", r.cpp);
        check_golden(stem + ".slots", r.slots_out);
        slots = r.slots_out;
    }
}

// Translates one script as generation 1. The script has no second generation.
void golden_one(const std::string& base)
{
    const std::string file = base + ".spp";
    sppc::TranslateOptions o = engine_options(file.c_str(), 1);
    o.script_name = file;
    const sppc::TranslateResult r = sppc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    check_golden(base + ".g1.cpp", r.cpp);
    check_golden(base + ".g1.slots", r.slots_out);
}

} // namespace

TEST(GoldenTranslation, Player)
{
    golden_pair("player");
}

TEST(GoldenTranslation, NativeStd)
{
    golden_pair("native_std");
}

TEST(GoldenTranslation, Timer)
{
    golden_pair("timer");
}

TEST(GoldenTranslation, Props)
{
    golden_one("props_v1");
}

TEST(GoldenTranslation, AbiChild)
{
    golden_pair("abi_child");
}
