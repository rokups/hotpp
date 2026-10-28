#include "clangd_process.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>

#include "jsonrpc.h"

extern char** environ;

namespace spp::ls {

using json = nlohmann::json;

ClangdProcess::~ClangdProcess() { stop(); }

bool ClangdProcess::start(const std::string& path, const std::vector<std::string>& args, const std::string& log,
                          Handler on_notification)
{
    on_notification_ = std::move(on_notification);
    // O_CLOEXEC: the child gets only the two ends that dup2 gives it.
    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC) != 0) return false;
    if (pipe2(out, O_CLOEXEC) != 0) {
        close(in[0]);
        close(in[1]);
        return false;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, log.empty() ? "/dev/null" : log.c_str(), O_WRONLY | O_CREAT | O_APPEND,
                                     0644);
    std::vector<std::string> argv_s = {path};
    argv_s.insert(argv_s.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : argv_s) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, path.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    close(out[1]);
    if (rc != 0) {
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

bool ClangdProcess::send(const json& msg)
{
    if (to_ < 0 || closed_) return false;
    std::lock_guard<std::mutex> lock(write_mutex_);
    return write_message(to_, msg.dump(-1, ' ', false, json::error_handler_t::replace));
}

void ClangdProcess::reader_loop()
{
    MessageReader reader(from_);
    std::string body;
    while (reader.read(body)) {
        json msg = json::parse(body, nullptr, false);
        if (msg.is_discarded() || !msg.is_object()) continue;
        if (msg.contains("method")) {
            if (msg.contains("id")) {
                // A request from clangd (for example window/workDoneProgress/create).
                send({{"jsonrpc", "2.0"}, {"id", msg["id"]}, {"result", nullptr}});
            } else if (on_notification_) {
                try {
                    on_notification_(msg);
                } catch (const std::exception&) {
                    // A bad notification is dropped. The reader goes on.
                }
            }
            continue;
        }
        if (msg.contains("id") && msg["id"].is_number_integer()) {
            const int64_t id = msg["id"].get<int64_t>();
            std::lock_guard<std::mutex> lock(mutex_);
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
    if (!params.is_null()) msg["params"] = std::move(params);
    if (!send(msg)) return nullptr;
    std::unique_lock<std::mutex> lock(mutex_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!responses_.count(id)) {
        if (closed_ || cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            if (!responses_.count(id)) return nullptr;
        }
    }
    json r = std::move(responses_[id]);
    responses_.erase(id);
    return r;
}

void ClangdProcess::notify(const std::string& method, json params)
{
    json msg = {{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.is_null()) msg["params"] = std::move(params);
    send(msg);
}

void ClangdProcess::stop()
{
    if (pid_ <= 0) return;
    if (!closed_) {
        request("shutdown", nullptr, 3000);
        notify("exit", nullptr);
    }
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        close(to_);
        to_ = -1;
    }
    // Wait up to 3 s for the process. Then kill it.
    int status = 0;
    bool done = false;
    for (int i = 0; i < 300 && !done; ++i) {
        done = waitpid(pid_, &status, WNOHANG) == pid_;
        if (!done) usleep(10000);
    }
    if (!done) {
        kill(pid_, SIGKILL);
        waitpid(pid_, &status, 0);
    }
    pid_ = -1;
    if (reader_.joinable()) reader_.join();
    close(from_);
    from_ = -1;
}

}  // namespace spp::ls
