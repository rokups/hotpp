// Test that the parsers accept every method body of the example scripts.
// CMake defines SPP_SOURCE_DIR.
#include <gtest/gtest.h>

#include <string>

#include "parser/parser.h"
#include "parser/statement.h"
#include "support/translator_test_support.h"

using namespace spc;
using translator_test::read_file;

TEST(ExampleScriptParse, ExampleScripts)
{
    const char* names[] = {"player_v1", "player_v2", "timer_v1", "timer_v2", "native_std_v1",
                           "native_std_v2"};
    size_t bodies = 0;
    for (const char* name : names) {
        const std::string text =
            read_file(std::string(SPP_SOURCE_DIR) + "/example/scripts/" + name + ".script");
        ASSERT_FALSE(text.empty()) << name;
        const LineMap map(text);
        Diagnostics sd;
        const ast::Script script = parse_script(text, map, sd);
        ASSERT_EQ(sd.items.size(), 0u) << name << "\n" << sd.format(name);
        for (const ast::ClassDef& c : script.classes) {
            for (const ast::Method& m : c.methods) {
                Diagnostics diag;
                const ast::BlockStmt b = parse_block({text, &map}, m.body_span, diag);
                EXPECT_EQ(diag.items.size(), 0u)
                    << name << " " << c.name << "::" << m.name << "\n" << diag.format(name);
                EXPECT_FALSE(b.stmts.empty()) << name << " " << m.name;
                ++bodies;
            }
        }
    }
    EXPECT_GE(bodies, 10u);
}
