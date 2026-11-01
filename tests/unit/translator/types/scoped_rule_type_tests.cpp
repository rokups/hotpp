// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the scoped stub rules: [self], "class : Base;" and the order of cpp_prelude.
#include "support/type_model_test_support.h"
#include "types.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;
using namespace type_model_test;

namespace
{

ast::Stub derived_stub(const std::string& base, const std::string& handle = "")
{
    ast::Stub s = stub(base, handle);
    s.derived = true;
    return s;
}

ast::ClassDef script_class(const std::string& name, const std::string& base, const std::string& prelude = "")
{
    ast::ClassDef c;
    c.name = name;
    c.base = base;
    c.cpp_prelude = prelude;
    return c;
}

std::vector<ast::ClassDef> classes_of(ast::ClassDef a, ast::ClassDef b = {})
{
    std::vector<ast::ClassDef> v;
    v.push_back(std::move(a));
    if (!b.name.empty())
        v.push_back(std::move(b));
    return v;
}

} // namespace

TEST(ScopedRuleType, SelfStopsInheritance)
{
    Fixture f;
    ast::Stub s = stub("Object", kShared);
    s.is_self = true;
    f.add({s});
    f.model.set_bases(bases_of({{"Item", {"Object"}}}));
    EXPECT_EQ(f.diags.items.size(), 0u);
    EXPECT_EQ(f.model.storage("Object"), "std::shared_ptr<Object>");
    EXPECT_EQ(f.model.lookup("Item").kind, Kind::Raw);
    EXPECT_TRUE(f.model.lookup("Item").owner.empty());
}

TEST(ScopedRuleType, DerivedRuleSkipsBase)
{
    Fixture f;
    f.add({derived_stub("Object", kShared)});
    f.model.set_bases(bases_of({{"Item", {"Object"}}}));
    EXPECT_EQ(f.model.lookup("Object").kind, Kind::Raw);
    EXPECT_EQ(f.model.storage("Item"), "std::shared_ptr<Item>");
    EXPECT_EQ(f.model.lookup("Item").owner, "Object");
}

TEST(ScopedRuleType, DerivedRuleBeatsExactStubOfBase)
{
    Fixture f;
    f.add({stub("Object", "Far<$T>"), derived_stub("Object", "Near<$T>")});
    f.model.set_bases(bases_of({{"Item", {"Object"}}}));
    EXPECT_EQ(f.diags.items.size(), 0u);
    EXPECT_EQ(f.model.storage("Object"), "Far<Object>");
    EXPECT_EQ(f.model.storage("Item"), "Near<Item>");
}

TEST(ScopedRuleType, ExactStubOfSubclassWins)
{
    Fixture f;
    f.add({derived_stub("Object", "Near<$T>"), stub("Item", "Own<$T>")});
    f.model.set_bases(bases_of({{"Item", {"Object"}}}));
    EXPECT_EQ(f.model.storage("Item"), "Own<Item>");
}

TEST(ScopedRuleType, ScriptClassInheritsDerivedRule)
{
    Fixture f;
    f.add({derived_stub("Object", "Near<$T>")});
    f.model.add_script_classes(classes_of(script_class("Hero", "Object")));
    EXPECT_EQ(f.model.storage("Hero"), "Near<Hero>");
}

TEST(ScopedRuleType, ScriptClassSkipsSelfStub)
{
    Fixture f;
    ast::Stub s = stub("Object", "Near<$T>");
    s.is_self = true;
    f.add({s});
    f.model.add_script_classes(classes_of(script_class("Hero", "Object")));
    EXPECT_EQ(f.model.storage("Hero"), "\x01std::shared_ptr<Hero>");
}

TEST(ScopedRuleType, DerivedRawRuleBlocksDefaultHandle)
{
    Fixture f;
    ast::Stub all = stub("*");
    all.default_handle = kShared;
    ast::Stub raw = derived_stub("Base");
    f.add({all, raw});
    f.model.set_bases(bases_of({{"Item", {"Base"}}}));
    f.model.set_classes([](const std::string&) { return TypeModel::ClassFacts{true, false, false}; });
    EXPECT_EQ(f.model.lookup("Item").kind, Kind::Raw);
    EXPECT_EQ(f.model.lookup("Other").kind, Kind::Handle);
}

TEST(ScopedRuleType, DerivedErrors)
{
    Fixture f;
    ast::Stub v = derived_stub("A");
    v.is_value = true;
    ast::Stub d = derived_stub("B");
    d.default_handle = kShared;
    f.add({v, d, derived_stub("C"), derived_stub("C")});
    ASSERT_EQ(f.diags.items.size(), 3u);
    EXPECT_EQ(f.diags.items[0].msg, "'value' is not allowed on 'class : Base': it breaks polymorphic types");
    EXPECT_EQ(f.diags.items[1].msg, "'default_handle' is valid only on 'class *'");
    EXPECT_EQ(f.diags.items[2].msg, "duplicate stub for ': C'");
}

TEST(ScopedRuleType, OneStructStar)
{
    Fixture f;
    ast::Stub s;
    s.name = "*";
    s.is_struct = true;
    f.add({s, s});
    ASSERT_EQ(f.diags.items.size(), 1u);
    EXPECT_EQ(f.diags.items[0].msg, "only one 'struct *' is allowed in a file");
}

TEST(ScopedRuleType, PreludesOrder)
{
    Fixture f;
    ast::Stub all = stub("*");
    all.cpp_prelude = "all";
    ast::Stub st;
    st.name = "*";
    st.is_struct = true;
    st.cpp_prelude = "struct_all";
    ast::Stub obj = stub("Object", kShared);
    obj.cpp_prelude = "object";
    ast::Stub mid = stub("Mid");
    mid.is_self = true;
    mid.cpp_prelude = "mid_self";
    ast::Stub dobj = derived_stub("Object");
    dobj.cpp_prelude = "below_object";
    ast::Stub dmid = derived_stub("Mid");
    dmid.cpp_prelude = "below_mid";
    f.add({all, st, obj, mid, dobj, dmid});
    EXPECT_EQ(f.diags.items.size(), 0u);
    f.model.set_bases(bases_of({{"Mid", {"Object"}}}));
    ast::ClassDef pt = script_class("Pt", "", "pt");
    pt.is_value = true;
    f.model.add_script_classes(classes_of(script_class("Hero", "Mid", "own"), std::move(pt)));
    const std::vector<std::string> want{"all", "object", "below_object", "below_mid", "own"};
    EXPECT_EQ(f.model.preludes("Hero"), want);
    const std::vector<std::string> want_pt{"struct_all", "pt"};
    EXPECT_EQ(f.model.preludes("Pt"), want_pt);
}
