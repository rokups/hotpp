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

// Restores the default handler at the end of the scope, also when an assertion fails.
struct HandlerGuard
{
    ~HandlerGuard() { hot::set_script_error_handler({}); }
};

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
