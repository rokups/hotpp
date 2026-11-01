// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the printer of "await" and of "co_return".
#include "emit.h"
#include "support/emit_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace hotc::ast;
using namespace emit_test;

TEST(AsyncEmit, AwaitIsCoAwait)
{
    EXPECT_EQ(print(*un("await", id("t"))), "co_await t");
    EXPECT_EQ(print(*bin("+", un("await", id("t")), num("1"))), "co_await t + 1");
    EXPECT_EQ(print(*un("-", un("await", id("t")))), "-co_await t");
}

TEST(AsyncEmit, CoReturn)
{
    ReturnStmt r;
    r.expr = id("x");
    r.co = true;
    EXPECT_EQ(print(*make_stmt(std::move(r))), "co_return x;");
    ReturnStmt v;
    v.co = true;
    EXPECT_EQ(print(*make_stmt(std::move(v))), "co_return;");
    ReturnStmt p;
    p.expr = id("x");
    EXPECT_EQ(print(*make_stmt(std::move(p))), "return x;");
}
