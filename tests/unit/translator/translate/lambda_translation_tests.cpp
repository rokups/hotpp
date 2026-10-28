// Tests for spc::translate with the arrow lambda.
#include <gtest/gtest.h>

#include <string>

#include "spp/spc_lib.h"
#include "support/translator_test_support.h"

using namespace translator_test;

TEST(LambdaTranslation, ArrowLambdaBecomesAClosureThatCapturesByValue)
{
    const spc::TranslateOptions o = engine_options("timer_v1.script", 1);
    const spc::TranslateResult r = spc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("script::make_closure(GEN, \"Beeper::arm lambda\", [=]()"),
              std::string::npos)
        << r.cpp;
    // The body uses self and a new state pointer, not the pointer of the outer function.
    EXPECT_NE(r.cpp.find("auto* st = state_of(self);\n#line 15"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("st->fired = st->fired + 1;"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("=>"), std::string::npos);
}

TEST(LambdaTranslation, ParametersGetTypesAndPointersUseArrow)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const spc::TranslateResult r = spc::translate(
        temp_options(dir, lambda_script("        var f = (float x, int* q) => { q.foo(); this.n = x; };")));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("[=](float x, int32_t* q)"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("script::deref(q).foo();"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("st->n = x;"), std::string::npos) << r.cpp;
}

TEST(LambdaTranslation, ParameterWithoutTypeIsAuto)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const spc::TranslateResult r =
        spc::translate(temp_options(dir, lambda_script("        var f = (x, int y) => { };")));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("[=](auto x, int32_t y)"), std::string::npos) << r.cpp;
}

TEST(LambdaTranslation, OldParameterFormIsAnError)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const spc::TranslateResult r =
        spc::translate(temp_options(dir, lambda_script("        var f = (x: int) => { };")));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("parameter syntax changed; write 'Type name'"),
              std::string::npos)
        << r.diagnostics;
}
