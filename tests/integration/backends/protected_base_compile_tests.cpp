// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A native base with a protected virtual method that the script does not override. The generated
// code calls the native implementation of it. That call must compile: only a class that derives
// from the base has access to a protected member (as OnNodeSet of an rbfx Component).
#include "hot/backend.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

const char* kProtectedHeader =
    "namespace eng {\n"
    "class Node { public: virtual ~Node() {} virtual void Tick(float dt) {}\n"
    "protected: virtual void OnAttach(int id) { attached = id; } virtual int Order() const { return 1; }\n"
    "    int attached = 0; };\n"
    "}\n";

const char* kScript = "using \"protected_node.h\";\n"
                      "class Mover : eng.Node {\n    int steps = 0;\n"
                      "    override void Tick(float dt) { steps = steps + 1; }\n}\n"
                      "class Parent : eng.Node {\n    int a = 0;\n}\n"
                      "class Child : Parent {\n    int b = 0;\n}\n";

hotc::TranslateResult translate_with(const TempDir& dir, bool aot)
{
    hotc::TranslateOptions opt = temp_options(dir, kScript);
    opt.aot = aot;
    opt.aot_optimized = aot;
    return hotc::translate(opt);
}

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

// The backend compiles the generated code of a root class and of a parent with a child.
TEST(ProtectedBaseCompile, NotOverriddenProtectedVirtualCompiles)
{
    TempDir dir;
    write_file(dir.path() / "protected_node.h", kProtectedHeader);
    hot::DefaultBackendOptions options;
    options.include_dirs.push_back(dir.path().string());
    auto backend = hot::make_default_backend(options);
    const hot::ScriptSource source{"protected_base.hot", kScript, {}};
    const hot::LoadResult r = backend->load(source, 1);
    ASSERT_TRUE(r) << r.error;
}

// Both code forms call the native implementation through a member of the shell.
TEST(ProtectedBaseCompile, BaseCallsGoThroughTheShell)
{
    TempDir dir;
    write_file(dir.path() / "protected_node.h", kProtectedHeader);
    const hotc::TranslateResult live = translate_with(dir, false);
    ASSERT_TRUE(live.ok) << live.diagnostics;
    EXPECT_TRUE(has(live.cpp, "return eng::Node::OnAttach(")) << live.cpp;
    EXPECT_FALSE(has(live.cpp, "(__hot_owner)->eng::Node::")) << live.cpp;
    const hotc::TranslateResult aot = translate_with(dir, true);
    ASSERT_TRUE(aot.ok) << aot.diagnostics;
    EXPECT_TRUE(has(aot.cpp, "->Parent_Shell::__hot_base_")) << aot.cpp;
    EXPECT_FALSE(has(aot.cpp, "->eng::Node::OnAttach(")) << aot.cpp;
}
