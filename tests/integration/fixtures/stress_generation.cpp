// A hand-written generation of the class Pulse for the reload stress test (design M8).
//
// The library has no fixed generation number. script_register uses the active generation plus
// one. So the test loads copies of the same file as generation 1, 2, 3 and so on. The core
// instance of a copy registers the same number, because the loader calls it before the commit.
//
// The class Pulse : Actor has the fields "ticks" (int) and "hits" (int), the slot "poke" and
// the thunk "Pulse::poke". Tick adds one to "ticks". poke adds the amount to "hits".
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <typeinfo>

#include "Actor.h"
#include "spp/runtime.h"

namespace {
uint32_t g_gen = 0;  // the generation of this copy; script_register sets it
}  // namespace

enum : unsigned {
    SLOT_Tick = 0,
    SLOT_TakeDamage_f = 1,
    SLOT_TakeDamage_Af = 2,
    SLOT_poke = 3,
    SLOT_COUNT = 4,
};

extern spp::ClassRecord Pulse_record;

struct Pulse_Shell final : Actor, spp::ShellBase {
    template <class... A>
    explicit Pulse_Shell(spp::Construct, A&&... a) : Actor(std::forward<A>(a)...)
    {
        spp::init_state(this, &Pulse_record);
    }
    ~Pulse_Shell() override { spp::destroy_state(this); }

    void Tick(float dt) override { return spp::call<void(Actor*, float)>(cls, SLOT_Tick, this, dt); }
    void TakeDamage(float a) override
    {
        return spp::call<void(Actor*, float)>(cls, SLOT_TakeDamage_f, this, a);
    }
    void TakeDamage(Actor& src, float a) override
    {
        return spp::call<void(Actor*, Actor&, float)>(cls, SLOT_TakeDamage_Af, this, src, a);
    }
};

struct PulseState {
    int ticks;
    int hits;
};

static void init_zero(void*, void* f) { new (f) int(0); }
static void move_int(void* dst, const void* src) { new (dst) int(*static_cast<const int*>(src)); }

static const spp::FieldInfo Pulse_fields[] = {
    {"ticks", nullptr, typeid(int).name(), offsetof(PulseState, ticks), sizeof(int), alignof(int),
     spp::field_flags<int>(), init_zero, move_int, nullptr},
    {"hits", nullptr, typeid(int).name(), offsetof(PulseState, hits), sizeof(int), alignof(int),
     spp::field_flags<int>(), init_zero, move_int, nullptr},
};

static const spp::StateLayout Pulse_layout = {"PulseState", sizeof(PulseState), alignof(PulseState), 2,
                                              Pulse_fields};

static PulseState* state_of(Actor* a) { return static_cast<PulseState*>(static_cast<Pulse_Shell*>(a)->state); }

static void Pulse__Tick(Actor* a, float)
{
    if (SPP_UNLIKELY(spp::active_generation != g_gen)) return spp::stale_call("Pulse::Tick");
    state_of(a)->ticks += 1;
}

static void Pulse__TakeDamage_f(Actor* a, float amount) { static_cast<Pulse_Shell*>(a)->Actor::TakeDamage(amount); }

static void Pulse__TakeDamage_Af(Actor* a, Actor& src, float amount)
{
    static_cast<Pulse_Shell*>(a)->Actor::TakeDamage(src, amount);
}

static void Pulse__poke(Actor* a, int amount)
{
    if (SPP_UNLIKELY(spp::active_generation != g_gen)) return spp::stale_call("Pulse::poke");
    state_of(a)->hits += amount;
}

static void Pulse__poke__thunk(Actor* a, int amount)
{
    auto* self = static_cast<Pulse_Shell*>(a);
    return spp::call<void(Actor*, int)>(self->cls, SLOT_poke, a, amount);
}

static void* Pulse_slots[SLOT_COUNT] = {
    reinterpret_cast<void*>(&Pulse__Tick),
    reinterpret_cast<void*>(&Pulse__TakeDamage_f),
    reinterpret_cast<void*>(&Pulse__TakeDamage_Af),
    reinterpret_cast<void*>(&Pulse__poke),
};

spp::ClassRecord Pulse_record = {
    "Pulse", 0, Pulse_slots, SLOT_COUNT, &Pulse_layout, nullptr,
    "Actor", spp::shell_layout_hash("Actor", sizeof(Pulse_Shell), alignof(Pulse_Shell), 3),
};

static spp::Instance Pulse_create()
{
    auto* p = new Pulse_Shell(spp::Construct{}, std::string("Pulse"));
    return {static_cast<Actor*>(p), p};
}

extern "C" void script_register(spp::Registry& reg)
{
    g_gen = spp::active_generation + 1;
    Pulse_record.generation = g_gen;
    reg.add(&Pulse_record, &Pulse_create);
    reg.add_thunk("Pulse::poke", reinterpret_cast<void*>(&Pulse__poke__thunk));
}
