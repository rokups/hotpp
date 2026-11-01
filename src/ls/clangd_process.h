// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A clangd child process with an LSP connection over two pipes.
#pragma once

#include <nlohmann/json.hpp>
#include <sys/types.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hot::ls
{

class ClangdProcess
{
public:
    // Gets each notification from clangd. It runs on the reader thread.
    using Handler = std::function<void(const nlohmann::json&)>;

    ClangdProcess() = default;
    ~ClangdProcess();
    ClangdProcess(const ClangdProcess&) = delete;
    ClangdProcess& operator=(const ClangdProcess&) = delete;

    // Starts "path" with "args". The stderr of clangd goes to "log" (empty: /dev/null).
    bool start(
        const std::string& path, const std::vector<std::string>& args, const std::string& log, Handler on_notification);
    bool running() const { return pid_ > 0 && !closed_; }

    // Sends a request and waits for the response. The result is the full response message
    // ("result" or "error"), or null when clangd does not answer in time or stopped.
    nlohmann::json request(const std::string& method, nlohmann::json params, int timeout_ms = 60000);
    void notify(const std::string& method, nlohmann::json params);

    // Sends "shutdown" and "exit", then waits for the process. It kills a process that hangs.
    void stop();

private:
    void reader_loop();
    bool send(const nlohmann::json& msg);

    pid_t pid_ = -1;
    int to_ = -1, from_ = -1;
    std::thread reader_;
    std::mutex write_mutex_, mutex_;
    std::condition_variable cv_;
    std::map<int64_t, nlohmann::json> responses_;
    std::atomic<int64_t> next_id_{1};
    std::atomic<bool> closed_{false};
    Handler on_notification_;
};

} // namespace hot::ls
