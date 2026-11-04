// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A native enum class. It has no formatter, so a script cannot print it with "${...}".
#pragma once

enum class Weather
{
    Sunny,
    Rain
};

inline Weather current_weather()
{
    return Weather::Rain;
}
