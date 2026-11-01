// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The host class of the SDK smoke test. The script class Doubler overrides Compute.
// The interface uses std types, so the parse fails when hotc does not find the standard headers.
#pragma once
#include <cstddef>
#include <string>

class Calc
{
public:
    virtual ~Calc() = default;
    virtual std::size_t Compute(std::size_t x) { return x; }
    std::string Name = "calc";
};
