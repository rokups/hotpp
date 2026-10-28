// minengine: the harness of the hot-reload prototype (design section 8.1).
// Mode "--r0 <gen1.so>" is the test driver of step R0.
// Mode "--r1 <gen1> <gen2> <gen4> <gen3_missing>" is the test driver of step R1.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <typeinfo>

#include "Actor.h"
#include "script_runtime.h"

static int fail(const char* what)
{
    std::fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

static int run_r0(const char* path)
{
    script::Loader loader;  // declared first: it is destroyed last
    World world;            // the world deletes the actors, the library must still be loaded

    std::string error;
    if (!loader.load(path, error)) return fail(error.c_str());
    const script::ClassEntry* entry = loader.find("Player");
    if (!entry) return fail("class Player is not registered");

    script::Instance inst = entry->create();
    Actor* a = static_cast<Actor*>(inst.native);
    world.Add(a);
    Actor* source = new Actor("Source");
    world.Add(source);

    for (int i = 0; i < 3; ++i) world.Tick(1.0f / 60.0f);

    a->TakeDamage(10.0f);
    a->TakeDamage(*source, 5.0f);

    const char* type_name = typeid(*a).name();
    std::printf("typeid(*a).name() = %s\n", type_name);
    if (!std::strstr(type_name, "Player_Shell")) return fail("type name has no Player_Shell");
    if (a->Name != "Player") return fail("name was not forwarded to the Actor constructor");
    if (a->Position.x <= 0.0f) return fail("Tick did not move the actor");

    float* hp = script::field_ptr<float>(inst.shell, "hp");
    if (!hp) return fail("field hp not found by reflection");
    std::printf("reflection: hp=%.1f\n", *hp);
    if (*hp != 85.0f) return fail("hp is not 85");

    std::printf("R0 OK\n");
    return 0;
}

#define CHECK(cond)                                  \
    do {                                             \
        if (!(cond)) return fail("check failed: " #cond); \
    } while (0)

// Step R1: reload with migration, a thunk and two rejected reloads.
static int run_r1(const char* gen1, const char* gen2, const char* gen4, const char* gen3_missing)
{
    script::Loader loader;  // destroyed last: the libraries stay loaded
    World world;
    const float dt = 1.0f / 60.0f;

    std::string error;
    if (!loader.load(gen1, error)) return fail(error.c_str());
    const script::ClassEntry* entry = loader.find("Player");
    CHECK(entry);

    script::Instance inst = entry->create();
    Actor* a = static_cast<Actor*>(inst.native);
    world.Add(a);

    bool commit_failed = false;
    world.SetFrameBoundaryHook([&] {
        std::string err;
        if (loader.reload_pending() && !loader.commit_pending(err)) {
            std::fprintf(stderr, "commit failed: %s\n", err.c_str());
            commit_failed = true;
        }
    });

    auto field = [&](const char* name) { return script::field_ptr<float>(inst.shell, name); };

    // Generation 1: change the state, then bind a delegate through the thunk.
    a->TakeDamage(30.0f);  // hp 70
    for (int i = 0; i < 3; ++i) world.Tick(dt);  // each Tick heals 1: hp 73
    CHECK(field("hp") && *field("hp") == 73.0f);

    using HealFn = void (*)(Actor*, float);
    void* thunk = loader.thunk("Player::heal");
    CHECK(thunk);
    Delegate<void(float)> heal;
    heal.Bind(a, reinterpret_cast<HealFn>(thunk));
    heal(5.0f);  // generation 1 body: hp 78
    CHECK(*field("hp") == 78.0f);

    // Validation phase: no side effect.
    Actor* const addr = a;
    ::script::ShellBase* const shell = inst.shell;
    void* const old_state = shell->state;
    CHECK(loader.request_reload(gen2, error));
    CHECK(loader.reload_pending());
    CHECK(script::active_generation == 1);
    CHECK(shell->state == old_state);
    CHECK(*field("hp") == 78.0f && !field("health") && !field("shield"));

    // The commit runs at the frame boundary, after the actors ticked.
    world.Tick(dt);  // generation 1 Tick heals 1: hp 79; then the commit
    CHECK(!commit_failed);
    CHECK(!loader.reload_pending());
    CHECK(script::active_generation == 2);
    CHECK(world.Actors().size() == 1 && world.Actors()[0] == addr);
    CHECK(shell->state != old_state);
    CHECK(shell->cls->generation == 2);
    CHECK(!field("hp"));
    CHECK(field("health") && *field("health") == 79.0f);
    CHECK(field("shield") && *field("shield") == 25.0f);
    CHECK(field("speed") && *field("speed") == 5.0f);

    // Generation 2: new Tick body, no heal in Tick.
    float x0 = a->Position.x;
    world.Tick(dt);
    CHECK(std::fabs((a->Position.x - x0) - 2.0f * 5.0f * dt) < 1e-5f);
    CHECK(*field("health") == 79.0f);

    // The delegate bound before the reload calls the generation 2 heal (x2).
    CHECK(loader.thunk("Player::heal") == thunk);
    heal(5.0f);
    CHECK(*field("health") == 89.0f);

    // Rejected reloads: other shell layout, and a library that is not there.
    void* const state2 = shell->state;
    const float pos2 = a->Position.x;
    CHECK(!loader.request_reload(gen4, error));
    std::printf("gen4 rejected: %s\n", error.c_str());
    CHECK(!error.empty());
    CHECK(!loader.reload_pending());
    CHECK(!loader.request_reload(gen3_missing, error));
    std::printf("gen3 rejected: %s\n", error.c_str());
    CHECK(!error.empty());
    CHECK(!loader.reload_pending());

    CHECK(script::active_generation == 2);
    CHECK(world.Actors()[0] == addr && shell->state == state2 && shell->cls->generation == 2);
    CHECK(*field("health") == 89.0f && *field("shield") == 25.0f && *field("speed") == 5.0f);
    CHECK(a->Position.x == pos2);

    world.Tick(dt);  // still generation 2
    CHECK(!commit_failed);
    CHECK(std::fabs((a->Position.x - pos2) - 2.0f * 5.0f * dt) < 1e-5f);
    CHECK(*field("health") == 89.0f && *field("shield") == 25.0f);
    heal(1.0f);
    CHECK(*field("health") == 91.0f);

    std::printf("R1 OK\n");
    return 0;
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--r0") == 0) return run_r0(argv[2]);
    if (argc == 6 && std::strcmp(argv[1], "--r1") == 0)
        return run_r1(argv[2], argv[3], argv[4], argv[5]);
    std::fprintf(stderr,
                 "usage: minengine --r0 <gen1.so>\n"
                 "       minengine --r1 <gen1.so> <gen2.so> <gen4.so> <gen3_missing.so>\n");
    return 2;
}
