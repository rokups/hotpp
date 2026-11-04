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
