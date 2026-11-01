// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The id of the running process. It has one form for each platform.
#pragma once

#ifdef _WIN32
    #include <process.h>
#else
    #include <unistd.h>
#endif

namespace hot::detail
{

// The id of the running process.
inline int process_id()
{
#ifdef _WIN32
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}

} // namespace hot::detail
