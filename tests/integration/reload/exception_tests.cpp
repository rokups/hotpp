// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the statements throw, try, catch and finally with the backend that HOT_BACKEND selected.
//
// CMake defines the macro HOT_SOURCE_DIR.
#include "hot/backend.h"
#include "hot/config.h"
#include "hot/runtime/errors.h"
#include "support/backend_fixture.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include "Damageable.h"
#include "Guarded.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

using test_support::kDt;
using test_support::Session;
using test_support::source;

// The statements throw, try and catch translate to C++ that compiles and runs.
TEST_P(BackendTests, TryCatchThrowRun)
{
    const hot::ScriptSource src = {"exc.hot", R"(
using "Actor.h";
using <stdexcept>;

class Player : Actor {
    float total = 0.0;

    override void Tick(float dt) {
        try {
            throw std.runtime_error("boom");
        } catch (std.exception e) {
            var w = e.what();
            if (w[0] == 'b') {
                this.total = this.total + 1.0;
            }
        }
        try {
            try {
                throw 3;
            } catch (...) {
                throw;
            }
        } catch (double v) {
            this.total = 100.0;
        } catch (int v) {
            this.total = this.total + v;
        }
    }

    override void TakeDamage(float amount) {}
    override void TakeDamage(ref Actor source, float amount) {}
}
)"};
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    s.world.Tick(kDt);
    ASSERT_NE(s.field("total"), nullptr);
    EXPECT_EQ(*s.field("total"), 4.0f);
}

// A finally block runs on the normal path and on the throw path. An exception from
// a finally body is dropped. Nested finally blocks run from the inside out.
TEST_P(BackendTests, TryFinallyRun)
{
    const hot::ScriptSource src = {"fin.hot", R"(
using "Actor.h";

class Player : Actor {
    float total = 0.0;

    override void Tick(float dt) {
        try {
            this.total = this.total + 1.0;
        } finally {
            this.total = this.total + 10.0;
        }
        try {
            try {
                throw 5;
            } finally {
                this.total = this.total + 100.0;
            }
        } catch (int v) {
            this.total = this.total + v;
        }
        try {
            this.total = this.total + 1000.0;
        } catch (...) {
            this.total = -1.0;
        } finally {
            throw 1;
        }
        try {
            try {
                this.total = this.total + 0.0;
            } finally {
                this.total = this.total + 2.0;
            }
        } finally {
            this.total = this.total * 2.0;
        }
    }

    override void TakeDamage(float amount) {}
    override void TakeDamage(ref Actor source, float amount) {}
}
)"};
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    s.world.Tick(kDt);
    ASSERT_NE(s.field("total"), nullptr);
    // (1 + 10 + 100 + 5 + 1000 + 2) * 2
    EXPECT_EQ(*s.field("total"), 2236.0f);
}

// Restores the default handler at the end of the scope, also when an assertion fails.
struct HandlerGuard
{
    ~HandlerGuard() { hot::set_script_error_handler({}); }
};

// The class of the recursion tests. Tick calls the body that the test gives.
const char* const kRecursionHead = R"(
using "Actor.h";

class Player : Actor {
    int result = 0;
    float caught = 0.0;
    float after = 0.0;

    int Down(int n) {
        if (n == 0) { return 0; }
        return this.Down(n - 1) + 1;
    }

    int Forever(int n) {
        return this.Forever(n + 1) + 1;
    }

    virtual int VForever(int n) {
        return this.VForever(n + 1) + 1;
    }

    int Probe(int n) {
        this.result = n;
        return this.Probe(n + 1) + 1;
    }

    override void Tick(float dt) {
)";
const char* const kRecursionTail = R"(
    }

    override void TakeDamage(float amount) {}
    override void TakeDamage(ref Actor source, float amount) {}
}
)";

hot::ScriptSource recursion_source(const char* name, const std::string& body)
{
    return {name, std::string(kRecursionHead) + body + kRecursionTail};
}

// A recursion under the limit works, and the depth is 0 after it.
TEST_P(BackendTests, RecursionUnderTheLimitWorks)
{
    // The depth is under the limit. The limit is 1000 on Windows, so the depth is the smaller value.
    const int depth = static_cast<int>(std::min<uint32_t>(1000, hot::max_call_depth() - 10));
    const hot::ScriptSource src =
        recursion_source("rec_ok.hot", "this.result = this.Down(" + std::to_string(depth) + ");");
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    s.world.Tick(kDt);
    EXPECT_EQ(*hot::field_ptr<int>(s.inst.shell, "result"), depth);
    EXPECT_EQ(hot::detail::call_depth(), 0u);
}

// An infinite recursion throws ScriptError "stack overflow". The script catches it, and the
// depth is 0 after it.
TEST_P(BackendTests, InfiniteRecursionIsCaughtInScript)
{
    const hot::ScriptSource src = recursion_source("rec_catch.hot", R"(
        try {
            this.result = this.Forever(0);
            this.after = 1.0;
        } catch (ScriptError e) {
            this.caught = 1.0;
        }
        this.result = this.Down(10);
)");
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    HandlerGuard guard;
    int reports = 0;
    hot::set_script_error_handler([&reports](const hot::ScriptErrorReport&) { ++reports; });
    s.world.Tick(kDt);
    EXPECT_EQ(*s.field("caught"), 1.0f);
    EXPECT_EQ(*s.field("after"), 0.0f);
    EXPECT_EQ(*hot::field_ptr<int>(s.inst.shell, "result"), 10);
    EXPECT_EQ(reports, 0);
    EXPECT_EQ(hot::detail::call_depth(), 0u);
}

// An infinite recursion of a virtual function that nobody catches goes to the error handler. The
// host stays alive, and the next tick works.
TEST_P(BackendTests, InfiniteVirtualRecursionIsReported)
{
    const hot::ScriptSource src = recursion_source("rec_report.hot", R"(
        this.after = this.after + 1.0;
        this.result = this.VForever(0);
)");
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    HandlerGuard guard;
    std::vector<std::string> messages;
    hot::set_script_error_handler([&messages](const hot::ScriptErrorReport& e)
                                  { messages.push_back(std::string(e.type) + ": " + e.message); });
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0], "hot::ScriptError: stack overflow");
    EXPECT_EQ(*s.field("after"), 2.0f);
    EXPECT_EQ(hot::detail::call_depth(), 0u);
}

// Restores the call depth limit at the end of the scope, also when an assertion fails.
struct LimitGuard
{
    uint32_t old = hot::max_call_depth();
    ~LimitGuard() { hot::set_max_call_depth(old); }
};

// The setter keeps the limit in the range that the 16 bit frame field holds.
TEST(CallDepthLimit, SetterClampsTheLimit)
{
    LimitGuard guard;
    hot::set_max_call_depth(50);
    EXPECT_EQ(hot::max_call_depth(), 50u);
    hot::set_max_call_depth(65519);
    EXPECT_EQ(hot::max_call_depth(), 65519u);
    hot::set_max_call_depth(65520);
    EXPECT_EQ(hot::max_call_depth(), 65519u);
    hot::set_max_call_depth(0xFFFFFFFFu);
    EXPECT_EQ(hot::max_call_depth(), 65519u);
}

// The limit is exact: with the limit 50, the frames are Tick and 49 calls of Probe. The call that
// would be the 51st frame throws.
TEST_P(BackendTests, RecursionLimitIsExactAndCanChange)
{
    const hot::ScriptSource src = recursion_source("rec_exact.hot", R"(
        try {
            this.Probe(0);
        } catch (ScriptError e) {
            this.caught = 1.0;
        }
)");
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    LimitGuard guard;
    hot::set_max_call_depth(50);
    s.world.Tick(kDt);
    EXPECT_EQ(*s.field("caught"), 1.0f);
    EXPECT_EQ(*hot::field_ptr<int>(s.inst.shell, "result"), 48);
    EXPECT_EQ(hot::detail::call_depth(), 0u);
    hot::set_max_call_depth(100);
    s.world.Tick(kDt);
    EXPECT_EQ(*hot::field_ptr<int>(s.inst.shell, "result"), 98);
    EXPECT_EQ(hot::detail::call_depth(), 0u);
}

// A free function and a static method count their frames too.
TEST_P(BackendTests, FreeFunctionAndStaticMethodRecursionThrow)
{
    const hot::ScriptSource src = {"rec_free.hot", R"(
using "Actor.h";

int FreeForever(int n) {
    return FreeForever(n + 1) + 1;
}

class Player : Actor {
    float caught = 0.0;
    float caught2 = 0.0;
    float after = 0.0;

    static int StaticForever(int n) {
        return Player.StaticForever(n + 1) + 1;
    }

    override void Tick(float dt) {
        try {
            var a = FreeForever(0);
        } catch (ScriptError e) {
            this.caught = 1.0;
        }
        try {
            var b = Player.StaticForever(0);
        } catch (ScriptError e) {
            this.caught2 = 1.0;
        }
        this.after = 1.0;
    }

    override void TakeDamage(float amount) {}
    override void TakeDamage(ref Actor source, float amount) {}
}
)"};
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    s.world.Tick(kDt);
    EXPECT_EQ(*s.field("caught"), 1.0f);
    EXPECT_EQ(*s.field("caught2"), 1.0f);
    EXPECT_EQ(*s.field("after"), 1.0f);
    EXPECT_EQ(hot::detail::call_depth(), 0u);
}

// A destructor that runs while the "stack overflow" unwinds the stack calls a script function at the
// limit. That call must not throw: the runtime would log "the destructor of Sentinel threw" on stderr
// and skip the rest of the destructor.
TEST_P(BackendTests, DestructorDuringTheUnwindOfAStackOverflow)
{
    const hot::ScriptSource src = {"rec_dtor.hot", R"(
using "Actor.h";

class Sentinel {
    int Touch() { return 1; }
    ~Sentinel() { var x = this.Touch(); }
}

class Player : Actor {
    float caught = 0.0;
    float after = 0.0;

    int Deep(int n) {
        $Sentinel s = new $Sentinel();
        return this.Deep(n + 1) + 1;
    }

    override void Tick(float dt) {
        try {
            var a = this.Deep(0);
        } catch (ScriptError e) {
            this.caught = 1.0;
        }
        this.after = 1.0;
    }

    override void TakeDamage(float amount) {}
    override void TakeDamage(ref Actor source, float amount) {}
}
)"};
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    LimitGuard guard;
    hot::set_max_call_depth(200);
    HandlerGuard handler;
    int reports = 0;
    hot::set_script_error_handler([&reports](const hot::ScriptErrorReport&) { ++reports; });
    testing::internal::CaptureStderr();
    s.world.Tick(kDt);
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(err.find("threw"), std::string::npos) << err;
    EXPECT_EQ(*s.field("caught"), 1.0f);
    EXPECT_EQ(*s.field("after"), 1.0f);
    EXPECT_EQ(reports, 0);
    EXPECT_EQ(hot::detail::call_depth(), 0u);
}

// The integer divide and modulo are checked. The checks do not depend on HOT_RUNTIME_CHECKS.
const char* const kArithSource = R"(
using "Actor.h";
using "Damageable.h";

class Boss : Actor, IDamageable {
    int zero = 0;
    int three = 3;
    int imin = -2147483647 - 1;
    long lmin = -9223372036854775807 - 1;
    int[2] arr = [7, 9];
    int next_calls = 0;
    float caught = 0.0;
    float after = 0.0;
    float r_div = 0.0;
    float r_mod = 0.0;
    float r_ldiv = 0.0;
    float r_lmod = 0.0;
    float r_short = 0.0;
    float r_udiv = 0.0;
    float r_assign = 0.0;
    float r_amod = 0.0;
    float r_elem = 0.0;
    float r_inf = 0.0;
    float r_lit = 0.0;
    float r_prop = 0.0;
    float r_ass_min = 0.0;
    float r_unsigned = 0.0;
    float n_div0 = 0.0;
    float n_mod0 = 0.0;
    float n_assign0 = 0.0;
    float n_amod0 = 0.0;
    float n_ldiv0 = 0.0;
    float n_udiv0 = 0.0;
    float n_byte0 = 0.0;
    float n_lit0 = 0.0;
    float n_elem0 = 0.0;
    float n_prop0 = 0.0;
    float msg_ok = 0.0;
    int P { get; set; } = 8;

    int Div(int a, int b) { return a / b; }
    int Mod(int a, int b) { return a % b; }
    long LDiv(long a, long b) { return a / b; }
    long LMod(long a, long b) { return a % b; }
    uint UDiv(uint a, uint b) { return a / b; }
    int ShortDiv(short a, short b) { return a / b; }
    int ByteDiv(sbyte a, sbyte b) { return a / b; }
    int Assign(int a, int b) { a /= b; return a; }
    int AssignMod(int a, int b) { a %= b; return a; }
    int LitDiv(int a) { return a / 2; }
    int LitZero(int a) { return a / 0; }
    float FDiv(float a, float b) { return a / b; }
    int Next() { this.next_calls = this.next_calls + 1; return 1; }

    override int Absorb(int amount) { return 100 / amount; }

    override void Tick(float dt) {
        this.r_div = this.Div(this.imin, -1);
        this.r_mod = this.Mod(this.imin, -1);
        this.r_ldiv = this.LDiv(this.lmin, -1);
        this.r_lmod = this.LMod(this.lmin, -1);
        this.r_short = this.ShortDiv(-32768, -1);
        this.r_udiv = this.UDiv(7, 2);
        this.r_assign = this.Assign(9, 2);
        this.r_amod = this.AssignMod(9, 4);
        this.r_ass_min = this.Assign(this.imin, -1);
        this.r_lit = this.LitDiv(9);
        this.r_inf = this.FDiv(1.0, this.zero);
        this.arr[this.Next()] /= this.three;
        this.r_elem = this.arr[1];
        this.P /= this.three;
        this.r_prop = this.P;
        try { var x = this.Div(1, this.zero); } catch (ArithmeticError e) {
            this.n_div0 = 1.0;
            if (e.what()[0] == 'i') { this.msg_ok = 1.0; }
        }
        try { var x = this.Mod(1, this.zero); } catch (ArithmeticError e) { this.n_mod0 = 1.0; }
        try { var x = this.Assign(1, this.zero); } catch (ScriptError e) { this.n_assign0 = 1.0; }
        try { var x = this.AssignMod(1, this.zero); } catch (ScriptError e) { this.n_amod0 = 1.0; }
        try { var x = this.LDiv(1, 0); } catch (ArithmeticError e) { this.n_ldiv0 = 1.0; }
        try { var x = this.UDiv(1, 0); } catch (ArithmeticError e) { this.n_udiv0 = 1.0; }
        try { var x = this.ByteDiv(1, 0); } catch (ArithmeticError e) { this.n_byte0 = 1.0; }
        try { var x = this.LitZero(1); } catch (ArithmeticError e) { this.n_lit0 = 1.0; }
        try { this.arr[0] %= this.zero; } catch (ArithmeticError e) { this.n_elem0 = 1.0; }
        try { this.P /= this.zero; } catch (ArithmeticError e) { this.n_prop0 = 1.0; }
        try { var x = HitThrough(*this, 0); } catch (ScriptError e) { this.caught = 1.0; }
        this.after = 1.0;
    }
}
)";

TEST_P(BackendTests, IntegerDivideAndModuloAreChecked)
{
    const hot::ScriptSource src = {"arith.hot", kArithSource};
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module), "Boss"));
    s.commit_at_frame_boundary();
    HandlerGuard guard;
    int reports = 0;
    hot::set_script_error_handler([&reports](const hot::ScriptErrorReport&) { ++reports; });
    s.world.Tick(kDt);
    EXPECT_EQ(*s.field("r_div"), -2147483648.0f); // INT_MIN / -1 wraps
    EXPECT_EQ(*s.field("r_mod"), 0.0f);
    EXPECT_EQ(*s.field("r_ldiv"), -9223372036854775808.0f);
    EXPECT_EQ(*s.field("r_lmod"), 0.0f);
    EXPECT_EQ(*s.field("r_short"), 32768.0f); // short promotes to int
    EXPECT_EQ(*s.field("r_udiv"), 3.0f);
    EXPECT_EQ(*s.field("r_assign"), 4.0f);
    EXPECT_EQ(*s.field("r_amod"), 1.0f);
    EXPECT_EQ(*s.field("r_ass_min"), -2147483648.0f);
    EXPECT_EQ(*s.field("r_lit"), 4.0f);
    EXPECT_TRUE(std::isinf(*s.field("r_inf"))); // a float divide by zero does not throw
    EXPECT_EQ(*s.field("r_elem"), 3.0f);
    EXPECT_EQ(*hot::field_ptr<int>(s.inst.shell, "next_calls"), 1); // the target is evaluated one time
    EXPECT_EQ(*s.field("r_prop"), 2.0f);
    for (const char* name : {"n_div0", "n_mod0", "n_assign0", "n_amod0", "n_ldiv0", "n_udiv0", "n_byte0", "n_lit0",
             "n_elem0", "n_prop0", "msg_ok", "caught", "after"})
        EXPECT_EQ(*s.field(name), 1.0f) << name;
    EXPECT_EQ(reports, 0);
    EXPECT_EQ(hot::detail::call_depth(), 0u);
    // A call from the host reports the error as a ScriptError and gives a default value.
    std::vector<std::string> types;
    hot::set_script_error_handler([&types](const hot::ScriptErrorReport& e) { types.push_back(e.type); });
    auto* dmg = dynamic_cast<IDamageable*>(s.actor);
    ASSERT_NE(dmg, nullptr);
    EXPECT_EQ(dmg->Absorb(5), 20);
    EXPECT_TRUE(types.empty());
    EXPECT_EQ(dmg->Absorb(0), 0);
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0], "hot::ArithmeticError");
}

// The shifts, the signed overflow and the cast from a float to an integer. The checks and the wrap
// do not depend on HOT_RUNTIME_CHECKS. The values come from fields and parameters, so the C++
// compiler cannot fold them.
const char* const kShiftSource = R"(
using "Actor.h";
using "Util.h";

class Boss : Actor {
    int imax = 2147483647;
    int imin = -2147483647 - 1;
    float fz = 0.0;
    float caught = 0.0;
    float after = 0.0;
    float msg_ok = 0.0;
    float ok = 0.0;
    float bad = 0.0;
    int P { get; set; } = 1;

    int Shl(int a, int n) { return a << n; }
    int Shr(int a, int n) { return a >> n; }
    uint UShl(uint a, int n) { return a << n; }
    uint UShr(uint a, int n) { return a >> n; }
    long LShl(long a, int n) { return a << n; }
    long LShr(long a, long n) { return a >> n; }
    int ByteShl(byte a, int n) { return a << n; }
    int SByteShl(sbyte a, int n) { return a << n; }
    int ShortShr(short a, int n) { return a >> n; }
    int ShlAssign(int a, int n) { a <<= n; return a; }
    int ShrAssign(int a, int n) { a >>= n; return a; }
    byte ByteAssign(byte a, int n) { a <<= n; return a; }
    int LitShl(int a) { return a << 3; }
    int LitBad(int a) { return a << 32; }
    int LitNeg(int a) { return a >> -1; }
    int Add(int a, int b) { return a + b; }
    int Sub(int a, int b) { return a - b; }
    int Mul(int a, int b) { return a * b; }
    int Neg(int a) { return -a; }
    int Inc(int a) { a++; return a; }
    int PlusOneIsGreater(int a) { if (a + 1 > a) { return 1; } return 0; }
    int AbsIsNegative(int a) { if (a < 0) { a = -a; } if (a < 0) { return 1; } return 0; }
    int F2I(float f) { return (int)f; }
    int D2I(double d) { return (int)d; }
    int Fn2I(double d) { return int(d); }
    int Sc2I(double d) { return static_cast<int>(d); }
    long D2L(double d) { return (long)d; }
    uint D2U(double d) { return (uint)d; }
    // A local goes to a native "int*" out-parameter as a real address: Bump writes through it.
    int BumpLocal() { int n = 5; Bump(n, 2); Bump(n, 3); return n; }
    // A local "char*" goes to a native "char**" out-parameter. Consumed counts the parsed characters.
    int OutPointer() { char* end = null; float v = ParseFloat("12.5xyz", end); return Consumed("12.5xyz", end) * 100 + (int)v; }
    byte D2B(double d) { return (byte)d; }
    sbyte D2SB(double d) { return (sbyte)d; }

    void Check(bool cond) { if (cond) { this.ok = this.ok + 1.0; } else { this.bad = this.bad + 1.0; } }

    override void Tick(float dt) {
        this.Check(this.Shl(1, 31) == this.imin);
        this.Check(this.Shl(1, 0) == 1);
        this.Check(this.Shl(3, 4) == 48);
        this.Check(this.Shl(-1, 4) == -16);
        this.Check(this.Shl(this.imax, 1) == -2);
        this.Check(this.Shr(this.imin, 31) == -1);
        this.Check(this.Shr(-8, 1) == -4);
        this.Check(this.Shr(48, 4) == 3);
        this.Check(this.UShl(1, 31) == 2147483648);
        this.Check(this.UShr(2147483648, 31) == 1);
        this.Check(this.LShl(1, 63) == -9223372036854775807 - 1);
        this.Check(this.LShl(1, 40) == 1099511627776);
        this.Check(this.LShr(-1, 63) == -1);
        this.Check(this.ByteShl(255, 24) == -16777216);
        this.Check(this.ByteShl(1, 8) == 256);
        this.Check(this.SByteShl(-1, 7) == -128);
        this.Check(this.ShortShr(-32768, 15) == -1);
        this.Check(this.ShlAssign(3, 2) == 12);
        this.Check(this.ShrAssign(-16, 2) == -4);
        this.Check(this.ByteAssign(129, 1) == 2);
        this.Check(this.LitShl(5) == 40);
        this.P <<= 4;
        this.Check(this.P == 16);
        this.P >>= 2;
        this.Check(this.P == 4);
        try { var x = this.Shl(1, 32); } catch (ArithmeticError e) {
            this.Check(true);
            if (e.what()[0] == 's') { this.msg_ok = 1.0; }
        }
        try { var x = this.Shl(1, -1); } catch (ArithmeticError e) { this.Check(true); }
        try { var x = this.Shr(1, 32); } catch (ArithmeticError e) { this.Check(true); }
        try { var x = this.Shr(1, -1); } catch (ScriptError e) { this.Check(true); }
        try { var x = this.UShl(1, 33); } catch (ArithmeticError e) { this.Check(true); }
        try { var x = this.LShl(1, 64); } catch (ArithmeticError e) { this.Check(true); }
        try { var x = this.LShr(1, -1); } catch (ArithmeticError e) { this.Check(true); }
        try { var x = this.ByteShl(1, 32); } catch (ArithmeticError e) { this.Check(true); }
        try { var x = this.ShlAssign(1, 32); } catch (ArithmeticError e) { this.Check(true); }
        try { var x = this.ShrAssign(1, -1); } catch (ArithmeticError e) { this.Check(true); }
        try { var x = this.ByteAssign(1, 32); } catch (ArithmeticError e) { this.Check(true); }
        try { var x = this.LitBad(1); } catch (ArithmeticError e) { this.Check(true); }
        try { var x = this.LitNeg(1); } catch (ArithmeticError e) { this.Check(true); }
        try { this.P <<= 32; } catch (ArithmeticError e) { this.Check(true); }
        this.Check(this.P == 4);
        // The signed overflow wraps.
        this.Check(this.Add(this.imax, 1) == this.imin);
        this.Check(this.Sub(this.imin, 1) == this.imax);
        this.Check(this.Mul(this.imax, 2) == -2);
        this.Check(this.Neg(this.imin) == this.imin);
        this.Check(this.Inc(this.imax) == this.imin);
        this.Check(this.PlusOneIsGreater(this.imax) == 0);
        this.Check(this.PlusOneIsGreater(5) == 1);
        this.Check(this.AbsIsNegative(this.imin) == 1);
        this.Check(this.AbsIsNegative(-5) == 0);
        // A float cast cuts the fraction, saturates, and gives 0 for NaN.
        this.Check(this.F2I(3.9) == 3);
        this.Check(this.F2I(-3.9) == -3);
        this.Check(this.D2I(3.9) == 3);
        this.Check(this.D2I(-3.9) == -3);
        this.Check(this.D2I(1e20) == this.imax);
        this.Check(this.D2I(-1e20) == this.imin);
        this.Check(this.F2I(1e20) == this.imax);
        this.Check(this.F2I(-1e20) == this.imin);
        this.Check(this.F2I(this.fz / this.fz) == 0);
        this.Check(this.F2I(1.0 / this.fz) == this.imax);
        this.Check(this.F2I(-1.0 / this.fz) == this.imin);
        this.Check(this.Fn2I(1e20) == this.imax);
        this.Check(this.Fn2I(3.9) == 3);
        this.Check(this.Sc2I(-1e20) == this.imin);
        this.Check(this.Sc2I(-3.9) == -3);
        this.Check(this.D2L(1e30) == 9223372036854775807);
        this.Check(this.D2L(-1e30) == -9223372036854775807 - 1);
        this.Check(this.D2L(this.fz / this.fz) == 0);
        this.Check(this.BumpLocal() == 10);
        this.Check(this.OutPointer() == 412);
        this.Check(this.D2U(1e20) == 4294967295);
        this.Check(this.D2U(-5.0) == 0);
        this.Check(this.D2B(300.0) == 255);
        this.Check(this.D2B(-1.0) == 0);
        this.Check(this.D2SB(-300.0) == -128);
        this.Check(this.D2SB(300.0) == 127);
        this.after = 1.0;
    }
}
)";

// Loads the script with the backend, runs one tick and reads the counters of the script checks.
void run_shift_script(hot::Backend& be)
{
    const hot::ScriptSource src = {"shift.hot", kShiftSource};
    hot::LoadResult r = be.load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module), "Boss"));
    s.commit_at_frame_boundary();
    HandlerGuard guard;
    int reports = 0;
    hot::set_script_error_handler([&reports](const hot::ScriptErrorReport&) { ++reports; });
    s.world.Tick(kDt);
    EXPECT_EQ(*s.field("bad"), 0.0f);
    EXPECT_EQ(*s.field("ok"), 73.0f);
    EXPECT_EQ(*s.field("msg_ok"), 1.0f);
    EXPECT_EQ(*s.field("after"), 1.0f);
    EXPECT_EQ(reports, 0);
    EXPECT_EQ(hot::detail::call_depth(), 0u);
}

TEST_P(BackendTests, ShiftsOverflowAndFloatCastsAreDefined)
{
    run_shift_script(*backend);
}

// The same script with the optimizer. The compiler exploits a signed overflow unless the
// script compile has -fwrapv.
TEST_P(BackendTests, SignedOverflowWrapsWithTheOptimizer)
{
    hot::DefaultBackendOptions o;
    o.extra_flags = {"-O2"};
    std::unique_ptr<hot::Backend> optimized = hot::make_default_backend(o);
    run_shift_script(*optimized);
}

// front, back, pop and at on an empty container throw. A native std exception in a try block
// with a catch clause for ScriptError becomes a catchable NativeError. The checks do not depend on
// HOT_RUNTIME_CHECKS.
const char* const kContainerSource = R"(
using "Actor.h";
using "Damageable.h";
using <vector>;
using <deque>;
using <list>;
using <queue>;
using <stack>;
using <map>;
using <string>;
using <optional>;
using <stdexcept>;

class Boss : Actor, IDamageable {
    std.vector<int> v = [];
    std.deque<int> d = [];
    std.list<int> l = [];
    std.queue<int> q;
    std.stack<int> st;
    std.priority_queue<int> pq;
    std.map<std.string, int> m = ["a": 1];
    float n_front = 0.0;
    float n_back = 0.0;
    float n_pop_back = 0.0;
    float n_pop_front = 0.0;
    float n_dq_front = 0.0;
    float n_list_back = 0.0;
    float n_queue_pop = 0.0;
    float n_stack_top = 0.0;
    float n_stack_pop = 0.0;
    float n_pq_top = 0.0;
    float n_at = 0.0;
    float n_native = 0.0;
    float n_native_type = 0.0;
    float n_native_sub = 0.0;
    float n_finally = 0.0;
    float n_order = 0.0;
    float n_std = 0.0;
    float r_ok = 0.0;
    float r_at = 0.0;
    float after = 0.0;

    override int Absorb(int amount) { return this.v.back(); }

    override void Tick(float dt) {
        try { var x = this.v.front(); } catch (IndexOutOfRange e) { this.n_front = 1.0; }
        try { var x = this.v.back(); } catch (ScriptError e) { this.n_back = 1.0; }
        try { this.v.pop_back(); } catch (IndexOutOfRange e) { this.n_pop_back = 1.0; }
        try { this.d.pop_front(); } catch (IndexOutOfRange e) { this.n_pop_front = 1.0; }
        try { var x = this.d.front(); } catch (IndexOutOfRange e) { this.n_dq_front = 1.0; }
        try { var x = this.l.back(); } catch (IndexOutOfRange e) { this.n_list_back = 1.0; }
        try { this.q.pop(); } catch (IndexOutOfRange e) { this.n_queue_pop = 1.0; }
        try { var x = this.st.top(); } catch (IndexOutOfRange e) { this.n_stack_top = 1.0; }
        try { this.st.pop(); } catch (IndexOutOfRange e) { this.n_stack_pop = 1.0; }
        try { var x = this.pq.top(); } catch (IndexOutOfRange e) { this.n_pq_top = 1.0; }
        this.v.push_back(4);
        this.v.push_back(5);
        this.r_ok = this.v.front() * 10 + this.v.back();
        this.v.pop_back();
        this.r_at = this.m.at("a");
        try { var x = this.m.at("zz"); } catch (ScriptError e) {
            this.n_at = 1.0;
        }
        try { throw std.out_of_range("native"); } catch (ScriptError e) {
            this.n_native = 1.0;
            if (e.what()[0] == 'n') { this.n_native_type = 1.0; }
        }
        try { throw std.out_of_range("native"); } catch (NativeError e) { this.n_native_sub = 1.0; }
        try {
            try { throw std.bad_optional_access(); } finally { this.n_finally = 1.0; }
        } catch (ScriptError e) { this.n_native = this.n_native + 1.0; }
        try { throw std.out_of_range("order"); }
        catch (ScriptError e) { this.n_order = 5.0; }
        catch (std.out_of_range e) { this.n_order = 1.0; }
        try { throw std.out_of_range("order"); }
        catch (std.exception e) { if (e.what()[0] == 'o') { this.n_std = 1.0; } }
        this.after = 1.0;
    }
}
)";

TEST_P(BackendTests, EmptyContainerOperationsAndNativeExceptionsAreCatchable)
{
    const hot::ScriptSource src = {"container_ops.hot", kContainerSource};
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module), "Boss"));
    s.commit_at_frame_boundary();
    HandlerGuard guard;
    std::vector<std::string> types, messages;
    hot::set_script_error_handler(
        [&](const hot::ScriptErrorReport& e)
        {
            types.push_back(e.type);
            messages.push_back(e.message);
        });
    s.world.Tick(kDt);
    for (const char* name : {"n_front", "n_back", "n_pop_back", "n_pop_front", "n_dq_front", "n_list_back",
             "n_queue_pop", "n_stack_top", "n_stack_pop", "n_pq_top", "n_at", "n_native_type", "n_native_sub",
             "n_finally", "n_order", "n_std", "after"})
        EXPECT_EQ(*s.field(name), 1.0f) << name;
    EXPECT_EQ(*s.field("n_native"), 2.0f); // two catches
    EXPECT_EQ(*s.field("r_ok"), 45.0f);
    EXPECT_EQ(*s.field("r_at"), 1.0f);
    EXPECT_TRUE(types.empty());
    EXPECT_EQ(hot::detail::call_depth(), 0u);
    // A call from the host reports the error and gives a default value.
    auto* dmg = dynamic_cast<IDamageable*>(s.actor);
    ASSERT_NE(dmg, nullptr);
    EXPECT_EQ(dmg->Absorb(0), 4); // v has one element left
    EXPECT_TRUE(types.empty());
}

TEST_P(BackendTests, NativeExceptionOutsideTryIsReportedWithItsType)
{
    const hot::ScriptSource src = {"native_report.hot", R"(
using "Actor.h";
using "Damageable.h";
using <map>;
using <string>;

class Boss : Actor, IDamageable {
    std.map<std.string, int> m = [];
    override int Absorb(int amount) { return this.m.at("none"); }
    override void Tick(float dt) { }
}
)"};
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module), "Boss"));
    s.commit_at_frame_boundary();
    HandlerGuard guard;
    std::vector<std::string> types, messages;
    hot::set_script_error_handler(
        [&](const hot::ScriptErrorReport& e)
        {
            types.push_back(e.type);
            messages.push_back(e.message);
        });
    auto* dmg = dynamic_cast<IDamageable*>(s.actor);
    ASSERT_NE(dmg, nullptr);
    EXPECT_EQ(dmg->Absorb(1), 0);
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0], "hot::ScriptError"); // the helper maps out_of_range
    EXPECT_NE(messages[0].find("key not found"), std::string::npos) << messages[0];
}

// A null handle passed to a native "const T&" parameter throws NullReference. A null handle passed
// to a native "T*" parameter gives nullptr. The checks do not depend on HOT_RUNTIME_CHECKS.
TEST_P(BackendTests, NullHandleToNativeReferenceParameterIsCatchable)
{
    const hot::ScriptSource src = {"null_native_ref.hot", R"(
using "Actor.h";
using "Equip.h";

[native, handle("std::shared_ptr<$T>")] class Charm;

class Boss : Actor {
    Charm last;
    float n_ref = 0.0;
    float n_ptr = 0.0;
    float after = 0.0;

    override void Tick(float dt) {
        Charm local = this.last;
        try { var x = Shine(local); } catch (NullReference e) { this.n_ref = 1.0; }
        try { var x = Shine(this.last); } catch (NullReference e) { this.n_ref = this.n_ref + 1.0; }
        this.n_ptr = Polish(local);
        this.after = 1.0;
    }
}
)"};
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module), "Boss"));
    s.commit_at_frame_boundary();
    HandlerGuard guard;
    std::vector<std::string> types;
    hot::set_script_error_handler([&](const hot::ScriptErrorReport& e) { types.push_back(e.type); });
    s.world.Tick(kDt);
    EXPECT_EQ(*s.field("n_ref"), 2.0f);
    EXPECT_EQ(*s.field("n_ptr"), -1.0f);
    EXPECT_EQ(*s.field("after"), 1.0f);
    EXPECT_TRUE(types.empty());
}

#if HOT_RUNTIME_CHECKS

// The class of the checks tests. Tick runs the body that the test gives. The handle is null.
const char* const kChecksHead = R"(
using "Actor.h";
using "Weapon.h";

[native, handle("std::shared_ptr<$T>")] class Weapon;

class Player : Actor {
    Weapon weapon = null;
    int[4] vals;
    float caught = 0.0;
    float caught2 = 0.0;
    float ticks = 0.0;
    float after = 0.0;
    float v = 0.0;

    override void Tick(float dt) {
)";
const char* const kChecksTail = R"(
    }

    override void TakeDamage(float amount) {}
    override void TakeDamage(ref Actor source, float amount) {}
}
)";

hot::ScriptSource checks_source(const char* name, const std::string& body)
{
    return {name, std::string(kChecksHead) + body + kChecksTail};
}

struct RecordedReport
{
    std::string type, message, class_name;
    int count = 0;
};

// A null handle in a try block throws hot::NullReference. The script catches it.
TEST_P(BackendTests, NullDerefCaughtInScript)
{
    const hot::ScriptSource src = checks_source("chk_null.hot", R"(
        try {
            this.v = this.weapon.dmg;
            this.after = 1.0;
        } catch (NullReference e) {
            this.caught = 1.0;
        }
)");
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    s.world.Tick(kDt);
    EXPECT_EQ(*s.field("caught"), 1.0f);
    EXPECT_EQ(*s.field("after"), 0.0f);
}

const char* const kBoxClass = R"(
class Box {
    int n = 3;
    $Box next;
}
)";

// "a?.b" in an expression gives an optional for a value member. A null handle gives an empty one.
// A handle member keeps its type. A chain of "?." stops at the first null.
TEST_P(BackendTests, SafeAccessInAnExpression)
{
    const hot::ScriptSource src = {"chk_safe_expr.hot", std::string(kChecksHead) + R"(
        var d = this.weapon?.dmg;
        if (d == null) { this.caught = 1.0; }
        this.v = this.weapon?.dmg ?? 7.0;
        $Box a = new $Box();
        var n = a?.n;
        if (n != null) { this.caught2 = 1.0; }
        $Box none = null;
        var np = none?.n;
        if (np == null) { this.after = 1.0; }
        var deep = a?.next?.n;
        if (deep == null) { this.ticks = 1.0; }
)" + kChecksTail + kBoxClass};
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    s.world.Tick(kDt);
    EXPECT_EQ(*s.field("caught"), 1.0f);
    EXPECT_EQ(*s.field("v"), 7.0f);
    EXPECT_EQ(*s.field("caught2"), 1.0f);
    EXPECT_EQ(*s.field("after"), 1.0f);
    EXPECT_EQ(*s.field("ticks"), 1.0f);
}

// A cast to the same handle type is a checked unwrap: a null handle throws hot::NullReference.
// The script catches it. A handle that is not null passes.
TEST_P(BackendTests, CheckedUnwrapCastThrowsOnNull)
{
    const hot::ScriptSource src = {"chk_unwrap.hot", std::string(kChecksHead) + R"(
        $Box a = new $Box();
        var ok = ($Box)a;
        this.v = 5.0;
        $Box none = null;
        try {
            var bad = ($Box)none;
            this.after = 1.0;
        } catch (NullReference e) {
            this.caught = 1.0;
        }
)" + kChecksTail + kBoxClass};
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    s.world.Tick(kDt);
    EXPECT_EQ(*s.field("v"), 5.0f);
    EXPECT_EQ(*s.field("caught"), 1.0f);
    EXPECT_EQ(*s.field("after"), 0.0f);
}

// "x as $C" and a cast "($C)x" to a derived script class never crash. A wrong class and a null
// source give a null handle. The use of that handle throws hot::NullReference.
TEST_P(BackendTests, BadAsAndDerivedCastGiveNullAndTheUseThrows)
{
    const hot::ScriptSource src = {"chk_bad_as.hot", R"(
using "Actor.h";

class Base { int n = 3; }
class Derived : Base { int m = 4; }

class Player : Actor {
    std.vector<$Derived> xs;
    float caught = 0.0;
    float caught2 = 0.0;
    float ticks = 0.0;
    float after = 0.0;
    float v = 0.0;

    override void Tick(float dt) {
        $Base b = new $Base();
        $Base none = null;
        this.xs.push_back(b as $Derived);
        this.xs.push_back(($Derived)b);
        this.xs.push_back(none as $Derived);
        this.xs.push_back(new $Derived());
        $Derived first = this.xs[0];
        if (first == null) { this.caught = 1.0; }
        try { this.v = this.xs[0].m; this.after = 1.0; } catch (NullReference e) { this.caught2 = 1.0; }
        try { this.v = this.xs[1].m; this.after = 1.0; } catch (NullReference e) { this.ticks = 1.0; }
        try { this.v = this.xs[2].m; this.after = 1.0; } catch (NullReference e) { this.ticks = this.ticks + 1.0; }
        this.v = this.xs[3].m;
    }

    override void TakeDamage(float amount) {}
    override void TakeDamage(ref Actor source, float amount) {}
}
)"};
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    s.world.Tick(kDt);
    EXPECT_EQ(*s.field("caught"), 1.0f);
    EXPECT_EQ(*s.field("caught2"), 1.0f);
    EXPECT_EQ(*s.field("ticks"), 2.0f);
    EXPECT_EQ(*s.field("after"), 0.0f);
    EXPECT_EQ(*s.field("v"), 4.0f);
}

// An index past the end throws hot::IndexOutOfRange. A catch of the base ScriptError also
// catches a null reference.
TEST_P(BackendTests, IndexOutOfRangeCaughtInScript)
{
    const hot::ScriptSource src = checks_source("chk_index.hot", R"(
        int i = 4;
        try {
            this.vals[i] = 1;
            this.after = 1.0;
        } catch (IndexOutOfRange e) {
            this.caught = 1.0;
        }
        try {
            this.v = this.weapon.dmg;
        } catch (ScriptError e) {
            this.caught2 = 1.0;
        }
)");
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    s.world.Tick(kDt);
    EXPECT_EQ(*s.field("caught"), 1.0f);
    EXPECT_EQ(*s.field("caught2"), 1.0f);
    EXPECT_EQ(*s.field("after"), 0.0f);
}

// The script type "T[]" is a hot::span<T> (std::span<T> with HOT_VIEW_TYPE=std). A vector and an array convert to it for a call. A
// write through the view changes the owner. An index past the end throws and does not crash.
TEST_P(BackendTests, ViewParameterConvertsAndChecksIndex)
{
    const hot::ScriptSource src = {"view_param.hot", R"(
using "Actor.h";
using <vector>;

class Player : Actor {
    std.vector<int> list = [1, 2, 3];
    int[3] fixed;
    float sum = 0.0;
    float caught = 0.0;
    float after = 0.0;

    int Pick(int[] s, int i) { return s[i]; }

    int Sum(const int[] s) {
        int total = 0;
        foreach (var x in s) { total += x; }
        return total;
    }

    void Bump(int[] s) {
        foreach (ref x in s) { x += 1; }
    }

    override void Tick(float dt) {
        this.fixed[2] = 4;
        Bump(this.list);
        Bump(this.fixed);
        this.sum = Sum(this.list) + Pick(this.fixed, 2);
        try {
            this.after = Pick(this.list, 3);
        } catch (IndexOutOfRange e) {
            this.caught = 1.0;
        }
    }

    override void TakeDamage(float amount) {}
    override void TakeDamage(ref Actor source, float amount) {}
}
)"};
    hot::DefaultBackendOptions options; // the default view type hot::span works with the host standard
    std::unique_ptr<hot::Backend> view_backend = hot::make_default_backend(options);
    hot::LoadResult r = view_backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    s.world.Tick(kDt);
    EXPECT_EQ(*s.field("sum"), 14.0f); // (2 + 3 + 4) + 5
    EXPECT_EQ(*s.field("caught"), 1.0f);
    EXPECT_EQ(*s.field("after"), 0.0f);
}

// The prefix "*" of a null handle throws hot::NullReference.
TEST_P(BackendTests, StarNullCaught)
{
    const hot::ScriptSource src = checks_source("chk_star.hot", R"(
        try {
            var w = *this.weapon;
            this.after = 1.0;
        } catch (NullReference e) {
            this.caught = 1.0;
        }
)");
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    s.world.Tick(kDt);
    EXPECT_EQ(*s.field("caught"), 1.0f);
}

// An exception that the script does not catch goes to the handler at the end of the script
// stack. The object stays alive, and the next Tick runs.
TEST_P(BackendTests, UnhandledReportsToHandler)
{
    const hot::ScriptSource src = checks_source("chk_report.hot", R"(
        this.ticks = this.ticks + 1.0;
        int i = 4;
        this.vals[i] = 1;
        this.after = 1.0;
)");
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    HandlerGuard guard;
    RecordedReport rec;
    hot::set_script_error_handler([&rec](const hot::ScriptErrorReport& e)
    {
        rec.type = e.type;
        rec.message = e.message;
        rec.class_name = e.class_name;
        ++rec.count;
    });
    s.world.Tick(kDt);
    EXPECT_EQ(rec.count, 1);
    EXPECT_EQ(rec.type, "hot::IndexOutOfRange");
    EXPECT_NE(rec.message.find("4"), std::string::npos) << rec.message;
    EXPECT_EQ(rec.class_name, "Player");
    EXPECT_EQ(*s.field("ticks"), 1.0f);
    s.world.Tick(kDt);
    EXPECT_EQ(rec.count, 2);
    EXPECT_EQ(*s.field("ticks"), 2.0f);
    EXPECT_EQ(*s.field("after"), 0.0f);
}

// Without a handler, the report is one line on stderr.
TEST_P(BackendTests, UnhandledDefaultPrintsToStderr)
{
    const hot::ScriptSource src = checks_source("chk_stderr.hot", R"(
        int i = 4;
        this.vals[i] = 1;
)");
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    HandlerGuard guard;
    hot::set_script_error_handler({});
    testing::internal::CaptureStderr();
    s.world.Tick(kDt);
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("[script] ERROR: unhandled hot::IndexOutOfRange in Player: "), std::string::npos) << err;
}

// A null handle in a script function that another script function calls. The report has the
// traceback with HOT_TRACEBACK=1, and an empty traceback without it.
TEST_P(BackendTests, UnhandledReportHasTheScriptTraceback)
{
    const std::string text = std::string(R"(
using "Actor.h";
using "Weapon.h";

[native, handle("std::shared_ptr<$T>")] class Weapon;

class Player : Actor {
    Weapon weapon = null;
    float v = 0.0;

    void Inner() {
        this.v = this.weapon.dmg;
    }

    void Outer() {
        this.v = 1.0;
        this.Inner();
    }

    override void Tick(float dt) {
        this.Outer();
    }

    override void TakeDamage(float amount) {}
    override void TakeDamage(ref Actor source, float amount) {}
}
)");
    const auto line_of = [&text](const std::string& needle)
    {
        const size_t at = text.find(needle);
        return 1 + std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(at), '\n');
    };
    hot::LoadResult r = backend->load({"chk_trace.hot", text}, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    HandlerGuard guard;
    std::vector<std::string> tracebacks;
    hot::set_script_error_handler([&tracebacks](const hot::ScriptErrorReport& e) { tracebacks.push_back(e.traceback); });
    s.world.Tick(kDt);
    ASSERT_EQ(tracebacks.size(), 1u);
#if HOT_TRACEBACK
    const std::string& t = tracebacks[0];
    const std::string inner = "  at Player.Inner (chk_trace.hot:" + std::to_string(line_of("this.weapon.dmg")) + ")\n";
    const std::string outer = "  at Player.Outer (chk_trace.hot:" + std::to_string(line_of("this.Inner();")) + ")\n";
    const std::string tick = "  at Player.Tick (chk_trace.hot:" + std::to_string(line_of("this.Outer();")) + ")\n";
    EXPECT_EQ(t, inner + outer + tick);
    // The default handler prints the lines under the error line.
    hot::set_script_error_handler({});
    testing::internal::CaptureStderr();
    s.world.Tick(kDt);
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("[script] ERROR: unhandled hot::NullReference in Player: "), std::string::npos) << err;
    EXPECT_NE(err.find(inner + outer + tick), std::string::npos) << err;
#else
    (void)line_of;
    EXPECT_TRUE(tracebacks[0].empty()) << tracebacks[0];
#endif
}

// A script override of a native virtual function that throws a ScriptError: the host call
// reports the exception and gets a value-initialized result.
TEST_P(BackendTests, ThrowInVirtualOverrideReportsAndReturnsDefault)
{
    const hot::ScriptSource src = {"chk_virtual.hot", R"(
using "Actor.h";
using "Damageable.h";

class Boss : Actor, IDamageable {
    int[2] a = [10, 20];

    override int Absorb(int amount) {
        return this.a[amount];
    }
}
)"};
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module), "Boss"));
    s.commit_at_frame_boundary();
    auto* dmg = dynamic_cast<IDamageable*>(s.actor);
    ASSERT_NE(dmg, nullptr);
    HandlerGuard guard;
    std::vector<std::string> types;
    hot::set_script_error_handler([&types](const hot::ScriptErrorReport& e) { types.push_back(e.type); });
    EXPECT_EQ(dmg->Absorb(1), 20);
    EXPECT_TRUE(types.empty());
    EXPECT_EQ(dmg->Absorb(5), 0);
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0], "hot::IndexOutOfRange");
}

// The module makes and throws a ScriptError, and the catch of the host (hot::call) gets it. On
// Windows the module is a DLL with its own static libstdc++. The type of the exception must match
// in the host, and the message (a std::string) must cross from the module to the host.
TEST_P(BackendTests, ScriptErrorFromModuleIsCaughtInHost)
{
    const hot::ScriptSource src = {"chk_module_throw.hot", R"(
using "Actor.h";

class Player : Actor {
    float ticks = 0.0;

    override void Tick(float dt) {
        this.ticks = this.ticks + 1.0;
        throw hot.ScriptError("module boom");
    }

    override void TakeDamage(float amount) {}
    override void TakeDamage(ref Actor source, float amount) {}
}
)"};
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    HandlerGuard guard;
    RecordedReport rec;
    hot::set_script_error_handler([&rec](const hot::ScriptErrorReport& e)
    {
        rec.type = e.type;
        rec.message = e.message;
        rec.class_name = e.class_name;
        ++rec.count;
    });
    s.world.Tick(kDt);
    EXPECT_EQ(rec.count, 1);
    EXPECT_EQ(rec.type, "hot::ScriptError");
    EXPECT_EQ(rec.message, "module boom");
    EXPECT_EQ(rec.class_name, "Player");
    EXPECT_EQ(*s.field("ticks"), 1.0f);
}

// A struct with the base ScriptError is an exception type. A child struct is caught by the parent type.
TEST_P(BackendTests, ExceptionStructCaughtByParentType)
{
    const hot::ScriptSource src = {"chk_exc_struct.hot", R"(
using "Actor.h";

struct GameError : ScriptError {
    int code = 0;
    GameError(int c) : base("game error") { code = c; }
}

struct NetError : GameError {
    NetError(int c) : base(c) { }
}

class Player : Actor {
    int code = 0;
    std.string msg = "";

    override void Tick(float dt) {
        try {
            throw NetError(7);
        } catch (GameError e) {
            this.code = e.code;
            this.msg = e.what();
        }
    }

    override void TakeDamage(float amount) {}
    override void TakeDamage(ref Actor source, float amount) {}
}
)"};
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    s.world.Tick(kDt);
    EXPECT_EQ(*hot::field_ptr<int>(s.inst.shell, "code"), 7);
    EXPECT_EQ(*hot::field_ptr<std::string>(s.inst.shell, "msg"), "game error");
}

// An exception struct that nothing catches reaches the handler of the host as a hot::ScriptError.
TEST_P(BackendTests, ExceptionStructEscapesToTheHost)
{
    const hot::ScriptSource src = {"chk_exc_escape.hot", R"(
using "Actor.h";

struct GameError : ScriptError {
    int code = 0;
    GameError(int c) : base("game error") { code = c; }
}

struct NetError : GameError {
    NetError(int c) : base(c) { }
}

class Player : Actor {
    override void Tick(float dt) {
        throw GameError(3);
    }

    override void TakeDamage(float amount) {}
    override void TakeDamage(ref Actor source, float amount) {}
}
)"};
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    HandlerGuard guard;
    RecordedReport rec;
    hot::set_script_error_handler([&rec](const hot::ScriptErrorReport& e)
    {
        rec.type = e.type;
        rec.message = e.message;
        rec.class_name = e.class_name;
        ++rec.count;
    });
    s.world.Tick(kDt);
    EXPECT_EQ(rec.count, 1);
    EXPECT_EQ(rec.type, "hot::ScriptError");
    EXPECT_EQ(rec.message, "game error");
    EXPECT_EQ(rec.class_name, "Player");
}

// A reload that adds a field to an exception struct: the new code throws and catches its own type.
TEST_P(BackendTests, ExceptionStructSurvivesAReload)
{
    const hot::ScriptSource src1 = {"chk_exc_reload.hot", R"(
using "Actor.h";

struct GameError : ScriptError {
    int code = 0;
    GameError(int c) : base("game error") { code = c; }
}

struct NetError : GameError {
    NetError(int c) : base(c) { }
}

class Player : Actor {
    int result = 0;

    override void Tick(float dt) {
        try {
            throw NetError(4);
        } catch (GameError e) {
            this.result = e.code;
        }
    }

    override void TakeDamage(float amount) {}
    override void TakeDamage(ref Actor source, float amount) {}
}
)"};
    const hot::ScriptSource src2 = {"chk_exc_reload.hot", R"(
using "Actor.h";

struct GameError : ScriptError {
    int code = 0;
    int extra = 5;
    GameError(int c) : base("game error") { code = c; extra = 10; }
}

struct NetError : GameError {
    NetError(int c) : base(c) { }
}

class Player : Actor {
    int result = 0;

    override void Tick(float dt) {
        try {
            throw NetError(4);
        } catch (GameError e) {
            this.result = e.code + e.extra;
        }
    }

    override void TakeDamage(float amount) {}
    override void TakeDamage(ref Actor source, float amount) {}
}
)"};
    hot::LoadResult v1 = backend->load(src1, 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module)));
    s.world.Tick(kDt);
    EXPECT_EQ(*hot::field_ptr<int>(s.inst.shell, "result"), 4);
    hot::LoadResult v2 = backend->load(src2, 2);
    ASSERT_TRUE(v2) << v2.error;
    EXPECT_TRUE(s.loader.request_reload(std::move(*v2.module)));
    EXPECT_TRUE(s.loader.commit_pending());
    s.world.Tick(kDt);
    EXPECT_EQ(*hot::field_ptr<int>(s.inst.shell, "result"), 14);
}

// Script code calls native code, and the native code calls a script override that throws. The
// error is not at the end of the script stack, so it passes to the catch of the calling script.
// Only a call from the host reports.
TEST_P(BackendTests, ErrorInNestedOverrideReachesTheCallingScript)
{
    const hot::ScriptSource src = {"chk_nested.hot", R"(
using "Actor.h";
using "Damageable.h";

class Boss : Actor, IDamageable {
    int[2] a = [10, 20];
    float caught = 0.0;
    float after = 0.0;

    override void Tick(float dt) {
        try {
            var x = HitThrough(*this, 5);
            this.after = 1.0;
        } catch (NullReference e) {
            this.caught = 2.0;
        } catch (IndexOutOfRange e) {
            this.caught = 1.0;
        }
    }

    override int Absorb(int amount) {
        return this.a[amount];
    }
}
)"};
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module), "Boss"));
    s.commit_at_frame_boundary();
    HandlerGuard guard;
    int reports = 0;
    hot::set_script_error_handler([&reports](const hot::ScriptErrorReport&) { ++reports; });
    s.world.Tick(kDt);
    EXPECT_EQ(*s.field("caught"), 1.0f);
    EXPECT_EQ(*s.field("after"), 0.0f);
    EXPECT_EQ(reports, 0);
    auto* dmg = dynamic_cast<IDamageable*>(s.actor);
    ASSERT_NE(dmg, nullptr);
    EXPECT_EQ(dmg->Absorb(5), 0);
    EXPECT_EQ(reports, 1);
}

// The script overrides a noexcept native virtual function and throws. An exception must not leave
// a noexcept function, so the override reports at once, also inside another script call. The
// calling script goes on, and the call gives a default value.
TEST_P(BackendTests, ErrorInNoexceptOverrideReportsAndDoesNotTerminate)
{
    const hot::ScriptSource src = {"chk_noexcept.hot", R"(
using "Actor.h";
using "Guarded.h";

class Boss : Actor, IGuarded {
    int[2] a = [10, 20];
    float result = -1.0;
    float after = 0.0;

    override void Tick(float dt) {
        try {
            this.result = CallGuard(*this, 5);
        } catch (ScriptError e) {
            this.result = -2.0;
        }
        this.after = 1.0;
    }

    override int Guard(int x) {
        return this.a[x];
    }
}
)"};
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module), "Boss"));
    s.commit_at_frame_boundary();
    HandlerGuard guard;
    std::vector<std::string> types;
    hot::set_script_error_handler([&types](const hot::ScriptErrorReport& e) { types.push_back(e.type); });
    s.world.Tick(kDt);
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0], "hot::IndexOutOfRange");
    EXPECT_EQ(*s.field("result"), 0.0f);
    EXPECT_EQ(*s.field("after"), 1.0f);
}

#endif // HOT_RUNTIME_CHECKS

} // namespace
