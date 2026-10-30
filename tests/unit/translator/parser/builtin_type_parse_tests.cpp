// Tests for the parser of the builtin type names: aliases, canonical types and C++ spellings.
#include "emit.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/statement_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;
using namespace stmt_test;

namespace
{

struct BuiltinRow
{
    const char* script;
    const char* alias; // nullptr when there is no alias
    const char* cpp;
};

const BuiltinRow kBuiltinRows[] = {
    {"bool", nullptr, "bool"},
    {"sbyte", "int8", "int8_t"},
    {"byte", "uint8", "uint8_t"},
    {"short", "int16", "int16_t"},
    {"ushort", "uint16", "uint16_t"},
    {"int", "int32", "int32_t"},
    {"uint", "uint32", "uint32_t"},
    {"long", "int64", "int64_t"},
    {"ulong", "uint64", "uint64_t"},
    {"nint", "intptr", "intptr_t"},
    {"nuint", "uintptr", "uintptr_t"},
    {"float", nullptr, "float"},
    {"double", nullptr, "double"},
    {"char", nullptr, "char"},
    {"wchar", "char16", "char16_t"},
};

} // namespace

TEST(BuiltinTypeParse, NameAndAliasGiveOneCanonicalType)
{
    for (const BuiltinRow& r : kBuiltinRows)
    {
        const std::string want = std::string(r.cpp) + " x = " + r.cpp + "();";
        EXPECT_EQ(round_trip(std::string(r.script) + " x;"), want) << r.script;
        if (r.alias)
            EXPECT_EQ(round_trip(std::string(r.alias) + " x;"), want) << r.alias;
    }
}

TEST(BuiltinTypeParse, AliasesInsideTypesAndNew)
{
    EXPECT_EQ(round_trip("std.vector<ulong> v = f();"), "std::vector<uint64_t> v = f();");
    EXPECT_EQ(
        round_trip("std.map<uint8, std.vector<int64>> m = f();"), "std::map<uint8_t, std::vector<int64_t>> m = f();");
    EXPECT_EQ(round_trip("ulong* p = f();"), "uint64_t* p = f();");
    EXPECT_EQ(round_trip("var v = new std.vector<ulong>(3);"), "auto v = new std::vector<uint64_t>(3);");
    EXPECT_EQ(round_trip("var v = std.vector<ulong>();"), "auto v = std::vector<uint64_t>();");
    EXPECT_EQ(round_trip("var f = (ulong a) => { return a; };"), "auto f = [=](uint64_t a) { return a; };");
}

TEST(BuiltinTypeParse, NativeNamesStayNormalNames)
{
    EXPECT_EQ(round_trip("int32_t x = 1;"), "int32_t x = 1;");
    EXPECT_EQ(round_trip("size_t x = 1;"), "size_t x = 1;");
    EXPECT_EQ(round_trip("char16_t x = 1;"), "char16_t x = 1;");
    EXPECT_EQ(round_trip("Foo.byte x = 1;"), "Foo::byte x = 1;"); // a member name is not a builtin
}

TEST(BuiltinTypeParse, CppSpellingsAreErrorsWithAHint)
{
    const struct
    {
        const char* type;
        const char* hint;
    } cases[] = {
        {"unsigned", "use 'uint'"},
        {"unsigned int", "use 'uint'"},
        {"signed", "use 'int'"},
        {"long long", "use 'long'"},
        {"long int", "use 'long'"},
        {"unsigned long", "use 'ulong'"},
        {"unsigned long long", "use 'ulong'"},
        {"unsigned long int", "use 'ulong'"},
        {"unsigned char", "use 'byte'"},
        {"signed char", "use 'sbyte'"},
        {"unsigned short", "use 'ushort'"},
        {"short int", "use 'short'"},
        {"long double", "use 'double'"},
    };
    for (const auto& c : cases)
    {
        const std::string text = std::string(c.type) + " x = 1;";
        Parsed p(text);
        ASSERT_GE(p.diag.items.size(), 1u) << c.type;
        EXPECT_NE(p.diag.items[0].msg.find(c.hint), std::string::npos) << c.type << ": " << p.diag.items[0].msg;
        EXPECT_NE(p.diag.items[0].msg.find(std::string("'") + c.type + "'"), std::string::npos)
            << c.type << ": " << p.diag.items[0].msg;
    }
    Parsed t("std.vector<unsigned int> v;");
    ASSERT_GE(t.diag.items.size(), 1u);
    EXPECT_NE(t.diag.items[0].msg.find("use 'uint'"), std::string::npos) << t.diag.items[0].msg;
    Parsed l("var f = (unsigned long a) => { return a; };");
    ASSERT_GE(l.diag.items.size(), 1u);
    EXPECT_NE(l.diag.items[0].msg.find("use 'ulong'"), std::string::npos) << l.diag.items[0].msg;
}
