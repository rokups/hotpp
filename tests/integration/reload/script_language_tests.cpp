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
    override void TakeDamage(Actor& source, float amount) {}
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

} // namespace
