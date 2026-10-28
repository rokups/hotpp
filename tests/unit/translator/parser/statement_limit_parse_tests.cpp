// Tests for the limits of the statement parser: truncated input and deep nesting.
#include <gtest/gtest.h>

#include <string>
#include <vector>
#include <pthread.h>
#include <cstddef>
#include <functional>

#include "emit.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/statement_parse_test_support.h"

using namespace sppc;
using namespace stmt_test;

namespace {

// Runs the function in a thread with a small stack. The parse must not use more.
void run_small_stack(std::size_t bytes, const std::function<void()>& fn)
{
    struct Ctx {
        const std::function<void()>* fn;
    } ctx{&fn};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, bytes);
    pthread_t th;
    ASSERT_EQ(pthread_create(
                  &th, &attr,
                  [](void* p) -> void* {
                      (*static_cast<Ctx*>(p)->fn)();
                      return nullptr;
                  },
                  &ctx),
              0);
    pthread_join(th, nullptr);
    pthread_attr_destroy(&attr);
}

}  // namespace

TEST(StatementParseLimits, Truncation)
{
    // Each prefix of a long block ends: no crash and no hang.
    const std::string full =
        "var a = 1;\nconst std.vector<int> b = f(1, 2);\nif (a < b) { f(a); } else g(b);\n"
        "while (a) a--;\ndo { a++; } while (a < 3);\n"
        "for (var i = 0; i < n; i++) { f(i); }\nforeach (Actor* x in xs) f(x);\n"
        "for (i = 0, j = 1; i < n; i++, j--) ;\nswitch (a) { case 1: case A.B: f(); break;\n"
        "default: { g(); } }\nvar h = (float x) => { if (x) { return x; } return 0; };\n"
        "return a + \"s;}\" + 'c' /* c */;\n// end\n"
        "a = 1;\nb = 2;\nc = 3;\nd = 4;\ne = 5;\nf = 6;\ng = 7;\nh = 8;\ni = 9;\nj = 10;\n"
        "k = 11;\nl = 12;\nm = 13;\nn = 14;\no = 15;\np = 16;\nq = 17;\nr = 18;\ns = 19;\nt = 20;\n"
        "u = 21;\nv = 22;\nw = 23;\nx = 24;\ny = 25;\nz = 26;\nbreak;\ncontinue;\n"
        "if (a) {\n  b = 1;\n  while (c) {\n    d();\n  }\n}\nforeach (var k in ks) {\n  f(k);\n}\n"
        "switch (a) {\n  case 1:\n    f();\n  default:\n    g();\n}\nint z;\nz = 1;\nz = 2;\nz = 3;\n"
        "try {\n  f();\n} catch (const std.exception& e) {\n  throw;\n} catch (...) {\n  throw 1;\n}\n";
    size_t lines = 0;
    for (char c : full) lines += c == '\n';
    EXPECT_GE(lines, 60u);
    {
        Parsed whole(full);
        EXPECT_EQ(whole.diag.items.size(), 0u) << whole.diag.format("t");
    }
    for (size_t n = 0; n <= full.size(); ++n) {
        Parsed p(full.substr(0, n));
        // Every cut gives a block, and a cut in a statement gives an error.
        if (p.diag.items.empty()) continue;
        EXPECT_FALSE(p.diag.items[0].msg.empty());
    }
}

TEST(StatementParseLimits, DeepNesting)
{
    auto fails = [](const std::string& text, const char* msg) {
        Parsed p(text);
        ASSERT_GE(p.diag.items.size(), 1u);
        bool found = false;
        for (const Diag& d : p.diag.items) found |= d.msg.rfind(msg, 0) == 0;
        EXPECT_TRUE(found) << p.diag.items[0].msg;
    };
    const std::size_t n = 100000;
    run_small_stack(512 << 10, [&] {
        fails(std::string(n, '{'), "maximum recursion depth exceeded");
        std::string t;
        for (std::size_t i = 0; i < n; ++i) t += "if (a) ";
        fails(t, "maximum recursion depth exceeded");
        t.clear();
        for (std::size_t i = 0; i < n; ++i) t += "try { ";
        fails(t, "maximum recursion depth exceeded");
        t.clear();
        for (std::size_t i = 0; i < n; ++i) t += "{ while (a) ";
        fails(t, "maximum recursion depth exceeded");
        // A lambda in a lambda crosses the expression grammar each time.
        t.clear();
        for (std::size_t i = 0; i < n; ++i) t += "f(() => { ";
        fails(t, "maximum");
        t.clear();
        for (std::size_t i = 0; i < 20; ++i) t += "f(() => { ";
        t += "x;";
        for (std::size_t i = 0; i < 20; ++i) t += " });";
        fails(t, "maximum nesting depth exceeded");
    });
    // A moderate depth works (the limit counts a block and an if as two levels).
    std::string ok, close;
    for (int i = 0; i < 20; ++i) {
        ok += "{ if (a) ";
        close += " }";
    }
    EXPECT_EQ(round_trip(ok + "x;" + close).empty(), false);
    std::string lam, lclose;
    for (int i = 0; i < 8; ++i) {
        lam += "f(() => { ";
        lclose += " });";
    }
    run_small_stack(512 << 10, [&] { EXPECT_EQ(round_trip(lam + "x;" + lclose).empty(), false); });
}
