// Golden-output tests. The files in tests/golden/expected/ hold the generated C++ and the slot
// file of each example script. A later parser port must give the same bytes.
// Set HOT_UPDATE_GOLDEN to write the files and pass.
// CMake defines HOT_SOURCE_DIR.
#include "hot/hotc_lib.h"
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

// The expected files are in tests/golden/expected/. HOT_UPDATE_GOLDEN writes them.
void check_golden(const std::string& name, const std::string& text)
{
    const fs::path path = fs::path(HOT_SOURCE_DIR) / "tests" / "golden" / "expected" / name;
    if (std::getenv("HOT_UPDATE_GOLDEN") != nullptr)
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
// "ext" is the extension of the script files.
void golden_pair(const std::string& base, const std::string& ext = ".hot")
{
    std::string slots;
    for (int gen = 1; gen <= 2; ++gen)
    {
        const std::string file = base + "_v" + std::to_string(gen) + ext;
        hotc::TranslateOptions o = engine_options(file.c_str(), gen);
        o.script_name = file; // the file name only, so the output has no absolute path
        o.slots_in = slots;
        const hotc::TranslateResult r = hotc::translate(o);
        ASSERT_TRUE(r.ok) << r.diagnostics;
        const std::string stem = base + "_v" + std::to_string(gen) + ".g" + std::to_string(gen);
        check_golden(stem + ".cpp", r.cpp);
        check_golden(stem + ".slots", r.slots_out);
        slots = r.slots_out;
    }
}

// Translates one script as generation 1. The script has no second generation.
void golden_one(const std::string& base, const std::string& ext = ".hot")
{
    const std::string file = base + ext;
    hotc::TranslateOptions o = engine_options(file.c_str(), 1);
    o.script_name = file;
    const hotc::TranslateResult r = hotc::translate(o);
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

// Fixed-size array types: fields, locals, parameters, a return type and a generic argument.
TEST(GoldenTranslation, Array)
{
    golden_pair("array");
}

// The runtime checks: "a[i]" is hot::index, "*p" is hot::star, and the catch types of the runtime.
TEST(GoldenTranslation, Checks)
{
    golden_one("checks_v1");
}

// The scoped stub rules and cpp_prelude: "self", "class : Base" and the text in the shell.
TEST(GoldenTranslation, Prelude)
{
    golden_one("prelude_v1");
}

TEST(GoldenTranslation, Props)
{
    golden_one("props_v1");
}

TEST(GoldenTranslation, AbiChild)
{
    golden_pair("abi_child");
}

TEST(GoldenTranslation, CppPlayer)
{
    golden_pair("cpp_player", ".cpp");
}

// A .cpp script with static members: a static data member is a module variable, a static method
// is a free function, and a static constant is a const before the class block.
TEST(GoldenTranslation, CppStatic)
{
    golden_pair("cpp_static", ".cpp");
}

// A .cpp script with methods, a constructor and a destructor that are defined outside the class.
// The body goes to the member in the class block, and the definition text does not stay.
TEST(GoldenTranslation, CppOutOfClass)
{
    golden_pair("cpp_outofclass", ".cpp");
}

// A .cpp script with a constructor member initializer list. Each entry of a field becomes a
// statement at the start of the body, in the order of the field declarations.
TEST(GoldenTranslation, CppCtorInit)
{
    golden_pair("cpp_ctor_init", ".cpp");
}

// A .cpp script uses a script class as a type: "new", pointers, references, field access,
// method calls and casts on another script object.
TEST(GoldenTranslation, CppObjects)
{
    golden_pair("cpp_objects", ".cpp");
}

// A .cpp script class derives from a script class. The parent declares a virtual method.
// Generation 2 adds an override of a native virtual and a new virtual method in the child.
TEST(GoldenTranslation, CppInherit)
{
    golden_pair("cpp_inherit", ".cpp");
}

// A .cpp script with lambdas that use the object: the capture of "this" becomes the capture of
// the shell, and a field reads through the state of the shell.
TEST(GoldenTranslation, CppLambda)
{
    golden_pair("cpp_lambda", ".cpp");
}

// A .cpp script with default arguments: the prototype has them and the definition does not.
TEST(GoldenTranslation, CppDefaults)
{
    golden_one("cpp_defaults", ".cpp");
}

// A .cpp script with free functions that use a module variable: the prototype comes first and
// the definition comes after the module block.
TEST(GoldenTranslation, CppFreeFunction)
{
    golden_one("cpp_freefn", ".cpp");
}

// A .cpp script with a function template that uses a module variable. The template stays in its
// place, after the module block.
TEST(GoldenTranslation, CppTemplateFunction)
{
    golden_one("cpp_template_fn", ".cpp");
}

// A .cpp script with a function in a namespace that uses a module variable.
TEST(GoldenTranslation, CppNamespaceFunction)
{
    golden_one("cpp_namespace_fn", ".cpp");
}

// A .cpp script with classes and module variables in namespaces, one nested with the compact form.
// Two classes have the same name in two namespaces. Generation 2 changes a variable type.
TEST(GoldenTranslation, CppNamespaces)
{
    golden_pair("cpp_ns", ".cpp");
}

// A .cpp script with a constexpr function that reads a module variable only at runtime.
TEST(GoldenTranslation, CppConstexprFunction)
{
    golden_one("cpp_constexpr", ".cpp");
}
