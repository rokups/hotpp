// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the lowering of front, back, top, pop, pop_back, pop_front and at (hot::front and the
// like), and for the conversion of a native exception in a try block.
#include "lower.h"
#include "support/lowering_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace hotc;
using namespace lowering_test;

TEST(ContainerOpsLowering, EmptyCheckedCallsUseTheHelper)
{
    Fixture f;
    f.param("xs");
    f.cx.local_type["xs"] = "int[]";
    EXPECT_EQ(f.expr("xs.front()"), "hot::front(xs)");
    EXPECT_EQ(f.expr("xs.back()"), "hot::back(xs)");
    EXPECT_EQ(f.expr("xs.top()"), "hot::top(xs)");
    EXPECT_EQ(f.expr("xs.pop()"), "hot::pop(xs)");
    EXPECT_EQ(f.expr("xs.pop_back()"), "hot::pop_back(xs)");
    EXPECT_EQ(f.expr("xs.pop_front()"), "hot::pop_front(xs)");
    EXPECT_EQ(f.expr("xs.at(2)"), "hot::at(xs, 2)");
    EXPECT_EQ(f.expr("xs.back() + 1"), "hot::back(xs) + 1");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(ContainerOpsLowering, OtherCallsStayPlain)
{
    Fixture f;
    f.param("xs");
    f.cx.local_type["xs"] = "int[]";
    EXPECT_EQ(f.expr("xs.size()"), "hot::deref(xs).size()");
    EXPECT_EQ(f.expr("xs.push_back(1)"), "hot::deref(xs).push_back(1)");
    EXPECT_EQ(f.expr("xs.back(1)"), "hot::deref(xs).back(1)"); // not the call without arguments
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(ContainerOpsLowering, TryWithScriptErrorCatchConvertsNativeExceptions)
{
    Fixture f;
    const std::string convert = "catch (const std::exception&) { hot::detail::rethrow_as_script_error(); }";
    for (const char* type : {"ScriptError", "NativeError"})
    {
        const std::string out = f.body(std::string("try { this.n = 1; } catch (") + type + " e) { this.n = 2; }");
        EXPECT_NE(out.find(convert), std::string::npos) << type << "\n" << out;
    }
    // A catch clause for a subtype, for "..." or for std::exception needs no conversion.
    for (const char* clause : {"catch (IndexOutOfRange e)", "catch (...)", "catch (std.exception e)"})
    {
        const std::string out = f.body(std::string("try { this.n = 1; } ") + clause + " { this.n = 2; }");
        EXPECT_EQ(out.find(convert), std::string::npos) << clause << "\n" << out;
    }
    EXPECT_EQ(f.errors(), std::vector<std::string>{});
}

TEST(ContainerOpsLowering, UnknownTypeAndScriptClassStayPlain)
{
    Fixture f;
    f.param("u");
    f.param("o");
    f.cx.local_type["o"] = "Actor?"; // the suffix "?" is removed before the lookup
    EXPECT_EQ(f.expr("u.back()"), "hot::deref(u).back()"); // the type is not known
    EXPECT_EQ(f.expr("this.items.back()"), "hot::deref(__self->items).back()");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(ContainerOpsLowering, OptionalReceiverOfAKnownTypeUsesTheHelper)
{
    Fixture f;
    f.param("xs");
    f.cx.local_type["xs"] = "std.vector<int>?";
    EXPECT_EQ(f.expr("xs.back()"), "hot::back(xs)");
}

TEST(ContainerOpsLowering, NativeCatchClauseKeepsTheOriginalException)
{
    Fixture f;
    const std::string convert = "hot::detail::rethrow_as_script_error()";
    for (const char* clauses : {"catch (ScriptError e) { this.n = 2; } catch (std.out_of_range e) { this.n = 3; }",
             "catch (NativeError e) { this.n = 2; } catch (std.exception e) { this.n = 3; }",
             "catch (ScriptError e) { this.n = 2; } catch (...) { this.n = 3; }"})
    {
        const std::string out = f.body(std::string("try { this.n = 1; } ") + clauses);
        EXPECT_EQ(out.find(convert), std::string::npos) << clauses << "\n" << out;
    }
    EXPECT_NE(f.body("try { this.n = 1; } catch (IndexOutOfRange e) { this.n = 2; } catch (ScriptError e) { }")
                  .find(convert),
        std::string::npos);
}
