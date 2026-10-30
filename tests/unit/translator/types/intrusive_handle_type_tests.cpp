// Tests for the type model: the intrusive handles.
#include "support/type_model_test_support.h"
#include "types.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

using namespace hotc;
using namespace type_model_test;

namespace
{

ast::Stub intrusive_stub(const std::string& name, const std::string& handle = "")
{
    ast::Stub s = stub(name, handle);
    s.intrusive_add = "AddRef";
    s.intrusive_release = "Release";
    return s;
}

} // namespace

TEST(IntrusiveHandleType, IntrusiveExact)
{
    Fixture f;
    f.add({intrusive_stub("Object", "$T*")});
    const std::string st = "hot::Intrusive<Object, &Object::AddRef, &Object::Release>";
    EXPECT_EQ(f.model.storage("Object"), st);
    EXPECT_EQ(f.model.param("Object"), "const " + st + "&");
    EXPECT_EQ(f.model.make("Object", "1, 2"), st + "(new Object(1, 2))");
    TypeModel::Callee c = f.model.make_callee("Object");
    EXPECT_EQ(c.text, st);
    EXPECT_TRUE(c.wrap_new);
    TypeRule r = f.model.lookup("Object");
    EXPECT_EQ(r.kind, Kind::Handle);
    EXPECT_EQ(r.intrusive_add, "AddRef");
    EXPECT_EQ(r.intrusive_release, "Release");
    EXPECT_TRUE(f.diags.items.empty());
}

TEST(IntrusiveHandleType, IntrusiveWithoutHandle)
{
    Fixture f;
    f.add({intrusive_stub("Object")});
    EXPECT_EQ(f.model.storage("Object"), "hot::Intrusive<Object, &Object::AddRef, &Object::Release>");
    EXPECT_TRUE(f.diags.items.empty());
}

TEST(IntrusiveHandleType, IntrusiveInherited)
{
    Fixture f;
    f.add({intrusive_stub("Object")});
    f.model.set_bases(bases_of({{"Item", {"Object"}}}));
    const std::string st = "hot::Intrusive<Item, &Object::AddRef, &Object::Release>";
    EXPECT_EQ(f.model.storage("Item"), st);
    EXPECT_EQ(f.model.make("Item", ""), st + "(new Item())");
    EXPECT_EQ(f.model.lookup("Item").intrusive_add, "AddRef");
}

TEST(IntrusiveHandleType, IntrusiveErrors)
{
    Fixture f;
    ast::Stub v = intrusive_stub("A");
    v.is_value = true;
    f.add({v});
    ASSERT_EQ(f.diags.items.size(), 1u);
    EXPECT_NE(f.diags.items[0].msg.find("'value' and 'intrusive' are exclusive"), std::string::npos);

    f.add({intrusive_stub("B", "std::shared_ptr<$T>")});
    ASSERT_EQ(f.diags.items.size(), 2u);
    EXPECT_NE(f.diags.items[1].msg.find("'intrusive' needs handle(\"$T*\")"), std::string::npos);
    EXPECT_EQ(f.model.storage("B"), "B");
}
