// Step F1: tests for the translator spc (design section 8.2, row F1).
//
// F1Reload repeats the R1 checks with libraries that spc made from
// scripts/player_v1.script and scripts/player_v2.script.
// F1Diag compiles scripts/bad.script. The Clang error must name the script.
//
// CMake defines these macros: F1_V1_SO, F1_V2_SO (the two libraries),
// SPP_SOURCE_DIR, SPP_BINARY_DIR, SPC_PATH, SPP_CXX and SPP_CXXFLAGS.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Actor.h"
#include "script_runtime.h"

namespace {

TEST(F1Reload, GeneratedLibrariesKeepAddressAndMigrateState)
{
    script::Loader loader;  // declared first: it is destroyed last
    World world;            // the world deletes the actors while the libraries are loaded
    const float dt = 1.0f / 60.0f;

    std::string error;
    ASSERT_TRUE(loader.load(F1_V1_SO, error)) << error;
    const script::ClassEntry* entry = loader.find("Player");
    ASSERT_NE(entry, nullptr);

    script::Instance inst = entry->create();
    Actor* a = static_cast<Actor*>(inst.native);
    world.Add(a);
    Actor* source = new Actor("Source");
    world.Add(source);
    EXPECT_EQ(a->Name, "Player");

    bool commit_failed = false;
    world.SetFrameBoundaryHook([&] {
        std::string err;
        if (loader.reload_pending() && !loader.commit_pending(err)) {
            ADD_FAILURE() << "commit failed: " << err;
            commit_failed = true;
        }
    });
    auto field = [&](const char* name) { return script::field_ptr<float>(inst.shell, name); };

    // Generation 1: change the state, then bind a delegate through the thunk.
    a->TakeDamage(30.0f);                        // hp 70
    for (int i = 0; i < 3; ++i) world.Tick(dt);  // each Tick heals 1: hp 73
    ASSERT_NE(field("hp"), nullptr);
    EXPECT_EQ(*field("hp"), 73.0f);

    a->TakeDamage(*source, 5.0f);  // hp 68, and target is set
    EXPECT_EQ(*field("hp"), 68.0f);
    Actor** target = script::field_ptr<Actor*>(inst.shell, "target");
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(*target, source);

    using HealFn = void (*)(Actor*, float);
    void* thunk = loader.thunk("Player::heal");
    ASSERT_NE(thunk, nullptr);
    Delegate<void(float)> heal;
    heal.Bind(a, reinterpret_cast<HealFn>(thunk));
    heal(5.0f);  // generation 1 body: hp 73
    EXPECT_EQ(*field("hp"), 73.0f);

    // Validation phase: no side effect.
    Actor* const addr = a;
    script::ShellBase* const shell = inst.shell;
    void* const old_state = shell->state;
    ASSERT_TRUE(loader.request_reload(F1_V2_SO, error)) << error;
    EXPECT_TRUE(loader.reload_pending());
    EXPECT_EQ(script::active_generation, 1u);
    EXPECT_EQ(shell->state, old_state);
    EXPECT_EQ(field("health"), nullptr);

    // The commit runs at the frame boundary, after the actors ticked.
    world.Tick(dt);  // generation 1 Tick heals 1: hp 74; then the commit
    ASSERT_FALSE(commit_failed);
    EXPECT_FALSE(loader.reload_pending());
    EXPECT_EQ(script::active_generation, 2u);
    ASSERT_EQ(world.Actors().size(), 2u);
    EXPECT_EQ(world.Actors()[0], addr);  // the same address
    EXPECT_NE(shell->state, old_state);
    EXPECT_EQ(shell->cls->generation, 2u);
    EXPECT_EQ(field("hp"), nullptr);
    ASSERT_NE(field("health"), nullptr);
    EXPECT_EQ(*field("health"), 74.0f);  // health carries hp
    ASSERT_NE(field("shield"), nullptr);
    EXPECT_EQ(*field("shield"), 25.0f);
    EXPECT_EQ(*field("speed"), 5.0f);
    Actor** target2 = script::field_ptr<Actor*>(inst.shell, "target");
    ASSERT_NE(target2, nullptr);
    EXPECT_EQ(*target2, source);

    // Generation 2: new Tick body, no heal in Tick.
    const float x0 = a->Position.x;
    world.Tick(dt);
    EXPECT_NEAR(a->Position.x - x0, 2.0f * 5.0f * dt, 1e-5f);
    EXPECT_EQ(*field("health"), 74.0f);

    // The delegate that was bound before the reload calls the generation 2 heal (x2).
    EXPECT_EQ(loader.thunk("Player::heal"), thunk);
    heal(5.0f);
    EXPECT_EQ(*field("health"), 84.0f);

    // Both TakeDamage overloads work in generation 2.
    a->TakeDamage(4.0f);
    EXPECT_EQ(*field("health"), 80.0f);
    a->TakeDamage(*source, 10.0f);
    EXPECT_EQ(*field("health"), 70.0f);
}

std::string read_file(const std::string& path)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A type error in a script must give a Clang error at the script line.
TEST(F1Diag, ClangErrorNamesTheScriptAndTheLine)
{
    const std::string script = std::string(SPP_SOURCE_DIR) + "/scripts/bad.script";
    const std::string out_dir = std::string(SPP_BINARY_DIR) + "/f1_diag";
    const std::string log = out_dir + "/diag.log";
    ASSERT_EQ(std::system(("mkdir -p '" + out_dir + "'").c_str()), 0);

    // Find the line of the error in the script.
    int bad_line = 0, n = 0;
    std::istringstream src(read_file(script));
    for (std::string l; std::getline(src, l);)
        if (++n, l.find("\"fast\"") != std::string::npos) bad_line = n;
    ASSERT_GT(bad_line, 0);

    std::string cmd = std::string("SPC='") + SPC_PATH + "' CXX='" + SPP_CXX + "' CXXFLAGS='" +
                      SPP_CXXFLAGS + "' '" + SPP_SOURCE_DIR + "/tools/build_gen.sh' '" + script +
                      "' 1 '" + out_dir + "/bad.so' > '" + log + "' 2>&1";
    EXPECT_NE(std::system(cmd.c_str()), 0) << "the build of bad.script must fail";

    const std::string text = read_file(log);
    const std::string expect = "bad.script:" + std::to_string(bad_line) + ":";
    EXPECT_NE(text.find(expect), std::string::npos) << "no '" << expect << "' in:\n" << text;

    // Each line with "error:" must name the script, not the generated file.
    std::istringstream ls(text);
    int errors = 0;
    for (std::string l; std::getline(ls, l);) {
        if (l.find(" error:") == std::string::npos) continue;
        ++errors;
        EXPECT_NE(l.find("bad.script:"), std::string::npos) << "error not at the script: " << l;
        EXPECT_EQ(l.find(".gen.cpp"), std::string::npos) << l;
    }
    EXPECT_GT(errors, 0) << text;
}

}  // namespace
