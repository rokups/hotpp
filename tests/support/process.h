// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A helper for the tests: it runs a process from an argument list. It does not use a shell, so
// it has the same behavior on Linux and on Windows. The tests that use it link hot_runtime on
// Windows (hot::windows_command_line).
#pragma once

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include "hot/command_line.h"

    #include <windows.h>
#else
    #include <fcntl.h>
    #include <spawn.h>
    #include <sys/wait.h>
    #include <unistd.h>
extern char** environ;
#endif

namespace test_support
{

// The result of run_process. The exit code is -1 when the process did not start or a signal
// killed it. In that case the output can have a short reason.
struct RunResult
{
    int exit_code = -1;
    std::string output;
};

// The text with "\r\n" changed to "\n". A Windows program writes "\r\n" in text mode. The text of
// run_process is raw, so a caller that compares text with an expected text uses this function.
inline std::string lf_only(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
        if (!(text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n'))
            out += text[i];
    return out;
}

// Runs argv[0] with the arguments argv[1...]. The program is found through PATH. The standard
// output and the standard error of the process go to one text. The standard input is empty. The
// function reads the output while the process runs, so a large output does not block the process.
inline RunResult run_process(const std::vector<std::string>& argv)
{
    RunResult result;
    if (argv.empty() || argv[0].empty())
    {
        result.output = "run_process: no program";
        return result;
    }
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE out_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&out_r, &out_w, &sa, 0))
    {
        result.output = "run_process: CreatePipe failed, error " + std::to_string(GetLastError());
        return result;
    }
    // The child must not get the read end.
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    HANDLE in_r = CreateFileA(
        "NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r == INVALID_HANDLE_VALUE ? nullptr : in_r;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    std::string cmd = hot::windows_command_line(argv);
    PROCESS_INFORMATION pi = {};
    const BOOL ok =
        CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    const DWORD spawn_error = ok ? 0 : GetLastError();
    // The parent closes its copy of the write end. The read gets the end of the file then.
    CloseHandle(out_w);
    if (in_r != INVALID_HANDLE_VALUE)
        CloseHandle(in_r);
    if (!ok)
    {
        CloseHandle(out_r);
        result.output = "run_process: cannot start '" + argv[0] + "', error " + std::to_string(spawn_error);
        return result;
    }
    CloseHandle(pi.hThread);
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(out_r, buf, sizeof buf, &got, nullptr) && got > 0)
        result.output.append(buf, got);
    CloseHandle(out_r);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    // A crash gives an NTSTATUS code (0xC0000005 and more). It maps to -1, as a signal does on Linux.
    if (GetExitCodeProcess(pi.hProcess, &code))
        result.exit_code = code >= 0xC0000000u ? -1 : static_cast<int>(code);
    CloseHandle(pi.hProcess);
#else
    // O_CLOEXEC: the child gets only the end that dup2 gives it.
    int out[2];
    if (pipe2(out, O_CLOEXEC) != 0)
    {
        result.output = std::string("run_process: pipe2 failed: ") + std::strerror(errno);
        return result;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, out[1], 2);
    std::vector<std::string> argv_s = argv;
    std::vector<char*> args;
    for (std::string& a : argv_s)
        args.push_back(a.data());
    args.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, argv_s[0].c_str(), &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(out[1]);
    if (rc != 0)
    {
        close(out[0]);
        result.output = "run_process: cannot start '" + argv[0] + "': " + std::strerror(rc);
        return result;
    }
    char buf[4096];
    for (;;)
    {
        const ssize_t n = read(out[0], buf, sizeof buf);
        if (n > 0)
            result.output.append(buf, static_cast<size_t>(n));
        else if (n == 0 || errno != EINTR)
            break;
    }
    close(out[0]);
    int status = 0;
    pid_t w;
    do
        w = waitpid(pid, &status, 0);
    while (w < 0 && errno == EINTR);
    if (w == pid && WIFEXITED(status))
        result.exit_code = WEXITSTATUS(status);
#endif
    return result;
}

} // namespace test_support
