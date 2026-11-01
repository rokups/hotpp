// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Hand-written output of the translator for this script class.
//
//   class Player : Actor {
//       float hp = 100;
//       float speed = 5;
//       override Tick(float dt);
//       override TakeDamage(float amount);
//       override TakeDamage(Actor& source, float amount);
//       heal(float amount);
//   }
//
// The file has three parts: the shell, the state and the generation functions.
#include "Actor.h"
#include "hot/runtime.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <new>

constexpr uint32_t GEN = 1;

// Slot numbers. Native virtuals come first, in the order of the Actor class.
enum : unsigned
{
    SLOT_Tick = 0,
    SLOT_TakeDamage_f = 1,
    SLOT_TakeDamage_Af = 2,
    SLOT_heal = 3, // a bindable method has a slot, so that a thunk reaches the live body
    SLOT_COUNT = 4,
};

extern hot::ClassRecord Player_record;

// ---------------------------------------------------------------- shell
// The shell overrides every virtual of Actor. Each stub reads one slot.
struct Player_Shell final
    : Actor
    , hot::ShellBase
{
    template <class... A>
    explicit Player_Shell(hot::Construct, A&&... a)
        : Actor(std::forward<A>(a)...)
    {
        hot::init_state(this, &Player_record);
    }
    ~Player_Shell() override { hot::destroy_state(this); }

    void Tick(float dt) override { return hot::call<void(Actor*, float)>(this, SLOT_Tick, dt); }
    void TakeDamage(float a) override { return hot::call<void(Actor*, float)>(this, SLOT_TakeDamage_f, a); }
    void TakeDamage(Actor& src, float a) override
    {
        return hot::call<void(Actor*, Actor&, float)>(this, SLOT_TakeDamage_Af, src, a);
    }
};

// ---------------------------------------------------------------- state
struct PlayerState_g1
{
    hot::ShellBase* __hot_owner; // the runtime writes it (has_owner)
    float hp;
    float speed;

    void Tick(float dt);
    void TakeDamage_f(float amount);
    void TakeDamage_Af(Actor& src, float amount);
    void heal(float amount);
};

static void init_hp(void*, void* f)
{
    new (f) float(100.0f);
}
static void init_speed(void*, void* f)
{
    new (f) float(5.0f);
}
static void move_float(void* dst, const void* src)
{
    new (dst) float(*static_cast<const float*>(src));
}

static const hot::FieldInfo Player_fields[] = {
    {"hp", nullptr, typeid(float).name(), offsetof(PlayerState_g1, hp), sizeof(float), alignof(float),
        hot::kTriviallyCopyable | hot::kNothrowMove, init_hp, move_float, nullptr},
    {"speed", nullptr, typeid(float).name(), offsetof(PlayerState_g1, speed), sizeof(float), alignof(float),
        hot::kTriviallyCopyable | hot::kNothrowMove, init_speed, move_float, nullptr},
};

static const hot::StateLayout Player_layout = {
    "PlayerState_g1",
    sizeof(PlayerState_g1),
    alignof(PlayerState_g1),
    2,
    Player_fields,
    true,
};

// ------------------------------------------------- generation functions
// Each function starts with the generation guard (section 4.6).
static inline PlayerState_g1* state_of(Player_Shell* self)
{
    return static_cast<PlayerState_g1*>(self->state);
}

void PlayerState_g1::heal(float amount)
{
    if (HOT_UNLIKELY(hot::active_generation != GEN))
        return hot::stale_call("Player::heal");
    [[maybe_unused]] auto* self = static_cast<Player_Shell*>(__hot_owner);
    [[maybe_unused]] auto* st = this;
    st->hp = std::min(st->hp + amount, 100.0f);
    std::printf("[gen1] %s: heal %.1f -> hp=%.1f\n", self->Name.c_str(), amount, st->hp);
}

// The wrapper for the direct calls of script code. The slot names the member.
static void Player__heal(Player_Shell* self, float amount)
{
    state_of(self)->heal(amount);
}

void PlayerState_g1::Tick(float dt)
{
    if (HOT_UNLIKELY(hot::active_generation != GEN))
        return hot::stale_call("Player::Tick");
    [[maybe_unused]] auto* self = static_cast<Player_Shell*>(__hot_owner);
    [[maybe_unused]] auto* st = this;
    self->Move({st->speed * dt, 0, 0}); // direct call of a native method
    std::printf("[gen1] %s: Tick dt=%.4f pos.x=%.4f hp=%.1f\n", self->Name.c_str(), dt, self->Position.x, st->hp);
    Player__heal(self, 1.0f); // non-virtual script method: direct call
}

void PlayerState_g1::TakeDamage_f(float amount)
{
    if (HOT_UNLIKELY(hot::active_generation != GEN))
        return hot::stale_call("Player::TakeDamage(float)");
    [[maybe_unused]] auto* self = static_cast<Player_Shell*>(__hot_owner);
    [[maybe_unused]] auto* st = this;
    st->hp = std::max(st->hp - amount, 0.0f);
    std::printf("[gen1] %s: TakeDamage %.1f -> hp=%.1f\n", self->Name.c_str(), amount, st->hp);
}

void PlayerState_g1::TakeDamage_Af(Actor& src, float amount)
{
    if (HOT_UNLIKELY(hot::active_generation != GEN))
        return hot::stale_call("Player::TakeDamage(Actor&, float)");
    [[maybe_unused]] auto* self = static_cast<Player_Shell*>(__hot_owner);
    [[maybe_unused]] auto* st = this;
    std::printf("[gen1] %s: hit by %s\n", self->Name.c_str(), src.Name.c_str());
    self->TakeDamage(amount); // script virtual method: goes through the slot
}

// ------------------------------------------------------------- thunk
// The thunk has the same address in each generation (section 4.11). It lives
// in this first module, which stays loaded. It reads the slot at call time.
static void Player__heal__thunk(Actor* a, float amount)
{
    auto* self = static_cast<Player_Shell*>(a);
    return hot::call<void(Actor*, float)>(self, SLOT_heal, amount);
}

// ------------------------------------------------------ record and entry
static const hot::SlotFn Player_slots[SLOT_COUNT] = {
    reinterpret_cast<hot::SlotFn>(&PlayerState_g1::Tick),
    reinterpret_cast<hot::SlotFn>(&PlayerState_g1::TakeDamage_f),
    reinterpret_cast<hot::SlotFn>(&PlayerState_g1::TakeDamage_Af),
    reinterpret_cast<hot::SlotFn>(&PlayerState_g1::heal),
};

hot::ClassRecord Player_record = {
    "Player",
    GEN,
    Player_slots,
    SLOT_COUNT,
    &Player_layout,
    nullptr,
    "Actor",
    hot::shell_layout_hash("Actor", sizeof(Player_Shell), alignof(Player_Shell), 3),
};

static hot::Instance Player_create()
{
    auto* p = new Player_Shell(hot::Construct{}, std::string("Player"));
    return {static_cast<Actor*>(p), p};
}

extern "C" void script_register(hot::Registry& reg)
{
    reg.add(&Player_record, &Player_create);
    reg.add_thunk("Player::heal", reinterpret_cast<void*>(&Player__heal__thunk));
}
