// Tests for the parser of the properties: auto accessors, bodies, access letters and errors.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "parser/parser.h"
#include "support/script_parse_test_support.h"

using namespace sppc;
using namespace script_test;

TEST(PropertyParse, Properties)
{
    Diagnostics sd;
    EXPECT_EQ(props_of("class A : B {\n"
                       "    int A1 { get; set; }\n"
                       "    std.string A2 { get; } = \"x\";\n"
                       "    private float A3 { get; protected set; } = 1.0;\n"
                       "    int A4 {\n"
                       "        get { return this.v; }\n"
                       "        private set { this.v = value; f(value); }\n"
                       "    }\n"
                       "    int value = 0;\n"
                       "}\n",
                       sd),
              (std::vector<std::string>{
                  "int32_t|A1|u|get:u:auto|set:u:auto|-",
                  "std::string|A2|u|get:u:auto|-|init",
                  "float|A3|r|get:r:auto|set:o:auto|init",
                  "int32_t|A4|u|get:u:body|set:r:body|-|g1|s2",
              }));
    EXPECT_EQ(sd.items.size(), 0u) << sd.format("t");
}

TEST(PropertyParse, PropertyErrors)
{
    for (const auto& [text, msg] : std::vector<std::pair<std::string, std::string>>{
             {"int N { }", "a property needs a 'get' or a 'set' accessor"},
             {"int N { set; set; }", "duplicate 'set' accessor"},
             {"int N { get { return 1; } set; }", "a property cannot mix auto and custom accessors"},
             {"int N { get { return 1; } } = 2;", "only an auto property can have an initializer"},
             {"int N { get; value; }", "expected 'get' or 'set'"},
             {"N { get; }", "a property needs a type"},
             {"int get;", "'get' is a reserved word"},
             {"int set = 1;", "'set' is a reserved word"},
             {"void get() { }", "'get' is a reserved word"},
             {"void f(int set) { }", "'set' is a reserved word"},
         }) {
        Diagnostics sd;
        (void)props_of("class A : B {\n    " + text + "\n}\n", sd);
        ASSERT_FALSE(sd.items.empty()) << text;
        EXPECT_NE(sd.format("t").find(msg), std::string::npos) << text << ": " << sd.format("t");
    }
}
