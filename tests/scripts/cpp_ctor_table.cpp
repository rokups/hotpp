// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The constructor table of a C++ script class (ClassEntry::construct).
// "how" tells which constructor ran.
#include "Actor.h"

#include <memory>
#include <string>
#include <utility>

using Meters = float;

class CppMade : public Actor
{
public:
    CppMade() : Actor("cpp") { how = 1; }
    // A parameter of an alias type: the key is the key of "float".
    CppMade(Meters m) : Actor("cpp")
    {
        meters = m;
        how = 2;
    }
    // A "&&" parameter of a move-only type: the constructor takes the object of the host.
    CppMade(std::unique_ptr<int>&& p) : Actor("cpp")
    {
        std::unique_ptr<int> mine = std::move(p);
        value = *mine;
        how = 3;
    }

    int how = 0;
    float meters = 0;
    int value = 0;
};
