// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The expression forms on a native pointer field, as a plain C++ script. The field "other" and
// the module variable "g_other" are hot::CheckedPtr<Charm>. The host sets "phase" and runs Tick.
// Phase 1 takes a raw pointer from a native call. Phase 2 takes a handle. Phase 3 sets null.
// A normal C++ build compiles this file. The tool hotc also translates it.
#include "Actor.h"
#include "Equip.h"

#include <functional>
#include <memory>

Charm* g_other = nullptr;

class Prober : public Actor, public Vault
{
public:
    Prober() : Actor("Prober") {}

    void Tick(float dt) override
    {
        if (phase == 1)
            other = Best();
        if (phase == 2)
        {
            std::shared_ptr<Charm> h = Held();
            other = h.get();
        }
        if (phase == 3)
            other = nullptr;
        is_null = other == nullptr ? 1 : 0;
        via_call = -1;
        via_pass = -1;
        via_lambda = -1;
        via_module = -1;
        via_deref = -1;
        if (other != nullptr)
        {
            other->Bump(1);
            via_call = other->Twice();
            via_deref = (*other).Twice();
            via_pass = Polish(other);
            std::function<int()> f = [this] { return other->Twice(); };
            via_lambda = f();
            g_other = other;
            via_module = g_other->Twice();
        }
        else
        {
            g_other = nullptr;
        }
    }

    Charm* other = nullptr;
    int phase = 0;
    int is_null = -1;
    int via_call = -1;
    int via_deref = -1;
    int via_pass = -1;
    int via_lambda = -1;
    int via_module = -1;

    ~Prober() { other = nullptr; }
};
