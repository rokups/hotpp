// A hand-written generation of the class Pulse for the reload stress test (design M8).
//
// The library has no fixed generation number. script_register uses the active generation plus
// one. So the test loads copies of the same file as generation 1, 2, 3 and so on. The core
// instance of a copy registers the same number, because the loader calls it before the commit.
//
// The class Pulse : Actor has the fields "ticks" (int) and "hits" (int), the slot "poke" and
// the thunk "Pulse::poke". Tick adds one to "ticks". poke adds the amount to "hits".
#include "Actor.h"
#include "hot/runtime.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <typeinfo>

namespace
{
uint32_t g_gen = 0; // the generation of this copy; script_register sets it
} // namespace

enum : unsigned
{
    SLOT_Tick = 0,
    SLOT_TakeDamage_f = 1,
    SLOT_TakeDamage_Af = 2,
    SLOT_poke = 3,
    SLOT_COUNT = 4,
};

extern hot::ClassRecord Pulse_record;

struct Pulse_Shell final
    : Actor
    , hot::ShellBase
{
    template <class... A>
    explicit Pulse_Shell(hot::Construct, A&&... a)
        : Actor(std::forward<A>(a)...)
    {
        hot::init_state(this, &Pulse_record);
    }
    ~Pulse_Shell() override { hot::destroy_state(this); }

    void Tick(float dt) override { return hot::call<void(Actor*, float)>(this, SLOT_Tick, dt); }
    void TakeDamage(float a) override { return hot::call<void(Actor*, float)>(this, SLOT_TakeDamage_f, a); }
    void TakeDamage(Actor& src, float a) override
    {
        return hot::call<void(Actor*, Actor&, float)>(this, SLOT_TakeDamage_Af, src, a);
    }
};

struct PulseState
{
    hot::ShellBase* __spp_owner; // the runtime writes it (has_owner)
    int ticks;
    int hits;

    void Tick(float);
    void __fwd_TakeDamage_f(float amount);
    void __fwd_TakeDamage_Af(Actor& src, float amount);
    void poke(int amount);
};

static void init_zero(void*, void* f)
{
    new (f) int(0);
}
static void move_int(void* dst, const void* src)
{
    new (dst) int(*static_cast<const int*>(src));
}

static const hot::FieldInfo Pulse_fields[] = {
    {"ticks", nullptr, typeid(int).name(), offsetof(PulseState, ticks), sizeof(int), alignof(int),
        hot::field_flags<int>(), init_zero, move_int, nullptr},
    {"hits", nullptr, typeid(int).name(), offsetof(PulseState, hits), sizeof(int), alignof(int),
        hot::field_flags<int>(), init_zero, move_int, nullptr},
};

static const hot::StateLayout Pulse_layout = {
    "PulseState", sizeof(PulseState), alignof(PulseState), 2, Pulse_fields, true};

void PulseState::Tick(float)
{
    if (HOT_UNLIKELY(hot::active_generation != g_gen))
        return hot::stale_call("Pulse::Tick");
    ticks += 1;
}

void PulseState::__fwd_TakeDamage_f(float amount)
{
    static_cast<Pulse_Shell*>(__spp_owner)->Actor::TakeDamage(amount);
}

void PulseState::__fwd_TakeDamage_Af(Actor& src, float amount)
{
    static_cast<Pulse_Shell*>(__spp_owner)->Actor::TakeDamage(src, amount);
}

void PulseState::poke(int amount)
{
    if (HOT_UNLIKELY(hot::active_generation != g_gen))
        return hot::stale_call("Pulse::poke");
    hits += amount;
}

static void Pulse__poke__thunk(Actor* a, int amount)
{
    auto* self = static_cast<Pulse_Shell*>(a);
    return hot::call<void(Actor*, int)>(self, SLOT_poke, amount);
}

static const hot::SlotFn Pulse_slots[SLOT_COUNT] = {
    reinterpret_cast<hot::SlotFn>(&PulseState::Tick),
    reinterpret_cast<hot::SlotFn>(&PulseState::__fwd_TakeDamage_f),
    reinterpret_cast<hot::SlotFn>(&PulseState::__fwd_TakeDamage_Af),
    reinterpret_cast<hot::SlotFn>(&PulseState::poke),
};

hot::ClassRecord Pulse_record = {
    "Pulse",
    0,
    Pulse_slots,
    SLOT_COUNT,
    &Pulse_layout,
    nullptr,
    "Actor",
    hot::shell_layout_hash("Actor", sizeof(Pulse_Shell), alignof(Pulse_Shell), 3),
};

static hot::Instance Pulse_create()
{
    auto* p = new Pulse_Shell(hot::Construct{}, std::string("Pulse"));
    return {static_cast<Actor*>(p), p};
}

extern "C" void script_register(hot::Registry& reg)
{
    g_gen = hot::active_generation + 1;
    Pulse_record.generation = g_gen;
    reg.add(&Pulse_record, &Pulse_create);
    reg.add_thunk("Pulse::poke", reinterpret_cast<void*>(&Pulse__poke__thunk));
}
