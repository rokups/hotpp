// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of hot::front, hot::back, hot::top, hot::pop, hot::pop_back, hot::pop_front and hot::at
// (container_ops.h), and of hot::detail::rethrow_as_script_error (errors.h).
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <deque>
#include <list>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <queue>
#include <stack>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST(ContainerOps, EmptyContainersThrowIndexOutOfRange)
{
    std::vector<int> v;
    std::deque<int> d;
    std::list<int> l;
    std::string s;
    std::queue<int> q;
    std::stack<int> st;
    std::priority_queue<int> pq;
    EXPECT_THROW(hot::front(v), hot::IndexOutOfRange);
    EXPECT_THROW(hot::back(v), hot::IndexOutOfRange);
    EXPECT_THROW(hot::pop_back(v), hot::IndexOutOfRange);
    EXPECT_THROW(hot::front(d), hot::IndexOutOfRange);
    EXPECT_THROW(hot::pop_front(d), hot::IndexOutOfRange);
    EXPECT_THROW(hot::pop_back(d), hot::IndexOutOfRange);
    EXPECT_THROW(hot::front(l), hot::IndexOutOfRange);
    EXPECT_THROW(hot::pop_front(l), hot::IndexOutOfRange);
    EXPECT_THROW(hot::back(s), hot::IndexOutOfRange);
    EXPECT_THROW(hot::pop_back(s), hot::IndexOutOfRange);
    EXPECT_THROW(hot::front(q), hot::IndexOutOfRange);
    EXPECT_THROW(hot::back(q), hot::IndexOutOfRange);
    EXPECT_THROW(hot::pop(q), hot::IndexOutOfRange);
    EXPECT_THROW(hot::top(st), hot::IndexOutOfRange);
    EXPECT_THROW(hot::pop(st), hot::IndexOutOfRange);
    EXPECT_THROW(hot::top(pq), hot::IndexOutOfRange);
    EXPECT_THROW(hot::pop(pq), hot::IndexOutOfRange);
    EXPECT_THROW(hot::front(v), hot::ScriptError); // IndexOutOfRange is a ScriptError
}

TEST(ContainerOps, NonEmptyContainersWork)
{
    std::vector<int> v{1, 2, 3};
    EXPECT_EQ(hot::front(v), 1);
    EXPECT_EQ(hot::back(v), 3);
    hot::back(v) = 9; // a reference to the element
    EXPECT_EQ(v[2], 9);
    hot::pop_back(v);
    EXPECT_EQ(v.size(), 2u);
    std::deque<int> d{4, 5};
    hot::pop_front(d);
    EXPECT_EQ(hot::front(d), 5);
    std::stack<int> st;
    st.push(7);
    EXPECT_EQ(hot::top(st), 7);
    hot::pop(st);
    EXPECT_TRUE(st.empty());
    std::queue<int> q;
    q.push(1);
    q.push(2);
    EXPECT_EQ(hot::back(q), 2);
    hot::pop(q);
    EXPECT_EQ(hot::front(q), 2);
    std::priority_queue<int> pq;
    pq.push(3);
    pq.push(8);
    EXPECT_EQ(hot::top(pq), 8);
}

TEST(ContainerOps, PointerHandleAndTemporary)
{
    std::vector<int> v;
    std::vector<int>* p = &v;
    EXPECT_THROW(hot::back(p), hot::IndexOutOfRange);
    v.push_back(5);
    EXPECT_EQ(hot::back(p), 5);
    std::vector<int>* null = nullptr;
    EXPECT_THROW(hot::back(null), hot::NullReference);
    auto sp = std::make_shared<std::vector<int>>(std::vector<int>{1, 2});
    EXPECT_EQ(hot::front(sp), 1);
    EXPECT_EQ(hot::back(std::vector<std::string>{"a", "b"}), "b"); // a copy: the temporary is gone
    EXPECT_THROW(hot::back(std::vector<int>{}), hot::IndexOutOfRange);
}

namespace
{
struct NoEmpty
{
    int back() { return 4; }
};
// A native type that has empty() and a pop() with a result.
struct Bag
{
    std::vector<int> items;
    bool empty() const { return items.empty(); }
    int pop()
    {
        const int v = items.back();
        items.pop_back();
        return v;
    }
};
} // namespace

TEST(ContainerOps, PopKeepsTheResultOfANativeType)
{
    Bag b;
    b.items = {1, 2};
    EXPECT_EQ(hot::pop(b), 2);
    EXPECT_EQ(hot::pop(b), 1);
    EXPECT_THROW(hot::pop(b), hot::IndexOutOfRange);
}

TEST(ContainerOps, ViewTemporaryGivesTheRealElement)
{
    std::vector<int> v{1, 2, 3};
    auto make = [&v] { return hot::span<int>(v.data(), v.size()); };
    hot::back(make()) = 9;
    hot::front(make()) = 7;
    EXPECT_EQ(v[2], 9);
    EXPECT_EQ(v[0], 7);
    std::string s = "abc";
    EXPECT_EQ(&hot::back(std::string_view(s)), &std::as_const(s).back());
    EXPECT_THROW(hot::back(hot::span<int>(v.data(), std::size_t(0))), hot::IndexOutOfRange);
}

TEST(ContainerOps, TypeWithoutEmptyKeepsTheMember)
{
    NoEmpty n;
    EXPECT_EQ(hot::back(n), 4);
}

TEST(ContainerOps, AtMapsStdOutOfRange)
{
    std::map<std::string, int> m{{"a", 1}};
    EXPECT_EQ(hot::at(m, std::string("a")), 1);
    try
    {
        hot::at(m, std::string("zz"));
        FAIL();
    }
    catch (const hot::ScriptError& e)
    {
        EXPECT_NE(std::string(e.what()).find("key not found"), std::string::npos) << e.what();
    }
    std::vector<int> v{1, 2};
    EXPECT_EQ(hot::at(v, 1), 2);
    EXPECT_THROW(hot::at(v, 2), hot::IndexOutOfRange);
}

TEST(ContainerOps, RethrowAsScriptError)
{
    auto convert = [](auto thrower)
    {
        try
        {
            try
            {
                thrower();
            }
            catch (const std::exception&)
            {
                hot::detail::rethrow_as_script_error();
            }
        }
        catch (const hot::ScriptError& e)
        {
            return std::string(e.what()) + "|" + (dynamic_cast<const hot::NativeError*>(&e) ? "native" : "script");
        }
        return std::string("none");
    };
    const std::string a = convert([] { throw std::out_of_range("bad key"); });
    EXPECT_NE(a.find("std::out_of_range"), std::string::npos) << a;
    EXPECT_NE(a.find("bad key"), std::string::npos) << a;
    EXPECT_NE(a.find("|native"), std::string::npos) << a;
    EXPECT_NE(convert([] { throw std::bad_optional_access(); }).find("|native"), std::string::npos);
    EXPECT_NE(convert([] { throw std::bad_alloc(); }).find("|native"), std::string::npos);
    // A ScriptError passes through with its own type.
    EXPECT_EQ(convert([] { throw hot::ScriptError("mine"); }), "mine|script");
    EXPECT_EQ(convert([] { throw hot::IndexOutOfRange(1, 0); }).find("|native"), std::string::npos);
}
