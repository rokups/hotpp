// Step R1: hand-written generation 4 of the Player class. The runtime must reject it.
//
// The shell has one extra member. The size of the shell changes, so the
// shell layout hash differs from generation 1. Live objects of generation 1
// cannot survive this change (class RECONSTRUCT, section 5.2).
#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <new>

#include "Actor.h"
#include "spp/script_runtime.h"

constexpr uint32_t GEN = 4;

enum : unsigned {
    SLOT_Tick = 0,
    SLOT_TakeDamage_f = 1,
    SLOT_TakeDamage_Af = 2,
    SLOT_heal = 3,
    SLOT_COUNT = 4,
};

extern script::ClassRecord Player_record;

// ---------------------------------------------------------------- shell
struct Player_Shell final : Actor, script::ShellBase {
    double extra_shell_member = 0;  // changes the shell layout
    template <class... A>
    explicit Player_Shell(script::Construct, A&&... a) : Actor(std::forward<A>(a)...)
    {
        script::init_state(this, &Player_record);
    }
    ~Player_Shell() override { script::destroy_state(this); }

    void Tick(float dt) override
    {
        return script::call<void(Actor*, float)>(cls, SLOT_Tick, this, dt);
    }
    void TakeDamage(float a) override
    {
        return script::call<void(Actor*, float)>(cls, SLOT_TakeDamage_f, this, a);
    }
    void TakeDamage(Actor& src, float a) override
    {
        return script::call<void(Actor*, Actor&, float)>(cls, SLOT_TakeDamage_Af, this, src, a);
    }
};

// ---------------------------------------------------------------- state
struct PlayerState_g4 {
    float health;
    float speed;
    float shield;
};

static void init_health(void*, void* f) { new (f) float(100.0f); }
static void init_speed(void*, void* f) { new (f) float(5.0f); }
static void init_shield(void*, void* f) { new (f) float(25.0f); }
static void move_float(void* dst, const void* src)
{
    new (dst) float(*static_cast<const float*>(src));
}

constexpr uint8_t kFloatFlags = script::kTriviallyCopyable | script::kNothrowMove;

static const script::FieldInfo Player_fields[] = {
    {"health", "hp", typeid(float).name(), offsetof(PlayerState_g4, health), sizeof(float),
     alignof(float), kFloatFlags, init_health, move_float, nullptr},
    {"speed", nullptr, typeid(float).name(), offsetof(PlayerState_g4, speed), sizeof(float),
     alignof(float), kFloatFlags, init_speed, move_float, nullptr},
    {"shield", nullptr, typeid(float).name(), offsetof(PlayerState_g4, shield), sizeof(float),
     alignof(float), kFloatFlags, init_shield, move_float, nullptr},
};

static const script::StateLayout Player_layout = {
    "PlayerState_g4", sizeof(PlayerState_g4), alignof(PlayerState_g4), 3, Player_fields,
};

// ------------------------------------------------- generation functions
static inline PlayerState_g4* state_of(Player_Shell* self)
{
    return static_cast<PlayerState_g4*>(self->state);
}

static void Player__heal(Player_Shell* self, float amount)
{
    if (SCRIPT_UNLIKELY(script::active_generation != GEN)) return script::stale_call("Player::heal");
    auto* st = state_of(self);
    st->health = std::min(st->health + 2.0f * amount, 100.0f);
    std::printf("[gen4] %s: heal %.1f (x2) -> health=%.1f\n", self->Name.c_str(), amount,
                st->health);
}

static void Player__heal_slot(Actor* a, float amount)
{
    Player__heal(static_cast<Player_Shell*>(a), amount);
}

static void Player__Tick(Actor* a, float dt)
{
    auto* self = static_cast<Player_Shell*>(a);
    if (SCRIPT_UNLIKELY(script::active_generation != GEN)) return script::stale_call("Player::Tick");
    auto* st = state_of(self);
    self->Move({2.0f * st->speed * dt, 0, 0});
    std::printf("[gen4] %s: Tick dt=%.4f pos.x=%.4f health=%.1f shield=%.1f\n", self->Name.c_str(),
                dt, self->Position.x, st->health, st->shield);
}

static void Player__TakeDamage_f(Actor* a, float amount)
{
    auto* self = static_cast<Player_Shell*>(a);
    if (SCRIPT_UNLIKELY(script::active_generation != GEN))
        return script::stale_call("Player::TakeDamage(float)");
    auto* st = state_of(self);
    st->health = std::max(st->health - amount, 0.0f);
    std::printf("[gen4] %s: TakeDamage %.1f -> health=%.1f\n", self->Name.c_str(), amount,
                st->health);
}

static void Player__TakeDamage_Af(Actor* a, Actor& src, float amount)
{
    auto* self = static_cast<Player_Shell*>(a);
    if (SCRIPT_UNLIKELY(script::active_generation != GEN))
        return script::stale_call("Player::TakeDamage(Actor&, float)");
    std::printf("[gen4] %s: hit by %s\n", self->Name.c_str(), src.Name.c_str());
    self->TakeDamage(amount);
}

// ------------------------------------------------------ record and entry
static void* Player_slots[SLOT_COUNT] = {
    reinterpret_cast<void*>(&Player__Tick),
    reinterpret_cast<void*>(&Player__TakeDamage_f),
    reinterpret_cast<void*>(&Player__TakeDamage_Af),
    reinterpret_cast<void*>(&Player__heal_slot),
};

script::ClassRecord Player_record = {
    "Player", GEN, Player_slots, SLOT_COUNT, &Player_layout, nullptr,
    "Actor", script::shell_layout_hash("Actor", sizeof(Player_Shell), alignof(Player_Shell), 3),
};

static script::Instance Player_create()
{
    auto* p = new Player_Shell(script::Construct{}, std::string("Player"));
    return {static_cast<Actor*>(p), p};
}

extern "C" void script_register(script::Registry& reg)
{
    reg.add(&Player_record, &Player_create);
}
