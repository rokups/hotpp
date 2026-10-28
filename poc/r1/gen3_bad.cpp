// Step R1: generation 3 does not compile on purpose.
// It is NOT part of the default build. Run poc/r1/make_bad.sh to see the failure.
#include "Actor.h"
#include "script_runtime.h"

struct PlayerState_g3 {
    float health;
};

static void Player__Tick(Actor* a, float dt)
{
    auto* st = static_cast<PlayerState_g3*>(static_cast<script::ShellBase*>(a)->state);
    st->health = "not a number";       // error: a string is not a float
    Player__undefined_function(a, dt);  // error: the function does not exist
}

extern "C" void script_register(script::Registry&)
{
    Player__Tick(nullptr, 0.0f);
}
