// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the "$T" shared pointer rules: raw pointer sources, derived to base, weak reads and the factory.
#include "hot/hotc_lib.h"
#include "support/stub_translate_test_support.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;
using namespace stub_translate_test;

namespace
{

const char* kRawError = "cannot create a shared pointer from a raw pointer; use new $T(...)";

const char* kShapeHeader =
    "#include <memory>\n"
    "struct Shape { virtual ~Shape() {} int sides = 0; };\n"
    "struct Circle : Shape { int r = 0; };\n"
    "struct Pt { int x = 0; };\n"
    "struct Rc { void AddRef(); void Release(); virtual ~Rc() {} };\n"
    "struct Owned : std::enable_shared_from_this<Owned> { virtual ~Owned() {} };\n"
    "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n"
    "struct Host { template <class T> T* Get() const { return nullptr; } Widget* Any() const { return nullptr; } };\n";

const char* kShapeStubs = "[native, handle(\"std::shared_ptr<$T>\")] class Shape;\n"
                          "[native, handle(\"std::shared_ptr<$T>\")] class Owned;\n"
                          "[native, handle(\"std::shared_ptr<$T>\")] class Widget;\n";

hotc::TranslateResult shape_translate(const std::string& members, const std::string& stubs = kShapeStubs)
{
    static TempDir* dir = nullptr;
    if (!dir)
        dir = new TempDir();
    write_file(dir->path() / "s.h", kShapeHeader);
    return hotc::translate(
        temp_options(*dir, "using \"s.h\";\n" + stubs + "class Foo : Widget {\n" + members + "}\n"));
}

} // namespace

TEST(SharedPointerRules, ThisToANativeHandleWithoutAWeakSourceIsAnError)
{
    const hotc::TranslateResult r = shape_translate("    void Run() { $Widget w = this; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, kRawError)) << r.diagnostics;
}

TEST(SharedPointerRules, ThisToTheScriptClassHandleWorks)
{
    const hotc::TranslateResult r = shape_translate("    void Run() { $Foo me = this; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::handle_from_this<std::shared_ptr<Foo>>(__self)")) << r.cpp;
}

TEST(SharedPointerRules, RawPointerLocalIsAnError)
{
    const hotc::TranslateResult r = shape_translate("    void Run(Shape* p) { $Shape s = p; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, kRawError)) << r.diagnostics;
}

TEST(SharedPointerRules, AddressOfIsAnError)
{
    const hotc::TranslateResult r = shape_translate("    void Run() { Pt v; $Pt s = &v; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, kRawError)) << r.diagnostics;
}

TEST(SharedPointerRules, ResetWithARawPointerIsAnError)
{
    const hotc::TranslateResult r = shape_translate("    void Run(Shape* p, $Shape s) { if (s == null) return; s.reset(p); }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, kRawError)) << r.diagnostics;
}

TEST(SharedPointerRules, ResetWithADereferenceOfARawPointerIsAnError)
{
    const hotc::TranslateResult d = shape_translate(
        "    void Run(Shape** pp, $Shape s) { if (s == null || pp == null) return; s.reset(*pp); }\n");
    EXPECT_FALSE(d.ok);
    EXPECT_TRUE(has(d.diagnostics, kRawError)) << d.diagnostics;
}

// The cast to a raw pointer of a handle class is an error already, so "reset((Shape*)v)" cannot pass.
TEST(SharedPointerRules, ResetWithACastToARawPointerIsAnError)
{
    const hotc::TranslateResult c = shape_translate(
        "    void Run(void* v, $Shape s) { if (s == null) return; s.reset((Shape*)v); }\n");
    EXPECT_FALSE(c.ok);
}

// The cast "($C)h" of an intrusive handle has the same checks as the cast of a shared handle.
TEST(SharedPointerRules, CastOfAnIntrusiveHandleIsChecked)
{
    static TempDir* dir = nullptr;
    if (!dir)
        dir = new TempDir();
    write_file(dir->path() / "s.h", kShapeHeader);
    const hotc::TranslateResult r = hotc::translate(temp_options(*dir,
        "using \"s.h\";\n[native, handle(\"$T*\"), intrusive(\"AddRef\", \"Release\")] class Rc;\n"
        "class Foo : Rc {\n    int a = 0;\n    void Fn() { }\n}\nclass Bar : Rc {\n    int b = 0;\n"
        "    void Run($Foo h, $Rc r) {\n"
        "        (($Foo)h).Fn();\n        $Rc u = ($Rc)h;\n        $Foo d = ($Foo)r;\n    }\n}\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::nonnull(h)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::as_script<Foo_Shell>(r, \"Foo\")")) << r.cpp;
}

TEST(SharedPointerRules, ResetWithAHandleOrWithoutAnArgumentWorks)
{
    const hotc::TranslateResult r = shape_translate("    void Run($Shape a, $Shape s) { if (s == null) return; s.reset(); }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(SharedPointerRules, NativeWithSharedFromThisAcceptsARawPointer)
{
    const hotc::TranslateResult r = shape_translate("    void Run(Owned* p) { $Owned s = p; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(SharedPointerRules, DerivedConvertsToBase)
{
    const hotc::TranslateResult r = shape_translate("    void Run($Circle c) { $Shape s = c; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Shape> s = c;")) << r.cpp;
}

TEST(SharedPointerRules, BaseToDerivedNeedsACast)
{
    const hotc::TranslateResult r = shape_translate("    void Run($Shape s) { $Circle c = s; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "cannot convert the base handle '$Shape' to the derived handle '$Circle' without a cast"))
        << r.diagnostics;
}

TEST(SharedPointerRules, WeakReadGivesAStrongHandle)
{
    const hotc::TranslateResult r = shape_translate(
        "    [weak]\n    $Shape w = null;\n    $Shape Get() { return w; }\n"
        "    void Set($Shape s) { w = s; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "return hot::lock(__st->w);") || has(r.cpp, "return hot::lock(w);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "w = s;")) << r.cpp;
}

TEST(SharedPointerRules, WeakPtrLockGivesAStrongHandle)
{
    const hotc::TranslateResult r = shape_translate(
        "    std.weak_ptr<Shape> w;\n    void Set($Shape s) { w = s; }\n    $Shape Get() { return w.lock(); }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(SharedPointerFactory, DefaultIsMakeShared)
{
    const hotc::TranslateResult r = shape_translate("    void Run() { $Shape s = new $Shape(); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::make_shared<Shape>()")) << r.cpp;
}

TEST(SharedPointerFactory, StubFactoryReplacesTheDefault)
{
    const hotc::TranslateResult r = shape_translate("    void Run() { $Shape s = new $Shape(); }\n",
        "[native, handle(\"std::shared_ptr<$T>\"), factory(\"my::make<$T>\")] class Shape;\n"
        "[native, handle(\"std::shared_ptr<$T>\")] class Widget;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "my::make<Shape>()")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "std::make_shared<Shape>")) << r.cpp;
}

TEST(SharedPointerFactory, IntrusiveStubWithAFactory)
{
    const hotc::TranslateResult r = shape_translate("    void Run() { $Rc s = new $Rc(); }\n",
        "[native, handle(\"$T*\"), intrusive(\"AddRef\", \"Release\"), factory(\"my::create<$T>\")] class Rc;\n"
        "[native, handle(\"std::shared_ptr<$T>\")] class Widget;\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "my::create<Rc>()")) << r.cpp;
}

TEST(SharedPointerFactory, FactoryNeedsAPlaceholder)
{
    const hotc::TranslateResult r = shape_translate("",
        "[native, handle(\"std::shared_ptr<$T>\"), factory(\"std::make_shared\")] class Shape;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'factory' needs a template with '$T'")) << r.diagnostics;
}

// A raw pointer of a script class from a native call becomes a handle at run time (hot::handle_from_raw).
TEST(SharedPointerRules, NativeCallResultToTheScriptClassHandle)
{
    const hotc::TranslateResult r = shape_translate("    void Run(Host* h) { if (h == null) return; $Foo f = h.Get<Foo>(); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::handle_from_raw<std::shared_ptr<Foo>>(")) << r.cpp;
}

TEST(SharedPointerRules, AsOfARawPointerToTheScriptClassHandle)
{
    const hotc::TranslateResult r = shape_translate("    void Run(Host* h) { if (h == null) return; $Foo f = h.Any() as $Foo; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::handle_from_raw<std::shared_ptr<Foo>>(hot::as_script<Foo_Shell>(")) << r.cpp;
}

TEST(SharedPointerRules, ScriptHandleToScriptHandleIsNotWrapped)
{
    const hotc::TranslateResult r = shape_translate("    void Run($Foo a) { $Foo f = a; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "hot::handle_from_raw")) << r.cpp;
}
