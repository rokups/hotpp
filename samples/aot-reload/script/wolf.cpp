// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The AOT script of the sample as a plain C++ file. The build compiles it into the binary. Wolf
// derives from the host class Creature and overrides Tick.
#include "Creature.h"

#include <cstdio>
#include <string>

class Wolf : public Creature
{
public:
    Wolf() : Creature(std::string("wolf")) {}

    void Tick(float dt) override
    {
        (void)dt;
        this->bites = this->bites + 1;
        this->Hp = this->Hp - 1;
        std::printf("[aot] %s: bites=%d hp=%d\n", this->Name.c_str(), this->bites, this->Hp);
    }

    int bites = 0;
};
