// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Set and remove an environment variable of the test process. The same code works on Windows.
#pragma once

#include <cstdlib>

namespace test_support
{

// Sets the variable "name" to "value". On POSIX an empty "value" sets an empty string. On Windows
// an empty "value" removes the variable (_putenv_s).
inline void set_env(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}

// Removes the variable "name".
inline void unset_env(const char* name)
{
#ifdef _WIN32
    _putenv_s(name, ""); // Windows removes a variable that is set to an empty string
#else
    ::unsetenv(name);
#endif
}

} // namespace test_support
