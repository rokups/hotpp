// Hand-written generation 2 of the Player class.
//
// Changes against generation 1:
//   - The Tick body is different: the actor moves twice as fast and no heal runs.
//   - The field hp has the new name health: @was("hp").
//   - The new field shield = 25.
//   - The heal body is different: it heals twice the amount.
//
// The shell is the same as in generation 1. The thunk of heal is in
// generation 1 and is not repeated here.
#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <new>

#include "Actor.h"
#include "spp/runtime.h"

constexpr uint32_t GEN = 2;

enum : unsigned {
    SLOT_Tick = 0,
    SLOT_TakeDamage_f = 1,
    SLOT_TakeDamage_Af = 2,
    SLOT_heal = 3,
    SLOT_COUNT = 4,
};

extern spp::ClassRecord Player_record;

// ---------------------------------------------------------------- shell
struct Player_Shell final : Actor, spp::ShellBase {
    template <class... A>
    explicit Player_Shell(spp::Construct, A&&... a) : Actor(std::forward<A>(a)...)
    {
        spp::init_state(this, &Player_record);
    }
    ~Player_Shell() override { spp::destroy_state(this); }

    void Tick(float dt) override
    {
        return spp::call<void(Actor*, float)>(cls, SLOT_Tick, this, dt);
    }
    void TakeDamage(float a) override
    {
        return spp::call<void(Actor*, float)>(cls, SLOT_TakeDamage_f, this, a);
    }
    void TakeDamage(Actor& src, float a) override
    {
        return spp::call<void(Actor*, Actor&, float)>(cls, SLOT_TakeDamage_Af, this, src, a);
    }
};

// ---------------------------------------------------------------- state
struct PlayerState_g2 {
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

constexpr uint8_t kFloatFlags = spp::kTriviallyCopyable | spp::kNothrowMove;

static const spp::FieldInfo Player_fields[] = {
    {"health", "hp", typeid(float).name(), offsetof(PlayerState_g2, health), sizeof(float),
     alignof(float), kFloatFlags, init_health, move_float, nullptr},
    {"speed", nullptr, typeid(float).name(), offsetof(PlayerState_g2, speed), sizeof(float),
     alignof(float), kFloatFlags, init_speed, move_float, nullptr},
    {"shield", nullptr, typeid(float).name(), offsetof(PlayerState_g2, shield), sizeof(float),
     alignof(float), kFloatFlags, init_shield, move_float, nullptr},
};

static const spp::StateLayout Player_layout = {
    "PlayerState_g2", sizeof(PlayerState_g2), alignof(PlayerState_g2), 3, Player_fields,
};

// ------------------------------------------------- generation functions
static inline PlayerState_g2* state_of(Player_Shell* self)
{
    return static_cast<PlayerState_g2*>(self->state);
}

static void Player__heal(Player_Shell* self, float amount)
{
    if (SPP_UNLIKELY(spp::active_generation != GEN)) return spp::stale_call("Player::heal");
    auto* st = state_of(self);
    st->health = std::min(st->health + 2.0f * amount, 100.0f);
    std::printf("[gen2] %s: heal %.1f (x2) -> health=%.1f\n", self->Name.c_str(), amount,
                st->health);
}

static void Player__heal_slot(Actor* a, float amount)
{
    Player__heal(static_cast<Player_Shell*>(a), amount);
}

static void Player__Tick(Actor* a, float dt)
{
    auto* self = static_cast<Player_Shell*>(a);
    if (SPP_UNLIKELY(spp::active_generation != GEN)) return spp::stale_call("Player::Tick");
    auto* st = state_of(self);
    self->Move({2.0f * st->speed * dt, 0, 0});
    std::printf("[gen2] %s: Tick dt=%.4f pos.x=%.4f health=%.1f shield=%.1f\n", self->Name.c_str(),
                dt, self->Position.x, st->health, st->shield);
}

static void Player__TakeDamage_f(Actor* a, float amount)
{
    auto* self = static_cast<Player_Shell*>(a);
    if (SPP_UNLIKELY(spp::active_generation != GEN))
        return spp::stale_call("Player::TakeDamage(float)");
    auto* st = state_of(self);
    st->health = std::max(st->health - amount, 0.0f);
    std::printf("[gen2] %s: TakeDamage %.1f -> health=%.1f\n", self->Name.c_str(), amount,
                st->health);
}

static void Player__TakeDamage_Af(Actor* a, Actor& src, float amount)
{
    auto* self = static_cast<Player_Shell*>(a);
    if (SPP_UNLIKELY(spp::active_generation != GEN))
        return spp::stale_call("Player::TakeDamage(Actor&, float)");
    std::printf("[gen2] %s: hit by %s\n", self->Name.c_str(), src.Name.c_str());
    self->TakeDamage(amount);
}

// ------------------------------------------------------ record and entry
static void* Player_slots[SLOT_COUNT] = {
    reinterpret_cast<void*>(&Player__Tick),
    reinterpret_cast<void*>(&Player__TakeDamage_f),
    reinterpret_cast<void*>(&Player__TakeDamage_Af),
    reinterpret_cast<void*>(&Player__heal_slot),
};

spp::ClassRecord Player_record = {
    "Player", GEN, Player_slots, SLOT_COUNT, &Player_layout, nullptr,
    "Actor", spp::shell_layout_hash("Actor", sizeof(Player_Shell), alignof(Player_Shell), 3),
};

static spp::Instance Player_create()
{
    auto* p = new Player_Shell(spp::Construct{}, std::string("Player"));
    return {static_cast<Actor*>(p), p};
}

extern "C" void script_register(spp::Registry& reg)
{
    reg.add(&Player_record, &Player_create);
}
