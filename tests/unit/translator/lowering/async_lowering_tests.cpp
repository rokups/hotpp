// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the lowering of the body of an "async" method: co_return, co_await and the places
// where C++ does not allow co_await.
#include "support/lowering_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace lowering_test;

namespace
{
// The fixture with an async body.
struct AsyncFixture : Fixture
{
    AsyncFixture()
    {
        cx.async_body = true;
        param("x");
        param("t");
    }
};
} // namespace

TEST(AsyncLowering, ReturnIsCoReturn)
{
    AsyncFixture f;
    EXPECT_EQ(f.body("return x;"), "co_return x;");
    EXPECT_EQ(f.body("return;"), "co_return;");
}

TEST(AsyncLowering, PlainBodyKeepsReturn)
{
    Fixture f;
    f.param("x");
    EXPECT_EQ(f.body("return x;"), "return x;");
}

TEST(AsyncLowering, AwaitIsCoAwait)
{
    AsyncFixture f;
    EXPECT_EQ(f.body("var v = await t;"), "auto v = co_await t;");
    EXPECT_EQ(f.body("await t;"), "co_await t;");
    EXPECT_EQ(f.body("return await t;"), "co_return co_await t;");
    EXPECT_EQ(f.body("this.n = (await t) + 1;"), "__st->n = (co_await t) + 1;");
    EXPECT_TRUE(f.errors().empty());
}

TEST(AsyncLowering, ReturnInTryIsCoReturn)
{
    AsyncFixture f;
    EXPECT_EQ(f.body("try { return await t; } catch (...) { return 0; }"),
        "try { co_return co_await t; } catch (...) { co_return 0; }");
    EXPECT_TRUE(f.errors().empty());
}

TEST(AsyncLowering, AwaitInCatchIsError)
{
    AsyncFixture f;
    f.body("try { } catch (...) { await t; }");
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "1:23: 'await' is not supported inside a 'catch' block yet");
}

TEST(AsyncLowering, AwaitInFinallyIsError)
{
    AsyncFixture f;
    f.body("try { } finally { await t; }");
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "1:19: 'await' is not supported inside a 'finally' block yet");
}

TEST(AsyncLowering, AwaitInCoalesceRightSideIsError)
{
    AsyncFixture f;
    f.body("var v = x ?? await t;");
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "1:14: 'await' cannot be used inside this expression yet");
}

TEST(AsyncLowering, AwaitInCoalesceLeftSideIsFine)
{
    AsyncFixture f;
    f.body("var v = (await t) ?? x;");
    EXPECT_TRUE(f.errors().empty());
}
