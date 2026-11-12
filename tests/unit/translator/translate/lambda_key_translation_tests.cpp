// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the identity key of a lambda in hotc::translate. The unit name is "t".
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace translator_test;

namespace
{

hotc::TranslateResult run(const std::string& script)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    return hotc::translate(temp_options(dir, "using \"w.h\";\n" + script));
}

using V = std::vector<std::string>;

} // namespace

TEST(LambdaKeyTranslation, MethodOfAClass)
{
    const auto r = run("class Foo : Widget {\n    void run() {\n        var a = () => { };\n"
                       "        add(() => { });\n    }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.lambda_keys, (V{"t:Foo::run/v:a", "t:Foo::run/#0"}));
}

TEST(LambdaKeyTranslation, OverloadsHaveDifferentKeys)
{
    const auto r = run("class Foo : Widget {\n    void run(int a) { add(() => { }); }\n"
                       "    void run(float a) { add(() => { }); }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    ASSERT_EQ(r.lambda_keys.size(), 2u);
    EXPECT_NE(r.lambda_keys[0], r.lambda_keys[1]);
    EXPECT_EQ(r.lambda_keys[0].rfind("t:Foo::run__ov_", 0), 0u) << r.lambda_keys[0];
    EXPECT_EQ(r.lambda_keys[0].substr(r.lambda_keys[0].size() - 3), "/#0");
}

TEST(LambdaKeyTranslation, NativeOverrideUsesTheSlotId)
{
    const auto r = run("class Foo : Widget {\n    override void Update(float dt) { add(() => { }); }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.lambda_keys, (V{"t:Foo::Update/#0"}));
}

TEST(LambdaKeyTranslation, ConstructorUsesTheOverloadKeyOfCtor)
{
    const auto r = run("class Foo : Widget {\n    Foo(int a) { add(() => { }); }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    ASSERT_EQ(r.lambda_keys.size(), 1u);
    EXPECT_EQ(r.lambda_keys[0].rfind("t:Foo::ctor__ov_", 0), 0u) << r.lambda_keys[0];
}

TEST(LambdaKeyTranslation, StaticMethodAndFreeFunction)
{
    const auto r = run("class Foo : Widget {\n    static void make() { add(() => { }); }\n}\n"
                       "void helper() { var f = () => { }; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.lambda_keys, (V{"t:helper/v:f", "t:Foo::make/#0"}));
}

TEST(LambdaKeyTranslation, NamedAndAliasInAMethod)
{
    const auto r = run("class Foo : Widget {\n    void run() { add([name(\"fire\"), alias(\"shoot\")] () => { }); }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.lambda_keys, (V{"t:Foo::run/n:fire", "t:Foo::run/n:shoot"}));
}

TEST(LambdaKeyTranslation, ErrorEqualVariables)
{
    const auto r = run("class Foo : Widget {\n    void run() {\n        var f = () => { };\n        f = () => { };\n    }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("are assigned to 'f'. Add [name(\"...\")] to each lambda"), std::string::npos)
        << r.diagnostics;
}

TEST(LambdaKeyTranslation, ErrorEqualNames)
{
    const auto r = run("class Foo : Widget {\n    void run() {\n        add([name(\"a\")] () => { });\n"
                       "        add([name(\"a\")] () => { });\n    }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("have the name 'a'"), std::string::npos) << r.diagnostics;
}

TEST(LambdaKeyTranslation, ErrorAliasWithoutName)
{
    const auto r = run("class Foo : Widget {\n    void run() { add([alias(\"a\")] () => { }); }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("needs [name(\"...\")]"), std::string::npos) << r.diagnostics;
}

// A lambda in a field initializer is an error already (the lowering). So it has no key.
TEST(LambdaKeyTranslation, FieldInitializerHasNoKey)
{
    const auto r = run("class Foo : Widget {\n    int n = add(() => { });\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("a lambda is not allowed in a field initializer"), std::string::npos) << r.diagnostics;
}
