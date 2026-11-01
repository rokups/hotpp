// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Math helpers for script code. A script writes Math.Min(a, b).
// The translator turns this into Math::Min(a, b).
#pragma once

struct Math
{
    template <class T> static constexpr T Min(T a, T b) { return b < a ? b : a; }
    template <class T> static constexpr T Max(T a, T b) { return a < b ? b : a; }
    template <class T> static constexpr T Clamp(T v, T lo, T hi) { return v < lo ? lo : (hi < v ? hi : v); }
};
