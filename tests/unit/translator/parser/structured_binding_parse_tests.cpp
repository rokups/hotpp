// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the structured bindings: "var (a, b) = e;" and "foreach (var (k, v) in c)".
#include "emit.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/statement_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;
using namespace stmt_test;

TEST(StructuredBindingParse, LocalDeclarations)
{
    EXPECT_EQ(round_trip("var (a, b) = f();"), "auto [a, b] = f();");
    EXPECT_EQ(round_trip("var (a, b, c) = f();"), "auto [a, b, c] = f();");
    EXPECT_EQ(round_trip("ref (a, b) = x;"), "auto& [a, b] = x;");
    EXPECT_EQ(round_trip("var(a,b)=f();"), "auto [a, b] = f();");
    EXPECT_EQ(round_trip("var ( a , b ) = f();"), "auto [a, b] = f();");
}

TEST(StructuredBindingParse, Foreach)
{
    EXPECT_EQ(round_trip("foreach (var (k, v) in m) f(k, v);"), "for (auto [k, v] : m) f(k, v);");
    EXPECT_EQ(round_trip("foreach (ref (k, v) in m) f(k, v);"), "for (auto& [k, v] : m) f(k, v);");
    EXPECT_EQ(
        round_trip("foreach (var (i, x) in enumerate(xs)) f(i, x);"), "for (auto [i, x] : enumerate(xs)) f(i, x);");
    EXPECT_EQ(round_trip("foreach (int i in range(n)) f(i);"), "for (int32_t i : range(n)) f(i);");
}

TEST(StructuredBindingParse, ErrorsOfMalformedBindings)
{
    EXPECT_EQ(errors_of(Parsed("var (a) = f();")),
        (std::vector<std::string>{"1:1: a structured binding needs two or more names"}));
    EXPECT_EQ(
        errors_of(Parsed("var (a, b);")), (std::vector<std::string>{"1:1: a structured binding needs an initializer"}));
    EXPECT_EQ(errors_of(Parsed("foreach (var (a) in xs) f(a);")),
        (std::vector<std::string>{"1:1: a structured binding needs two or more names"}));
    EXPECT_FALSE(errors_of(Parsed("var () = f();")).empty());
    EXPECT_FALSE(errors_of(Parsed("var (a, ) = f();")).empty());
    EXPECT_FALSE(errors_of(Parsed("var (a, b = f();")).empty());
}
