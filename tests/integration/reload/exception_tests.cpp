// Tests of the statements throw, try, catch and finally with the backend that SPP_BACKEND selected.
//
// CMake defines the macro SPP_SOURCE_DIR.
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>

#include "spp/backend.h"
#include "support/backend_fixture.h"
#include "support/script_files.h"
#include "support/test_support.h"

namespace {

using test_support::kDt;
using test_support::Session;
using test_support::source;

// The statements throw, try and catch translate to C++ that compiles and runs.
TEST_P(BackendTests, TryCatchThrowRun)
{
    const script::ScriptSource src = {"exc.script", R"(
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
    script::LoadResult r = backend->load(src, 1);
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
    const script::ScriptSource src = {"fin.script", R"(
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
    script::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    s.world.Tick(kDt);
    ASSERT_NE(s.field("total"), nullptr);
    // (1 + 10 + 100 + 5 + 1000 + 2) * 2
    EXPECT_EQ(*s.field("total"), 2236.0f);
}

}  // namespace
