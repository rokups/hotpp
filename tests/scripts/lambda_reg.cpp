// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A hand-written ".cpp" script that has the shape of the lowering of a lambda. The method
// Never has a lambda and nothing calls it. The module must still register the lambda at load
// (tests/integration/closures/lambda_reg_tests.cpp).
#include "Actor.h"
#include "hot/runtime/lambda.h"

struct __LamTag_0
{
    static constexpr const char* key = "LambdaHolder::Never#0";
    static constexpr const char* names[] = {"a", "b"};
};

class LambdaHolder : public Actor
{
public:
    LambdaHolder() : Actor("LambdaHolder") {}

    int Never(int a, int b)
    {
        auto c = hot::make_lambda<__LamTag_0, int(int)>(
            hot::active_generation, hot::caps(a, b), [](auto& __c, int p) { return std::get<0>(__c) + std::get<1>(__c) + p; });
        return c(0);
    }
};
