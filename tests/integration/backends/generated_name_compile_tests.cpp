// A script with the user names a, st and self, an expression lambda and C style casts must pass
// the whole chain: sppc, then the compile of the generated C++ by the selected backend.
#include "spp/backend.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

const char* kScript = R"(using "Actor.h";
using <functional>;

class Names : Actor {
    int total = 0;
    float ratio = 1.5;

    int Add(int a, int st, int self) { return a + st + self; }

    int Apply(std.function<int(int)> fn, int a) { return fn(a); }

    void Run(void* raw) {
        int a = 3;
        int st = 4;
        int self = 5;
        this.total = this.Add(a, st, self);
        this.total = this.total + this.Apply((int x) => x * 2, a);
        var alive = () => this.total > 0;
        var big = (float)this.total * this.ratio;
        int back = (int)big;
        var p = (int*)raw;
        this.total = back + (a * st);
    }
}
)";

TEST(GeneratedNameCompile, UserNamesLambdaExpressionAndCastCompile)
{
    auto backend = spp::make_default_backend();
    const spp::ScriptSource source{"names_compile.spp", kScript, {}};
    const spp::LoadResult r = backend->load(source, 1);
    ASSERT_TRUE(r) << r.error;
}

} // namespace
