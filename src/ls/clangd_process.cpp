// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "clangd_process.h"

#include "jsonrpc.h"

#include <chrono>
#include <fcntl.h>

#ifdef _WIN32
    #include "hot/command_line.h"

    #include <io.h>
    #include <windows.h>
#else
    #include "hot/runtime/process.h"

    #include <sys/wait.h>

    #include <signal.h>
    #include <spawn.h>
    #include <unistd.h>

extern char** environ;
#endif

namespace hot::ls
{

using json = nlohmann::json;

ClangdProcess::~ClangdProcess()
{
    stop();
}

#ifdef _WIN32

namespace
{
void close_fd(int fd)
{
    _close(fd);
}
} // namespace

bool ClangdProcess::start(
    const std::string& path, const std::vector<std::string>& args, const std::string& log, Handler on_notification)
{
    on_notification_ = std::move(on_notification);
    // The child inherits only the ends of the pipes that it uses. The other ends are not inheritable.
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&in_r, &in_w, &sa, 0))
        return false;
    if (!CreatePipe(&out_r, &out_w, &sa, 0))
    {
        CloseHandle(in_r);
        CloseHandle(in_w);
        return false;
    }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    HANDLE err = CreateFileA(log.empty() ? "NUL" : log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = err == INVALID_HANDLE_VALUE ? nullptr : err;
    std::vector<std::string> argv_s = {path};
    argv_s.insert(argv_s.end(), args.begin(), args.end());
    std::string cmd = hot::windows_command_line(argv_s);
    PROCESS_INFORMATION pi = {};
    const BOOL ok =
        CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (err != INVALID_HANDLE_VALUE)
        CloseHandle(err);
    if (!ok)
    {
        CloseHandle(in_w);
        CloseHandle(out_r);
        return false;
    }
    CloseHandle(pi.hThread);
    // The C runtime descriptors own the handles. _close closes them.
    const int to = _open_osfhandle(reinterpret_cast<intptr_t>(in_w), 0);
    const int from = _open_osfhandle(reinterpret_cast<intptr_t>(out_r), _O_RDONLY);
    if (to < 0 || from < 0)
    {
        if (to >= 0)
            _close(to);
        else
            CloseHandle(in_w);
        if (from >= 0)
            _close(from);
        else
            CloseHandle(out_r);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return false;
    }
    _setmode(to, _O_BINARY);
    _setmode(from, _O_BINARY);
    process_ = pi.hProcess;
    to_ = to;
    from_ = from;
    closed_ = false;
    reader_ = std::thread([this] { reader_loop(); });
    return true;
}

#else

namespace
{
void close_fd(int fd)
{
    close(fd);
}
} // namespace

bool ClangdProcess::start(
    const std::string& path, const std::vector<std::string>& args, const std::string& log, Handler on_notification)
{
    on_notification_ = std::move(on_notification);
    // O_CLOEXEC: the child gets only the two ends that dup2 gives it.
    int in[2], out[2];
    if (hot::detail::pipe_cloexec(in) != 0)
        return false;
    if (hot::detail::pipe_cloexec(out) != 0)
    {
        close(in[0]);
        close(in[1]);
        return false;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addopen(
        &fa, 2, log.empty() ? "/dev/null" : log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    std::vector<std::string> argv_s = {path};
    argv_s.insert(argv_s.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : argv_s)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, path.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    close(out[1]);
    if (rc != 0)
    {
        close(in[1]);
        close(out[0]);
        return false;
    }
    pid_ = pid;
    to_ = in[1];
    from_ = out[0];
    closed_ = false;
    reader_ = std::thread([this] { reader_loop(); });
    return true;
}

#endif

bool ClangdProcess::send(const json& msg)
{
    if (to_ < 0 || closed_)
        return false;
    std::lock_guard<std::mutex> lock(write_mutex_);
    return write_message(to_, msg.dump(-1, ' ', false, json::error_handler_t::replace));
}

void ClangdProcess::reader_loop()
{
    MessageReader reader(from_);
    std::string body;
    while (reader.read(body))
    {
        json msg = json::parse(body, nullptr, false);
        if (msg.is_discarded() || !msg.is_object())
            continue;
        if (msg.contains("method"))
        {
            if (msg.contains("id"))
            {
                // A request from clangd (for example window/workDoneProgress/create).
                send({{"jsonrpc", "2.0"}, {"id", msg["id"]}, {"result", nullptr}});
            }
            else if (on_notification_)
            {
                try
                {
                    on_notification_(msg);
                }
                catch (const std::exception&)
                {
                    // A bad notification is dropped. The reader goes on.
                }
            }
            continue;
        }
        if (msg.contains("id") && msg["id"].is_number_integer())
        {
            const int64_t id = msg["id"].get<int64_t>();
            std::lock_guard<std::mutex> lock(mutex_);
            // The waiter of an abandoned id is gone (a timeout): the response is dropped.
            if (abandoned_.erase(id) == 0)
                responses_[id] = std::move(msg);
            cv_.notify_all();
        }
    }
    closed_ = true;
    std::lock_guard<std::mutex> lock(mutex_);
    cv_.notify_all();
}

json ClangdProcess::request(const std::string& method, json params, int timeout_ms)
{
    const int64_t id = next_id_++;
    json msg = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.is_null())
        msg["params"] = std::move(params);
    if (!send(msg))
        return nullptr;
    std::unique_lock<std::mutex> lock(mutex_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!responses_.count(id))
    {
        if (closed_ || cv_.wait_until(lock, deadline) == std::cv_status::timeout)
        {
            if (!responses_.count(id))
            {
                abandoned_.insert(id);
                return nullptr;
            }
        }
    }
    json r = std::move(responses_[id]);
    responses_.erase(id);
    return r;
}

void ClangdProcess::notify(const std::string& method, json params)
{
    json msg = {{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.is_null())
        msg["params"] = std::move(params);
    send(msg);
}

void ClangdProcess::stop()
{
#ifdef _WIN32
    if (process_ == nullptr)
        return;
#else
    if (pid_ <= 0)
        return;
#endif
    if (!closed_)
    {
        request("shutdown", nullptr, 3000);
        notify("exit", nullptr);
    }
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        close_fd(to_);
        to_ = -1;
    }
#ifdef _WIN32
    // Wait up to 3 s for the process. Then kill it.
    HANDLE process = static_cast<HANDLE>(process_);
    if (WaitForSingleObject(process, 3000) != WAIT_OBJECT_0)
    {
        TerminateProcess(process, 1);
        WaitForSingleObject(process, INFINITE);
    }
    CloseHandle(process);
    process_ = nullptr;
#else
    // Wait up to 3 s for the process. Then kill it.
    int status = 0;
    bool done = false;
    for (int i = 0; i < 300 && !done; ++i)
    {
        done = waitpid(pid_, &status, WNOHANG) == pid_;
        if (!done)
            usleep(10000);
    }
    if (!done)
    {
        kill(pid_, SIGKILL);
        waitpid(pid_, &status, 0);
    }
    pid_ = -1;
#endif
    if (reader_.joinable())
        reader_.join();
    close_fd(from_);
    from_ = -1;
}

} // namespace hot::ls
