// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The id of the running process and a pipe that a child does not get. They have one form for
// each platform.
#pragma once

#ifdef _WIN32
    #include <process.h>
#else
    #include <fcntl.h>
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

#ifndef _WIN32
// A pipe with FD_CLOEXEC on both ends, so a child process gets only the ends that dup2 gives it.
// Returns 0, or -1 with errno. Linux uses pipe2. macOS has no pipe2: there, pipe and fcntl set
// the flag in two steps. A fork in another thread between the steps can give the ends to its
// child. The callers start their children from one thread, so this does not occur.
inline int pipe_cloexec(int fds[2])
{
    #ifdef __APPLE__
    if (pipe(fds) != 0)
        return -1;
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(fds[1], F_SETFD, FD_CLOEXEC) != 0)
    {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    return 0;
    #else
    return pipe2(fds, O_CLOEXEC);
    #endif
}
#endif

} // namespace hot::detail
