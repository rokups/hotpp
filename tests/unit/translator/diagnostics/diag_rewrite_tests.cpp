// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the pure function hotc::rewrite_diagnostic (design 4.2, experiment F2).
// The input is Clang text that the tests write by hand. The golden tests with a real
// compile are in tests/integration/backends/diagnostic_mistake_tests.cpp.
#include "hot/hotc_lib.h"

#include <gtest/gtest.h>

#include <string>

using hotc::rewrite_diagnostic;

TEST(DiagRewrite, EmptyTextStaysEmpty)
{
    EXPECT_EQ(rewrite_diagnostic(""), "");
}

TEST(DiagRewrite, TextWithoutDiagnosticStaysAsItIs)
{
    const std::string text = "the compile of x.hot failed\nPlayer__heal is not a diagnostic\n";
    EXPECT_EQ(rewrite_diagnostic(text), text);
}

TEST(DiagRewrite, ShellAndStateNamesBecomeScriptNames)
{
    const std::string in =
        "a.hot:3:5: error: no member named 'x' in 'Boss_Shell'\n"
        "a.hot:4:5: error: no member named 'y' in 'BossState_g12'\n";
    EXPECT_EQ(rewrite_diagnostic(in),
        "a.hot:3:1: error: script class 'Boss' has no member 'x'\n"
        "a.hot:4:1: error: script class 'Boss' has no member 'y'\n");
}

TEST(DiagRewrite, TypoSuggestionStaysInTheMessage)
{
    const std::string in = "a.hot:3:5: error: no member named 'hp' in 'Boss_Shell'; did you mean 'Hp'?\n";
    EXPECT_EQ(rewrite_diagnostic(in), "a.hot:3:1: error: script class 'Boss' has no member 'hp'; did you mean 'Hp'?\n");
}

TEST(DiagRewrite, FunctionNameUsesDotAndKeepsOtherNames)
{
    const std::string in =
        "a.hot:3:5: error: use of undeclared identifier 'Boss__roar'\n"
        "a.hot:4:5: error: no member named '__x' in 'std::__cxx11::list'\n";
    EXPECT_EQ(rewrite_diagnostic(in),
        "a.hot:3:1: error: script class 'Boss' has no method 'roar'\n"
        "a.hot:4:1: error: 'std.__cxx11.list' has no member '__x'\n");
}

TEST(DiagRewrite, VirtualWrapperNameLosesItsSuffix)
{
    const std::string in = "a.hot:3:5: error: no matching function for call to 'Boss__roar_virt'\n";
    EXPECT_EQ(rewrite_diagnostic(in), "a.hot:3:1: error: no method or function 'Boss.roar' fits these arguments\n");
}

// The mangled name of an operator shows as the operator.
TEST(DiagRewrite, OperatorNameShowsAsOperator)
{
    const std::string in = "a.hot:3:5: error: no matching function for call to 'Vec__op_add_assign_virt'\n"
                           "a.hot:4:5: error: no matching function for call to 'Vec__op_add_assign'\n";
    EXPECT_EQ(rewrite_diagnostic(in),
        "a.hot:3:1: error: no method or function 'Vec.operator+=' fits these arguments\n"
        "a.hot:4:1: error: no method or function 'Vec.operator+=' fits these arguments\n");
}

TEST(DiagRewrite, BuiltinTypesAndStdScopeUseScriptSpelling)
{
    const std::string in =
        "a.hot:3:5: error: cannot initialize a variable of type 'uint64_t' (aka 'unsigned long') "
        "with an lvalue of type 'std::string' (aka 'basic_string<char>')\n";
    EXPECT_EQ(rewrite_diagnostic(in),
        "a.hot:3:1: error: cannot initialize a variable of type 'ulong' with a value of type 'std.string'\n");
}

TEST(DiagRewrite, ArgumentCountOfScriptFunctionDoesNotCountSelf)
{
    const std::string in =
        "a.hot:3:5: error: too few arguments to function call, expected 3, have 1\n"
        "a.hot:9:6: note: 'Boss__roar' declared here\n";
    EXPECT_EQ(rewrite_diagnostic(in),
        "a.hot:3:1: error: too few arguments: expected 2, got 0\n"
        "a.hot:9:1: note: 'Boss.roar' declared here\n");
}

TEST(DiagRewrite, ArgumentCountOfNativeFunctionStays)
{
    const std::string in =
        "a.hot:3:5: error: too many arguments to function call, expected 1, have 2\n"
        "Actor.h:24:10: note: 'Move' declared here\n";
    EXPECT_EQ(rewrite_diagnostic(in),
        "a.hot:3:1: error: too many arguments: expected 1, got 2\n"
        "Actor.h:24:10: note: 'Move' declared here\n");
}

TEST(DiagRewrite, ArgumentPositionOfScriptFunctionDoesNotCountSelf)
{
    const std::string in =
        "a.hot:3:5: error: no matching function for call to 'Boss__roar'\n"
        "a.hot:9:13: note: candidate function not viable: no known conversion from 'int' to 'float' for 3rd argument\n"
        "a.hot:9:13: note: candidate function not viable: no known conversion from 'int' to 'Boss_Shell *' for 1st "
        "argument\n";
    EXPECT_EQ(rewrite_diagnostic(in),
        "a.hot:3:1: error: no method or function 'Boss.roar' fits these arguments\n"
        "a.hot:9:1: note: candidate does not fit: the 2nd argument needs 'float', but it has type 'int'\n"
        "a.hot:9:1: note: candidate does not fit: no known conversion from 'int' to 'Boss *' for the object\n");
}

TEST(DiagRewrite, CountsInTheNoteGetPluralWords)
{
    const std::string in =
        "a.hot:3:5: error: no matching function for call to 'Boss__roar'\n"
        "a.hot:9:13: note: candidate function not viable: requires 3 arguments, but 1 was provided\n";
    EXPECT_EQ(rewrite_diagnostic(in),
        "a.hot:3:1: error: no method or function 'Boss.roar' fits these arguments\n"
        "a.hot:9:1: note: candidate does not fit: takes 2 arguments, but 0 were given\n");
}

TEST(DiagRewrite, SnippetOfGeneratedCodeIsDroppedWithoutScriptText)
{
    const std::string in =
        "a.hot:3:5: error: no member named 'x' in 'Boss_Shell'\n"
        "    3 |     __self->x = 1;\n"
        "      |     ~~~~  ^\n"
        "1 error generated.\n";
    EXPECT_EQ(rewrite_diagnostic(in), "a.hot:3:1: error: script class 'Boss' has no member 'x'\n1 error generated.\n");
}

TEST(DiagRewrite, SnippetIsTheScriptLineWhenTheTextIsKnown)
{
    const std::string in =
        "a.hot:2:5: error: no member named 'x' in 'Boss_Shell'\n"
        "    2 |     __self->x = 1;\n"
        "      |     ~~~~  ^\n"
        "1 error generated.\n";
    const hotc::ScriptTexts scripts = {{"a.hot", "class Boss {\n    this.x = 1;\n}\n"}};
    EXPECT_EQ(rewrite_diagnostic(in, scripts),
        "a.hot:2:1: error: script class 'Boss' has no member 'x'\n"
        "    2 |     this.x = 1;\n"
        "1 error generated.\n");
}

TEST(DiagRewrite, SnippetOfNativeHeaderStays)
{
    const std::string in =
        "Actor.h:24:10: error: something is wrong\n"
        "   24 |     void Move(Vec3 delta);\n"
        "      |          ^\n";
    EXPECT_EQ(rewrite_diagnostic(in), in);
}

TEST(DiagRewrite, NoteInGeneratedFileGetsTheLocationOfItsError)
{
    const std::string in =
        "a.hot:3:5: error: no matching function for call to 'Boss__roar'\n"
        "/w/a.g1.cpp:91:13: note: candidate function not viable: requires 2 arguments, but 1 was provided\n"
        "   91 | static void Boss__roar(Boss_Shell* __self, float a)\n"
        "      |             ^\n";
    EXPECT_EQ(rewrite_diagnostic(in),
        "a.hot:3:1: error: no method or function 'Boss.roar' fits these arguments\n"
        "a.hot:3:1: note: candidate does not fit: takes 1 argument, but 0 were given\n");
}

TEST(DiagRewrite, ErrorInGeneratedFileUsesTheLastScriptLineBeforeIt)
{
    const std::string cpp = "#line 1 \"a.hot\"\nint a;\n#line 7 \"a.hot\"\nint b;\n#line 9 \"/w/a.g1.cpp\"\n}\n";
    const std::string in =
        "/w/a.g1.cpp:6:1: error: non-void function does not return a value [-Werror,-Wreturn-type]\n"
        "    6 | }\n"
        "      | ^\n";
    EXPECT_EQ(rewrite_diagnostic(in, {{"a.hot", "x\n"}}, {{"/w/a.g1.cpp", cpp}}),
        "a.hot:7:1: error: the method does not return a value on every path\n");
}

TEST(DiagRewrite, ErrorInGeneratedFileStaysWithoutTheGeneratedText)
{
    const std::string in = "/w/a.g1.cpp:6:1: error: static assertion failed\n    6 | }\n";
    EXPECT_EQ(rewrite_diagnostic(in), "/w/a.g1.cpp:6:1: error: static assertion failed\n");
}

TEST(DiagRewrite, NotesInSystemHeadersAreHiddenAndCounted)
{
    const std::string in =
        "a.hot:3:5: error: invalid operands to binary expression ('float' and 'int *')\n"
        "/usr/include/c++/16/bits/x.h:10:5: note: candidate template ignored\n"
        "   10 |   operator+()\n"
        "      |   ^\n"
        "/usr/lib64/gcc/x/16/include/c++/y.h:11:5: note: candidate template ignored\n"
        "1 error generated.\n";
    EXPECT_EQ(rewrite_diagnostic(in),
        "a.hot:3:1: error: the operator does not fit these operand types ('float' and 'int *')\n"
        "a.hot:3:1: note: 2 notes in system headers hidden (use --raw-diagnostics to see them)\n"
        "1 error generated.\n");
}

TEST(DiagRewrite, WarningFlagIsRemoved)
{
    const std::string in = "a.hot:3:5: warning: unused variable 'x' [-Wunused-variable]\n";
    EXPECT_EQ(rewrite_diagnostic(in), "a.hot:3:1: warning: unused variable 'x'\n");
}

TEST(DiagRewrite, MessageWithColonsInTextStillParses)
{
    const std::string in = "a.hot:3:5: error: unknown type name 'a: b: error: c'\n";
    EXPECT_EQ(rewrite_diagnostic(in),
        "a.hot:3:1: error: unknown type 'a: b: error: c': add a 'using' line or a native stub\n");
}

TEST(DiagRewrite, WindowsPathKeepsItsDrive)
{
    const std::string in = "C:\\x\\Actor.h:24:10: error: something\n";
    EXPECT_EQ(rewrite_diagnostic(in), in);
}

TEST(DiagRewrite, VeryLongMessageDoesNotUseTheRegexTable)
{
    const std::string name(20000, 'a');
    const std::string in = "a.hot:3:5: error: no member named 'x' in '" + name + "'\n";
    // The plain rename runs. The table does not. The call must return.
    EXPECT_EQ(rewrite_diagnostic(in), "a.hot:3:1: error: no member named 'x' in '" + name + "'\n");
}

TEST(DiagRewrite, TextWithoutFinalNewlineKeepsThat)
{
    EXPECT_EQ(rewrite_diagnostic("a.hot:3:5: error: redefinition of 'x'"),
        "a.hot:3:1: error: 'x' is already declared in this scope");
}

TEST(DiagRewrite, RewriteTwiceGivesTheSameText)
{
    const std::string in = "a.hot:3:5: error: no member named 'x' in 'Boss_Shell'\n    3 |   __self->x\n      |   ^\n";
    const std::string once = rewrite_diagnostic(in);
    EXPECT_EQ(rewrite_diagnostic(once), once);
}

// The rewriter with a source map: the column of a script location is the real one.
TEST(DiagRewrite, SourceMapGivesTheScriptColumn)
{
    // Script line 2: "  go(total);" - "total" starts at column 6.
    const std::string script = "class A {\n  go(total);\n}\n";
    const std::string gen = "#line 2 \"a.hot\"\n    go(total);\n";
    hotc::SourceMap map;
    map.add({static_cast<uint32_t>(script.find("total")), 5, static_cast<uint32_t>(gen.find("total")), 5});
    map.build(script, gen);
    // Clang reports the generated column: "total" is at column 8 of the physical line 2.
    const std::string in = "a.hot:2:8: error: use of undeclared identifier 'total'\n";
    EXPECT_EQ(rewrite_diagnostic(in, {{"a.hot", script}}, {{"a.cpp", gen}}, &map),
        "a.hot:2:6: error: unknown name 'total': it is not a local, a parameter, a field or a type\n");
}

TEST(DiagRewrite, WithoutSegmentTheColumnStaysOne)
{
    const std::string script = "class A {\n  go(total);\n}\n";
    const std::string gen = "#line 2 \"a.hot\"\n    go(total);\n";
    hotc::SourceMap map;
    map.add({static_cast<uint32_t>(script.find("total")), 5, static_cast<uint32_t>(gen.find("total")), 5});
    map.build(script, gen);
    // Column 13 is the ")" after the mapped name: no segment at or after it.
    const std::string in = "a.hot:2:13: error: expected ';'\n";
    EXPECT_EQ(rewrite_diagnostic(in, {{"a.hot", script}}, {{"a.cpp", gen}}, &map), "a.hot:2:1: error: expected ';'\n");
    // No map: the column is 1.
    EXPECT_EQ(rewrite_diagnostic("a.hot:2:8: error: expected ';'\n", {{"a.hot", script}}, {{"a.cpp", gen}}),
        "a.hot:2:1: error: expected ';'\n");
}

// A segment of an imported script gives the real column in that script. The map holds the text
// of the script, so the caller does not list it.
TEST(DiagRewrite, SourceMapGivesTheColumnInAnImportedScript)
{
    const std::string script = "using \"b.hot\";\nclass K : B { }\n";
    const std::string imported = "class B {\n  int f() { return total; }\n}\n";
    const std::string gen = "#line 2 \"b.hot\"\n    int f() { return total; }\n";
    hotc::SourceMap map;
    const uint32_t id = map.add_file("b.hot", imported);
    map.add({static_cast<uint32_t>(imported.find("total")), 5, static_cast<uint32_t>(gen.find("total")), 5, id});
    map.build(script, gen);
    const std::string in = "b.hot:2:" + std::to_string(gen.find("total") - gen.rfind('\n', gen.find("total")))
        + ": error: use of undeclared identifier 'total'\n    2 | generated\n      | ^\n";
    EXPECT_EQ(rewrite_diagnostic(in, {{"a.hot", script}}, {{"a.cpp", gen}}, &map),
        "b.hot:2:20: error: unknown name 'total': it is not a local, a parameter, a field or a type\n"
        "    2 |   int f() { return total; }\n");
}

TEST(DiagRewrite, TranslatorDiagnosticGetsSnippetAndCount)
{
    const std::string script = "class A {\n  int x = ;\n}\n";
    EXPECT_EQ(hotc::rewrite_translator_diagnostic("a.hot:2:11: error: expected an expression\n", {{"a.hot", script}}),
        "a.hot:2:11: error: expected an expression\n"
        "    2 |   int x = ;\n"
        "1 error generated.\n");
}

TEST(DiagRewrite, TranslatorDiagnosticKeepsOtherLinesAndCountsAll)
{
    const std::string script = "class A {\n  int x = ;\n}\n";
    const std::string in =
        "a.hot:2:11: error: one\nhotc: error: two\nh.h:3:1: error: three\n"
        "a.hot:1:1: warning: four\n";
    EXPECT_EQ(hotc::rewrite_translator_diagnostic(in, {{"a.hot", script}}),
        "a.hot:2:11: error: one\n"
        "    2 |   int x = ;\n"
        "hotc: error: two\n"
        "h.h:3:1: error: three\n"
        "a.hot:1:1: warning: four\n"
        "    1 | class A {\n"
        "1 warning and 3 errors generated.\n");
    EXPECT_EQ(hotc::rewrite_translator_diagnostic("", {{"a.hot", script}}), "");
}

TEST(DiagRewrite, InTraitOfAPlainParameterBecomesTheType)
{
    const std::string in = "a.hot:3:5: error: no viable conversion from 'int' to 'hot::In<Vec3>'\n";
    EXPECT_EQ(rewrite_diagnostic(in), "a.hot:3:1: error: cannot convert 'int' to 'Vec3'\n");
}

TEST(DiagRewrite, InTraitWithTemplateArgumentsBecomesTheType)
{
    EXPECT_EQ(hotc::rewrite_clang_message("cannot bind 'hot::In<std::vector<int, std::allocator<int>>>' and 'hot::In<Big>'"),
        "cannot bind 'std.vector<int, std.allocator<int>>' and 'Big'");
}

// A value without a formatter. The error is in a library header. The note "requested here" has the
// script position. The rewriter moves the error to that position and hides the library lines.
TEST(DiagRewrite, FmtValueWithoutFormatterMovesToTheScriptLine)
{
    const std::string in =
        "In file included from /w/a.g1.cpp:10:\n"
        "In file included from /usr/include/fmt/format.h:41:\n"
        "/usr/include/fmt/base.h:2260:45: error: implicit instantiation of undefined template "
        "'fmt::detail::type_is_unformattable_for<Weather, char>'\n"
        " 2260 |     type_is_unformattable_for<T, char_type> _;\n"
        "      |                                             ^\n"
        "/usr/include/fmt/base.h:2233:42: note: in instantiation of function template specialization "
        "'fmt::detail::value<fmt::context>::value<Weather, 0>' requested here\n"
        " 2233 |   FMT_CONSTEXPR FMT_INLINE value(T& x) : value(x, custom_tag()) {}\n"
        "a.hot:9:35: note: in instantiation of function template specialization 'fmt::format<Weather>' requested here\n"
        "/usr/include/fmt/base.h:2107:45: note: template is declared here\n"
        "1 error generated.\n";
    const std::string out = rewrite_diagnostic(in);
    EXPECT_NE(out.find("a.hot:9:1: error: 'Weather' cannot be printed in a '${...}' string: it has no formatter"),
        std::string::npos)
        << out;
    EXPECT_EQ(out.find("fmt"), std::string::npos) << out;
    EXPECT_EQ(out.find("In file included"), std::string::npos) << out;
    EXPECT_EQ(out.find("note:"), std::string::npos) << out;
}

// The std library gives several errors for one value. Each error has the same script position.
// The rewriter shows one error.
TEST(DiagRewrite, StdValueWithoutFormatterShowsOneError)
{
    const std::string in =
        "/usr/include/c++/16/format:4829:18: error: static assertion failed due to requirement "
        "'is_default_constructible_v<std::formatter<Weather, char>>': std::formatter must be specialized\n"
        "/usr/include/c++/16/format:4851:12: note: in instantiation of function template specialization 'x' requested "
        "here\n"
        "a.hot:9:35: note: in instantiation of function template specialization 'std::format<Weather &>' requested "
        "here\n"
        "/usr/include/c++/16/concepts:162:30: error: call to deleted constructor of 'std::formatter<Weather>'\n"
        "a.hot:9:35: note: in instantiation of function template specialization 'std::format<Weather &>' requested "
        "here\n"
        "2 errors generated.\n";
    const std::string out = rewrite_diagnostic(in);
    size_t count = 0;
    for (size_t p = out.find("cannot be printed"); p != std::string::npos; p = out.find("cannot be printed", p + 1))
        ++count;
    EXPECT_EQ(count, 1u) << out;
    EXPECT_NE(out.find("'Weather' cannot be printed"), std::string::npos) << out;
}

// An error in a library header with no script note stays as it is.
TEST(DiagRewrite, FormatErrorWithoutScriptNoteIsNotMoved)
{
    const std::string in =
        "/usr/include/fmt/base.h:2260:45: error: implicit instantiation of undefined template "
        "'fmt::detail::type_is_unformattable_for<Weather, char>'\n";
    EXPECT_EQ(rewrite_diagnostic(in).find("cannot be printed"), std::string::npos);
}

// An error in a library header that is not a missing formatter stays as it is, also when a script
// note follows it.
TEST(DiagRewrite, OtherLibraryErrorWithScriptNoteIsNotMoved)
{
    const std::string in =
        "/usr/include/c++/16/vector:100:5: error: a library problem\n"
        "a.hot:9:1: note: requested here\n"
        "1 error generated.\n";
    EXPECT_EQ(rewrite_diagnostic(in), in);
}

// ------------------------------------------------------------------ .cpp scripts
//
// The translator copies the body of a .cpp script into the generated file, with "#line" before
// it. The column of the compiler is then right, and the caret and fix-it lines are right.

namespace
{

// The script of the .cpp tests. Line 10 and line 11 are the lines of the warnings.
const std::string kWarnScript =
    "// line 1\n"
    "// line 2\n"
    "// line 3\n"
    "// line 4\n"
    "// line 5\n"
    "// line 6\n"
    "class Player : public Actor\n"
    "{\n"
    "    void Tick(float dt)\n"
    "        int unused = 1;\n"
    "        if (hp = dt)\n"
    "            hp = 0;\n"
    "        auto f = [this] { return hp; };\n"
    "    }\n"
    "class My_Class\n"
    "{\n"
    "};\n";

// A generated text. The body from line 9 is a copy of the script. The last directive names the
// generated file again, as the translator does.
std::string warn_generated(const std::string& line13 = "        auto f = [this] { return hp; };")
{
    return "#line 9 \"warn.cpp\"\n"
           "    void Tick(float dt)\n"
           "        int unused = 1;\n"
           "        if (hp = dt)\n"
           "            hp = 0;\n"
        + line13
        + "\n"
          "    }\n"
          "#line 8 \"out/warn.g1.cpp\"\n";
}

std::string cpp_rewrite(const std::string& in, const std::string& gen = warn_generated())
{
    return rewrite_diagnostic(in, {{"warn.cpp", kWarnScript}}, {{"out/warn.g1.cpp", gen}}, nullptr);
}

} // namespace

// A warning in a copied line keeps its column, its caret line and its flag.
TEST(DiagRewrite, CppScriptKeepsColumnCaretAndFlag)
{
    const std::string in =
        "warn.cpp:10:13: error: unused variable 'unused' [-Werror,-Wunused-variable]\n"
        "   10 |         int unused = 1;\n"
        "      |             ^~~~~~\n"
        "1 error generated.\n";
    EXPECT_EQ(cpp_rewrite(in), in);
}

// The fix-it lines and the notes stay too.
TEST(DiagRewrite, CppScriptKeepsFixItLines)
{
    const std::string in =
        "warn.cpp:11:16: error: using the result of an assignment as a condition without parentheses "
        "[-Werror,-Wparentheses]\n"
        "   11 |         if (hp = dt)\n"
        "      |                ~~~^~~~\n"
        "warn.cpp:11:16: note: place parentheses around the assignment to silence this warning\n"
        "   11 |         if (hp = dt)\n"
        "      |                ^\n"
        "      |             (       )\n"
        "warn.cpp:11:16: note: use '==' to turn this assignment into an equality comparison\n"
        "   11 |         if (hp = dt)\n"
        "      |                ^\n"
        "      |                ==\n"
        "1 error generated.\n";
    EXPECT_EQ(cpp_rewrite(in), in);
}

// A line that the translator changed: the column is right before the first change only. The caret
// line shows generated code, so it goes.
TEST(DiagRewrite, CppScriptColumnOnRewrittenLine)
{
    const std::string gen = warn_generated("        auto f = [__self] { return state_of(__self)->hp; };");
    const std::string shown = "   13 |         auto f = [__self] { return state_of(__self)->hp; };\n";
    const std::string script_line = "   13 |         auto f = [this] { return hp; };\n";
    EXPECT_EQ(cpp_rewrite("warn.cpp:13:18: error: x\n" + shown + "      |                  ^\n1 error generated.\n", gen),
        "warn.cpp:13:18: error: x\n" + script_line + "1 error generated.\n");
    EXPECT_EQ(cpp_rewrite("warn.cpp:13:36: error: x\n" + shown + "      |                    ^\n1 error generated.\n", gen),
        "warn.cpp:13:1: error: x\n" + script_line + "1 error generated.\n");
}

// An error in the glue of the generated file goes to the last script line. It shows the script line.
TEST(DiagRewrite, CppScriptMapsGeneratedGlueToScriptLine)
{
    const std::string gen =
        "#line 3 \"warn.cpp\"\n"
        "// line 3\n"
        "// line 4\n"
        "#line 5 \"out/warn.g1.cpp\"\n"
        "int glue = Player_Shell().x;\n";
    const std::string in =
        "out/warn.g1.cpp:5:12: error: no member named 'x' in 'Player_Shell'\n"
        "    5 | int glue = Player_Shell().x;\n"
        "      |                           ^\n"
        "1 error generated.\n";
    EXPECT_EQ(cpp_rewrite(in, gen),
        "warn.cpp:3:1: error: no member named 'x' in 'Player'\n"
        "    3 | // line 3\n"
        "1 error generated.\n");
}

// A message of a .cpp script keeps the spelling of C++. Only the generated names change.
TEST(DiagRewrite, CppScriptKeepsCppSpelling)
{
    auto message = [](const std::string& m) { return "warn.cpp:10:1: error: " + m + "\n"; };
    const std::pair<const char*, const char*> cases[] = {
        {"no member named 'x' in 'Player_Shell'", "no member named 'x' in 'Player'"},
        {"no member named 'x' in 'PlayerState_g1'", "no member named 'x' in 'Player'"},
        {"no matching function for call to 'Player__heal'", "no matching function for call to 'Player::heal'"},
        {"no matching function for call to 'Player__heal_virt'", "no matching function for call to 'Player::heal'"},
        {"no matching function for call to 'Player__op_add_assign'",
            "no matching function for call to 'Player::operator+='"},
        {"no matching function for call to 'Player__op_call_virt'", "no matching function for call to 'Player::operator()'"},
        {"call to 'Player__ctor'", "call to 'Player::Player'"},
        {"call to 'Player__dtor'", "call to 'Player::~Player'"},
        {"use of undeclared identifier '__self'", "use of undeclared identifier 'this'"},
        {"use of undeclared identifier '__st'", "use of undeclared identifier 'this'"},
        {"no member named 'hp' in 'state_of(__self)->hp'", "no member named 'hp' in 'this->hp'"},
        {"no member named 'hp' in 'state_of(this)->hp'", "no member named 'hp' in 'this->hp'"},
        {"no viable conversion from 'const char[2]' to 'std::string' (aka 'basic_string<char>')",
            "no viable conversion from 'const char[2]' to 'std::string' (aka 'basic_string<char>')"},
        {"cannot initialize a variable of type 'int32_t' with an lvalue of type 'std::string'",
            "cannot initialize a variable of type 'int32_t' with an lvalue of type 'std::string'"},
        {"unused variable 'x' [-Werror,-Wunused-variable]", "unused variable 'x' [-Werror,-Wunused-variable]"},
        {"std::vector<int>::iterator is not a type", "std::vector<int>::iterator is not a type"},
        // A name of the user stays. The class of a generated name must be a class of the script.
        {"use of undeclared identifier 'my__var'", "use of undeclared identifier 'my__var'"},
        {"use of undeclared identifier 'My_Class__m'", "use of undeclared identifier 'My_Class::m'"},
        {"use of undeclared identifier 'My__Class__m'", "use of undeclared identifier 'My__Class__m'"},
        {"unknown type name 'Foo_Shell'", "unknown type name 'Foo_Shell'"},
        {"unknown type name 'FooState_g3'", "unknown type name 'FooState_g3'"},
        {"use of undeclared identifier 'foo_virt'", "use of undeclared identifier 'foo_virt'"},
    };
    for (const auto& c : cases)
        EXPECT_EQ(cpp_rewrite(message(c.first)), message(c.second));
}

// The same texts for a .hot script keep the old words.
TEST(DiagRewrite, HotScriptOutputDoesNotChange)
{
    const std::string script = "class Boss\n{\n    void Run() { }\n}\n";
    const std::string gen = "#line 3 \"a.hot\"\n    void Run() { }\n";
    const std::string in =
        "a.hot:3:5: error: no member named 'x' in 'Boss_Shell'\n"
        "    3 |     auto Boss__x = hot::deref(p);\n"
        "      |     ^\n"
        "a.hot:3:5: error: cannot convert 'const char[2]' to 'int32_t' (aka 'int') [-Wfoo]\n";
    EXPECT_EQ(rewrite_diagnostic(in, {{"a.hot", script}}, {{"out/a.g1.cpp", gen}}, nullptr),
        "a.hot:3:1: error: script class 'Boss' has no member 'x'\n"
        "    3 |     void Run() { }\n"
        "a.hot:3:1: error: cannot convert 'string' to 'int'\n");
}

// The dialect comes from the file of each diagnostic, not from the first script.
TEST(DiagRewrite, CppFirstScriptDoesNotChangeAHotError)
{
    const std::string hot = "class Boss\n{\n    void Run() { }\n}\n";
    const std::string in = "b.hot:3:5: error: no member named 'x' in 'Boss_Shell'\n";
    EXPECT_EQ(rewrite_diagnostic(in, {{"warn.cpp", kWarnScript}, {"b.hot", hot}}, {}, nullptr),
        "b.hot:3:1: error: script class 'Boss' has no member 'x'\n");
}

TEST(DiagRewrite, HotFirstScriptDoesNotChangeACppError)
{
    const std::string hot = "class Boss\n{\n    void Run() { }\n}\n";
    const std::string in =
        "warn.cpp:10:13: error: unused variable 'unused' [-Werror,-Wunused-variable]\n"
        "   10 |         int unused = 1;\n"
        "      |             ^~~~~~\n"
        "1 error generated.\n";
    EXPECT_EQ(rewrite_diagnostic(in, {{"a.hot", hot}, {"warn.cpp", kWarnScript}},
                  {{"out/warn.g1.cpp", warn_generated()}}, nullptr),
        in);
}

// A "#line" with a ".cpp" name that is a script counts. A ".cpp" name that is not a script does not.
TEST(DiagRewrite, MixedRunMapsGlueToACppScript)
{
    const std::string hot = "class Boss\n{\n    void Run() { }\n}\n";
    const std::string gen =
        "#line 2 \"warn.cpp\"\n"
        "// line 2\n"
        "#line 4 \"other.cpp\"\n"
        "// other\n"
        "#line 5 \"out/a.g1.cpp\"\n"
        "int glue = Player_Shell().x;\n";
    const std::string in =
        "out/a.g1.cpp:6:12: error: no member named 'x' in 'Player_Shell'\n"
        "    6 | int glue = Player_Shell().x;\n"
        "1 error generated.\n";
    EXPECT_EQ(rewrite_diagnostic(in, {{"a.hot", hot}, {"warn.cpp", kWarnScript}}, {{"out/a.g1.cpp", gen}}, nullptr),
        "warn.cpp:2:1: error: no member named 'x' in 'Player'\n"
        "    2 | // line 2\n"
        "1 error generated.\n");
}

// A line beyond the script has no column and no text.
TEST(DiagRewrite, CppScriptLineBeyondTheScript)
{
    const std::string in =
        "warn.cpp:99:3: error: x\n"
        "   99 | foo\n"
        "      |   ^\n"
        "1 error generated.\n";
    EXPECT_EQ(cpp_rewrite(in), "warn.cpp:99:1: error: x\n1 error generated.\n");
}

// No generated text: the column is not known, but the script line shows.
TEST(DiagRewrite, CppScriptWithoutGeneratedText)
{
    const std::string in =
        "warn.cpp:10:13: error: unused variable 'unused' [-Werror,-Wunused-variable]\n"
        "   10 |         int unused = 1;\n"
        "      |             ^~~~~~\n"
        "1 error generated.\n";
    EXPECT_EQ(rewrite_diagnostic(in, {{"warn.cpp", kWarnScript}}, {}, nullptr),
        "warn.cpp:10:1: error: unused variable 'unused' [-Werror,-Wunused-variable]\n"
        "   10 |         int unused = 1;\n"
        "1 error generated.\n");
}

// The caret stays only when the line that the compiler shows is the script line.
TEST(DiagRewrite, CppScriptCaretNeedsTheShownLine)
{
    // The shown line is not a line of the generated text: no caret.
    const std::string in =
        "warn.cpp:10:13: error: x\n"
        "   10 |         something else\n"
        "      |             ^~~~~~\n"
        "1 error generated.\n";
    EXPECT_EQ(cpp_rewrite(in),
        "warn.cpp:10:13: error: x\n"
        "   10 |         int unused = 1;\n"
        "1 error generated.\n");
}
