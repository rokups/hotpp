// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the constructor table: ClassEntry::construct and Loader::create_as make a script
// object with constructor arguments of the host, with the backend that HOT_BACKEND selected.
#include "hot/backend.h"
#include "support/backend_fixture.h"
#include "support/reload_assert.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{

using test_support::kDt;
using test_support::Session;
using test_support::source;

template <class T> T* fld(const hot::Instance& inst, const char* name)
{
    return hot::field_ptr<T>(inst.shell, name);
}

// The text without spaces and without the MSVC marks "__ptr64" and "__cdecl".
// The compilers print the same type with different spaces: "int (*)(int), float" and
// "int (__cdecl*__ptr64)(int),float". The order and the content of the types stay strict.
std::string squeeze(std::string text)
{
    for (const char* mark : {"__ptr64", "__cdecl"})
        for (size_t at; (at = text.find(mark)) != std::string::npos;)
            text.erase(at, std::string(mark).size());
    text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
    return text;
}

// The engine owns an object of the factory: the test deletes it.
std::unique_ptr<Actor> own(const hot::Instance& inst)
{
    return std::unique_ptr<Actor>(static_cast<Actor*>(inst.native));
}

int times_two(int x)
{
    return x * 2;
}

// Loads ctor_table_v1.hot. "s.inst" is a Plain object.
void start_v1(hot::Backend& backend, Session& s)
{
    hot::LoadResult v1 = backend.load(source("ctor_table_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Plain"));
}

// A class without a constructor has the entry "()". Without arguments, construct makes the
// object with it.
TEST_P(BackendTests, CtorTableNoArgumentConstructor)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start_v1(*backend, s));
    const hot::ClassEntry* plain = s.loader.find("Plain");
    ASSERT_NE(plain, nullptr);
    EXPECT_TRUE(plain->has_constructor<>());
    EXPECT_EQ(hot::constructor_list(*plain), "()");
    hot::Instance a = plain->construct();
    auto oa = own(a);
    EXPECT_EQ(*fld<int>(a, "n"), 3);
}

// Each overload: a value, a pointer, a reference, a "&&" and an alias type.
TEST_P(BackendTests, CtorTableSelectsTheOverloadByTheArgumentTypes)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start_v1(*backend, s));
    const hot::ClassEntry* made = s.loader.find("Made");
    ASSERT_NE(made, nullptr);

    hot::Instance a = made->construct();
    auto oa = own(a);
    EXPECT_EQ(*fld<int>(a, "how"), 1);

    hot::Instance b = made->construct(5); // a value
    auto ob = own(b);
    EXPECT_EQ(*fld<int>(b, "how"), 2);
    EXPECT_EQ(*fld<int>(b, "value"), 5);

    Actor other("other");
    hot::Instance c = made->construct(&other); // a pointer
    auto oc = own(c);
    EXPECT_EQ(*fld<int>(c, "how"), 3);
    EXPECT_EQ(*fld<std::string>(c, "text"), "other");

    int out = 0;
    hot::Instance d = made->construct(out, 4); // a reference: the constructor writes "out"
    auto od = own(d);
    EXPECT_EQ(*fld<int>(d, "how"), 4);
    EXPECT_EQ(out, 8);

    std::string text = "moved";
    hot::Instance e = made->construct(std::move(text)); // a "&&" parameter
    auto oe = own(e);
    EXPECT_EQ(*fld<int>(e, "how"), 5);
    EXPECT_EQ(*fld<std::string>(e, "text"), "moved");

    // The parameter type is the alias IntFn. "int (*)(int)" gives the same key.
    int (*fn)(int) = &times_two;
    hot::Instance f = made->construct(fn, 1.0f);
    auto of = own(f);
    EXPECT_EQ(*fld<int>(f, "how"), 6);
    EXPECT_EQ(*fld<int>(f, "value"), 6);

    // A const argument and a lvalue argument give the key of the decayed type.
    const int five = 5;
    hot::Instance g = made->construct(five);
    auto og = own(g);
    EXPECT_EQ(*fld<int>(g, "how"), 2);
}

// No constructor matches: the error lists the constructors of the class.
TEST_P(BackendTests, CtorTableMismatchListsTheConstructors)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start_v1(*backend, s));
    const hot::ClassEntry* made = s.loader.find("Made");
    ASSERT_NE(made, nullptr);
    EXPECT_FALSE(made->has_constructor<double>());
    try
    {
        made->construct(2.5); // double: no implicit conversion to int
        FAIL() << "construct did not throw";
    }
    catch (const std::invalid_argument& e)
    {
        const std::string what = squeeze(e.what());
        EXPECT_NE(what.find(squeeze("'Made' has no constructor (double)")), std::string::npos) << what;
        for (const char* ctor : {"()", "(int)", "(Actor*)", "(int,int)", "(int(*)(int),float)"})
            EXPECT_NE(what.find(squeeze(ctor)), std::string::npos) << ctor << " in " << what;
    }
    // A const argument for a "T&" parameter: the constructor could change it.
    const int out = 0;
    EXPECT_THROW(made->construct(out, 4), std::invalid_argument);
    // An unknown class.
    EXPECT_THROW(s.loader.create_as<Actor>("Nobody", 1), std::runtime_error);
}

// Loader::create_as gives the native base. After a reload the constructor of the new
// generation runs, and a new overload is in the table.
TEST_P(BackendTests, CtorTableUsesTheCurrentGeneration)
{
    hot::LoadResult v2 = backend->load(source("ctor_table_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(start_v1(*backend, s));
    s.commit_at_frame_boundary();
    const hot::ClassEntry* made = s.loader.find("Made");
    ASSERT_NE(made, nullptr);
    std::unique_ptr<Actor> a(s.loader.create_as<Actor>("Made", 5));
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->Name, "made");
    hot::Instance b = made->construct(5);
    auto ob = own(b);
    EXPECT_EQ(*fld<int>(b, "how"), 2);
    EXPECT_FALSE((made->has_constructor<int, int, int>()));

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt); // the commit happens at the end of this frame
    ASSERT_FALSE(s.commit_failed);

    hot::Instance c = made->construct(5);
    auto oc = own(c);
    EXPECT_EQ(*fld<int>(c, "how"), 20);
    EXPECT_EQ(*fld<int>(c, "value"), 105);
    hot::Instance d = made->construct(2, 3, 4);
    auto od = own(d);
    EXPECT_EQ(*fld<int>(d, "how"), 7);
    EXPECT_EQ(*fld<int>(d, "value"), 9);
}

// A C++ script class: an alias type and a "&&" of a move-only type.
TEST_P(BackendTests, CtorTableOfACppScript)
{
    hot::LoadResult v1 = backend->load(source("cpp_ctor_table.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "CppMade"));
    const hot::ClassEntry* made = s.loader.find("CppMade");
    ASSERT_NE(made, nullptr);
    EXPECT_EQ(*fld<int>(s.inst, "how"), 1);

    hot::Instance a = made->construct(2.5f); // Meters is float
    auto oa = own(a);
    EXPECT_EQ(*fld<int>(a, "how"), 2);
    EXPECT_EQ(*fld<float>(a, "meters"), 2.5f);

    auto p = std::make_unique<int>(7);
    hot::Instance b = made->construct(std::move(p));
    auto ob = own(b);
    EXPECT_EQ(*fld<int>(b, "how"), 3);
    EXPECT_EQ(*fld<int>(b, "value"), 7);
    EXPECT_EQ(p, nullptr); // the constructor took the object

}

} // namespace
