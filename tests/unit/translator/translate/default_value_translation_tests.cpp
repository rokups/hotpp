// Tests for hotc::translate with fields and locals that have no initializer.
#include "hot/hotc_lib.h"
#include "support/stub_translate_test_support.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;
using namespace stub_translate_test;

// No initializer: the value is the zero or default value, never an indeterminate one.
TEST(DefaultValueTranslation, FieldsAreValueInitialized)
{
    const hotc::TranslateResult r =
        weapon_translate("    int a;\n    Weapon w;\n    Vec3 v;\n    float f { get; set; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "using T = int32_t; new (f) T();")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "using T = std::shared_ptr<Weapon>; new (f) T();")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "using T = Vec3; new (f) T();")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "using T = float; new (f) T();")) << r.cpp;
}

TEST(DefaultValueTranslation, LocalsAreValueInitialized)
{
    const hotc::TranslateResult r = weapon_translate(
        "    int K() { int x; Vec3 v; Weapon w; Weapon* p; return x; }\n"
        "    void L() { for (int i; i < 3; i++) { } }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "int32_t x = int32_t();")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Vec3 v = Vec3();")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Weapon> w = std::shared_ptr<Weapon>();")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "for (int32_t i = int32_t(); i < 3; i++)")) << r.cpp;
}

TEST(DefaultValueTranslation, VarAndConstNeedAnInitializer)
{
    const hotc::TranslateResult a = weapon_translate("    void F() { var x; }\n");
    EXPECT_FALSE(a.ok);
    EXPECT_TRUE(has(a.diagnostics, "var needs an initializer")) << a.diagnostics;
    const hotc::TranslateResult b = weapon_translate("    void F() { const int X; }\n");
    EXPECT_FALSE(b.ok);
    EXPECT_TRUE(has(b.diagnostics, "a const needs an initializer")) << b.diagnostics;
}
