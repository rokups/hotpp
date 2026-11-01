// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Generation 2 of the script as a plain C++ file. It adds the field howls.
// The field bites and the base state stay.
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
        this->howls = this->howls + 1;
        this->Hp = this->Hp - 1;
        std::printf("[v2] %s: hp=%d howls=%d\n", this->Name.c_str(), this->Hp, this->howls);
    }

    int bites = 0;
    int howls = 0;
};
