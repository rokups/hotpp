// Tests for the line map and for the format of a diagnostic (src/translator/diag.h).
#include "diag.h"
#include "parser/parser.h"

#include <gtest/gtest.h>

#include <string>

using namespace sppc;

TEST(DiagFormat, LineMapFirstChar)
{
    sppc::LineMap m("ab\ncd");
    EXPECT_EQ(m.line(0), 1);
    EXPECT_EQ(m.col(0), 1);
}

TEST(DiagFormat, LineMapCharAfterNewline)
{
    sppc::LineMap m("ab\ncd");
    EXPECT_EQ(m.line(2), 1); // the newline char
    EXPECT_EQ(m.col(2), 3);
    EXPECT_EQ(m.line(3), 2);
    EXPECT_EQ(m.col(3), 1);
}

TEST(DiagFormat, LineMapLastCharWithAndWithoutTrailingNewline)
{
    sppc::LineMap a("ab\ncd");
    EXPECT_EQ(a.line(4), 2);
    EXPECT_EQ(a.col(4), 2);
    sppc::LineMap b("ab\ncd\n");
    EXPECT_EQ(b.line(4), 2);
    EXPECT_EQ(b.col(4), 2);
    EXPECT_EQ(b.line(5), 2); // the trailing newline
    EXPECT_EQ(b.col(5), 3);
    EXPECT_EQ(b.line(6), 3); // the end of the text
    EXPECT_EQ(b.col(6), 1);
}

TEST(DiagFormat, LineMapEmptyText)
{
    sppc::LineMap m("");
    EXPECT_EQ(m.line(0), 1);
    EXPECT_EQ(m.col(0), 1);
}

TEST(DiagFormat, FormatWithColumn)
{
    sppc::LineMap m("ab\ncd");
    sppc::Diagnostics d;
    d.add(4, m, "bad token");
    EXPECT_EQ(d.format("a.spp"), "a.spp:2:2: error: bad token\n");
}

TEST(DiagFormat, FormatWithoutColumn)
{
    sppc::Diagnostics d;
    d.add_line(7, "bad line");
    d.add_line(9, "second");
    EXPECT_EQ(d.format("a.spp"), "a.spp:7: error: bad line\na.spp:9: error: second\n");
}

TEST(DiagFormat, FormatWithLineZero)
{
    sppc::Diagnostics d;
    d.add_line(0, "no line");
    EXPECT_EQ(d.format("a.spp"), "sppc: error: no line\n");
}
