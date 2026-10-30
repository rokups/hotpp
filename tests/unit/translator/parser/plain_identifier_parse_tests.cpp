// Tests that the old words are plain identifiers, and that "[value] class" is an error.
#include "parser/parser.h"

#include <gtest/gtest.h>

#include <string>

using namespace hotc;

namespace
{

// Parses the script and gives the text of the errors.
std::string errors_of_script(const std::string& text)
{
    const LineMap map(text);
    Diagnostics diag;
    parse_script(text, map, diag);
    return diag.format("t");
}

} // namespace

TEST(PlainIdentifierParse, OldWordsAreFieldNames)
{
    EXPECT_EQ(errors_of_script("class A {\n"
                               "    int let;\n"
                               "    int function = 1;\n"
                               "    std.string import;\n"
                               "    float super = 2.0;\n"
                               "    int of;\n"
                               "}\n"),
        "");
}

TEST(PlainIdentifierParse, OldWordsAreMethodsAndParameters)
{
    EXPECT_EQ(errors_of_script("class A {\n"
                               "    void import(int let, int function) { }\n"
                               "    int super(int of) { return of; }\n"
                               "}\n"),
        "");
}

TEST(PlainIdentifierParse, OldWordsAreLocalsAndExpressions)
{
    EXPECT_EQ(errors_of_script("class A {\n"
                               "    int Run() {\n"
                               "        int let = 1;\n"
                               "        var function = let + 1;\n"
                               "        int import = function * 2;\n"
                               "        int super = import;\n"
                               "        int of = super;\n"
                               "        foreach (int import2 in this.items) { of = import2; }\n"
                               "        return of;\n"
                               "    }\n"
                               "    std.vector<int> items;\n"
                               "}\n"),
        "");
}

TEST(PlainIdentifierParse, OldWordsAreClassNames)
{
    EXPECT_EQ(errors_of_script("class import { }\nstruct let { int function; }\n"), "");
}

TEST(PlainIdentifierParse, ValueAttributeOnAScriptClassIsAnError)
{
    const std::string d = errors_of_script("[value] class Foo { }\n");
    EXPECT_NE(d.find("t:1:2: error: '[value]' is not allowed on a script class: use 'struct'"), std::string::npos) << d;
    // A native stub still takes the attribute.
    EXPECT_EQ(errors_of_script("[native, value] class std.string;\n"), "");
    // A struct is the way to make a value type.
    EXPECT_EQ(errors_of_script("struct Foo { int a; }\n"), "");
}
