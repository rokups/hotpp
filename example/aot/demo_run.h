// The run of the AOT demo (S12). The AOT program and the test of the loader use this
// same function, so they run the same steps. The result is a text that shows the state of each
// object after each frame. Two runs that give the same text behave the same.
#pragma once
#include "Actor.h"
#include "spp/runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <typeinfo>

namespace aot_demo
{

// The text of one field of a script object. Types that the demo does not print show "?".
inline std::string field_text(const spp::FieldInfo& f, const char* state)
{
    const void* p = state + f.offset;
    char buf[64];
    if (!std::strcmp(f.type_key, typeid(int32_t).name()))
    {
        std::snprintf(buf, sizeof buf, "%d", *static_cast<const int32_t*>(p));
        return buf;
    }
    if (!std::strcmp(f.type_key, typeid(float).name()))
    {
        std::snprintf(buf, sizeof buf, "%.3f", static_cast<double>(*static_cast<const float*>(p)));
        return buf;
    }
    if (!std::strcmp(f.type_key, typeid(bool).name()))
        return *static_cast<const bool*>(p) ? "true" : "false";
    if (!std::strcmp(f.type_key, typeid(std::string).name()))
        return "'" + *static_cast<const std::string*>(p) + "'";
    return "?";
}

inline std::string describe(Actor* actor)
{
    auto* shell = dynamic_cast<spp::ShellBase*>(actor);
    if (!shell)
        return "native";
    const spp::StateLayout* l = shell->cls->layout;
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s %s pos.x=%.3f", shell->cls->name, actor->Name.c_str(),
        static_cast<double>(actor->Position.x));
    std::string text = buf;
    for (uint32_t i = 0; i < l->field_count; ++i)
        text += std::string(" ") + l->fields[i].name + "="
            + field_text(l->fields[i], static_cast<const char*>(shell->state));
    return text;
}

// "create" makes an object of a class name and gives an spp::Instance.
// The run makes one object of each class, ticks the world 3 times and hits the objects.
template <class Create> std::string run(Create&& create)
{
    std::string log;
    {
        World world;
        for (const char* name : {"Fighter", "Veteran", "Spawner"})
            world.Add(static_cast<Actor*>(create(name).native));
        for (int frame = 1; frame <= 3; ++frame)
        {
            world.Tick(0.5f);
            for (Actor* a : world.Actors())
                log += "frame " + std::to_string(frame) + ": " + describe(a) + "\n";
        }
        // A native call of a virtual function runs the override of the script class.
        for (Actor* a : world.Actors())
            a->TakeDamage(5.0f);
        for (Actor* a : world.Actors())
            log += "hit: " + describe(a) + "\n";
    }
    log += "done\n";
    return log;
}

} // namespace aot_demo
