// aot_cover: runs the script tests/scripts/aot_cover.hot as AOT code. The parity test builds it
// twice (plain AOT and optimized AOT) and compares the two outputs. The script has a noexcept
// override, a const virtual that the shell forwards, a pure virtual of an interface base, a
// script virtual that a call through a base handle reaches, and a script error in a noexcept
// override. It also has "new" with a throwing constructor and a throwing field initializer.
#include "Actor.h"
#include "Damageable.h"
#include "Guarded.h"
#include "hot/aot.h"
#include "hot/runtime.h"

#include <cstdio>
#include <exception>
#include <string>

int main()
{
    try
    {
        std::string reports;
        hot::set_script_error_handler([&](const hot::ScriptErrorReport& r) { reports += std::string(r.type) + ";"; });
        {
            hot::AotRegistry registry(hot::aot_table);
            World world;
            Actor* boss = registry.create_as<Actor>("Boss");
            world.Add(boss);
            world.Tick(0.5f);
            world.Tick(0.5f);
            auto* damageable = dynamic_cast<IDamageable*>(boss);
            auto* guarded = dynamic_cast<IGuarded*>(boss);
            // The class of the ".cpp" script: its destructor prints the facts.
            World cpp_world;
            cpp_world.Add(registry.create_as<Actor>("CppBoss"));
            cpp_world.Tick(0.5f);
            cpp_world.Tick(0.5f);
            const int hit1 = HitThrough(*damageable, 3), hit2 = HitThrough(*damageable, 4);
            std::printf("hit=%d hit=%d\n", hit1, hit2);
            const int guard = CallGuard(*guarded, 1), guard_bad = CallGuard(*guarded, 5);
            std::printf("guard=%d guard_bad=%d reports=%s\n", guard, guard_bad, reports.c_str());
        }
        hot::set_script_error_handler({});
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "aot_cover: %s\n", e.what());
        return 1;
    }
    return 0;
}
