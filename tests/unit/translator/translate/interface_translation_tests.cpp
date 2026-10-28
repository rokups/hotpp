// Tests for sppc::translate with native interface bases (S8b, design 4.4 and 5.3): the shell,
// the slots, the forwarders, the record and the conflict errors.
#include <gtest/gtest.h>

#include <string>

#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

using namespace translator_test;

namespace {

const char* kIfaceHeader =
    "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} virtual int Size() { return 1; } };\n"
    "class IPing { public: virtual ~IPing() {} virtual int Ping(int n) = 0; virtual int Size(int k) { return k; } };\n"
    "class IName { public: virtual ~IName() {} virtual int Id() const { return 3; } };\n"
    "class ISame { public: virtual ~ISame() {} virtual void Update(float dt) {} };\n"
    "class IConst { public: virtual ~IConst() {} virtual int Size() const { return 2; } };\n"
    "class IFinal { public: virtual ~IFinal() {} virtual int Lock() final { return 0; } };\n"
    "class ISealed final { public: virtual ~ISealed() {} };\n"
    "class INoDefault { public: INoDefault(int) {} virtual ~INoDefault() {} };\n";

sppc::TranslateResult run(const std::string& classes)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kIfaceHeader);
    return sppc::translate(temp_options(dir, "using \"w.h\";\n" + classes));
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

}  // namespace

// The shell derives from all bases. The virtuals of the interfaces get slots after the virtuals
// of the primary base. A name in two bases gets a suffix of its parameter types.
TEST(InterfaceTranslation, ShellDerivesFromAllBasesWithOneSlotForEachVirtual)
{
    const sppc::TranslateResult r =
        run("class Foo : Widget, IPing, IName {\n    override int Ping(int n) { return n; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "struct Foo_Shell final : Widget, IPing, IName, spp::ShellBase {")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "SLOT_Update = 0,\n    SLOT_Size_v = 1,\n    SLOT_Ping = 2,\n    SLOT_Size_i = 3,\n"
                           "    SLOT_Id = 4,\n    SLOT_COUNT = 5,"))
        << r.cpp;
    EXPECT_TRUE(has(r.cpp, "int Ping(int a0) override")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "int Id() const override")) << r.cpp;
    // A virtual without an override calls the implementation of the base that declares it.
    EXPECT_TRUE(has(r.cpp, "static_cast<Foo_Shell*>(a)->IPing::Size(a0);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static_cast<Foo_Shell*>(a)->IName::Id();")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static_cast<Foo_Shell*>(a)->Widget::Size();")) << r.cpp;
    // The record lists all bases. The reload check compares the list (design 5.3).
    EXPECT_TRUE(has(r.cpp, "\"Widget, IPing, IName\", spp::shell_layout_hash(\"Widget, IPing, IName\"")) << r.cpp;
}

// An override of a const interface virtual keeps the const stub. "super.F()" of an interface
// virtual calls the implementation of the interface base.
TEST(InterfaceTranslation, OverrideAndSuperCallOfAnInterfaceVirtual)
{
    const sppc::TranslateResult r = run(
        "class Foo : Widget, IPing, IName {\n    override int Ping(int n) { return super.Id() + super.Size(); }\n"
        "    override int Id() { return 9; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "self->IName::Id()")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "self->Widget::Size()")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "reinterpret_cast<void*>(&Foo__Id)")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "Foo__fwd_Id")) << r.cpp;
}

// A child of a class with interface bases inherits the interface slots. It can override them.
TEST(InterfaceTranslation, ChildOverridesAnInterfaceVirtualOfTheRoot)
{
    const sppc::TranslateResult r = run(
        "class Foo : Widget, IPing {\n    override int Ping(int n) { return n; }\n}\n"
        "class Bar : Foo {\n    override int Ping(int n) { return super.Ping(n) + 1; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "struct Foo_Shell : Widget, IPing, spp::ShellBase")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "struct Bar_Shell final : Foo_Shell {")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo__base_Ping(self, n)")) << r.cpp;
}

// A pure virtual of an interface base needs an override.
TEST(InterfaceTranslation, MissingOverrideOfAPureInterfaceVirtualIsAnError)
{
    const sppc::TranslateResult r = run("class Foo : Widget, IPing { }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "class Foo does not override the pure virtual method 'Ping(int)' of IPing"))
        << r.diagnostics;
}

// The same signature in two bases is an error: one shell function cannot have two slots.
TEST(InterfaceTranslation, SameSignatureInTwoBasesIsAnError)
{
    const sppc::TranslateResult r = run("class Foo : Widget, ISame { }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'Update(float)' is a virtual method of Widget (base 'Widget') and of ISame "
                                   "(base 'ISame'). One method cannot override a virtual of two bases"))
        << r.diagnostics;
}

// "F()" in one base and "F() const" in another base: the script override cannot pick one.
TEST(InterfaceTranslation, AmbiguousOverrideIsAnError)
{
    const sppc::TranslateResult ok = run("class Foo : Widget, IConst { }\n");
    EXPECT_TRUE(ok.ok) << ok.diagnostics;  // no override: both slots forward
    const sppc::TranslateResult r = run("class Foo : Widget, IConst {\n    override int Size() { return 4; }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics,
                    "the override 'Size' is ambiguous: it matches 'Size()' of Widget and 'Size() const' of IConst"))
        << r.diagnostics;
}

// A final virtual of an interface base cannot be overridden. A final interface class is an error.
TEST(InterfaceTranslation, FinalVirtualAndFinalClassAreErrors)
{
    const sppc::TranslateResult ok = run("class Foo : Widget, IFinal { }\n");
    EXPECT_TRUE(ok.ok) << ok.diagnostics;
    EXPECT_FALSE(has(ok.cpp, "SLOT_Lock")) << ok.cpp;  // a final method gets no slot
    const sppc::TranslateResult r = run("class Foo : Widget, IFinal {\n    override int Lock() { return 1; }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'Lock()' of IFinal is final and cannot be overridden")) << r.diagnostics;
    const sppc::TranslateResult s = run("class Foo : Widget, ISealed { }\n");
    EXPECT_FALSE(s.ok);
    EXPECT_TRUE(has(s.diagnostics, "interface base 'ISealed' is final")) << s.diagnostics;
}

// The other rules of the base list.
TEST(InterfaceTranslation, BaseListErrors)
{
    struct Case {
        const char* script;
        const char* error;
    };
    const Case cases[] = {
        {"class Foo : Widget, IMissing { }\n", "interface base 'IMissing' is not found in the imported headers"},
        {"class Foo : Widget, INoDefault { }\n", "interface base 'INoDefault' has no default constructor"},
        {"class Foo : Widget, IName, IName { }\n", "class 'Foo' names the base 'IName' two times"},
        {"class Foo : Widget, Widget { }\n", "class 'Foo' names the base 'Widget' two times"},
        {"class Foo : Widget { }\nclass Bar : Foo, IName { }\n",
         "class 'Bar' has the script parent 'Foo'. Only a class with a native base can have interface bases: the "
         "shell of a child adds no overrides (design 4.5). Add 'IName' to the bases of the root class 'Foo'"},
        {"class Foo : Widget { }\nclass Mid : Foo { }\nclass Bar : Mid, IName { }\n",
         "Add 'IName' to the bases of the root class 'Foo'"},
        {"class Foo : Widget { }\nclass Bar : Widget, Foo { }\n",
         "the interface base 'Foo' of class 'Bar' is a script class. An interface base must be a native class"},
        {"[value] class V : Widget, IName { }\n", "a [value] class 'V' has no base class"},
    };
    for (const Case& c : cases) {
        const sppc::TranslateResult r = run(c.script);
        EXPECT_FALSE(r.ok) << c.script;
        EXPECT_TRUE(has(r.diagnostics, c.error)) << c.script << "\n" << r.diagnostics;
    }
}
