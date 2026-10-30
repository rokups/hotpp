// Hand-written generation 4 of the Player class. The runtime must reject it.
//
// The shell has one extra member. The size of the shell changes, so the
// shell layout hash differs from generation 1. Live objects of generation 1
// cannot survive this change (class RECONSTRUCT, section 5.2).
#include "Actor.h"
#include "spp/runtime.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <new>

constexpr uint32_t GEN = 4;

enum : unsigned
{
    SLOT_Tick = 0,
    SLOT_TakeDamage_f = 1,
    SLOT_TakeDamage_Af = 2,
    SLOT_heal = 3,
    SLOT_COUNT = 4,
};

extern spp::ClassRecord Player_record;

// ---------------------------------------------------------------- shell
struct Player_Shell final
    : Actor
    , spp::ShellBase
{
    double extra_shell_member = 0; // changes the shell layout
    template <class... A>
    explicit Player_Shell(spp::Construct, A&&... a)
        : Actor(std::forward<A>(a)...)
    {
        spp::init_state(this, &Player_record);
    }
    ~Player_Shell() override { spp::destroy_state(this); }

    void Tick(float dt) override { return spp::call<void(Actor*, float)>(this, SLOT_Tick, dt); }
    void TakeDamage(float a) override { return spp::call<void(Actor*, float)>(this, SLOT_TakeDamage_f, a); }
    void TakeDamage(Actor& src, float a) override
    {
        return spp::call<void(Actor*, Actor&, float)>(this, SLOT_TakeDamage_Af, src, a);
    }
};

// ---------------------------------------------------------------- state
struct PlayerState_g4
{
    spp::ShellBase* __spp_owner; // the runtime writes it (has_owner)
    float health;
    float speed;
    float shield;

    void Tick(float dt);
    void TakeDamage_f(float amount);
    void TakeDamage_Af(Actor& src, float amount);
    void heal(float amount);
};

static void init_health(void*, void* f)
{
    new (f) float(100.0f);
}
static void init_speed(void*, void* f)
{
    new (f) float(5.0f);
}
static void init_shield(void*, void* f)
{
    new (f) float(25.0f);
}
static void move_float(void* dst, const void* src)
{
    new (dst) float(*static_cast<const float*>(src));
}

constexpr uint8_t kFloatFlags = spp::kTriviallyCopyable | spp::kNothrowMove;

static const spp::FieldInfo Player_fields[] = {
    {"health", "hp", typeid(float).name(), offsetof(PlayerState_g4, health), sizeof(float), alignof(float), kFloatFlags,
        init_health, move_float, nullptr},
    {"speed", nullptr, typeid(float).name(), offsetof(PlayerState_g4, speed), sizeof(float), alignof(float),
        kFloatFlags, init_speed, move_float, nullptr},
    {"shield", nullptr, typeid(float).name(), offsetof(PlayerState_g4, shield), sizeof(float), alignof(float),
        kFloatFlags, init_shield, move_float, nullptr},
};

static const spp::StateLayout Player_layout = {
    "PlayerState_g4",
    sizeof(PlayerState_g4),
    alignof(PlayerState_g4),
    3,
    Player_fields,
    true,
};

// ------------------------------------------------- generation functions
static inline PlayerState_g4* state_of(Player_Shell* self)
{
    return static_cast<PlayerState_g4*>(self->state);
}

void PlayerState_g4::heal(float amount)
{
    if (SPP_UNLIKELY(spp::active_generation != GEN))
        return spp::stale_call("Player::heal");
    [[maybe_unused]] auto* self = static_cast<Player_Shell*>(__spp_owner);
    [[maybe_unused]] auto* st = this;
    st->health = std::min(st->health + 2.0f * amount, 100.0f);
    std::printf("[gen4] %s: heal %.1f (x2) -> health=%.1f\n", self->Name.c_str(), amount, st->health);
}

void PlayerState_g4::Tick(float dt)
{
    if (SPP_UNLIKELY(spp::active_generation != GEN))
        return spp::stale_call("Player::Tick");
    [[maybe_unused]] auto* self = static_cast<Player_Shell*>(__spp_owner);
    [[maybe_unused]] auto* st = this;
    self->Move({2.0f * st->speed * dt, 0, 0});
    std::printf("[gen4] %s: Tick dt=%.4f pos.x=%.4f health=%.1f shield=%.1f\n", self->Name.c_str(), dt,
        self->Position.x, st->health, st->shield);
}

void PlayerState_g4::TakeDamage_f(float amount)
{
    if (SPP_UNLIKELY(spp::active_generation != GEN))
        return spp::stale_call("Player::TakeDamage(float)");
    [[maybe_unused]] auto* self = static_cast<Player_Shell*>(__spp_owner);
    [[maybe_unused]] auto* st = this;
    st->health = std::max(st->health - amount, 0.0f);
    std::printf("[gen4] %s: TakeDamage %.1f -> health=%.1f\n", self->Name.c_str(), amount, st->health);
}

void PlayerState_g4::TakeDamage_Af(Actor& src, float amount)
{
    if (SPP_UNLIKELY(spp::active_generation != GEN))
        return spp::stale_call("Player::TakeDamage(Actor&, float)");
    [[maybe_unused]] auto* self = static_cast<Player_Shell*>(__spp_owner);
    [[maybe_unused]] auto* st = this;
    std::printf("[gen4] %s: hit by %s\n", self->Name.c_str(), src.Name.c_str());
    self->TakeDamage(amount);
}

// ------------------------------------------------------ record and entry
static const spp::SlotFn Player_slots[SLOT_COUNT] = {
    reinterpret_cast<spp::SlotFn>(&PlayerState_g4::Tick),
    reinterpret_cast<spp::SlotFn>(&PlayerState_g4::TakeDamage_f),
    reinterpret_cast<spp::SlotFn>(&PlayerState_g4::TakeDamage_Af),
    reinterpret_cast<spp::SlotFn>(&PlayerState_g4::heal),
};

spp::ClassRecord Player_record = {
    "Player",
    GEN,
    Player_slots,
    SLOT_COUNT,
    &Player_layout,
    nullptr,
    "Actor",
    spp::shell_layout_hash("Actor", sizeof(Player_Shell), alignof(Player_Shell), 3),
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
