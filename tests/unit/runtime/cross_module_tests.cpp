// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Unit tests of the cross-module helpers (field, mod_var, call_fn, class_ref, unit_ref, construct)
// with hand-built records and a test resolver. They need no backend and no script.
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

namespace
{

struct V1
{
    int a;
    float b;
};
// The layout after a reload: the order changes, so the offsets change. The field "c" is new.
struct V2
{
    float b;
    int pad;
    int a;
    int c;
};

template <class T> hot::FieldInfo mk(const char* name, uint32_t off)
{
    return {name, nullptr, hot::type_key<T>(), off, sizeof(T), alignof(T), hot::field_flags<T>(), nullptr,
        hot::move_field<T>, hot::field_destroyer<T>()};
}

hot::FieldInfo fields1[] = {mk<int>("a", offsetof(V1, a)), mk<float>("b", offsetof(V1, b))};
hot::StateLayout layout1{"V1", sizeof(V1), alignof(V1), 2, fields1};
hot::FieldInfo fields2[] = {
    mk<float>("b", offsetof(V2, b)), mk<int>("a", offsetof(V2, a)), mk<int>("c", offsetof(V2, c))};
hot::StateLayout layout2{"V2", sizeof(V2), alignof(V2), 3, fields2};

hot::SlotFn no_slots[1] = {nullptr};

struct TestShell : hot::ShellBase
{
    int made_with = 0;
};

// The constructor "(int)" of the test class. It stores the argument in the shell.
hot::Instance make_int(void** args)
{
    auto* s = new TestShell;
    s->made_with = *static_cast<int*>(args[0]);
    return {nullptr, s};
}
// The constructor "(int&)": it can change its argument, so a const argument is wrong.
hot::Instance make_int_ref(void** args)
{
    auto* s = new TestShell;
    s->made_with = ++*static_cast<int*>(args[0]);
    return {nullptr, s};
}
hot::ShellCore* new_default_shell()
{
    auto* s = new TestShell;
    s->made_with = -1;
    return s;
}

hot::CtorEntry ctors[] = {
    {hot::ctor_key<int>(), &make_int, 0},
    {hot::ctor_key<int&, int&>(), &make_int_ref, hot::ctor_write_mask<int&, int&>()},
    {nullptr, nullptr, 0},
};

hot::ClassRecord make_record(const char* name, const hot::StateLayout* layout)
{
    hot::ClassRecord r{name, 1, no_slots, 1, layout, nullptr, "", 0};
    r.ctors = ctors;
    r.new_default = &new_default_shell;
    return r;
}

// The test resolvers.
hot::ClassRecord g_class = make_record("B", &layout1);
int g_class_lookups = 0;
hot::ClassRecord* resolve_test_class(const char* name)
{
    ++g_class_lookups;
    return std::strcmp(name, "B") == 0 ? &g_class : nullptr;
}
hot::UnitInfo g_unit;
bool resolve_test_unit(const char* name, hot::UnitInfo& out)
{
    if (std::strcmp(name, "X") != 0)
        return false;
    out = g_unit;
    return true;
}

int add_fn(int a, int b)
{
    return a + b;
}
int sub_fn(int a, int b)
{
    return a - b;
}

class CrossModule : public ::testing::Test
{
protected:
    void SetUp() override
    {
        g_class = make_record("B", &layout1);
        g_class_lookups = 0;
        g_unit = {};
        hot::detail::set_class_resolver(&resolve_test_class);
        hot::detail::set_unit_resolver(&resolve_test_unit);
    }
    void TearDown() override
    {
        hot::detail::set_class_resolver(nullptr);
        hot::detail::set_unit_resolver(nullptr);
    }
};

TEST_F(CrossModule, FieldReadsAndWritesByName)
{
    V1 block{3, 1.5f};
    TestShell sh;
    hot::ClassRecord rec = make_record("B", &layout1);
    sh.cls = &rec;
    sh.state = &block;
    hot::FieldSite site;
    EXPECT_EQ(hot::field<int>(&sh, site, "a"), 3);
    hot::field<int>(&sh, site, "a") = 9;
    EXPECT_EQ(block.a, 9);
    EXPECT_EQ(site.layout, &layout1);
    EXPECT_EQ(site.offset, offsetof(V1, a));
}

TEST_F(CrossModule, FieldOffsetResolvesAgainAfterALayoutChange)
{
    V1 old_block{3, 1.5f};
    V2 new_block{2.5f, 0, 7, 8};
    TestShell sh;
    hot::ClassRecord rec = make_record("B", &layout1);
    sh.cls = &rec;
    sh.state = &old_block;
    hot::FieldSite site;
    EXPECT_EQ(hot::field<int>(&sh, site, "a"), 3);
    // A reload: the record gets the new layout and the object gets the new block.
    rec.layout = &layout2;
    sh.state = &new_block;
    EXPECT_EQ(hot::field<int>(&sh, site, "a"), 7);
    EXPECT_EQ(site.layout, &layout2);
    EXPECT_EQ(site.offset, offsetof(V2, a));
    // The same layout pointer uses the cached offset: a wrong cached value shows it.
    site.offset = offsetof(V2, c);
    EXPECT_EQ(hot::field<int>(&sh, site, "a"), 8);
}

TEST_F(CrossModule, FieldTypeMismatchThrows)
{
    V1 block{3, 1.5f};
    TestShell sh;
    hot::ClassRecord rec = make_record("B", &layout1);
    sh.cls = &rec;
    sh.state = &block;
    hot::FieldSite site;
    EXPECT_THROW(hot::field<float>(&sh, site, "a"), hot::ScriptError);
    EXPECT_EQ(site.layout, nullptr); // a failed resolve does not fill the site
    // A change of the type at a reload: the cached site resolves again and throws.
    EXPECT_EQ(hot::field<float>(&sh, site, "b"), 1.5f);
    rec.layout = &layout2;
    V2 new_block{2.5f, 0, 7, 8};
    sh.state = &new_block;
    EXPECT_EQ(hot::field<float>(&sh, site, "b"), 2.5f);
    // A site has one type. A new site with another type for the same field throws.
    hot::FieldSite other;
    EXPECT_THROW(hot::field<double>(&sh, other, "b"), hot::ScriptError);
}

TEST_F(CrossModule, FieldMissingNameThrows)
{
    V1 block{3, 1.5f};
    TestShell sh;
    hot::ClassRecord rec = make_record("B", &layout1);
    sh.cls = &rec;
    sh.state = &block;
    hot::FieldSite site;
    try
    {
        hot::field<int>(&sh, site, "nope");
        FAIL() << "no exception";
    }
    catch (const hot::ScriptError& e)
    {
        EXPECT_NE(std::string(e.what()).find("nope"), std::string::npos);
        EXPECT_NE(std::string(e.what()).find("B"), std::string::npos);
    }
    EXPECT_THROW(hot::field<int>(nullptr, site, "a"), hot::NullReference);
}

TEST_F(CrossModule, ModVarUsesTheBlockOfTheUnit)
{
    V1 block1{4, 0.5f};
    g_unit.layout = &layout1;
    g_unit.block = &block1;
    hot::FieldSite site;
    EXPECT_EQ(hot::mod_var<int>(hot::unit_ref("X"), site, "a"), 4);
    hot::mod_var<int>(hot::unit_ref("X"), site, "a") = 5;
    EXPECT_EQ(block1.a, 5);
    // A reload: a new layout and a new block.
    V2 block2{1.0f, 0, 6, 0};
    g_unit.layout = &layout2;
    g_unit.block = &block2;
    EXPECT_EQ(hot::mod_var<int>(hot::unit_ref("X"), site, "a"), 6);
    EXPECT_EQ(site.offset, offsetof(V2, a));
    hot::FieldSite wrong_type, missing;
    EXPECT_THROW(hot::mod_var<double>(hot::unit_ref("X"), wrong_type, "b"), hot::ScriptError);
    EXPECT_THROW(hot::mod_var<int>(hot::unit_ref("X"), missing, "missing"), hot::ScriptError);
    g_unit.block = nullptr; // a unit without module variables
    EXPECT_THROW(hot::mod_var<int>(hot::unit_ref("X"), site, "a"), hot::ScriptError);
    EXPECT_THROW(hot::unit_ref("Y"), hot::ScriptError);
}

TEST_F(CrossModule, CallFnCallsThroughTheTable)
{
    hot::Registry reg;
    reg.add_fn("f", typeid(int(int, int)).name(), reinterpret_cast<void*>(&add_fn));
    ASSERT_NE(reg.find_fn("f"), nullptr);
    EXPECT_EQ(reg.find_fn("g"), nullptr);
    g_unit.fns = &reg.fn_table();
    hot::FnSite site;
    EXPECT_EQ((hot::call_fn<int(int, int)>(hot::unit_ref("X"), site, "f", 5, 3)), 8);
    EXPECT_EQ(site.table_id, reg.fn_table().id);
    // A table swap: a new table with another function. The site finds it by the table id.
    hot::Registry reg2;
    reg2.add_fn("f", typeid(int(int, int)).name(), reinterpret_cast<void*>(&sub_fn));
    EXPECT_NE(reg.fn_table().id, reg2.fn_table().id);
    g_unit.fns = &reg2.fn_table();
    EXPECT_EQ((hot::call_fn<int(int, int)>(hot::unit_ref("X"), site, "f", 5, 3)), 2);
}

TEST_F(CrossModule, CallFnSignatureMismatchThrows)
{
    hot::Registry reg;
    reg.add_fn("f", typeid(int(int, int)).name(), reinterpret_cast<void*>(&add_fn));
    g_unit.fns = &reg.fn_table();
    hot::FnSite site;
    EXPECT_THROW((hot::call_fn<float(int, int)>(hot::unit_ref("X"), site, "f", 5, 3)), hot::ScriptError);
    EXPECT_THROW((hot::call_fn<int(int)>(hot::unit_ref("X"), site, "f", 5)), hot::ScriptError);
    EXPECT_THROW((hot::call_fn<int(int, int)>(hot::unit_ref("X"), site, "missing", 5, 3)), hot::ScriptError);
    EXPECT_EQ(site.fn, nullptr);
    // A reload changes the signature: the cached site resolves again and throws.
    EXPECT_EQ((hot::call_fn<int(int, int)>(hot::unit_ref("X"), site, "f", 1, 2)), 3);
    hot::Registry reg2;
    reg2.add_fn("f", typeid(float(int, int)).name(), reinterpret_cast<void*>(&add_fn));
    g_unit.fns = &reg2.fn_table();
    EXPECT_THROW((hot::call_fn<int(int, int)>(hot::unit_ref("X"), site, "f", 1, 2)), hot::ScriptError);
    g_unit.fns = nullptr;
    EXPECT_THROW((hot::call_fn<int(int, int)>(hot::unit_ref("X"), site, "f", 1, 2)), hot::ScriptError);
}

TEST_F(CrossModule, ClassRefCachesTheRecord)
{
    hot::ClassRef a = hot::class_ref("B");
    hot::ClassRef b = hot::class_ref("B");
    EXPECT_EQ(a.record, &g_class);
    EXPECT_EQ(b.record, &g_class);
    EXPECT_EQ(g_class_lookups, 1);
    EXPECT_THROW(hot::class_ref("Nope"), hot::ScriptError);
    // A canonical record replaces a generation record.
    hot::ClassRecord canon = make_record("C", &layout1);
    hot::ClassRecord gen = make_record("C", &layout2);
    gen.canonical = &canon;
    static hot::ClassRecord* g_gen;
    g_gen = &gen;
    hot::detail::set_class_resolver([](const char*) { return g_gen; }); // also clears the cache
    EXPECT_EQ(hot::class_ref("C").record, &canon);
}

TEST_F(CrossModule, ClassRefWithoutAResolverThrows)
{
    hot::detail::set_class_resolver(nullptr);
    EXPECT_THROW(hot::class_ref("B"), hot::ScriptError);
}

TEST_F(CrossModule, ConstructUsesTheCtorKey)
{
    std::unique_ptr<TestShell> s(hot::construct<TestShell>(hot::class_ref("B"), 42));
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->made_with, 42);
    // No argument and no "()" constructor: the default constructor of the record runs.
    std::unique_ptr<TestShell> d(hot::construct<TestShell>(hot::class_ref("B")));
    EXPECT_EQ(d->made_with, -1);
    // A reference parameter writes to the argument.
    int x = 1, y = 2;
    std::unique_ptr<TestShell> r(hot::construct<TestShell>(hot::class_ref("B"), x, y));
    EXPECT_EQ(r->made_with, 2);
    EXPECT_EQ(x, 2);
}

TEST_F(CrossModule, ConstructWithoutAMatchingKeyThrows)
{
    EXPECT_THROW(hot::construct<TestShell>(hot::class_ref("B"), std::string("x")), hot::ScriptError);
    EXPECT_THROW(hot::construct<TestShell>(hot::class_ref("B"), 1, 2, 3), hot::ScriptError);
    // A const argument for a parameter that can change it.
    const int cx = 1;
    int y = 2;
    EXPECT_THROW(hot::construct<TestShell>(hot::class_ref("B"), cx, y), hot::ScriptError);
    EXPECT_EQ(y, 2);
}

} // namespace
