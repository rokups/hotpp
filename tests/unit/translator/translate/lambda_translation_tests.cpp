// Tests for sppc::translate with the arrow lambda.
#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

TEST(LambdaTranslation, ArrowLambdaBecomesAClosureThatCapturesByValue)
{
    const sppc::TranslateOptions o = engine_options("timer_v1.hot", 1);
    const sppc::TranslateResult r = sppc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("spp::make_closure(GEN, \"Beeper::arm lambda\", [=]()"), std::string::npos) << r.cpp;
    // The body uses self and a new state pointer, not the pointer of the outer function.
    EXPECT_NE(r.cpp.find("auto* __st = state_of(__self);\n#line 15"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("__st->fired = __st->fired + 1;"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("=>"), std::string::npos);
}

TEST(LambdaTranslation, ParametersGetTypesAndPointersUseArrow)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const sppc::TranslateResult r = sppc::translate(
        temp_options(dir, lambda_script("        var f = (float x, int* q) => { q.foo(); this.n = x; };")));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("[=](float x, int32_t* q)"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("spp::deref(q).foo();"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("__st->n = x;"), std::string::npos) << r.cpp;
}

TEST(LambdaTranslation, ParameterWithoutTypeIsAuto)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const sppc::TranslateResult r =
        sppc::translate(temp_options(dir, lambda_script("        var f = (x, int y) => { };")));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("[=](auto x, int32_t y)"), std::string::npos) << r.cpp;
}
