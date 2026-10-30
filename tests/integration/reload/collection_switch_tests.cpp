// Tests of the string switch and the collection literal with the backend that SPP_BACKEND selected.
#include "hot/backend.h"
#include "support/backend_fixture.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace
{

using test_support::kDt;
using test_support::Session;

// A switch on a string runs the matching case. Several labels share a body. A nested break,
// a return and the default case work. A continue in a case goes to the outer loop.
TEST_P(BackendTests, StringSwitchRuns)
{
    const hot::ScriptSource src = {"strsw.hot", R"(
using "Actor.h";
using <string>;
using <vector>;

class Player : Actor {
    float total = 0.0;

    int pick(std.string s) {
        switch (s) {
            case "a":
                return 1;
            case "b":
            case "c":
                return 2;
            default:
                return 9;
        }
    }

    int nested(std.string s, int n) {
        int r = 0;
        switch (s) {
            case "x":
                if (n > 1) {
                    r = 10;
                    break;
                }
                r = 20;
                break;
            case "y":
                r = 30;
                break;
        }
        return r + 1;
    }

    int skip(std.vector<std.string> v) {
        int r = 0;
        foreach (var s in v) {
            switch (s) {
                case "k":
                    continue;
                case "m":
                    r += 100;
                    break;
                default:
                    r += 1;
                    break;
            }
            r += 1000;
        }
        return r;
    }

    override void Tick(float dt) {
        this.total = this.pick("a") * 1 + this.pick("c") * 10 + this.pick("zz") * 100;
        this.total += this.nested("x", 5) * 1000 + this.nested("x", 0) * 100000;
        this.total += this.nested("q", 0) + this.nested("y", 0);
        std.vector<std.string> v = ["k", "m", "n"];
        this.total += this.skip(v);
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
    // pick: 1 + 20 + 900 = 921. nested: 11 * 1000 + 21 * 100000 = 2111000.
    // nested("q") = 1, nested("y") = 31. skip: (100 + 1000) + (1 + 1000) = 2101.
    EXPECT_EQ(*s.field("total"), 2114054.0f);
}

// A collection literal builds a vector and a map from a declared type. A field initializer works.
TEST_P(BackendTests, CollectionLiteralRuns)
{
    const hot::ScriptSource src = {"coll.hot", R"(
using "Actor.h";
using <map>;
using <string>;
using <vector>;

class Player : Actor {
    std.vector<int> base = [4, 5, 6];
    float total = 0.0;

    override void Tick(float dt) {
        std.vector<int> xs = [1, 2, 3];
        std.map<std.string, int> m = ["a": 10, "b": xs.size() > 2 ? 20 : 30];
        std.vector<int> none = [];
        std.vector<std.vector<int>> nest = [[1], [2, 3]];
        this.total = xs[0] + xs[2] + m["a"] + m["b"] + none.size() + this.base[1] + nest[1][1];
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
    // 1 + 3 + 10 + 20 + 0 + 5 + 3
    EXPECT_EQ(*s.field("total"), 42.0f);
}

} // namespace
