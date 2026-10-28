// Step R0: hand-written output of the translator for this script class.
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
#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <new>

#include "Actor.h"
#include "script_runtime.h"

constexpr uint32_t GEN = 1;

// Slot numbers. Native virtuals come first, in the order of the Actor class.
enum : unsigned {
    SLOT_Tick = 0,
    SLOT_TakeDamage_f = 1,
    SLOT_TakeDamage_Af = 2,
    SLOT_heal = 3,  // a bindable method has a slot, so that a thunk reaches the live body
    SLOT_COUNT = 4,
};

extern script::ClassRecord Player_record;

// ---------------------------------------------------------------- shell
// The shell overrides every virtual of Actor. Each stub reads one slot.
struct Player_Shell final : Actor, script::ShellBase {
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
struct PlayerState_g1 {
    float hp;
    float speed;
};

static void init_hp(void*, void* f) { new (f) float(100.0f); }
static void init_speed(void*, void* f) { new (f) float(5.0f); }
static void move_float(void* dst, const void* src)
{
    new (dst) float(*static_cast<const float*>(src));
}

static const script::FieldInfo Player_fields[] = {
    {"hp", nullptr, typeid(float).name(), offsetof(PlayerState_g1, hp), sizeof(float),
     alignof(float), script::kTriviallyCopyable | script::kNothrowMove, init_hp, move_float,
     nullptr},
    {"speed", nullptr, typeid(float).name(), offsetof(PlayerState_g1, speed), sizeof(float),
     alignof(float), script::kTriviallyCopyable | script::kNothrowMove, init_speed, move_float,
     nullptr},
};

static const script::StateLayout Player_layout = {
    "PlayerState_g1", sizeof(PlayerState_g1), alignof(PlayerState_g1), 2, Player_fields,
};

// ------------------------------------------------- generation functions
// Each function starts with the generation guard (section 4.6).
static inline PlayerState_g1* state_of(Player_Shell* self)
{
    return static_cast<PlayerState_g1*>(self->state);
}

static void Player__heal(Player_Shell* self, float amount)
{
    if (SCRIPT_UNLIKELY(script::active_generation != GEN)) return script::stale_call("Player::heal");
    auto* st = state_of(self);
    st->hp = std::min(st->hp + amount, 100.0f);
    std::printf("[gen1] %s: heal %.1f -> hp=%.1f\n", self->Name.c_str(), amount, st->hp);
}

static void Player__Tick(Actor* a, float dt)
{
    auto* self = static_cast<Player_Shell*>(a);
    if (SCRIPT_UNLIKELY(script::active_generation != GEN)) return script::stale_call("Player::Tick");
    auto* st = state_of(self);
    self->Move({st->speed * dt, 0, 0});  // direct call of a native method
    std::printf("[gen1] %s: Tick dt=%.4f pos.x=%.4f hp=%.1f\n", self->Name.c_str(), dt,
                self->Position.x, st->hp);
    Player__heal(self, 1.0f);  // non-virtual script method: direct call
}

static void Player__TakeDamage_f(Actor* a, float amount)
{
    auto* self = static_cast<Player_Shell*>(a);
    if (SCRIPT_UNLIKELY(script::active_generation != GEN))
        return script::stale_call("Player::TakeDamage(float)");
    auto* st = state_of(self);
    st->hp = std::max(st->hp - amount, 0.0f);
    std::printf("[gen1] %s: TakeDamage %.1f -> hp=%.1f\n", self->Name.c_str(), amount, st->hp);
}

static void Player__TakeDamage_Af(Actor* a, Actor& src, float amount)
{
    auto* self = static_cast<Player_Shell*>(a);
    if (SCRIPT_UNLIKELY(script::active_generation != GEN))
        return script::stale_call("Player::TakeDamage(Actor&, float)");
    std::printf("[gen1] %s: hit by %s\n", self->Name.c_str(), src.Name.c_str());
    self->TakeDamage(amount);  // script virtual method: goes through the slot
}

static void Player__heal_slot(Actor* a, float amount)
{
    Player__heal(static_cast<Player_Shell*>(a), amount);
}

// ------------------------------------------------------------- thunk
// The thunk has the same address in each generation (section 4.11). It lives
// in this first module, which stays loaded. It reads the slot at call time.
static void Player__heal__thunk(Actor* a, float amount)
{
    auto* self = static_cast<Player_Shell*>(a);
    return script::call<void(Actor*, float)>(self->cls, SLOT_heal, a, amount);
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
    reg.add_thunk("Player::heal", reinterpret_cast<void*>(&Player__heal__thunk));
}
