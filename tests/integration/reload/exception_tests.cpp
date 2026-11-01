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
        } catch (const std.exception& e) {
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
    override void TakeDamage(Actor& source, float amount) {}
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
    override void TakeDamage(Actor& source, float amount) {}
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
    override void TakeDamage(Actor& source, float amount) {}
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
    override void TakeDamage(Actor& source, float amount) {}
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
    override void TakeDamage(Actor& source, float amount) {}
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
