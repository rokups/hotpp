// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the entry function "main": the check of the signature and the symbol "hot_main".
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

const char* kMainError = "the function 'main' must be 'void main()', 'int main()', 'void main(std.string[] args)' or "
                         "'int main(std.string[] args)'";

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

hotc::TranslateResult translate_script(const std::string& text)
{
    hotc::TranslateOptions o;
    o.script_name = "t.hot";
    o.script_text = text;
    o.generation = 1;
    return hotc::translate(o);
}

void expect_main_error(const std::string& text)
{
    const hotc::TranslateResult r = translate_script(text);
    EXPECT_FALSE(r.ok) << text;
    EXPECT_TRUE(has(r.diagnostics, kMainError)) << text << "\n" << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "hot_main")) << text;
}

} // namespace

TEST(EntryMainTranslation, VoidWithoutArguments)
{
    const hotc::TranslateResult r = translate_script("void main() { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "extern \"C\" HOT_MODULE_EXPORT int32_t hot_main(int32_t argc, const char* const* argv)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "    hot_fn_main();\n    return 0;\n")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "std::vector")) << r.cpp;
}

TEST(EntryMainTranslation, IntWithoutArguments)
{
    const hotc::TranslateResult r = translate_script("int main() { return 3; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "extern \"C\" HOT_MODULE_EXPORT int32_t hot_main(int32_t argc, const char* const* argv)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "    return hot_fn_main();\n")) << r.cpp;
}

TEST(EntryMainTranslation, VoidWithArguments)
{
    const hotc::TranslateResult r = translate_script("void main(std.string[] args) { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    using Args = hot::span<std::string>;\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "        args.emplace_back(argv[i]);\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "    hot_fn_main(Args(args.data(), args.size()));\n    return 0;\n")) << r.cpp;
}

TEST(EntryMainTranslation, IntWithArguments)
{
    const hotc::TranslateResult r = translate_script("int main(std.string[] args) { return 0; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp,
        "extern \"C\" HOT_MODULE_EXPORT int32_t hot_main(int32_t argc, const char* const* argv)\n{\n"
        "    using Args = hot::span<std::string>;\n"
        "    std::vector<std::remove_cv_t<typename Args::value_type>> args;\n"
        "    args.reserve(argc > 0 ? static_cast<size_t>(argc) : 0);\n"
        "    for (int32_t i = 0; i < argc; ++i)\n"
        "        args.emplace_back(argv[i]);\n"
        "    return hot_fn_main(Args(args.data(), args.size()));\n"
        "}\n"))
        << r.cpp;
    EXPECT_LT(r.cpp.find("hot_fn_main(hot::span"), r.cpp.find("hot_main(int32_t argc")) << r.cpp;
    EXPECT_LT(r.cpp.find("hot_main(int32_t argc"), r.cpp.find("script_register")) << r.cpp;
}

TEST(EntryMainTranslation, NoMainNoSymbol)
{
    const hotc::TranslateResult r = translate_script("int Twice(int x) { return x * 2; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "hot_main")) << r.cpp;
}

TEST(EntryMainTranslation, BadSignaturesAreErrors)
{
    expect_main_error("float main() { return 0; }\n");
    expect_main_error("std.string main() { return \"\"; }\n");
    expect_main_error("void main(int x) { }\n");
    expect_main_error("void main(std.string[] a, int b) { }\n");
    expect_main_error("void main(int[] a) { }\n");
    expect_main_error("void main(in std.string[] a) { }\n");
    expect_main_error("async void main() { }\n");
    expect_main_error("static void main() { }\n");
    expect_main_error("public void main() { }\n");
    expect_main_error("ref int main() { return 0; }\n");
    expect_main_error("void main(std.string[] a = null) { }\n");
    expect_main_error("void main(ref std.string[] a) { }\n");
}

TEST(EntryMainTranslation, TwoMainOverloadsAreAnError)
{
    // Each form alone is valid. The second one gives the overload error of the translator.
    const hotc::TranslateResult r = translate_script("void main() { }\nint main(std.string[] args) { return 0; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "function 'main' is declared twice (overloads are not supported)")) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "hot_main"));
}

TEST(EntryMainTranslation, StaticMethodMainIsNotAnEntryPoint)
{
    const hotc::TranslateResult r = translate_script("class A { static void main() { } }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "hot_main")) << r.cpp;
}

TEST(EntryMainTranslation, MainInImportedScriptIsAnError)
{
    TempDir dir;
    write_file(dir.path() / "lib.hot", "int main() { return 1; }\n");
    hotc::TranslateOptions o = temp_options(dir, "using \"lib.hot\";\nint main() { return 0; }\n");
    o.modules.emplace_back("lib.hot", "int main() { return 1; }\n");
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, kMainError)) << r.diagnostics;
}

TEST(EntryMainTranslation, BadMainInImportedScriptHasOneError)
{
    for (const char* lib : {"public void main() { }\n", "static void main() { }\n"})
    {
        TempDir dir;
        write_file(dir.path() / "lib.hot", lib);
        hotc::TranslateOptions o = temp_options(dir, "using \"lib.hot\";\nint Twice(int x) { return x * 2; }\n");
        o.modules.emplace_back("lib.hot", lib);
        const hotc::TranslateResult r = hotc::translate(o);
        EXPECT_FALSE(r.ok) << lib;
        EXPECT_TRUE(has(r.diagnostics, kMainError)) << lib << r.diagnostics;
        EXPECT_FALSE(has(r.diagnostics, "free function")) << lib << r.diagnostics;
        EXPECT_EQ(r.diagnostics.find(kMainError), r.diagnostics.rfind(kMainError)) << lib << r.diagnostics;
    }
}
