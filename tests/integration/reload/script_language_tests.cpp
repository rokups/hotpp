// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of script language features with the backend that HOT_BACKEND selected.
// The scripts must compile and run: default values and a for-of loop over a temporary.
//
// CMake defines the macro HOT_SOURCE_DIR.
#include "Weapon.h"
#include "hot/backend.h"
#include "support/backend_fixture.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace
{

using test_support::kDt;
using test_support::Session;
using test_support::source;

// A field or a local without an initializer has the zero value.
TEST_P(BackendTests, DefaultValues)
{
    hot::LoadResult v1 = backend->load(source("default_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Knight"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(*hot::field_ptr<int>(s.inst.shell, "n"), 0);
    EXPECT_EQ(*hot::field_ptr<float>(s.inst.shell, "f"), 0.0f);
    EXPECT_FALSE(*hot::field_ptr<bool>(s.inst.shell, "b"));
    EXPECT_EQ(hot::field_ptr<std::shared_ptr<Weapon>>(s.inst.shell, "weapon")->get(), nullptr);
    EXPECT_EQ(*hot::field_ptr<int>(s.inst.shell, "seen"), 0);
}

// A member of a temporary in a for-of loop: "this.mk().first". The temporary must live
// during the whole loop. The generated code compiles and runs with the selected backend.
TEST_P(BackendTests, ForOfOverAMemberOfATemporaryRuns)
{
    const hot::ScriptSource src = {"temp.hot", R"(
using "Actor.h";
using <utility>;
using <vector>;

class Player : Actor {
    float total = 0.0;

    std.pair<std.vector<int>, int> mk() {
        var p = std.pair<std.vector<int>, int>();
        p.first.push_back(1);
        p.first.push_back(2);
        p.first.push_back(3);
        return p;
    }

    override void Tick(float dt) {
        foreach (var a in this.mk().first) {
            this.total = this.total + a;
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
    EXPECT_EQ(*s.field("total"), 6.0f);
}

// A "ref" return is a place. The caller writes through the result, and the field changes. The test
// covers a virtual method, an element of a field and a static method with "ref" parameters.
TEST_P(BackendTests, RefReturnWritesThroughTheResult)
{
    const hot::ScriptSource src = {"ref_return.hot", R"(
using "Actor.h";
using <vector>;

class Player : Actor {
    int hp = 5;
    int seen = 0;
    std.vector<int> xs;

    virtual ref int Hp() {
        return this.hp;
    }

    ref int Item(int i) {
        return this.xs[i];
    }

    static ref int Pick(ref int a, ref int b) {
        return a > 0 ? a : b;
    }

    override void Tick(float dt) {
        {
            ref h = this.Hp();
            h = h + 2;
        }
        this.xs.push_back(1);
        ref r = this.Item(0);
        r = 9;
        this.Hp() = this.Hp() + 1;
        var a = 0;
        var b = 3;
        ref p = Player.Pick(a, b);
        p = 11;
        this.seen = a + b;
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
    EXPECT_EQ(*hot::field_ptr<int>(s.inst.shell, "hp"), 8);
    EXPECT_EQ(*hot::field_ptr<int>(s.inst.shell, "seen"), 11);
    const auto* xs = hot::field_ptr<std::vector<int>>(s.inst.shell, "xs");
    ASSERT_NE(xs, nullptr);
    ASSERT_EQ(xs->size(), 1u);
    EXPECT_EQ((*xs)[0], 9);
}

} // namespace
