// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The host of the SDK smoke test. It makes the script class Doubler and calls Compute.
// It prints the result. It returns 0 only when the result is 46.
#include "Calc.h"
#include "hot/aot.h"

#include <cstddef>
#include <cstdio>
#include <exception>
#include <memory>

int main()
{
    try
    {
        hot::AotRegistry registry(hot::aot_table);
        std::unique_ptr<Calc> calc(registry.create_as<Calc>("Doubler"));
        if (!calc)
        {
            std::fprintf(stderr, "sdk_smoke: the class Doubler is not available\n");
            return 1;
        }
        const std::size_t result = calc->Compute(20);
        std::printf("sdk_smoke: result=%llu\n", static_cast<unsigned long long>(result));
        std::fflush(stdout);
        return result == 46 ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "sdk_smoke: %s\n", e.what());
        return 1;
    }
}
