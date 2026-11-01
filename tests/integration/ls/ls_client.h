// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A small LSP test client. It runs hot::ls::run_server() on a thread of the test process and
// talks to it over two pipes.
#pragma once

#include "jsonrpc.h"
#include "server.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

namespace ls_test
{

using json = nlohmann::json;

class LsClient
{
public:
    explicit LsClient(const hot::ls::ServerOptions& options)
    {
        // O_CLOEXEC: clangd must not get a copy of an end. Else the end of the input never comes.
        if (pipe2(to_server_, O_CLOEXEC) != 0 || pipe2(from_server_, O_CLOEXEC) != 0)
            std::abort();
        server_ = std::thread([this, options]
        {
            exit_code_ = hot::ls::run_server(to_server_[0], from_server_[1], options);
            close(from_server_[1]);
        });
        reader_ = std::thread([this]
        {
            hot::ls::MessageReader r(from_server_[0]);
            std::string body;
            while (r.read(body))
            {
                std::lock_guard<std::mutex> lock(mutex_);
                messages_.push_back(json::parse(body));
                cv_.notify_all();
            }
            std::lock_guard<std::mutex> lock(mutex_);
            eof_ = true;
            cv_.notify_all();
        });
    }
    ~LsClient() { stop(); }

    // Sends a request and waits for its response. Gives the full response, or null.
    json request(const std::string& method, json params, int timeout_ms = 120000)
    {
        const int id = next_id_++;
        send({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}});
        return wait(
            [&](const json& m) { return m.contains("id") && m["id"] == id && !m.contains("method"); }, timeout_ms);
    }
    void notify(const std::string& method, json params)
    {
        send({{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}});
    }
    // Waits for a notification that "match" accepts and removes it. Gives null on a timeout.
    json wait_notification(
        const std::string& method, const std::function<bool(const json&)>& match, int timeout_ms = 120000)
    {
        return wait([&](const json& m) { return m.value("method", std::string()) == method && match(m["params"]); },
            timeout_ms);
    }

    // initialize and initialized. Gives the result of initialize.
    json initialize(const std::string& root_uri)
    {
        json r =
            request("initialize", {{"processId", nullptr}, {"rootUri", root_uri}, {"capabilities", json::object()}});
        notify("initialized", json::object());
        return r.value("result", json());
    }

    void open(const std::string& uri, const std::string& text)
    {
        notify("textDocument/didOpen",
            {{"textDocument", {{"uri", uri}, {"languageId", "hot"}, {"version", 1}, {"text", text}}}});
    }
    void change(const std::string& uri, int version, const std::string& text)
    {
        notify("textDocument/didChange",
            {{"textDocument", {{"uri", uri}, {"version", version}}},
                {"contentChanges", json::array({{{"text", text}}})}});
    }

    // shutdown and exit. Gives the exit code of the server.
    int stop()
    {
        if (stopped_)
            return exit_code_;
        stopped_ = true;
        request("shutdown", nullptr, 30000);
        notify("exit", nullptr);
        close(to_server_[1]);
        server_.join();
        reader_.join();
        close(to_server_[0]);
        close(from_server_[0]);
        return exit_code_;
    }

private:
    void send(const json& m)
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        hot::ls::write_message(to_server_[1], m.dump());
    }
    json wait(const std::function<bool(const json&)>& match, int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;)
        {
            for (auto it = messages_.begin(); it != messages_.end(); ++it)
                if (match(*it))
                {
                    json m = std::move(*it);
                    messages_.erase(it);
                    return m;
                }
            if (eof_ || cv_.wait_until(lock, deadline) == std::cv_status::timeout)
            {
                for (auto it = messages_.begin(); it != messages_.end(); ++it)
                    if (match(*it))
                    {
                        json m = std::move(*it);
                        messages_.erase(it);
                        return m;
                    }
                return nullptr;
            }
        }
    }

    int to_server_[2] = {-1, -1}, from_server_[2] = {-1, -1};
    std::thread server_, reader_;
    std::mutex mutex_, write_mutex_;
    std::condition_variable cv_;
    std::deque<json> messages_;
    bool eof_ = false, stopped_ = false;
    int next_id_ = 1;
    int exit_code_ = -1;
};

} // namespace ls_test
