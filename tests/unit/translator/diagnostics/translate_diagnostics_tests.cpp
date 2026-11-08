// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the diagnostics of hotc::translate: the count, the order, the position and the recovery.
#include "diag.h"
#include "parser/parser.h"
#include "hot/hotc_lib.h"
#include "support/stub_translate_test_support.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>

using namespace translator_test;
using namespace stub_translate_test;

TEST(TranslateDiagnostics, TwoBrokenMembersGiveTwoErrors)
{
    const hotc::TranslateResult r = translate_text(
        "class A : B {\n"
        "    x : ;\n"
        "    void ok() { }\n"
        "    [alias(1)] int y;\n"
        "}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(count_errors(r.diagnostics), 2) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("t.hot:2:"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("t.hot:4:"), std::string::npos) << r.diagnostics;
}

// The syntax errors of two bodies and an unknown annotation come in source order, each with
// a line and a column.
TEST(TranslateDiagnostics, BodyErrorsAndAnnotationErrorsComeInSourceOrder)
{
    const hotc::TranslateResult r = translate_text(
        "class A : B {\n"
        "    void f() { var x = ; }\n"
        "    void g() { var y = 1 + ; }\n"
        "}\n"
        "class C : B {\n"
        "    [weak(\"x\")] int n;\n"
        "    void h() { var z = ; }\n"
        "}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.cpp.empty());
    std::vector<std::string> prefixes;
    std::istringstream in(r.diagnostics);
    for (std::string line; std::getline(in, line);)
    {
        const size_t e = line.find(": error:");
        ASSERT_NE(e, std::string::npos) << r.diagnostics;
        prefixes.push_back(line.substr(0, e));
    }
    EXPECT_EQ(prefixes, (std::vector<std::string>{"t.hot:2:24", "t.hot:3:28", "t.hot:6:6", "t.hot:7:24"}))
        << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("attribute 'weak' takes 0 arguments"), std::string::npos) << r.diagnostics;
}

// One fault gives one error. The end of the file after an unterminated comment or literal
// is not a new fault.
TEST(TranslateDiagnostics, OneFaultGivesOneError)
{
    const char* const scripts[] = {
        "class A : B {\n    void f() { var s = \"abc; }\n}\n",
        "class A : B {\n    void f() { var s = 1; /* open }\n}\n",
        "class A : B { }\n/* open\n",
        "foo bar baz\n",
        "class A : B {\n    string x = \"abc;\n}\n",
    };
    for (const char* text : scripts)
    {
        const hotc::TranslateResult r = translate_text(text);
        EXPECT_FALSE(r.ok) << text;
        EXPECT_EQ(count_errors(r.diagnostics), 1) << text << "\n" << r.diagnostics;
    }
}

TEST(TranslateDiagnostics, StrayControlByteHasNoFoundText)
{
    const hotc::TranslateResult r = translate_text(std::string("\0abc\n", 5));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.diagnostics, "t.hot:1:1: error: expected 'using', 'class', 'struct', 'interface', 'enum' or a variable\n")
        << r.diagnostics;
}

TEST(TranslateDiagnostics, DeepNestingGivesOneError)
{
    const std::string deep(300, '{');
    const hotc::TranslateResult r =
        translate_text("class A : B {\n    void f() " + deep + std::string(300, '}') + "\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(count_errors(r.diagnostics), 1) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("maximum recursion depth exceeded"), std::string::npos) << r.diagnostics;
}

TEST(TranslateDiagnostics, BadImportKeepsItsText)
{
    const hotc::TranslateResult r = translate_text("using 5;\nclass A : B { }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("using needs"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("t.hot:1:7: error:"), std::string::npos) << r.diagnostics;
}

TEST(TranslateDiagnostics, UnterminatedBraceEndsWithAnError)
{
    const hotc::TranslateResult r = translate_text("class A : B {\n    void f() {\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("unexpected end of file"), std::string::npos) << r.diagnostics;
}

TEST(TranslateDiagnostics, UnterminatedCommentEndsWithAnError)
{
    const hotc::TranslateResult r = translate_text("class A : B { }\n/* open\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("unterminated comment"), std::string::npos) << r.diagnostics;
    const hotc::TranslateResult r2 = translate_text("class A : B { void f() { /* open }\n");
    EXPECT_FALSE(r2.ok);
    EXPECT_FALSE(r2.diagnostics.empty());
}

TEST(TranslateDiagnostics, AnyByteIsAccepted)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    // The byte 0xE9 is not valid UTF-8 here. It is in a comment, a string and a body comment.
    const std::string script =
        "using \"w.h\";\n// caf\xE9\nclass Foo : Widget {\n    int n = 3;  // caf\xE9\n"
        "    override void Update(float dt) { /* \xE9 */ var t = \"na\xE9\"; t; }\n}\n";
    const hotc::TranslateResult r = hotc::translate(temp_options(dir, script));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("\"na\xE9\""), std::string::npos) << r.cpp;
}

TEST(TranslateDiagnostics, DoubleColonIsNotAField)
{
    const hotc::TranslateResult r = translate_text("class A : B {\n    x::y : int;\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("t.hot:2:6: error: use '.' instead of '::'"), std::string::npos) << r.diagnostics;
}

TEST(TranslateDiagnostics, BacktickInATypeIsAnError)
{
    const hotc::TranslateResult r = translate_text("class A : B {\n    Foo`a` x;\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("string interpolation is not supported"), std::string::npos) << r.diagnostics;
}

// A script cut at any byte must end with a result and not hang. A failure has a text.
TEST(TranslateDiagnostics, TruncatedInputsTerminate)
{
    // The parser runs on every prefix (cut at each byte). A prefix that parses runs the whole
    // translator, so the full translation uses each 16th cut only. The session keeps the parse of
    // the headers (libclang), and a translation parses again only for a new name.
    // The sums are in microseconds: one parse of a prefix needs less than 1 ms.
    using Us = std::chrono::microseconds;
    auto us_since = [](Clock::time_point t) { return std::chrono::duration_cast<Us>(Clock::now() - t).count(); };
    long long parser_us = 0;  // parse_script on all cuts
    long long cached_us = 0;  // the translations without a libclang parse
    long long clang_us = 0;   // the translations with a libclang parse
    long long slowest_us = 0; // the slowest translation without a libclang parse
    int clang_parses = 0;
    int most_parses = 0; // the most libclang parses of one file
    size_t first = 0;
    for (const char* base : {"player", "native_std", "timer"})
    {
        for (int gen = 1; gen <= 2; ++gen, ++first)
        {
            const std::string file = std::string(base) + "_v" + std::to_string(gen) + ".hot";
            const hotc::TranslateOptions full = engine_options(file.c_str(), gen);
            hotc::TranslatorSession session; // the header parse is cached
            for (size_t cut = 0; cut <= full.script_text.size(); ++cut)
            {
                const std::string text = full.script_text.substr(0, cut);
                const auto parse_start = Clock::now();
                const hotc::LineMap map(text);
                hotc::Diagnostics diag;
                (void)hotc::parse_script(text, map, diag);
                parser_us += us_since(parse_start);
                if (cut % 16 != first * 3 % 16)
                    continue;
                hotc::TranslateOptions o = full;
                o.script_text = text;
                const int parses = session.parse_count();
                const auto translate_start = Clock::now();
                const hotc::TranslateResult r = hotc::translate(o, session);
                const long long us = us_since(translate_start);
                if (session.parse_count() != parses)
                    clang_us += us;
                else
                {
                    cached_us += us;
                    slowest_us = std::max(slowest_us, us);
                }
                if (!r.ok)
                {
                    ASSERT_FALSE(r.diagnostics.empty()) << file << " cut at " << cut;
                }
            }
            clang_parses += session.parse_count();
            most_parses = std::max(most_parses, session.parse_count());
        }
    }
    // The bounds find a hang or work that grows too fast, not a slow run. The time of a libclang
    // parse depends on the machine and on the load: on a Windows CI runner the same test took
    // 3.8 s and 11.6 s in one run. So the number of libclang parses has a bound, and their time
    // has no bound. The parser and the cached translations are the code of the translator. A normal
    // build needs less than 0.5 s for them. A sanitizer build under a parallel ctest load needs up
    // to 5 times more, so the bound of each part is 15 s.
    const std::string times = "parser " + std::to_string(parser_us / 1000) + " ms, cached translations "
        + std::to_string(cached_us / 1000) + " ms (slowest " + std::to_string(slowest_us / 1000) + " ms), "
        + std::to_string(clang_parses) + " libclang parses " + std::to_string(clang_us / 1000) + " ms";
    RecordProperty("times", times);
    EXPECT_LE(most_parses, 2) << times;
    EXPECT_LT(parser_us, 15000000) << times;
    EXPECT_LT(cached_us, 15000000) << times;
}

// A non-string attribute argument on a native stub gives one error and no second error.
TEST(TranslateDiagnostics, NonStringStubAttributeArgumentGivesOneError)
{
    for (const char* stub : {"[native, handle(x)] class Weapon;\n", "[native, handle(1)] class Weapon;\n",
             "[native, handle(\"std::shared_ptr<$T>\"), intrusive(x, \"R\")] class Weapon;\n",
             "[native, intrusive(\"A\", 2)] class Weapon;\n"})
    {
        const hotc::TranslateResult r = translate_text(std::string(stub) + "class A : B {\n    void f() { }\n}\n");
        EXPECT_FALSE(r.ok);
        EXPECT_EQ(count_errors(r.diagnostics), 1) << stub << r.diagnostics;
    }
}

// A storage rule for the subclasses of a script class is an error. A prelude text is allowed.
TEST(TranslateDiagnostics, DerivedStorageRuleOnAScriptClassIsAnError)
{
    const hotc::TranslateResult r = hier_translate("[handle(\"std::shared_ptr<$T>\")] class : Foo;\n", "");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(count_errors(r.diagnostics), 1) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("'class : Foo': a storage rule for the subclasses of a script class is not allowed"),
        std::string::npos)
        << r.diagnostics;
    const hotc::TranslateResult ok = hier_translate("[cpp_prelude(\"x;\")] class : Foo;\n", "");
    EXPECT_TRUE(ok.ok) << ok.diagnostics;
}

TEST(TranslateDiagnostics, PreludeWithSelfOnANativeTypeWarns)
{
    const hotc::TranslateResult r = hier_translate(
        "[native, self, handle(\"std::shared_ptr<$T>\"), cpp_prelude(\"x;\")] class Object;\n", "");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.warnings.find("warning: 'cpp_prelude' with 'self' has no effect on the native type 'Object': it has no shell"),
        std::string::npos)
        << r.warnings;
}
