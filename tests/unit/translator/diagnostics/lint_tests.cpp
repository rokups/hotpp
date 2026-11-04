// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the lint warnings of a ".hot" script (lint.h): dead code, unused locals and shadowing.
// Each warning has a case that warns and a case that does not warn.
#include "ast.h"
#include "diag.h"
#include "hot/hotc_lib.h"
#include "lint.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

// A class with a field, a helper, the members "members" and the method "run" with the body "body".
// The first line of "body" is line 6. The first statement starts at column 9.
std::string script(const std::string& body, const std::string& members = "")
{
    return "class Node {\n"
           "    int count = 0;\n"
           "    int calc(int a) { return a + 1; }\n"
           "    void fill(out int r) { r = 5; }\n"
           + members
           + "    void run(int p) {\n"
           + body + "\n    }\n}\n";
}

bool has(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

int count_of(const std::string& text, const std::string& needle)
{
    int n = 0;
    for (size_t p = text.find(needle); p != std::string::npos; p = text.find(needle, p + 1))
        ++n;
    return n;
}

// The translation must succeed. Returns the warnings.
std::string warnings_of(const std::string& body, const std::string& members = "")
{
    const hotc::TranslateResult r = translate_text(script(body, members));
    EXPECT_TRUE(r.ok) << r.diagnostics;
    return r.warnings;
}

} // namespace

// ---------------------------------------------------------------- dead code

TEST(LintDeadCode, StatementAfterReturnWarns)
{
    const std::string w = warnings_of("        return;\n        this.count = 1;");
    EXPECT_TRUE(has(w, "t.hot:7:9: warning: unreachable code: this statement follows 'return'")) << w;
}

TEST(LintDeadCode, StatementAfterThrowWarns)
{
    const std::string w = warnings_of("        throw std.runtime_error(\"x\");\n        this.count = 1;");
    EXPECT_TRUE(has(w, "t.hot:7:9: warning: unreachable code: this statement follows 'throw'")) << w;
}

TEST(LintDeadCode, StatementAfterBreakAndContinueWarns)
{
    const std::string w = warnings_of("        for (int i = 0; i < p; i++) {\n"
                                      "            if (i == 1) { break; this.count = 1; }\n"
                                      "            continue;\n"
                                      "            this.count = 2;\n"
                                      "        }");
    EXPECT_TRUE(has(w, "follows 'break'")) << w;
    EXPECT_TRUE(has(w, "follows 'continue'")) << w;
    EXPECT_EQ(count_of(w, "unreachable code"), 2) << w;
}

TEST(LintDeadCode, OneWarningForSeveralDeadStatements)
{
    const std::string w = warnings_of("        return;\n        this.count = 1;\n        this.count = 2;");
    EXPECT_EQ(count_of(w, "unreachable code"), 1) << w;
}

TEST(LintDeadCode, ReturnAtTheEndOfABlockDoesNotWarn)
{
    const std::string w = warnings_of("        if (p > 1) { return; }\n        this.count = 1;\n        return;");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintDeadCode, ReturnInOneBranchDoesNotWarn)
{
    const std::string w =
        warnings_of("        if (p > 1) { return; } else { this.count = 2; }\n        this.count = 1;");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintDeadCode, DeadCodeInALambdaWarns)
{
    const std::string w = warnings_of("        var f = (int x) => { return x; x = 2; };\n        f(1);");
    EXPECT_TRUE(has(w, "follows 'return'")) << w;
}

TEST(LintDeadCode, BreakAfterReturnInACaseDoesNotWarn)
{
    const std::string w = warnings_of("        switch (p) {\n"
                                      "            case 1: return;\n"
                                      "                break;\n"
                                      "            default: this.count = 1;\n"
                                      "        }");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintDeadCode, OtherStatementAfterReturnInACaseWarns)
{
    const std::string w = warnings_of("        switch (p) {\n"
                                      "            case 1: return;\n"
                                      "                this.count = 3;\n"
                                      "            default: this.count = 1;\n"
                                      "        }");
    EXPECT_EQ(count_of(w, "unreachable code"), 1) << w;
}

// ---------------------------------------------------------------- unused locals

TEST(LintUnused, UnusedLocalWarnsAtTheName)
{
    const std::string w = warnings_of("        int x = 1;");
    EXPECT_TRUE(has(w, "t.hot:6:13: warning: local variable 'x' is never used")) << w;
}

TEST(LintUnused, UnusedLocalWithSideEffectInitializerStillSaysNeverUsed)
{
    const std::string w = warnings_of("        int x = this.calc(2);");
    EXPECT_TRUE(has(w, "local variable 'x' is never used")) << w;
}

TEST(LintUnused, UnusedVarLocalWarns)
{
    const std::string w = warnings_of("        var y = 2.5;");
    EXPECT_TRUE(has(w, "local variable 'y' is never used")) << w;
}

TEST(LintUnused, NameWithUnderscoreIsExempt)
{
    const std::string w = warnings_of("        int _x = 1;\n        int _ = this.calc(2);");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintUnused, ReadLocalDoesNotWarn)
{
    const std::string w = warnings_of("        int x = 1;\n        this.count = x;");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintUnused, AssignedOnlyLocalWarns)
{
    const std::string w = warnings_of("        int x = 1;\n        x = 2;");
    EXPECT_TRUE(has(w, "local variable 'x' is never used")) << w;
}

TEST(LintUnused, AssignedAndReadLaterDoesNotWarn)
{
    const std::string w = warnings_of("        int x;\n        x = 2;\n        this.count = x;");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintUnused, CompoundAssignmentCountsAsUse)
{
    const std::string w = warnings_of("        int x = 1;\n        x += 2;");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintUnused, UseInANestedBlockDoesNotWarn)
{
    const std::string w =
        warnings_of("        int x = 1;\n        if (p > 0) { while (p > 5) { this.count = x; p = p - 1; } }");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintUnused, UseOnlyInALambdaDoesNotWarn)
{
    const std::string w = warnings_of("        int x = 1;\n        var f = () => { return x; };\n        f();");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintUnused, UnusedLocalInALambdaWarns)
{
    const std::string w = warnings_of("        var f = () => { int z = 1; return 2; };\n        f();");
    EXPECT_TRUE(has(w, "local variable 'z' is never used")) << w;
}

TEST(LintUnused, UnusedForeachVariableDoesNotWarn)
{
    const std::string w = warnings_of("        std.vector<int> list = std.vector<int>();\n"
                                      "        foreach (var item in list) { this.count = this.count + 1; }");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintUnused, UnusedCatchVariableDoesNotWarn)
{
    const std::string w =
        warnings_of("        try { this.count = 1; } catch (std.exception e) { this.count = 2; }");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintUnused, UnusedParameterDoesNotWarn)
{
    // "p" is a parameter and "run" never reads it.
    const std::string w = warnings_of("        this.count = 1;");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintUnused, OutArgumentCountsAsUse)
{
    // The call of a function with an "out" parameter has no marker: "fill(r)" (doc/types-handles.md).
    const std::string w = warnings_of("        int r;\n        this.fill(r);");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintUnused, UseInAStringInterpolationDoesNotWarn)
{
    const std::string w = warnings_of("        int x = 1;\n        var s = \"v=${x}\";\n        this.count = s.size();");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintUnused, UnusedBindingDoesNotWarn)
{
    const std::string w = warnings_of("        var o = new $Node();\n"
                                      "        if (o is $Node n) { this.count = 1; }");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintUnused, ForLoopVariableReadByConditionDoesNotWarn)
{
    const std::string w = warnings_of("        for (int i = 0; i < 3; i++) { this.count = 1; }");
    EXPECT_TRUE(w.empty()) << w;
}

// ---------------------------------------------------------------- shadowing

TEST(LintShadow, LocalHidesParameterWarns)
{
    const std::string w = warnings_of("        if (p > 0) { int p = 2; this.count = p; }");
    EXPECT_TRUE(has(w, "warning: 'p' hides a parameter with the same name")) << w;
}

TEST(LintShadow, LocalHidesLocalWarns)
{
    const std::string w = warnings_of("        int x = 1;\n"
                                      "        if (p > 0) { int x = 2; this.count = x; }\n"
                                      "        this.count = x;");
    EXPECT_TRUE(has(w, "t.hot:7:26: warning: 'x' hides the local variable 'x' that is declared at line 6")) << w;
}

TEST(LintShadow, LambdaParameterHidesLocalWarns)
{
    const std::string w = warnings_of("        int x = 1;\n"
                                      "        var f = (int x) => { return x; };\n"
                                      "        this.count = f(x);");
    EXPECT_TRUE(has(w, "t.hot:7:17: warning: 'x' hides the local variable 'x' that is declared at line 6")) << w;
}

TEST(LintShadow, ForeachVariableHidesLocalWarns)
{
    const std::string w = warnings_of("        int item = 1;\n"
                                      "        std.vector<int> list = std.vector<int>();\n"
                                      "        foreach (var item in list) { this.count = item; }\n"
                                      "        this.count = item;");
    EXPECT_TRUE(has(w, "'item' hides the local variable 'item'")) << w;
}

TEST(LintShadow, LocalNamedLikeAFieldDoesNotWarn)
{
    const std::string w = warnings_of("        int count = 5;\n        this.count = count;");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintShadow, SameNameInSiblingBlocksDoesNotWarn)
{
    const std::string w = warnings_of("        if (p > 0) { int x = 1; this.count = x; }\n"
                                      "        else { int x = 2; this.count = x; }\n"
                                      "        for (int i = 0; i < 2; i++) { this.count = i; }\n"
                                      "        for (int i = 0; i < 3; i++) { this.count = i; }");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintShadow, SameNameInTwoLambdasDoesNotWarn)
{
    const std::string w = warnings_of("        var f = (int x) => { return x; };\n"
                                      "        var g = (int x) => { return x + 1; };\n"
                                      "        this.count = f(1) + g(2);");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintShadow, SameNameInTwoBindingsDoesNotWarn)
{
    const std::string w = warnings_of("        var o = new $Node();\n"
                                      "        if (o is $Node n) { this.count = n.count; }\n"
                                      "        if (o is $Node n) { this.count = n.count + 1; }");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintShadow, ElseIfWithTheSameBindingDoesNotWarn)
{
    const std::string w = warnings_of("        var o = new $Node();\n"
                                      "        if (o is $Node n) { this.count = n.count; }\n"
                                      "        else if (o is $Node n) { this.count = n.count + 1; }\n"
                                      "        else { this.count = 3; }");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintShadow, LocalHidesBindingSaysPatternBinding)
{
    const std::string w = warnings_of("        var o = new $Node();\n"
                                      "        if (o is $Node n) { int n = 2; this.count = n; }");
    EXPECT_TRUE(has(w, "'n' hides the pattern binding 'n' that is declared at line 7")) << w;
    EXPECT_FALSE(has(w, "local variable 'n'")) << w;
}

TEST(LintShadow, WhileBindingScopeIsTheLoop)
{
    const std::string w = warnings_of("        var o = new $Node();\n"
                                      "        while (o is $Node n) { int n = 1; this.count = n; o = null; }");
    EXPECT_TRUE(has(w, "'n' hides the pattern binding 'n'")) << w;
    const std::string ok = warnings_of("        var o = new $Node();\n"
                                       "        while (o is $Node n) { this.count = n.count; o = null; }\n"
                                       "        int n = 4;\n"
                                       "        this.count = n;");
    EXPECT_TRUE(ok.empty()) << ok;
}

TEST(LintShadow, StructuredBindingHidesLocalWarns)
{
    const std::string w = warnings_of("        int a = 1;\n"
                                      "        if (p > 0) { var (a, b) = std.make_pair(1, 2); this.count = a + b; }\n"
                                      "        this.count = a;");
    EXPECT_TRUE(has(w, "'a' hides the local variable 'a' that is declared at line 6")) << w;
}

TEST(LintShadow, PropertySetterValueHiddenByLocalWarns)
{
    const std::string members = "    int Val { get { return 1; } set { if (this.count > 0) { int value = 2; "
                                "this.count = value; } } }\n";
    const std::string w = warnings_of("        this.count = 1;", members);
    EXPECT_TRUE(has(w, "'value' hides a parameter with the same name")) << w;
}

TEST(LintShadow, PropertySetterWithoutHidingDoesNotWarn)
{
    const std::string members = "    int Val { get { return 1; } set { int twice = value * 2; this.count = twice; } }\n";
    const std::string w = warnings_of("        this.count = 1;", members);
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintShadow, ConstructorBodyIsChecked)
{
    const std::string w = warnings_of("        this.count = 1;", "    Node(int a) { if (a > 0) { int a = 2; this.count = a; } }\n");
    EXPECT_TRUE(has(w, "'a' hides a parameter with the same name")) << w;
}

TEST(LintUnused, ConstructorBodyIsChecked)
{
    const std::string w = warnings_of("        this.count = 1;", "    Node(int a) { int z = a; }\n");
    EXPECT_TRUE(has(w, "local variable 'z' is never used")) << w;
}

TEST(LintUnused, FieldInitializerDoesNotWarn)
{
    // A lambda is not allowed in a field initializer, so the pass has no local to check there.
    const std::string w = warnings_of("        this.count = 1;", "    int k = 1 + 2;\n");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintUnused, MemberWriteCountsAsUse)
{
    const std::string w = warnings_of("        $Node o = new $Node();\n        o.count = 3;");
    EXPECT_TRUE(w.empty()) << w;
}

TEST(LintUnused, RaiiLocalWarnsAndUnderscoreNameIsExempt)
{
    const std::string w = warnings_of("        $Node w = new $Node();");
    EXPECT_TRUE(has(w, "local variable 'w' is never used")) << w;
    const std::string ok = warnings_of("        $Node _w = new $Node();");
    EXPECT_TRUE(ok.empty()) << ok;
}

TEST(LintDeadCode, TryCatchAndFinallyBlocksAreChecked)
{
    const std::string w = warnings_of("        try { return; this.count = 1; }\n"
                                      "        catch (std.exception e) { throw; this.count = 2; }\n"
                                      "        finally { throw std.runtime_error(\"y\"); this.count = 3; }");
    EXPECT_EQ(count_of(w, "unreachable code"), 3) << w;
    EXPECT_TRUE(has(w, "t.hot:6:")) << w;
    EXPECT_TRUE(has(w, "t.hot:7:")) << w;
    EXPECT_TRUE(has(w, "t.hot:8:")) << w;
}

TEST(LintDeadCode, BreakInABlockOfACaseWarns)
{
    const std::string w = warnings_of("        switch (p) {\n"
                                      "            case 1: { return; break; }\n"
                                      "            default: this.count = 1;\n"
                                      "        }");
    EXPECT_EQ(count_of(w, "unreachable code"), 1) << w;
}

TEST(LintOrder, WarningsComeInTheOrderOfTheScript)
{
    // The unused warning is ready at the end of the body, and the dead code warning is found at the
    // start of the block. The pass sorts both by position.
    const std::string w = warnings_of("        int a = 1;\n"
                                      "        if (p > 0) { int p = 2; this.count = p; }\n"
                                      "        return;\n"
                                      "        this.count = 4;");
    const size_t unused = w.find("t.hot:6:");
    const size_t shadow = w.find("t.hot:7:");
    const size_t dead = w.find("t.hot:9:");
    ASSERT_NE(unused, std::string::npos) << w;
    ASSERT_NE(shadow, std::string::npos) << w;
    ASSERT_NE(dead, std::string::npos) << w;
    EXPECT_LT(unused, shadow) << w;
    EXPECT_LT(shadow, dead) << w;
}

// ---------------------------------------------------------------- opaque bodies

namespace
{

// A class with the method "run". The body declares the local "x" at the offset 1 and never reads it.
// With "raw" the body also has a RawStmt, which can use any name.
size_t lint_warning_count(bool raw)
{
    using namespace hotc::ast;
    ClassDef cd;
    Method m;
    m.name = "run";
    VarDeclStmt d;
    d.name = "x";
    d.type = "int";
    d.pos = 1;
    d.name_pos = 1;
    m.body.stmts.push_back(make_stmt(std::move(d)));
    if (raw)
        m.body.stmts.push_back(make_stmt(RawStmt{"use(x);", 2}));
    cd.methods.push_back(std::move(m));
    const hotc::LineMap map("int x;\nuse(x);\n");
    hotc::Diagnostics diag;
    hotc::lint_class(cd, map, diag);
    return diag.warnings.size();
}

} // namespace

TEST(LintUnused, BodyWithRawCppTextGivesNoUnusedWarning)
{
    EXPECT_EQ(lint_warning_count(false), 1u);
    EXPECT_EQ(lint_warning_count(true), 0u);
}
