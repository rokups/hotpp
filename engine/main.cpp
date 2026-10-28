// minengine: the harness of the hot-reload prototype (design section 8.1).
// Mode "--r0 <gen1.so>" is the test driver of step R0.
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

int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--r0") == 0) return run_r0(argv[2]);
    std::fprintf(stderr, "usage: minengine --r0 <path-to-gen1.so>\n");
    return 2;
}
