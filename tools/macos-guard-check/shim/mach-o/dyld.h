// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A stand-in for the macOS header. It declares only what the Hot++ sources use. The guard check
// (check.py) compiles the macOS branches on Linux with it. It is not the real SDK.
#pragma once

#include <cstdint>

extern "C" int _NSGetExecutablePath(char* buf, uint32_t* bufsize);
