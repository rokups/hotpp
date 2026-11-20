// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the script formatter (src/ls/format.h). The safety tests run on every example script.
#include "format.h"
#include "hot/hotc_lib.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using hot::ls::BraceStyle;
using hot::ls::format_hot;
using hot::ls::FormatOptions;

namespace
{

std::string read_file(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// All the example scripts: tests/scripts/*.hot and tests/golden/**/*.hot.
std::vector<fs::path> example_scripts()
{
    std::vector<fs::path> out;
    const fs::path root = HOT_SOURCE_DIR;
    for (const char* dir : {"tests/scripts", "tests/golden"})
        for (const auto& e : fs::recursive_directory_iterator(root / dir))
            if (e.is_regular_file() && e.path().extension() == ".hot")
                out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

std::string without_space(const std::string& s)
{
    std::string r;
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c)))
            r += c;
    return r;
}

// What a user sees of a script: the result, the syntax errors and the members. The line and column go away.
std::string facts_of(const std::string& text, const fs::path& file, hotc::TranslatorSession& session)
{
    const fs::path root = HOT_SOURCE_DIR;
    hotc::TranslateOptions o;
    o.script_name = file.string();
    o.script_text = text;
    o.generation = 1;
    o.include_dirs = {(root / "tests/engine").string(), (root / "include").string()};
    const hotc::AnalyzeResult r = hotc::analyze(text, o, session);
    std::string s = r.ok ? "ok\n" : "failed\n";
    for (const auto& e : r.errors)
        s += "error " + e.message + "\n";
    for (const auto& c : r.classes)
    {
        s += "class " + c.name + " : " + c.base + "\n";
        for (const auto& m : c.members)
            s += "  " + m.kind + " " + m.name + "\n";
    }
    // Only the "line:column" pairs go away, because the formatter can remove a blank line.
    static const std::regex position(":[0-9]+:[0-9]+");
    s += std::regex_replace(r.diagnostics, position, ":");
    return s;
}

} // namespace

TEST(Formatter, SpacesAroundBinaryOperatorsAndAfterCommas)
{
    EXPECT_EQ(format_hot("class A {\nint x=1+2*3;\nbool b = x==2&&y!=3||z<=4;\nx+=1;\nf(a,b ,c);\n}\n"),
        "class A {\n    int x = 1 + 2 * 3;\n    bool b = x == 2 && y != 3 || z <= 4;\n    x += 1;\n    f(a, b, "
        "c);\n}\n");
    // A unary operator takes no space. A binary one takes it.
    EXPECT_EQ(format_hot("x = - y;\nz = a-b;\nw = a - -b;\nq = !  p;\ni ++;\n"),
        "x = -y;\nz = a - b;\nw = a - -b;\nq = !p;\ni++;\n");
}

TEST(Formatter, PostfixBangTakesNoSpaceAndNotEqualIsUnchanged)
{
    EXPECT_EQ(format_hot("x = a!.b();\ny = f()!.m;\nz = a![i];\nw = a!=b;\nv = a != b;\nu = a!.b!.c;\nt = !a;\n"),
        "x = a!.b();\ny = f()!.m;\nz = a![i];\nw = a != b;\nv = a != b;\nu = a!.b!.c;\nt = !a;\n");
}

TEST(Formatter, KeywordsTakeASpaceBeforeParenAndCallsTakeNone)
{
    EXPECT_EQ(
        format_hot(
            "if(a) {}\nwhile(b){}\nfor(int i=0;i<n;i++){}\nswitch(c){}\ntry{}catch(E e){}\nfoo (1);\nbar( 1 , 2 );\n"),
        "if (a) {}\nwhile (b) {}\nfor (int i = 0; i<n; i++) {}\nswitch (c) {}\ntry {} catch (E e) {}\nfoo(1);\nbar(1, "
        "2);\n");
    EXPECT_EQ(format_hot("Money operator +(in Money o) {}\nbool operator==(in Money o) {}\n"),
        "Money operator+(in Money o) {}\nbool operator==(in Money o) {}\n");
    EXPECT_EQ(format_hot("for(;;){}\nx[ 1 ] = y [2];\n"), "for (;;) {}\nx[1] = y[2];\n");
}

TEST(Formatter, IndentFollowsBracesAndContinuationLinesGetOneMoreLevel)
{
    const std::string messy =
        "class A {\n"
        "void f(int a,\n"
        "int b) {\n"
        "call(x,\n"
        "y,\n"
        "z);\n"
        "call(x, () => {\n"
        "body();\n"
        "});\n"
        "int v = a +\n"
        "b;\n"
        "if (a)\n"
        "b();\n"
        "else\n"
        "c();\n"
        "switch (m) {\n"
        "case 1:\n"
        "foo();\n"
        "break;\n"
        "default: return;\n"
        "}\n"
        "}\n"
        "}\n";
    const std::string good =
        "class A {\n"
        "    void f(int a,\n"
        "        int b) {\n"
        "        call(x,\n"
        "            y,\n"
        "            z);\n"
        "        call(x, () => {\n"
        "            body();\n"
        "        });\n"
        "        int v = a +\n"
        "            b;\n"
        "        if (a)\n"
        "            b();\n"
        "        else\n"
        "            c();\n"
        "        switch (m) {\n"
        "            case 1:\n"
        "                foo();\n"
        "                break;\n"
        "            default: return;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(format_hot(messy), good);
    FormatOptions tabs;
    tabs.use_tabs = true;
    EXPECT_EQ(format_hot("class A {\nint x;\n}\n", tabs), "class A {\n\tint x;\n}\n");
    FormatOptions two;
    two.indent_width = 2;
    EXPECT_EQ(format_hot("class A {\nint x;\n}\n", two), "class A {\n  int x;\n}\n");
}

TEST(Formatter, KeepsTheBracesCommentsAndStringsOfTheAuthor)
{
    // An Allman brace stays on its own line. A comment keeps its place. A string keeps every byte.
    const std::string in =
        "class A\n"
        "{\n"
        "      string s = \"a  ,  b {\"  +  'x'; // c  {\n"
        "   /* block\n"
        "      { stays  */\n"
        "string t = \"v=${ f( a,b ) }  \\\" {\";\n"
        "}\n";
    const std::string out =
        "class A\n"
        "{\n"
        "    string s = \"a  ,  b {\" + 'x'; // c  {\n"
        "    /* block\n"
        "      { stays  */\n"
        "    string t = \"v=${ f( a,b ) }  \\\" {\";\n"
        "}\n";
    EXPECT_EQ(format_hot(in), out);
    // A string with a new line inside "${...}" is not changed.
    const std::string multi = "string t = \"a${ f(\n   x ,y) }b\";\n";
    EXPECT_EQ(format_hot(multi), multi);
}

TEST(Formatter, CollapsesBlankLinesAndTrimsTheEnds)
{
    EXPECT_EQ(format_hot("\n\nint a;   \n\n\n\n  int b;\t\n\n\n"), "int a;\n\nint b;\n");
    EXPECT_EQ(format_hot("int a;"), "int a;\n");
    EXPECT_EQ(format_hot("   \n\n"), "");
    EXPECT_EQ(format_hot("// c   \nint a; //  d  "), "// c\nint a; //  d\n");
}

TEST(Formatter, KeepsTheLineEndOfTheInput)
{
    EXPECT_EQ(format_hot("class A {\r\nint x=1;\r\n\r\n\r\n}"), "class A {\r\n    int x = 1;\r\n\r\n}\r\n");
}

TEST(Formatter, LeavesUnknownSequencesAsTheyAre)
{
    // "<", ">" and "*" can be a generic or a pointer: the spacing of the author stays.
    EXPECT_EQ(format_hot("Map<int,List<int>> m;\nVec3* p = &q;\nVec3 *r;\n"),
        "Map<int, List<int>> m;\nVec3* p = &q;\nVec3 *r;\n");
}

namespace
{

// The format of the text must be idempotent and must keep the tokens, also for a broken text.
void expect_stable(const std::string& text)
{
    const std::string once = format_hot(text);
    EXPECT_EQ(format_hot(once), once) << "not idempotent: " << text;
    EXPECT_EQ(hot::ls::format_tokens(once), hot::ls::format_tokens(text)) << "tokens changed: " << text;
}

} // namespace

TEST(Formatter, BlockBelowABracelessHeadTakesTheLevelOfItsOwnLine)
{
    const std::string expected = "class A {\n"
                                 "    void f() {\n"
                                 "        if (a)\n"
                                 "            for (;;) {\n"
                                 "                d();\n"
                                 "            }\n"
                                 "        e();\n"
                                 "    }\n"
                                 "}\n";
    const std::string once = format_hot("class A {\n"
                                        "    void f() {\n"
                                        "        if (a)\n"
                                        "            for (;;) {\n"
                                        "                d();\n"
                                        "            }\n"
                                        "        e();\n"
                                        "    }\n"
                                        "}\n");
    EXPECT_EQ(once, expected);
    EXPECT_EQ(format_hot(once), once);
    // A brace on its own line below the head.
    const std::string own = format_hot("class A {\n"
                                       "    void f() {\n"
                                       "        while (a)\n"
                                       "            if (b)\n"
                                       "            {\n"
                                       "                d();\n"
                                       "            }\n"
                                       "    }\n"
                                       "}\n");
    EXPECT_EQ(own, "class A {\n"
                   "    void f() {\n"
                   "        while (a)\n"
                   "            if (b)\n"
                   "            {\n"
                   "                d();\n"
                   "            }\n"
                   "    }\n"
                   "}\n");
    EXPECT_EQ(format_hot(own), own);
}

TEST(Formatter, StatementsOfABlockKeepTheLevelOfTheBlock)
{
    // The lines after "Combatant c = Combatant();" have too little indent. They go to the level of the block.
    EXPECT_EQ(format_hot("class A {\n"
                         "    void f() {\n"
                         "        if (a) {\n"
                         "            Combatant c = Combatant();\n"
                         "        c.node = 1;\n"
                         "        c.slot = 2;\n"
                         "            d();\n"
                         "        }\n"
                         "    }\n"
                         "}\n"),
        "class A {\n"
        "    void f() {\n"
        "        if (a) {\n"
        "            Combatant c = Combatant();\n"
        "            c.node = 1;\n"
        "            c.slot = 2;\n"
        "            d();\n"
        "        }\n"
        "    }\n"
        "}\n");
    // The same after a lambda statement and after a lambda in the arguments of a call.
    EXPECT_EQ(format_hot("class A {\n"
                         "    void f() {\n"
                         "        var g = (T x) => {\n"
                         "        h();\n"
                         "        };\n"
                         "      a();\n"
                         "      b();\n"
                         "        s.Subscribe(r,\n"
                         "        (int q) => {\n"
                         "        body();\n"
                         "        });\n"
                         "      c();\n"
                         "      d();\n"
                         "    }\n"
                         "}\n"),
        "class A {\n"
        "    void f() {\n"
        "        var g = (T x) => {\n"
        "            h();\n"
        "        };\n"
        "        a();\n"
        "        b();\n"
        "        s.Subscribe(r,\n"
        "            (int q) => {\n"
        "                body();\n"
        "            });\n"
        "        c();\n"
        "        d();\n"
        "    }\n"
        "}\n");
    // Two closing braces on one line: the line has the level of the first one.
    EXPECT_EQ(format_hot("class A {\n    void f() {\n        g();\n}}\n"), "class A {\n    void f() {\n        g();\n    } }\n");
    expect_stable("class A {\n"
                         "    void f() {\n"
                         "        if (a) {\n"
                         "            Combatant c = Combatant();\n"
                         "        c.node = 1;\n"
                         "        c.slot = 2;\n"
                         "            d();\n"
                         "        }\n"
                         "    }\n"
                         "}\n");
    expect_stable("class A {\n"
        "    void f() {\n"
        "        if (a) {\n"
        "            Combatant c = Combatant();\n"
        "            c.node = 1;\n"
        "            c.slot = 2;\n"
        "            d();\n"
        "        }\n"
        "    }\n"
        "}\n");
    expect_stable("class A {\n"
                         "    void f() {\n"
                         "        var g = (T x) => {\n"
                         "        h();\n"
                         "        };\n"
                         "      a();\n"
                         "      b();\n"
                         "        s.Subscribe(r,\n"
                         "        (int q) => {\n"
                         "        body();\n"
                         "        });\n"
                         "      c();\n"
                         "      d();\n"
                         "    }\n"
                         "}\n");
    expect_stable("class A {\n"
        "    void f() {\n"
        "        var g = (T x) => {\n"
        "            h();\n"
        "        };\n"
        "        a();\n"
        "        b();\n"
        "        s.Subscribe(r,\n"
        "            (int q) => {\n"
        "                body();\n"
        "            });\n"
        "        c();\n"
        "        d();\n"
        "    }\n"
        "}\n");
    expect_stable("class A {\n    void f() {\n        g();\n}}\n");
}

TEST(Formatter, ClosingBraceOfALambdaInArgumentsHasTheLevelOfItsLine)
{
    const auto check = [](const std::string& in, const std::string& want, const FormatOptions& o = FormatOptions())
    {
        EXPECT_EQ(format_hot(in, o), want);
        expect_stable(in);
        expect_stable(want);
    };
    const std::string head = "class A {\n    void f() {\n";
    // The "}" of the lambda body has the level of "foo(", also when the line goes on.
    check(head + "        foo(x, () => {\n            b();\n            }, 1);\n    }\n}\n",
        head + "        foo(x, () => {\n            b();\n        }, 1);\n    }\n}\n");
    check(head + "        foo(x, () => {\n        b();\n            });\n    }\n}\n",
        head + "        foo(x, () => {\n            b();\n        });\n    }\n}\n");
    check(head + "        foo(() => {\n            b();\n        }).x();\n        c([() => {\n        d();\n            }].x);\n    }\n}\n",
        head + "        foo(() => {\n            b();\n        }).x();\n        c([() => {\n            d();\n        }].x);\n    }\n}\n");
    // A nested lambda.
    check(head + "        foo(a, () => {\n        bar(() => {\n        b();\n        }, 2);\n        }, 1);\n    }\n}\n",
        head + "        foo(a, () => {\n            bar(() => {\n                b();\n            }, 2);\n        }, 1);\n    }\n}\n");
    // The lambda starts on its own line.
    check(head + "        s.Subscribe(r,\n        (int q) => {\n        body();\n        }, 1);\n    }\n}\n",
        head + "        s.Subscribe(r,\n            (int q) => {\n                body();\n            }, 1);\n    }\n}\n");
    // A normal block, else and lists keep the old rule.
    check("class A {\n    void f() {\n        if (a) {\n        b();\n            } else {\n        c();\n        }\n    }\n}\n",
        "class A {\n    void f() {\n        if (a) {\n            b();\n        } else {\n            c();\n        }\n    }\n}\n");
    check(head + "        var l = new[] {\n        { 1 },\n            }, {\n        2 };\n        var m = [\n        { 1 },\n        { 2 }];\n    }\n}\n",
        head + "        var l = new[] {\n            { 1 },\n        }, {\n            2 };\n        var m = [\n            { 1 },\n            { 2 }];\n    }\n}\n");
    // Tabs.
    FormatOptions tabs;
    tabs.use_tabs = true;
    check("class A {\n\tvoid f() {\n\t\tfoo(x, () => {\n\t\t\tb();\n\t\t\t}, 1);\n\t}\n}\n",
        "class A {\n\tvoid f() {\n\t\tfoo(x, () => {\n\t\t\tb();\n\t\t}, 1);\n\t}\n}\n", tabs);
}

TEST(Formatter, TheTokenListSeesAChangeOfTheTokens)
{
    using V = std::vector<std::string>;
    // "a - -b" and "a--b" differ in the tokens, and "a b" and "ab" too.
    EXPECT_NE(hot::ls::format_tokens("a - -b"), hot::ls::format_tokens("a--b"));
    EXPECT_NE(hot::ls::format_tokens("a b"), hot::ls::format_tokens("ab"));
    EXPECT_EQ(
        hot::ls::format_tokens("x = \"a  b\" /* c  */ ; // d  "), (V{"x", "=", "\"a  b\"", "/* c  */", ";", "// d"}));
    // The formatter keeps the space between two operators that would join.
    EXPECT_EQ(format_hot("w = a - -b;\nv = a + +b;\nu = - -c;\nt = a - --b;\n"),
        "w = a - -b;\nv = a + +b;\nu = - -c;\nt = a - --b;\n");
}

TEST(Formatter, BrokenTextIsIdempotent)
{
    expect_stable("/* abc");
    expect_stable("/* abc   \n\n  \n");
    expect_stable("int a; /* abc  \n  x\n\n");
    expect_stable("\"abc");
    expect_stable("s = \"abc   \nint b;");
    expect_stable("s = \"a${ b");
    expect_stable("s = \"a${ ' x \n int b;\n");
    expect_stable("s = \"a${ \"b${ c \" } d");
    expect_stable("c = 'x");
    expect_stable("R\"d( abc");
    expect_stable("x = R\"(a)\" + R\"zz(b)zz\";");
    expect_stable("class A { void f( {{{{ ((( [[[ )))");
    expect_stable("}}} ))) ]]] class A {");
    // The text of an unterminated block comment keeps one new line at the end.
    EXPECT_EQ(format_hot("/* abc   \n\n"), "/* abc\n");
    // Many unmatched openers are fast. The test would time out on a quadratic scan.
    std::string many, deep;
    for (int i = 0; i < 20000; ++i)
    {
        many += "s = \"a${ ;\n";
        deep += "\"${{{{{{{{";
    }
    expect_stable(many);
    expect_stable(deep);
    std::string raw;
    for (int i = 0; i < 20000; ++i)
        raw += "R\"a" + std::to_string(i % 10) + "( x\n";
    expect_stable(raw);
}

TEST(Formatter, LineMapTellsWhereTheLinesGo)
{
    // A run of blank lines keeps the first one only. The other ones have -1.
    auto r = hot::ls::format_hot_map("a;\n\n\n\nb;\n");
    EXPECT_EQ(r.text, "a;\n\nb;\n");
    EXPECT_EQ(r.line_map, (std::vector<int>{0, 1, -1, -1, 2, -1}));
    // The blank lines before the first token go away.
    r = hot::ls::format_hot_map("\n\nx;\n");
    EXPECT_EQ(r.line_map, (std::vector<int>{-1, -1, 0, -1}));
    // The lines of a block comment and of a string stay in a row.
    r = hot::ls::format_hot_map("class A {\n   /* a\n  b\n\n\n c */\nint x;\n}");
    EXPECT_EQ(r.text, "class A {\n    /* a\n  b\n\n\n c */\n    int x;\n}\n");
    EXPECT_EQ(r.line_map, (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
    r = hot::ls::format_hot_map("x = \"a${ f(\n\n\n y) }\";\n\n\n\ny;");
    EXPECT_EQ(r.line_map, (std::vector<int>{0, 1, 2, 3, 4, -1, -1, 5}));
}

TEST(Formatter, LineEndsAndTabsOfTheInput)
{
    // A lone CR is white space. It is not a line end.
    EXPECT_EQ(format_hot("int a;\rint b;\r"), "int a; int b;\n");
    EXPECT_EQ(format_hot("x;\r// c\r"), "x; // c\n");
    // The majority of the line ends decides. The new lines that the formatter writes use it.
    EXPECT_EQ(format_hot("a;\r\nb;\nc;\n"), "a;\nb;\nc;\n");
    EXPECT_EQ(format_hot("a;\r\nb;\r\nc;\n"), "a;\r\nb;\r\nc;\r\n");
    expect_stable("a;\r\nb;\nc;\r\n\n\r\n\r\nd;");
    // Tabs and spaces in the indent and between tokens.
    EXPECT_EQ(format_hot("class A {\n\tint x;\n \t  int\ty\t=\t1;\n}\n"), "class A {\n    int x;\n    int y = 1;\n}\n");
    // The white space before a comment stays as it is.
    EXPECT_EQ(format_hot("x;\t// c\ny;  /* d */  z;\n"), "x;\t// c\ny;  /* d */  z;\n");
    FormatOptions tabs;
    tabs.use_tabs = true;
    EXPECT_EQ(format_hot("class A {\n    int x;\n  \t  int y;\n}\n", tabs), "class A {\n\tint x;\n\tint y;\n}\n");
}

TEST(Formatter, IsIdempotentAndKeepsTheTokensOfEveryExampleScript)
{
    const std::vector<fs::path> files = example_scripts();
    ASSERT_GT(files.size(), 100u);
    for (const fs::path& f : files)
    {
        const std::string text = read_file(f);
        const std::string once = format_hot(text);
        EXPECT_EQ(format_hot(once), once) << "not idempotent: " << f;
        EXPECT_EQ(without_space(once), without_space(text)) << "characters changed: " << f;
        EXPECT_EQ(hot::ls::format_tokens(once), hot::ls::format_tokens(text)) << "tokens changed: " << f;
        EXPECT_FALSE(once.empty()) << f;
        EXPECT_EQ(once.back(), '\n') << f;
        FormatOptions tabs;
        tabs.use_tabs = true;
        const std::string t = format_hot(text, tabs);
        EXPECT_EQ(format_hot(t, tabs), t) << "tabs: not idempotent: " << f;
        EXPECT_EQ(without_space(t), without_space(text)) << f;
        EXPECT_EQ(hot::ls::format_tokens(t), hot::ls::format_tokens(text)) << "tabs: tokens changed: " << f;
        for (const BraceStyle b : {BraceStyle::SameLine, BraceStyle::NextLine})
        {
            FormatOptions o;
            o.brace = b;
            const std::string m = format_hot(text, o);
            EXPECT_EQ(format_hot(m, o), m) << "brace " << int(b) << ": not idempotent: " << f;
            EXPECT_EQ(without_space(m), without_space(text)) << "brace " << int(b) << ": " << f;
            EXPECT_EQ(hot::ls::format_tokens(m), hot::ls::format_tokens(text))
                << "brace " << int(b) << ": tokens changed: " << f;
        }
    }
}

TEST(Formatter, TheFormattedExampleScriptsAnalyzeTheSame)
{
    hotc::TranslatorSession session;
    size_t same = 0;
    for (const fs::path& f : example_scripts())
    {
        const std::string text = read_file(f);
        const std::string once = format_hot(text);
        const std::string facts = facts_of(text, f, session);
        EXPECT_EQ(facts_of(once, f, session), facts) << "result changed: " << f;
        for (const BraceStyle b : {BraceStyle::SameLine, BraceStyle::NextLine})
        {
            FormatOptions o;
            o.brace = b;
            EXPECT_EQ(facts_of(format_hot(text, o), f, session), facts) << "brace " << int(b) << ": " << f;
        }
        ++same;
    }
    EXPECT_GT(same, 100u);
}

namespace
{

std::string with_brace(const std::string& text, BraceStyle b)
{
    FormatOptions o;
    o.brace = b;
    return format_hot(text, o);
}

// "same" has the "{" at the end of the head line, "next" has it on its own line. Each style gives
// its form from both forms, a second format changes nothing, and Keep leaves both as they are.
void expect_brace_forms(const std::string& same, const std::string& next)
{
    EXPECT_EQ(with_brace(next, BraceStyle::SameLine), same);
    EXPECT_EQ(with_brace(same, BraceStyle::SameLine), same);
    EXPECT_EQ(with_brace(same, BraceStyle::NextLine), next);
    EXPECT_EQ(with_brace(next, BraceStyle::NextLine), next);
    EXPECT_EQ(format_hot(same), same);
    EXPECT_EQ(format_hot(next), next);
}

// The brace style does not move any brace of the text.
void expect_braces_stay(const std::string& text)
{
    const std::string kept = format_hot(text);
    EXPECT_EQ(with_brace(text, BraceStyle::SameLine), kept) << text;
    EXPECT_EQ(with_brace(text, BraceStyle::NextLine), kept) << text;
}

} // namespace

TEST(Formatter, BraceStyleMovesTheBraceOfEachBlockHead)
{
    expect_brace_forms("namespace N {\n"
                       "    class A : B {\n"
                       "        int P {\n"
                       "            get {\n"
                       "                return 1;\n"
                       "            }\n"
                       "            set {\n"
                       "                x = value;\n"
                       "            }\n"
                       "        }\n"
                       "        void f(int a) {\n"
                       "            g();\n"
                       "        }\n"
                       "        class Inner {\n"
                       "            int y;\n"
                       "        }\n"
                       "    }\n"
                       "    struct S {\n"
                       "        int x;\n"
                       "    }\n"
                       "    enum E {\n"
                       "        One,\n"
                       "        Two\n"
                       "    }\n"
                       "    class L<T> : List<T> {\n"
                       "        int z;\n"
                       "    }\n"
                       "}\n",
        "namespace N\n"
        "{\n"
        "    class A : B\n"
        "    {\n"
        "        int P\n"
        "        {\n"
        "            get\n"
        "            {\n"
        "                return 1;\n"
        "            }\n"
        "            set\n"
        "            {\n"
        "                x = value;\n"
        "            }\n"
        "        }\n"
        "        void f(int a)\n"
        "        {\n"
        "            g();\n"
        "        }\n"
        "        class Inner\n"
        "        {\n"
        "            int y;\n"
        "        }\n"
        "    }\n"
        "    struct S\n"
        "    {\n"
        "        int x;\n"
        "    }\n"
        "    enum E\n"
        "    {\n"
        "        One,\n"
        "        Two\n"
        "    }\n"
        "    class L<T> : List<T>\n"
        "    {\n"
        "        int z;\n"
        "    }\n"
        "}\n");
}

TEST(Formatter, BraceStyleMovesTheBraceOfEachStatement)
{
    // The lexer reads ">>" as two ">": the "{" after a nested generic moves.
    expect_brace_forms("class C<List<T>> {\n    int x;\n}\n", "class C<List<T>>\n{\n    int x;\n}\n");
    expect_brace_forms("void f() {\n"
                       "    if (a > 0) {\n"
                       "        g();\n"
                       "    } else if (a < 0) {\n"
                       "        h();\n"
                       "    } else {\n"
                       "        k();\n"
                       "    }\n"
                       "    for (int i = 0; i < 3; ++i) {\n"
                       "        g();\n"
                       "    }\n"
                       "    foreach (var x in xs) {\n"
                       "        g();\n"
                       "    }\n"
                       "    while (a) {\n"
                       "        g();\n"
                       "    }\n"
                       "    do {\n"
                       "        g();\n"
                       "    } while (a);\n"
                       "    try {\n"
                       "        g();\n"
                       "    } catch (E e) {\n"
                       "        h();\n"
                       "    } finally {\n"
                       "        k();\n"
                       "    }\n"
                       "    lock (o) {\n"
                       "        g();\n"
                       "    }\n"
                       "    using (var r = R()) {\n"
                       "        g();\n"
                       "    }\n"
                       "    var p = new Pt {\n"
                       "        x = 1\n"
                       "    };\n"
                       "}\n",
        "void f()\n"
        "{\n"
        "    if (a > 0)\n"
        "    {\n"
        "        g();\n"
        "    }\n"
        "    else if (a < 0)\n"
        "    {\n"
        "        h();\n"
        "    }\n"
        "    else\n"
        "    {\n"
        "        k();\n"
        "    }\n"
        "    for (int i = 0; i < 3; ++i)\n"
        "    {\n"
        "        g();\n"
        "    }\n"
        "    foreach (var x in xs)\n"
        "    {\n"
        "        g();\n"
        "    }\n"
        "    while (a)\n"
        "    {\n"
        "        g();\n"
        "    }\n"
        "    do\n"
        "    {\n"
        "        g();\n"
        "    } while (a);\n"
        "    try\n"
        "    {\n"
        "        g();\n"
        "    }\n"
        "    catch (E e)\n"
        "    {\n"
        "        h();\n"
        "    }\n"
        "    finally\n"
        "    {\n"
        "        k();\n"
        "    }\n"
        "    lock (o)\n"
        "    {\n"
        "        g();\n"
        "    }\n"
        "    using (var r = R())\n"
        "    {\n"
        "        g();\n"
        "    }\n"
        "    var p = new Pt\n"
        "    {\n"
        "        x = 1\n"
        "    };\n"
        "}\n");
    // A switch: the "{" of "case 1:" stays.
    expect_brace_forms("switch (a) {\n"
                       "    case 1: {\n"
                       "        g();\n"
                       "        break;\n"
                       "    }\n"
                       "}\n",
        "switch (a)\n"
        "{\n"
        "    case 1: {\n"
        "        g();\n"
        "        break;\n"
        "    }\n"
        "}\n");
    // A block below a head without braces. The brace moves. The level of its "{" and "}" is not
    // right, also with Keep: they get the level of the "if". The level logic is not in this test.
    const std::string body = "if (a)\n    for (;;) {\n        g();\n    }\n";
    const std::string next = with_brace(body, BraceStyle::NextLine);
    const std::string same = with_brace(next, BraceStyle::SameLine);
    EXPECT_NE(next.find("for (;;)\n"), std::string::npos) << next;
    EXPECT_NE(same.find("for (;;) {\n"), std::string::npos) << same;
    EXPECT_EQ(with_brace(next, BraceStyle::NextLine), next);
    EXPECT_EQ(with_brace(same, BraceStyle::SameLine), same);
    EXPECT_EQ(hot::ls::format_tokens(next), hot::ls::format_tokens(body));
}

TEST(Formatter, BraceStyleKeepsOtherBraces)
{
    // A lambda, an initializer, a block on one line, a free block, a brace in a call or in a string.
    expect_braces_stay("var g = (int x) => {\n"
                       "    h();\n"
                       "};\n"
                       "int[] a = {\n"
                       "    1, 2\n"
                       "};\n"
                       "void f() { g(); }\n"
                       "if (a)\n"
                       "{ g(); }\n"
                       "{\n"
                       "    g();\n"
                       "}\n"
                       "s.Run(new Pt {\n"
                       "    x = 1\n"
                       "});\n"
                       "string t = $\"${ new Pt {\n"
                       "x = 1 } }\";\n"
                       "return {\n"
                       "    1\n"
                       "};\n");
    // A comment between the head and the "{" keeps the "{" in its place.
    expect_braces_stay("if (a) // c\n"
                       "{\n"
                       "    g();\n"
                       "}\n"
                       "if (a) /* c */ {\n"
                       "    g();\n"
                       "}\n");
    // A comment between "}" and "else" keeps the "else" in its place.
    EXPECT_EQ(with_brace("if (a)\n{\n    g();\n}\n// c\nelse\n{\n    h();\n}\n", BraceStyle::SameLine),
        "if (a) {\n    g();\n}\n// c\nelse {\n    h();\n}\n");
    // An unmatched "{" stays.
    expect_braces_stay("void f()\n{\n    g();\n");
}

TEST(Formatter, BraceStyleMapsTheLines)
{
    FormatOptions next;
    next.brace = BraceStyle::NextLine;
    auto r = hot::ls::format_hot_map("if (a) {\n    g();\n}\n", next);
    EXPECT_EQ(r.text, "if (a)\n{\n    g();\n}\n");
    EXPECT_EQ(r.line_map, (std::vector<int>{0, 2, 3, -1}));
    FormatOptions same;
    same.brace = BraceStyle::SameLine;
    r = hot::ls::format_hot_map("if (a)\n\n{\n    g();\n}\n", same);
    EXPECT_EQ(r.text, "if (a) {\n    g();\n}\n");
    EXPECT_EQ(r.line_map, (std::vector<int>{0, -1, -1, 1, 2, -1}));
}
