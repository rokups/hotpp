// Tests for the diagnostics of sppc::translate: the count, the order, the position and the recovery.
#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "diag.h"
#include "parser/parser.h"
#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

using namespace translator_test;

TEST(TranslateDiagnostics, TwoBrokenMembersGiveTwoErrors)
{
    const sppc::TranslateResult r = translate_text(
        "class A : B {\n"
        "    x : ;\n"
        "    void ok() { }\n"
        "    [alias(1)] int y;\n"
        "}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(count_errors(r.diagnostics), 2) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("t.spp:2:"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("t.spp:4:"), std::string::npos) << r.diagnostics;
}

// The syntax errors of two bodies and an unknown annotation come in source order, each with
// a line and a column.
TEST(TranslateDiagnostics, BodyErrorsAndAnnotationErrorsComeInSourceOrder)
{
    const sppc::TranslateResult r = translate_text(
        "class A : B {\n"
        "    void f() { var x = ; }\n"
        "    void g() { var y = 1 + ; }\n"
        "}\n"
        "class C : B {\n"
        "    [foo(\"x\")] int n;\n"
        "    void h() { var z = ; }\n"
        "}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.cpp.empty());
    std::vector<std::string> prefixes;
    std::istringstream in(r.diagnostics);
    for (std::string line; std::getline(in, line);) {
        const size_t e = line.find(": error:");
        ASSERT_NE(e, std::string::npos) << r.diagnostics;
        prefixes.push_back(line.substr(0, e));
    }
    EXPECT_EQ(prefixes, (std::vector<std::string>{"t.spp:2:24", "t.spp:3:28", "t.spp:6:6",
                                                  "t.spp:7:24"}))
        << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("unknown attribute 'foo'"), std::string::npos) << r.diagnostics;
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
    for (const char* text : scripts) {
        const sppc::TranslateResult r = translate_text(text);
        EXPECT_FALSE(r.ok) << text;
        EXPECT_EQ(count_errors(r.diagnostics), 1) << text << "\n" << r.diagnostics;
    }
}

TEST(TranslateDiagnostics, StrayControlByteHasNoFoundText)
{
    const sppc::TranslateResult r = translate_text(std::string("\0abc\n", 5));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.diagnostics, "t.spp:1:1: error: expected 'using', 'class', 'struct', 'enum' or a variable\n") << r.diagnostics;
}

TEST(TranslateDiagnostics, DeepNestingGivesOneError)
{
    const std::string deep(300, '{');
    const sppc::TranslateResult r = translate_text("class A : B {\n    void f() " + deep +
                                                  std::string(300, '}') + "\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(count_errors(r.diagnostics), 1) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("maximum recursion depth exceeded"), std::string::npos)
        << r.diagnostics;
}

TEST(TranslateDiagnostics, BadImportKeepsItsText)
{
    const sppc::TranslateResult r = translate_text("using 5;\nclass A : B { }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("using needs"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("t.spp:1:7: error:"), std::string::npos) << r.diagnostics;
}

TEST(TranslateDiagnostics, ImportIsRemoved)
{
    const sppc::TranslateResult r = translate_text("import \"x.h\";\nclass A : B { }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("'import' is removed; write 'using'"), std::string::npos)
        << r.diagnostics;
}

TEST(TranslateDiagnostics, UnterminatedBraceEndsWithAnError)
{
    const sppc::TranslateResult r = translate_text("class A : B {\n    void f() {\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("unexpected end of file"), std::string::npos) << r.diagnostics;
}

TEST(TranslateDiagnostics, UnterminatedCommentEndsWithAnError)
{
    const sppc::TranslateResult r = translate_text("class A : B { }\n/* open\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("unterminated comment"), std::string::npos) << r.diagnostics;
    const sppc::TranslateResult r2 = translate_text("class A : B { void f() { /* open }\n");
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
        "    override void Update(float dt) { /* \xE9 */ const char* t = \"na\xE9\"; t; }\n}\n";
    const sppc::TranslateResult r = sppc::translate(temp_options(dir, script));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("\"na\xE9\""), std::string::npos) << r.cpp;
}

TEST(TranslateDiagnostics, DoubleColonIsNotAField)
{
    const sppc::TranslateResult r = translate_text("class A : B {\n    x::y : int;\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("t.spp:2:6: error: use '.' instead of '::'"),
              std::string::npos)
        << r.diagnostics;
}

TEST(TranslateDiagnostics, BacktickInATypeIsAnError)
{
    const sppc::TranslateResult r = translate_text("class A : B {\n    Foo`a` x;\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("string interpolation is not supported"), std::string::npos)
        << r.diagnostics;
}

// A script cut at any byte must end with a result and not hang. A failure has a text.
TEST(TranslateDiagnostics, TruncatedInputsTerminate)
{
    // The parser runs on every prefix (cut at each byte). A prefix that parses runs the whole
    // translator and libclang (about 15 ms), so the full translation uses each 16th cut only.
    const auto start = Clock::now();
    size_t first = 0;
    for (const char* base : {"player", "native_std", "timer"}) {
        for (int gen = 1; gen <= 2; ++gen, ++first) {
            const std::string file = std::string(base) + "_v" + std::to_string(gen) + ".spp";
            const sppc::TranslateOptions full = engine_options(file.c_str(), gen);
            sppc::TranslatorSession session;  // the header parse is cached
            for (size_t cut = 0; cut <= full.script_text.size(); ++cut) {
                const std::string text = full.script_text.substr(0, cut);
                const sppc::LineMap map(text);
                sppc::Diagnostics diag;
                (void)sppc::parse_script(text, map, diag);
                if (cut % 16 != first * 3 % 16) continue;
                sppc::TranslateOptions o = full;
                o.script_text = text;
                const sppc::TranslateResult r = sppc::translate(o, session);
                if (!r.ok) {
                    ASSERT_FALSE(r.diagnostics.empty()) << file << " cut at " << cut;
                }
            }
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    EXPECT_LT(ms.count(), 5000);
}

// A non-string attribute argument on a native stub gives one error and no second error.
TEST(TranslateDiagnostics, NonStringStubAttributeArgumentGivesOneError)
{
    for (const char* stub : {"[native, handle(x)] class Weapon;\n", "[native, handle(1)] class Weapon;\n",
                             "[native, handle(\"std::shared_ptr<$T>\"), intrusive(x, \"R\")] class Weapon;\n",
                             "[native, intrusive(\"A\", 2)] class Weapon;\n"}) {
        const sppc::TranslateResult r =
            translate_text(std::string(stub) + "class A : B {\n    void f() { }\n}\n");
        EXPECT_FALSE(r.ok);
        EXPECT_EQ(count_errors(r.diagnostics), 1) << stub << r.diagnostics;
    }
}
