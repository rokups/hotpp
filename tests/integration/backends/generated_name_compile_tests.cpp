// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A script with the user names a, st and self, an expression lambda and C style casts must pass
// the whole chain: hotc, then the compile of the generated C++ by the selected backend.
#include "hot/backend.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
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
    auto backend = hot::make_default_backend();
    const hot::ScriptSource source{"names_compile.hot", kScript, {}};
    const hot::LoadResult r = backend->load(source, 1);
    ASSERT_TRUE(r) << r.error;
}

} // namespace

namespace
{
const char* kStructScript = R"(struct Team {
    int slot = 0;
    bool IsT() { return slot % 2 == 0; }
    bool IsCt() { return !IsT(); }
    void Next() { slot += 1; }
}

int Count(in Team t) {
    return t.IsT() || t.IsCt() ? 1 : 0;
}
)";

const char* kStructWriteScript = R"(struct Team {
    int slot = 0;
    void Next() { slot += 1; }
}

int Count(in Team t) {
    t.Next();
    return t.slot;
}
)";

// A read-only struct method is const: a call on an "in" value compiles.
TEST(GeneratedNameCompile, ReadOnlyStructMethodOnInValueCompiles)
{
    auto backend = hot::make_default_backend();
    const hot::ScriptSource source{"struct_const_method.hot", kStructScript, {}};
    const hot::LoadResult r = backend->load(source, 1);
    ASSERT_TRUE(r) << r.error;
}

// A writing struct method is not const: a call on an "in" value is an error.
TEST(GeneratedNameCompile, WritingStructMethodOnInValueFails)
{
    auto backend = hot::make_default_backend();
    const hot::ScriptSource source{"struct_write_method.hot", kStructWriteScript, {}};
    const hot::LoadResult r = backend->load(source, 1);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error.find("'Next' changes the object, but the object is const"), std::string::npos) << r.error;
}
} // namespace

namespace
{
// The script class "GameState" and the state struct of the script class "Game" have different
// C++ names.
const char* kStateNameScript = R"(class GameState {
    int score = 1;
}
class Game {
    GameState state;
    int Get() { return state.score; }
}
)";

// The native header has "WolfState". The state struct of the script class "Wolf" has another name.
const char* kNativeStateHeader = "#pragma once\nstruct WolfState { int mood = 0; };\n"
                                 "struct Creature { virtual ~Creature() = default; virtual void Tick(float) {} };\n";
const char* kNativeStateScript = R"(using "wolf_world.h";
class Wolf : Creature {
    int bites = 0;
    WolfState st;
    override void Tick(float dt) { bites = bites + st.mood + 1; }
}
)";

TEST(GeneratedNameCompile, ScriptClassWithTheStateNameOfAnotherClassCompiles)
{
    auto backend = hot::make_default_backend();
    const hot::LoadResult r = backend->load({"game_state_name.hot", kStateNameScript, {}}, 1);
    ASSERT_TRUE(r) << r.error;
}

TEST(GeneratedNameCompile, NativeTypeWithTheStateNameOfAScriptClassCompiles)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("hot_state_name_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" + std::to_string(reinterpret_cast<uintptr_t>(&dir)));
    fs::create_directories(dir);
    std::ofstream(dir / "wolf_world.h") << kNativeStateHeader;
    hot::DefaultBackendOptions o;
    o.include_dirs = {dir.string()};
    auto backend = hot::make_default_backend(o);
    const hot::LoadResult r = backend->load({(dir / "wolf.hot").string(), kNativeStateScript, {}}, 1);
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_TRUE(r) << r.error;
}
} // namespace
