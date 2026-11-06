// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Translation tests of the reload contract rows that S17 adds (design 5.3): the signature lines
// and the keyed thunk, the layout key of a native container of a class, and the record
// flag of a shell with std::enable_shared_from_this.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

using namespace translator_test;

namespace
{

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

hotc::TranslateResult gen(const std::string& script, int generation, const std::string& slots_in)
{
    hotc::TranslateOptions o;
    o.script_name = "t.hot";
    o.script_text = script;
    o.generation = generation;
    o.slots_in = slots_in;
    return hotc::translate(o);
}

std::string counter(const std::string& params, const std::string& body)
{
    return "class Counter {\n    int n = 0;\n    void add(" + params + ") { " + body + " }\n}\n";
}

// Each script function gets a signature line. The slot lines do not change.
TEST(SignatureLines, EachScriptFunctionHasALine)
{
    hotc::TranslateResult r = gen(counter("int x1", "this.n = this.n + x1;"), 1, "");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.slots_out, "Counter::add 1\n");
    EXPECT_TRUE(has(r.sigs_out, "sig Counter::add ")) << r.sigs_out;
    EXPECT_TRUE(has(r.sigs_out, "int32_t)")) << r.sigs_out;
}

// A changed signature gives a new thunk with a signature key. The same signature gives none.
TEST(SignatureLines, ChangedSignatureEmitsAKeyedThunk)
{
    hotc::TranslateResult g1 = gen(counter("int x1", "this.n = this.n + x1;"), 1, "");
    ASSERT_TRUE(g1.ok) << g1.diagnostics;
    const std::string slots = g1.slots_out + g1.sigs_out;

    hotc::TranslateResult same = gen(counter("int x1", "this.n = this.n + 2 * x1;"), 2, slots);
    ASSERT_TRUE(same.ok) << same.diagnostics;
    EXPECT_FALSE(has(same.cpp, "add__thunk(")) << same.cpp;
    EXPECT_EQ(same.sigs_out, g1.sigs_out);

    hotc::TranslateResult changed = gen(counter("int x1, int x2", "this.n = this.n + x1 + x2;"), 2, slots);
    ASSERT_TRUE(changed.ok) << changed.diagnostics;
    EXPECT_TRUE(has(changed.cpp, "add__thunk(")) << changed.cpp;
    EXPECT_TRUE(
        has(changed.cpp, "reg.add_thunk(\"Counter::add\", reinterpret_cast<void*>(&Counter__add__thunk), typeid("))
        << changed.cpp;
    EXPECT_TRUE(has(changed.cpp, "int32_t, int32_t)).name(), SLOT_add);")) << changed.cpp;
    EXPECT_EQ(changed.slots_out, g1.slots_out); // the function keeps its slot

    // A slot file without signature lines (an older file) gives no keyed thunk.
    hotc::TranslateResult old_file = gen(counter("int x1, int x2", "this.n = this.n + x1 + x2;"), 2, g1.slots_out);
    ASSERT_TRUE(old_file.ok) << old_file.diagnostics;
    EXPECT_FALSE(has(old_file.cpp, "add__thunk(")) << old_file.cpp;
}

std::string bag(const std::string& item_fields)
{
    return "using <vector>;\nstruct Item {\n" + item_fields
        + "}\nclass Bag {\n    std.vector<Item> items;\n    Item one;\n}\n";
}

std::string key_of(const std::string& cpp)
{
    const std::string head = "hot::layout_keyed(typeid(hot::typekey_t<std::vector<Item>>).name(), \"";
    const size_t at = cpp.find(head);
    return at == std::string::npos ? std::string() : cpp.substr(at + head.size(), 16);
}

// A native container of a class has a layout key. A field itself has none.
// The key changes with the fields of the class, not with an initializer.
TEST(ContainerLayoutKey, FollowsTheLayoutOfTheValueClass)
{
    hotc::TranslateResult a = translate_text(bag("    int weight = 0;\n"));
    ASSERT_TRUE(a.ok) << a.diagnostics;
    const std::string ka = key_of(a.cpp);
    ASSERT_EQ(ka.size(), 16u) << a.cpp;
    EXPECT_FALSE(has(a.cpp, "hot::layout_keyed(typeid(Item)")) << a.cpp;
    hotc::TranslateResult b = translate_text(bag("    int weight = 5;\n"));
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_EQ(key_of(b.cpp), ka);
    hotc::TranslateResult c = translate_text(bag("    int weight = 0;\n    int color = 0;\n"));
    ASSERT_TRUE(c.ok) << c.diagnostics;
    EXPECT_NE(key_of(c.cpp), ka);
    hotc::TranslateResult d = translate_text(bag("    float weight = 0;\n"));
    ASSERT_TRUE(d.ok) << d.diagnostics;
    EXPECT_NE(key_of(d.cpp), ka);
}

// A shell with std::enable_shared_from_this sets the record flag in script_register.
TEST(SharesThisFlag, SetOnlyWhenTheShellSharesThis)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const std::string head = "using \"w.h\";\nclass Foo : Widget {\n    int n = 0;\n";
    hotc::TranslateResult plain = hotc::translate(temp_options(dir, head + "}\n"));
    ASSERT_TRUE(plain.ok) << plain.diagnostics;
    EXPECT_FALSE(has(plain.cpp, "shares_this")) << plain.cpp;
    hotc::TranslateResult shared = hotc::translate(temp_options(dir, head + "    $Foo Self() { return this; }\n}\n"));
    ASSERT_TRUE(shared.ok) << shared.diagnostics;
    EXPECT_TRUE(has(shared.cpp, "std::enable_shared_from_this<Foo_Shell>")) << shared.cpp;
    EXPECT_TRUE(has(shared.cpp, "    Foo_record.shares_this = true;\n")) << shared.cpp;
}

// A native struct with a function pointer field gets native_holds_code (S26). This covers a
// plain pointer, a member function pointer, an array, a nested struct and a base struct. A
// struct with a data pointer only, or with a data member pointer, does not get it. The type
// is found also inside a native container.
TEST(NativeHoldsCode, FunctionPointerFieldsMarkTheStruct)
{
    TempDir dir;
    write_file(dir.path() / "w.h", std::string(kHeaderV1) +
        "using Fn = int (*)(int);\n"
        "struct Plain { int a; float b; };\n"
        "struct DataPtr { int* p; int Plain::* m; };\n"
        "struct RawCb { Fn fn; int tag; };\n"
        "struct MemCb { int (Widget::*mf)(int); };\n"
        "struct ArrCb { void (*fns[2])(); };\n"
        "struct Inner { Fn fn; };\n"
        "struct Nested { int a; Inner in; };\n"
        "struct Derived : RawCb { int more; };\n");
    const std::string head = "using \"w.h\";\nclass Foo : Widget {\n";
    const std::string tail = "    void f() { }\n}\n";
    const auto mark = [](const char* t) { return std::string("native_holds_code<") + t + "> : std::true_type"; };
    for (const char* t : {"RawCb", "MemCb", "ArrCb", "Nested", "Derived"})
    {
        hotc::TranslateResult r = hotc::translate(temp_options(dir, head + "    " + t + " v;\n" + tail));
        ASSERT_TRUE(r.ok) << t << ": " << r.diagnostics;
        EXPECT_TRUE(has(r.cpp, mark(t))) << t << "\n" << r.cpp;
    }
    for (const char* t : {"Plain", "DataPtr"})
    {
        hotc::TranslateResult r = hotc::translate(temp_options(dir, head + "    " + t + " v;\n" + tail));
        ASSERT_TRUE(r.ok) << t << ": " << r.diagnostics;
        EXPECT_FALSE(has(r.cpp, "native_holds_code<")) << t << "\n" << r.cpp;
    }
    hotc::TranslateResult v =
        hotc::translate(temp_options(dir, "using <vector>;\n" + head + "    std.vector<RawCb> v;\n" + tail));
    ASSERT_TRUE(v.ok) << v.diagnostics;
    EXPECT_TRUE(has(v.cpp, mark("RawCb"))) << v.cpp;
}

// A .cpp script has its includes in source order. The mark of a struct comes just before the first
// class that uses the struct: after the include of the struct, and before the first use.
TEST(NativeHoldsCode, CppMarkComesAfterTheIncludeOfTheStruct)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    write_file(dir.path() / "cb.h", "using Fn = int (*)(int);\nstruct RawCb { Fn fn; int tag; };\nstruct Plain { int a; };\n");
    hotc::TranslateOptions o = temp_options(dir, "#include \"w.h\"\n"
                                                 "class A : public Widget { public: int x = 0; };\n"
                                                 "#include \"cb.h\"\n"
                                                 "class B : public Widget { public: RawCb cb{}; Plain p{}; };\n"
                                                 "class C : public Widget { public: RawCb cb2{}; };\n");
    o.script_name = "t.cpp";
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const std::string mark = "native_holds_code<RawCb> : std::true_type";
    const size_t at = r.cpp.find(mark);
    ASSERT_NE(at, std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find(mark, at + 1), std::string::npos) << "one mark only";
    EXPECT_GT(at, r.cpp.find("#include \"cb.h\"")) << "after the include";
    EXPECT_LT(at, r.cpp.find("B_fields")) << "before the class that uses it";
    EXPECT_FALSE(has(r.cpp, "native_holds_code<Plain>"));
}

// The slot text has a line "field Cls::name type" for each field. The slot numbers do not change.
TEST(FieldRename, SlotTextHasFieldLines)
{
    hotc::TranslateResult r = gen("class A { int hp = 0; float[4] xs; void f() { } }\n", 1, "");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.slots_out, "A::f 1\n");
    EXPECT_TRUE(has(r.sigs_out, "field A::hp int32_t\n")) << r.sigs_out;
    EXPECT_TRUE(has(r.sigs_out, "field A::xs ")) << r.sigs_out;
    // The next generation keeps the slot numbers, and the field lines stay the same.
    hotc::TranslateResult r2 = gen("class A { int hp = 0; float[4] xs; void f() { } void g() { } }\n", 2,
        r.slots_out + r.sigs_out);
    ASSERT_TRUE(r2.ok) << r2.diagnostics;
    EXPECT_EQ(r2.slots_out, "A::f 1\nA::g 2\n");
    EXPECT_TRUE(r2.warnings.empty()) << r2.warnings;
}

// A removed field and an added field in the same class give a warning that says "rename".
TEST(FieldRename, RemovedAndAddedFieldWarn)
{
    hotc::TranslateResult g1 = gen("class A { int hp = 0; int mp = 0; }\n", 1, "");
    ASSERT_TRUE(g1.ok) << g1.diagnostics;
    hotc::TranslateResult g2 = gen("class A { int health = 0; int mp = 0; }\n", 2, g1.slots_out + g1.sigs_out);
    ASSERT_TRUE(g2.ok) << g2.diagnostics;
    EXPECT_EQ(std::count(g2.warnings.begin(), g2.warnings.end(), '\n'), 1) << g2.warnings;
    EXPECT_TRUE(has(g2.warnings, "looks like a rename")) << g2.warnings;
    EXPECT_TRUE(has(g2.warnings, "field 'hp' of class 'A' is removed and field 'health' is added"))
        << g2.warnings;
    EXPECT_TRUE(has(g2.warnings, "[alias(\"hp\")]")) << g2.warnings;
}

// An [alias] that names the old field gives no warning.
TEST(FieldRename, AliasGivesNoWarning)
{
    hotc::TranslateResult g1 = gen("class A { int hp = 0; }\n", 1, "");
    ASSERT_TRUE(g1.ok) << g1.diagnostics;
    hotc::TranslateResult g2 = gen("class A { [alias(\"hp\")] int health = 0; }\n", 2, g1.slots_out + g1.sigs_out);
    ASSERT_TRUE(g2.ok) << g2.diagnostics;
    EXPECT_TRUE(g2.warnings.empty()) << g2.warnings;
}

// A new field with no removed field gives no warning. A slot text with no field line gives none.
TEST(FieldRename, AddedFieldAndOldSlotTextGiveNoWarning)
{
    hotc::TranslateResult g1 = gen("class A { int hp = 0; void f() { } }\n", 1, "");
    ASSERT_TRUE(g1.ok) << g1.diagnostics;
    hotc::TranslateResult g2 = gen("class A { int hp = 0; int mp = 0; void f() { } }\n", 2, g1.slots_out + g1.sigs_out);
    ASSERT_TRUE(g2.ok) << g2.diagnostics;
    EXPECT_TRUE(g2.warnings.empty()) << g2.warnings;

    const size_t at = g1.sigs_out.find("field ");
    ASSERT_NE(at, std::string::npos);
    const std::string old_text = g1.slots_out + g1.sigs_out.substr(0, at); // no field line
    hotc::TranslateResult g3 = gen("class A { int health = 0; void f() { } }\n", 2, old_text);
    ASSERT_TRUE(g3.ok) << g3.diagnostics;
    EXPECT_TRUE(g3.warnings.empty()) << g3.warnings;
}

// A .cpp script gives the same warning.
TEST(FieldRename, CppScriptWarns)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    auto run = [&](const std::string& field, int generation, const std::string& slots_in)
    {
        hotc::TranslateOptions o = temp_options(
            dir, "#include \"w.h\"\nclass A : public Widget { public: int " + field + " = 0; };\n");
        o.script_name = "t.cpp";
        o.generation = generation;
        o.slots_in = slots_in;
        return hotc::translate(o);
    };
    const hotc::TranslateResult g1 = run("hp", 1, "");
    ASSERT_TRUE(g1.ok) << g1.diagnostics;
    EXPECT_TRUE(has(g1.sigs_out, "field A::hp int\n")) << g1.sigs_out;
    const hotc::TranslateResult g2 = run("health", 2, g1.slots_out + g1.sigs_out);
    ASSERT_TRUE(g2.ok) << g2.diagnostics;
    EXPECT_EQ(std::count(g2.warnings.begin(), g2.warnings.end(), '\n'), 1) << g2.warnings;
    EXPECT_TRUE(has(g2.warnings, "looks like a rename")) << g2.warnings;
}

// A removed function keeps its slot as a defunct entry. The reload applies live.
std::string two_functions(const std::string& sub_params)
{
    return "class Counter {\n    int n = 0;\n    void add(int x1) { this.n = this.n + x1; }\n"
           "    void sub(" + sub_params + ") { this.n = this.n - x1; }\n}\n";
}

TEST(DefunctSlots, RemovedFunctionKeepsItsSlot)
{
    const hotc::TranslateResult g1 = gen(two_functions("int x1"), 1, "");
    ASSERT_TRUE(g1.ok) << g1.diagnostics;
    const hotc::TranslateResult g2 = gen(counter("int x1", "this.n = this.n + x1;"), 2, g1.slots_out + g1.sigs_out);
    ASSERT_TRUE(g2.ok) << g2.diagnostics;
    EXPECT_EQ(g2.slots_out, g1.slots_out);
    EXPECT_TRUE(has(g2.cpp, "reinterpret_cast<hot::SlotFn>(&hot::StateBase::__removed)")) << g2.cpp;
    EXPECT_TRUE(has(g2.cpp, "SLOT_sub = ")) << g2.cpp;
    EXPECT_TRUE(has(g2.cpp, "SLOT_COUNT = 2")) << g2.cpp;
    EXPECT_TRUE(has(g2.sigs_out, "sig Counter::sub ")) << g2.sigs_out;
    EXPECT_FALSE(g2.slots_removed);
}

TEST(DefunctSlots, ReturnedFunctionReusesItsSlot)
{
    const hotc::TranslateResult g1 = gen(two_functions("int x1"), 1, "");
    ASSERT_TRUE(g1.ok) << g1.diagnostics;
    const hotc::TranslateResult g2 = gen(counter("int x1", "this.n = this.n + x1;"), 2, g1.slots_out + g1.sigs_out);
    ASSERT_TRUE(g2.ok) << g2.diagnostics;
    const hotc::TranslateResult g3 = gen(two_functions("int x1"), 3, g2.slots_out + g2.sigs_out);
    ASSERT_TRUE(g3.ok) << g3.diagnostics;
    EXPECT_EQ(g3.slots_out, g1.slots_out);
    EXPECT_FALSE(has(g3.cpp, "sub__thunk(")) << g3.cpp;
    EXPECT_FALSE(has(g3.cpp, "__removed")) << g3.cpp;
}

TEST(DefunctSlots, ReturnedFunctionWithANewSignatureEmitsAKeyedThunk)
{
    const hotc::TranslateResult g1 = gen(two_functions("int x1"), 1, "");
    ASSERT_TRUE(g1.ok) << g1.diagnostics;
    const hotc::TranslateResult g2 = gen(counter("int x1", "this.n = this.n + x1;"), 2, g1.slots_out + g1.sigs_out);
    ASSERT_TRUE(g2.ok) << g2.diagnostics;
    const hotc::TranslateResult g3 = gen(two_functions("int x1, int x2"), 3, g2.slots_out + g2.sigs_out);
    ASSERT_TRUE(g3.ok) << g3.diagnostics;
    EXPECT_EQ(g3.slots_out, g1.slots_out);
    EXPECT_TRUE(has(g3.cpp, "sub__thunk(")) << g3.cpp;
}

TEST(DefunctSlots, RemovedFamilyFunctionKeepsItsNumber)
{
    const std::string with_b = "class Base {\n    virtual void a() {}\n    void b() {}\n}\n"
                               "class Child : Base {\n    void c() {}\n}\n";
    const std::string no_b = "class Base {\n    virtual void a() {}\n}\n"
                             "class Child : Base {\n    void c() {}\n}\n";
    const hotc::TranslateResult g1 = gen(with_b, 1, "");
    ASSERT_TRUE(g1.ok) << g1.diagnostics;
    const hotc::TranslateResult g2 = gen(no_b, 2, g1.slots_out + g1.sigs_out);
    ASSERT_TRUE(g2.ok) << g2.diagnostics;
    EXPECT_EQ(g2.slots_out, g1.slots_out);
    EXPECT_TRUE(has(g2.cpp, "__removed")) << g2.cpp;
}

} // namespace
