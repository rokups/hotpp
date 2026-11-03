// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the nullability analyzer (nullcheck.h). Each narrowing form has a case that gives no
// finding and a case that gives a finding. The tests run with the real default: a finding is an
// error.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

// A class with a handle field and a function, and a class with a function that has the body "body".
// The parameters "p" and "q" hold a handle of Node. They can be null.
std::string script(const std::string& body, const std::string& extra_members = "")
{
    return "class Node {\n"
           "    int hp = 0;\n"
           "    $Node next;\n"
           "    int val() { return 1; }\n"
           "    $Node find() { return null; }\n"
           "}\n"
           "class Tester {\n"
           "    $Node f;\n"
           + extra_members
           + "    void run($Node p, $Node q) {\n" + body + "\n    }\n}\n";
}

int count_of(const std::string& text, const std::string& needle)
{
    int n = 0;
    for (size_t p = text.find(needle); p != std::string::npos; p = text.find(needle, p + 1))
        ++n;
    return n;
}

// The tests of the analyzer use the real default: a finding is an error.
hotc::TranslateResult translate_real(const std::string& script)
{
    return translate_text(script);
}

// The number of findings.
int null_warnings(const hotc::TranslateResult& r)
{
    return count_of(r.diagnostics, "can be null here");
}

// The number of warnings of the body.
int warns(const std::string& body, const std::string& extra_members = "")
{
    const hotc::TranslateResult r = translate_real(script(body, extra_members));
    EXPECT_EQ(r.ok, null_warnings(r) == 0) << r.diagnostics;
    return null_warnings(r);
}

} // namespace

TEST(NullCheck, UnguardedAccessIsAnError)
{
    const hotc::TranslateResult r = translate_real(script("p.hp = 1;"));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("error:"), std::string::npos) << r.diagnostics;
    EXPECT_EQ(null_warnings(r), 1) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("t.hot:"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("\"p\" can be null here; check it with \"if (p != null)\" or use \"p?.m\""),
        std::string::npos)
        << r.diagnostics;
}

TEST(NullCheck, ComparisonWithNullNeverWarns)
{
    EXPECT_EQ(warns("var a = p == null; var b = null != p; if (p != null) { }"), 0);
}

TEST(NullCheck, CallIndexAndDereferenceWarn)
{
    EXPECT_EQ(warns("p.val();"), 1);
    EXPECT_EQ(warns("var n = p.val() + q.val();"), 2);
}

// ---- early exit
TEST(NullCheck, EarlyReturnNarrows)
{
    EXPECT_EQ(warns("if (p == null) return; p.hp = 1; p.val();"), 0);
    EXPECT_EQ(warns("if (p == null) return; q.hp = 1;"), 1);
}

TEST(NullCheck, EarlyContinueBreakAndThrowNarrow)
{
    EXPECT_EQ(warns("for (var i = 0; i < 3; i++) { if (p == null) continue; p.hp = 1; }"), 0);
    EXPECT_EQ(warns("for (var i = 0; i < 3; i++) { if (p == null) break; p.hp = 1; }"), 0);
    EXPECT_EQ(warns("if (p == null) throw 1; p.hp = 1;"), 0);
    EXPECT_EQ(warns("for (var i = 0; i < 3; i++) { if (p == null) { } p.hp = 1; }"), 1);
}

// ---- if
TEST(NullCheck, IfNotNullNarrowsTheThenBranch)
{
    EXPECT_EQ(warns("if (p != null) { p.hp = 1; }"), 0);
    EXPECT_EQ(warns("if (p != null) { } p.hp = 1;"), 1);
    EXPECT_EQ(warns("if (null != p) { p.hp = 1; }"), 0);
}

TEST(NullCheck, ElseBranchOfEqualsNullNarrows)
{
    EXPECT_EQ(warns("if (p == null) { } else { p.hp = 1; }"), 0);
    EXPECT_EQ(warns("if (p == null) { p.hp = 1; }"), 1);
}

TEST(NullCheck, NegatedConditionNarrows)
{
    EXPECT_EQ(warns("if (!(p == null)) { p.hp = 1; }"), 0);
    EXPECT_EQ(warns("if (!(p != null)) { p.hp = 1; }"), 1);
    EXPECT_EQ(warns("if (!(p != null)) return; p.hp = 1;"), 0);
}

TEST(NullCheck, NestedIfs)
{
    EXPECT_EQ(warns("if (p != null) { if (q != null) { p.hp = q.hp; } }"), 0);
    EXPECT_EQ(warns("if (p != null) { if (q != null) { } q.hp = 1; }"), 1);
    EXPECT_EQ(warns("if (p == null) return; if (q == null) return; p.hp = q.hp;"), 0);
}

TEST(NullCheck, NarrowingJoinsAtTheEndOfAnIf)
{
    EXPECT_EQ(warns("if (q == null) { if (p == null) return; } p.hp = 1;"), 1);
    EXPECT_EQ(warns("if (q == null) { if (p == null) return; } else { if (p == null) return; } p.hp = 1;"), 0);
}

// ---- while
TEST(NullCheck, WhileConditionNarrowsTheBody)
{
    EXPECT_EQ(warns("var c = p; while (c != null) { c.hp = 1; c = c.next; }"), 0);
    EXPECT_EQ(warns("var c = p; while (c != null) { } c.hp = 1;"), 1);
}

TEST(NullCheck, LoopBodyKeepsTheNarrowingOfTheEntry)
{
    EXPECT_EQ(warns("if (p == null) return; for (var i = 0; i < 3; i++) { p.hp = i; }"), 0);
    EXPECT_EQ(warns("if (p == null) return; if (q == null) return; foreach (var x in q.hp) { p.hp = 1; }"), 0);
}

TEST(NullCheck, LoopBodyDropsTheNarrowingOfAssignedNames)
{
    EXPECT_EQ(warns("if (p == null) return; for (var i = 0; i < 3; i++) { p.hp = i; p = q; }"), 1);
    // The condition and the body read "c" with no narrowing: the loop assigns "c".
    EXPECT_EQ(warns("var c = new $Node(); while (c.hp < 3) { c.hp = 1; c = c.find(); }"), 3);
    EXPECT_EQ(warns("var c = new $Node(); var d = new $Node(); while (c.hp < 3) { d.hp = 1; c = q; }"), 1);
}

// A call with a "ref" argument can store null in the name. The next iteration sees the null name.
TEST(NullCheck, LoopBackEdgeDropsTheNarrowingOfRefArguments)
{
    const std::string members = "    void reset(ref $Node r) { r = null; }\n";
    EXPECT_EQ(warns("if (p == null) return; while (q.hp < 3) { p.hp = 1; reset(p); }", members), 2); // "q" and "p"
    EXPECT_EQ(warns("if (p == null) return; if (q == null) return; while (q.hp < 3) { p.hp = 1; reset(p); }", members), 1);
    EXPECT_EQ(warns("if (p == null) return; if (q == null) return; for (var i = 0; i < 3; i++) { p.hp = 1; reset(p); }", members), 1);
    EXPECT_EQ(warns("if (p == null) return; if (q == null) return; do { p.hp = 1; reset(p); } while (q.hp < 3);", members), 1);
    EXPECT_EQ(warns("if (p == null) return; if (q == null) return; foreach (var x in q.hp) { p.hp = 1; reset(p); }", members), 1);
    // A name that no call stores through stays narrowed in each kind of loop.
    EXPECT_EQ(warns("if (p == null) return; if (q == null) return; while (q.hp < 3) { p.hp = 1; reset(q); }", members), 1);
    EXPECT_EQ(warns("if (p == null) return; if (q == null) return; for (var i = 0; i < 3; i++) { p.hp = 1; reset(q); }", members), 0);
    EXPECT_EQ(warns("if (p == null) return; if (q == null) return; do { p.hp = 1; reset(q); } while (p.hp < 3);", members), 0);
    EXPECT_EQ(warns("if (p == null) return; if (q == null) return; foreach (var x in p.hp) { p.hp = 1; reset(q); }", members), 0);
}

TEST(NullCheck, LoopKeepsTheNarrowingOfNamesThatTheBodyDoesNotAssign)
{
    EXPECT_EQ(warns("if (p == null) return; if (q == null) return; while (p.hp < 3) { p.hp = 1; q = null; }"), 0);
    EXPECT_EQ(warns("if (p == null) return; if (q == null) return; for (var i = 0; i < 3; i++) { p.hp = 1; q = null; }"), 0);
    EXPECT_EQ(warns("if (p == null) return; if (q == null) return; do { p.hp = 1; q = null; } while (p.hp < 3);"), 0);
    EXPECT_EQ(warns("if (p == null) return; if (q == null) return; foreach (var x in p.hp) { p.hp = 1; q = null; }"), 0);
}

// ---- && and ||
TEST(NullCheck, AndNarrowsTheRightSide)
{
    EXPECT_EQ(warns("if (p != null && p.hp > 0) { }"), 0);
    EXPECT_EQ(warns("if (p != null && p.hp > 0 && p.val() > 0) { p.hp = 2; }"), 0);
    EXPECT_EQ(warns("if (p == null && p.hp > 0) { }"), 1);
    EXPECT_EQ(warns("if (p != null && q.hp > 0) { }"), 1);
}

TEST(NullCheck, OrNarrowsTheRightSide)
{
    EXPECT_EQ(warns("if (p == null || p.hp > 0) { }"), 0);
    EXPECT_EQ(warns("if (p == null || p.hp > 0) return; p.hp = 1;"), 0);
    EXPECT_EQ(warns("if (p != null || p.hp > 0) { }"), 1);
}

TEST(NullCheck, AndAsAValueNarrowsTheRightSide)
{
    EXPECT_EQ(warns("var ok = p != null && p.hp > 0;"), 0);
    EXPECT_EQ(warns("var ok = p == null || p.hp > 0;"), 0);
    EXPECT_EQ(warns("var ok = p == null && p.hp > 0;"), 1);
}

// ---- is, ?., ??, ?:
TEST(NullCheck, IsBindsANonNullName)
{
    EXPECT_EQ(warns("if (p is $Node n) { n.hp = 1; }"), 0);
    EXPECT_EQ(warns("if (p is $Node n) { p.hp = 1; }"), 0);
    EXPECT_EQ(warns("if (p is $Node n) { } p.hp = 1;"), 1);
}

TEST(NullCheck, SafeMemberIsTheNullCheck)
{
    EXPECT_EQ(warns("p?.val();"), 0);
    EXPECT_EQ(warns("p?.val(); q.val();"), 1);
}

TEST(NullCheck, NullCoalescingGivesTheRightSide)
{
    EXPECT_EQ(warns("var r = p ?? new $Node(); r.hp = 1;"), 0);
    EXPECT_EQ(warns("var r = p ?? q; r.hp = 1;"), 1);
    EXPECT_EQ(warns("(p ?? new $Node()).hp = 1;"), 0);
}

TEST(NullCheck, SafeMemberResultCanBeNull)
{
    // A handle member stays a handle that can be null. A value member gives "T?".
    EXPECT_EQ(warns("var n = p?.next; n.hp = 1;"), 1);
    EXPECT_EQ(warns("var n = p?.next; if (n != null) n.hp = 1;"), 0);
    EXPECT_EQ(warns("var n = p?.find(); n.hp = 1;"), 1);
    EXPECT_EQ(warns("var n = p?.next?.next; n.hp = 1;"), 1);
    EXPECT_EQ(warns("var n = p?.val(); var m = n ?? 0;"), 0);
    EXPECT_EQ(warns("var n = p?.next ?? new $Node(); n.hp = 1;"), 0);
}

TEST(NullCheck, CoalesceWithANonNullRightSideIsNonNull)
{
    EXPECT_EQ(warns("var n = p.next ?? new $Node(); n.hp = 1;", ""), 1); // "p" can be null
    EXPECT_EQ(warns("if (p == null) return; var n = p.next ?? new $Node(); n.hp = 1;"), 0);
    EXPECT_EQ(warns("var n = p?.next ?? q; n.hp = 1;"), 1);
}

TEST(NullCheck, CoalesceOfANonNullLeftSideIsNonNull)
{
    // The left side is not null: the result is not null, and nothing is reported for it.
    EXPECT_EQ(warns("if (p == null) return; var n = p ?? q; n.hp = 1;"), 0);
    EXPECT_EQ(warns("if (p == null) return; var n = p ?? q; n.hp = 1; q.hp = 2;"), 1); // only "q"
    EXPECT_EQ(warns("var n = new $Node() ?? q; n.hp = 1;"), 0);
    // The left side can be null and the right side can be null: the result can be null.
    EXPECT_EQ(warns("var n = p ?? q; n.hp = 1;"), 1);
    // The left side can be null and the right side is not null: the result is not null.
    EXPECT_EQ(warns("var n = p ?? new $Node(); n.hp = 1;"), 0);
}

// "($C)h" to the same class or to a base class is a checked unwrap. A cast to a derived class can give null.
std::string cast_script(const std::string& body)
{
    return "class Node {\n    int hp = 0;\n}\n"
           "class Sub : Node {\n    int extra = 0;\n}\n"
           "class Tester {\n    void run($Node b, $Sub s) {\n"
        + body + "\n    }\n}\n";
}

int cast_warns(const std::string& body)
{
    const hotc::TranslateResult r = translate_real(cast_script(body));
    EXPECT_EQ(r.ok, null_warnings(r) == 0) << r.diagnostics;
    return null_warnings(r);
}

TEST(NullCheck, CastToTheSameHandleTypeIsNonNull)
{
    EXPECT_EQ(cast_warns("var a = ($Node)b; a.hp = 1;"), 0);
    EXPECT_EQ(cast_warns("var a = b; a.hp = 1;"), 1);
}

TEST(NullCheck, CastToABaseHandleTypeIsNonNull)
{
    EXPECT_EQ(cast_warns("var a = ($Node)s; a.hp = 1;"), 0);
}

TEST(NullCheck, CastToADerivedHandleTypeCanBeNull)
{
    EXPECT_EQ(cast_warns("var a = ($Sub)b; a.extra = 1;"), 1);
    EXPECT_EQ(cast_warns("var a = ($Sub)b; if (a != null) a.extra = 1;"), 0);
}

TEST(NullCheck, CastUnwrapNarrowsTheName)
{
    EXPECT_EQ(cast_warns("var a = ($Node)b; b.hp = 1;"), 0);
    EXPECT_EQ(cast_warns("var a = ($Sub)b; b.hp = 1;"), 1);
}

TEST(NullCheck, CastOfAPointerToTheSameTypeIsNonNull)
{
    auto count = [](const char* body)
    { return null_warnings(translate_real(std::string("class Tester {\n    void run(int* a, const int* c) {\n") + body + "\n    }\n}\n")); };
    EXPECT_EQ(count("var b = (int*)a; b[0] = 1;"), 0);
    EXPECT_EQ(count("var b = a; b[0] = 1;"), 1);
    EXPECT_EQ(count("var b = (int*)a; a[0] = 1;"), 0);
    // A const cast keeps the null state of the pointer.
    EXPECT_EQ(count("var b = (const int*)a; b[0] = 1;"), 1);
    EXPECT_EQ(count("if (a == null) return; var b = (const int*)a; b[0] = 1;"), 0);
    // A cast of a const pointer to "int*" is not the same type: the C cast keeps the null state.
    EXPECT_EQ(count("var b = (int*)c; b[0] = 1;"), 1);
}

TEST(NullCheck, ConditionalExpressionNarrows)
{
    EXPECT_EQ(warns("var v = p != null ? p.hp : 0;"), 0);
    EXPECT_EQ(warns("var v = p == null ? 0 : p.hp;"), 0);
    EXPECT_EQ(warns("var v = !(p == null) ? p.hp : 0;"), 0);
    EXPECT_EQ(warns("var v = p == null ? p.hp : 0;"), 1);
    EXPECT_EQ(warns("var v = p != null ? 0 : p.hp;"), 1);
}

// ---- known non null values
TEST(NullCheck, NewThisAndAddressAreNonNull)
{
    EXPECT_EQ(warns("var n = new $Node(); n.hp = 1; n.val();"), 0);
    EXPECT_EQ(warns("var n = new Node(); n.hp = 1; n.val();"), 0);
    EXPECT_EQ(warns("this.val2(); var n = new $Node(); n.hp = 1;", "    int val2() { return 1; }\n"), 0);
    EXPECT_EQ(warns("int x = 0; var a = &x; var b = *a;"), 0);
}

TEST(NullCheck, NewIsNonNullOnlyUntilAnAssignment)
{
    EXPECT_EQ(warns("var n = new $Node(); n = q; n.hp = 1;"), 1);
    EXPECT_EQ(warns("var n = new $Node(); n = null; n.hp = 1;"), 1);
    EXPECT_EQ(warns("var n = new $Node(); n = new $Node(); n.hp = 1;"), 0);
}

TEST(NullCheck, ValueLocalAndParameterAreNotPointers)
{
    EXPECT_EQ(warns("Node v; v.hp = 1; v.val();"), 0);
}

TEST(NullCheck, DeclarationWithoutInitializerStartsNullable)
{
    EXPECT_EQ(warns("$Node n; n.hp = 1;"), 1);
    EXPECT_EQ(warns("$Node n = p; n.hp = 1;"), 1);
    EXPECT_EQ(warns("var n = p; n.hp = 1;"), 1);
    EXPECT_EQ(warns("var n = p; if (n == null) return; n.hp = 1;"), 0);
}

// ---- assignment
TEST(NullCheck, AssignmentClearsTheNarrowing)
{
    EXPECT_EQ(warns("if (p == null) return; p.hp = 1; p = q; p.hp = 2;"), 1);
    EXPECT_EQ(warns("if (p == null) return; p = new $Node(); p.hp = 2;"), 0);
    EXPECT_EQ(warns("p = new $Node(); p.hp = 2;"), 0);
}

// ---- fields
TEST(NullCheck, FieldNeverNarrows)
{
    EXPECT_EQ(warns("this.f.hp = 1;"), 1);
    EXPECT_EQ(warns("f.hp = 1;"), 1);
    EXPECT_EQ(warns("if (f != null) { f.hp = 1; }"), 1);
    EXPECT_EQ(warns("if (this.f == null) return; this.f.hp = 1;"), 1);
    EXPECT_EQ(warns("var l = f; if (l != null) { l.hp = 1; }"), 0);
}

TEST(NullCheck, FieldMessageAsksForALocal)
{
    const hotc::TranslateResult r = translate_real(script("f.hp = 1;"));
    EXPECT_NE(r.diagnostics.find("\"f\" can be null here; copy it to a local and check it"), std::string::npos)
        << r.diagnostics;
}

TEST(NullCheck, FieldOfAnObjectNeverNarrows)
{
    EXPECT_EQ(warns("if (p == null) return; p.next.hp = 1;"), 1);
    EXPECT_EQ(warns("if (p == null) return; var n = p.next; if (n != null) { n.hp = 1; }"), 0);
}

TEST(NullCheck, MethodResultCanBeNull)
{
    EXPECT_EQ(warns("if (p == null) return; p.find().hp = 1;"), 1);
    EXPECT_EQ(warns("if (p == null) return; var n = p.find(); n.hp = 1;"), 1);
    EXPECT_EQ(warns("if (p == null) return; var n = p.find(); if (n == null) return; n.hp = 1;"), 0);
}

// ---- lambda
TEST(NullCheck, LambdaBodyStartsWithNoNarrowing)
{
    EXPECT_EQ(warns("if (p == null) return; var g = () => { p.hp = 1; };"), 1);
    EXPECT_EQ(warns("var g = () => { if (p == null) return; p.hp = 1; };"), 0);
    EXPECT_EQ(warns("if (p == null) return; var g = () => { }; p.hp = 1;"), 0);
}

TEST(NullCheck, LambdaThatAssignsACaptureClearsTheNarrowing)
{
    EXPECT_EQ(warns("if (p == null) return; var g = () => { p = q; }; p.hp = 1;"), 1);
}

// ---- try
TEST(NullCheck, NarrowingOfTheTryBlockDoesNotHoldInCatch)
{
    EXPECT_EQ(warns("try { if (p == null) return; p.hp = 1; } catch (...) { p.hp = 2; }"), 1);
    EXPECT_EQ(warns("try { if (p == null) return; p.hp = 1; } catch (...) { }"), 0);
}

TEST(NullCheck, NarrowingBeforeTheTryHoldsInCatchUnlessAssigned)
{
    EXPECT_EQ(warns("if (p == null) return; try { p.hp = 1; } catch (...) { p.hp = 2; }"), 0);
    EXPECT_EQ(warns("if (p == null) return; try { p = q; } catch (...) { p.hp = 2; }"), 1);
}

// A "break" in the try leaves through the finally block. The block can assign a name.
TEST(NullCheck, BreakThroughAFinallyThatAssignsClearsTheNarrowing)
{
    EXPECT_EQ(warns("switch (1) { default: try { if (p == null) return; break; } finally { p = null; } } p.val();"), 1);
    EXPECT_EQ(warns("while (true) { try { if (p == null) return; continue; } finally { p = q; } } "), 0);
    EXPECT_EQ(warns("while (true) { try { if (p == null) return; break; } finally { p = null; } } p.val();"), 1);
    // A finally block that does not assign the name keeps the narrowing.
    EXPECT_EQ(warns("switch (1) { default: try { if (p == null) return; break; } finally { q = null; } } p.val();"), 0);
}

// ---- the cast "(T*)p" of a pointer and the cast "($C)h" of a handle
// A call through a callee that is not a script method can take a name by reference.
TEST(NullCheck, UnknownCalleeCanStoreThroughANameArgument)
{
    EXPECT_EQ(warns("var g = (ref $Node x) => { x = null; }; if (p == null) return; g(p); p.hp = 1;"), 1);
    EXPECT_EQ(warns("var g = (ref $Node x) => { x = null; }; if (p == null) return; p.hp = 1; g(q);"), 0);
}

// ---- ref
TEST(NullCheck, RefToANullableNameDisablesTheNarrowing)
{
    EXPECT_EQ(warns("if (p == null) return; ref r = p; p.hp = 1;"), 1);
    EXPECT_EQ(warns("if (p == null) return; ref r = p; r.hp = 1;"), 1);
    EXPECT_EQ(warns("ref r = p; if (r != null) { r.hp = 1; }"), 1);
    EXPECT_EQ(warns("var n = 1; ref r = n; r = 2;"), 0);
}

TEST(NullCheck, RefToANullableFieldIsAnError)
{
    const hotc::TranslateResult r = translate_real(script("ref r = this.f;"));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("a ref cannot bind to a nullable field"), std::string::npos) << r.diagnostics;
    const hotc::TranslateResult r2 = translate_real(script("if (p == null) return; ref r = p.next;"));
    EXPECT_NE(r2.diagnostics.find("a ref cannot bind to a nullable field"), std::string::npos) << r2.diagnostics;
}

// ---- switch and dead code
TEST(NullCheck, SwitchCasesStartWithTheNarrowingOfTheSubject)
{
    EXPECT_EQ(warns("if (p == null) return; switch (1) { case 1: p.hp = 1; break; default: p.hp = 2; }"), 0);
    EXPECT_EQ(warns("switch (1) { case 1: if (p == null) return; p.hp = 1; break; default: p.hp = 2; }"), 1);
}

TEST(NullCheck, CodeAfterReturnGivesNoWarning)
{
    EXPECT_EQ(warns("return; p.hp = 1;"), 0);
}

// ---- parameters and members of the class
TEST(NullCheck, PointerParameterIsNullable)
{
    const hotc::TranslateResult r = translate_real(
        "class Tester {\n"
        "    void run(int* a) { a[0] = 1; }\n"
        "    void run2(int* a) { if (a == null) return; a[0] = 1; }\n"
        "}\n");
    EXPECT_EQ(null_warnings(r), 1) << r.diagnostics;
}

TEST(NullCheck, CallToBareMethodUsesTheReturnType)
{
    const std::string members = "    $Node make() { return null; }\n";
    EXPECT_EQ(warns("var n = make(); n.hp = 1;", members), 1);
    EXPECT_EQ(warns("var n = make(); if (n == null) return; n.hp = 1;", members), 0);
}

// ---- the option
TEST(NullCheck, NullWarningsMakesTheFindingsWarnings)
{
    hotc::TranslateOptions o;
    o.script_name = "t.hot";
    o.script_text = script("p.hp = 1;");
    o.generation = 1;
    o.null_warnings = true;
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(null_warnings(r), 0) << r.diagnostics;
    EXPECT_EQ(count_of(r.warnings, "can be null here"), 1) << r.warnings;
}

// ---- native values
namespace
{

const char* kNativeHeader =
    "class Item { public: virtual ~Item() {} int x = 0; };\n"
    "class Factory { public: virtual ~Factory() {} Item* make() { return nullptr; } };\n";

hotc::TranslateResult native_translate(const std::string& stubs, const std::string& body)
{
    static TempDir* dir = nullptr;
    if (!dir)
    {
        dir = new TempDir();
        write_file(dir->path() / "n.h", kNativeHeader);
    }
    const std::string text = "using \"n.h\";\n" + stubs + "class Tester : Factory {\n    void run() {\n" + body + "\n    }\n}\n";
    return hotc::translate(temp_options(*dir, text));
}

} // namespace

TEST(NullCheck, NativeCallReturnIsNullable)
{
    const hotc::TranslateResult r = native_translate("", "var i = this.make(); i.x = 1;");
    EXPECT_EQ(r.ok, null_warnings(r) == 0) << r.diagnostics;
    EXPECT_EQ(null_warnings(r), 1) << r.diagnostics;
    const hotc::TranslateResult r2 = native_translate("", "var i = this.make(); if (i == null) return; i.x = 1;");
    EXPECT_EQ(null_warnings(r2), 0) << r2.diagnostics;
}

TEST(NullCheck, NonNullStubMakesTheReturnNonNull)
{
    const hotc::TranslateResult r = native_translate("[native, nonnull] class Item;\n", "var i = this.make(); i.x = 1;");
    EXPECT_EQ(r.ok, null_warnings(r) == 0) << r.diagnostics;
    EXPECT_EQ(null_warnings(r), 0) << r.diagnostics;
}

TEST(NullCheck, NonNullDefaultOfTheLibrary)
{
    const hotc::TranslateResult r = native_translate("[native, nonnull] class *;\n", "var i = this.make(); i.x = 1;");
    EXPECT_EQ(r.ok, null_warnings(r) == 0) << r.diagnostics;
    EXPECT_EQ(null_warnings(r), 0) << r.diagnostics;
}
