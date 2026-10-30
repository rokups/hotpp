// Tests for sppc::translate with the string switch and the collection literal.
#include <gtest/gtest.h>

#include <string>

#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

using namespace translator_test;

TEST(CollectionSwitchTranslation, StringSwitchBecomesAnIfChain)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const sppc::TranslateResult r = sppc::translate(temp_options(
        dir, lambda_script("        std.string s = \"a\";\n"
                           "        switch (s) { case \"a\": this.n = 1; break; case \"b\": case \"c\": this.n = 2; break; "
                           "default: this.n = 3; }")));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("const auto& __sw_1 = s;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("if (__sw_1 == \"a\")"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("__sw_1 == \"b\" || __sw_1 == \"c\""), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("switch"), std::string::npos) << r.cpp;
}

TEST(CollectionSwitchTranslation, StringSwitchFallthroughIsAnError)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const sppc::TranslateResult r = sppc::translate(temp_options(
        dir, lambda_script("        std.string s = \"a\";\n"
                           "        switch (s) { case \"a\": this.n = 1; case \"b\": this.n = 2; break; }")));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("a string switch case must end with break, return, throw or continue"),
              std::string::npos)
        << r.diagnostics;
}

TEST(CollectionSwitchTranslation, MixedLabelsAreAnError)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const sppc::TranslateResult r = sppc::translate(temp_options(
        dir, lambda_script("        std.string s = \"a\";\n"
                           "        switch (s) { case \"a\": break; case 1: break; }")));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("a switch cannot mix string labels and other labels"), std::string::npos)
        << r.diagnostics;
}

TEST(CollectionSwitchTranslation, CollectionLiteralsBecomeBracedLists)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const sppc::TranslateResult r = sppc::translate(temp_options(
        dir, "using <map>;\nusing <string>;\nusing <vector>;\n" +
                 lambda_script("        std.vector<int> xs = [1, 2, 3];\n"
                           "        std.map<std.string, int> m = [\"a\": 1, \"b\": 2];\n"
                           "        std.vector<int> e = [];")));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("std::vector<int32_t> xs = {1, 2, 3};"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("std::map<std::string, int32_t> m = {{\"a\", 1}, {\"b\", 2}};"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("std::vector<int32_t> e = {};"), std::string::npos) << r.cpp;
}

TEST(CollectionSwitchTranslation, VarCollectionLiteralIsAnError)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const sppc::TranslateResult r =
        sppc::translate(temp_options(dir, lambda_script("        var xs = [1, 2, 3];")));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("a collection literal needs a declared type"), std::string::npos)
        << r.diagnostics;
}
