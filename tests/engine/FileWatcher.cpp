// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "FileWatcher.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

#ifndef _WIN32
    #include <sys/stat.h>
    #include <unistd.h>
#endif

#ifdef __linux__
    #include <poll.h>
    #include <sys/inotify.h>
#endif

namespace harness
{

FileWatcher::FileWatcher(std::string path, Mode mode)
    : path_(std::move(path))
{
    namespace fs = std::filesystem;
    const fs::path p = fs::absolute(path_);
    name_ = p.filename().string();
    poll_stamp(); // the first stamp: a later change differs from it
#ifdef __linux__
    if (mode == Mode::Auto)
    {
        fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd_ >= 0 && inotify_add_watch(fd_, p.parent_path().c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE) < 0)
        {
            close(fd_);
            fd_ = -1; // use the polling fallback
        }
    }
#else
    (void)mode;
#endif
}

FileWatcher::~FileWatcher()
{
#ifdef __linux__
    if (fd_ >= 0)
        close(fd_);
#endif
}

namespace
{

long long ns_of(const std::filesystem::file_time_type& t)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

// FNV-1a hash of the content of a file.
unsigned long long hash_file(const std::string& path)
{
    unsigned long long h = 14695981039346656037ULL;
    std::ifstream f(path, std::ios::binary);
    char buf[8192];
    while (f.read(buf, sizeof buf) || f.gcount() > 0)
    {
        for (std::streamsize i = 0; i < f.gcount(); ++i)
        {
            h ^= static_cast<unsigned char>(buf[i]);
            h *= 1099511628211ULL;
        }
    }
    return h;
}

} // namespace

bool FileWatcher::poll_stamp()
{
    namespace fs = std::filesystem;
    long long ns = -1, size = -1, cns = -1;
    unsigned long long ino = 0;
    std::error_code ec;
    const auto mtime = fs::last_write_time(path_, ec);
    if (!ec)
    {
        ns = ns_of(mtime);
        size = static_cast<long long>(fs::file_size(path_, ec));
        if (ec)
            size = -1;
    }
#ifndef _WIN32
    // The change time and the inode have no portable form. On Windows the time and the size
    // decide, with the content hash.
    struct stat st{};
    if (stat(path_.c_str(), &st) == 0)
    {
        cns = static_cast<long long>(st.st_ctim.tv_sec) * 1000000000LL + st.st_ctim.tv_nsec;
        ino = static_cast<unsigned long long>(st.st_ino);
    }
#endif
    bool changed = ns != stamp_ns_ || size != size_;
    // The time and the size are equal, but the inode or the ctime differ: an edit can have
    // set the old time back. The content decides.
#ifdef _WIN32
    // No ctime and no inode here: the content hash decides on each poll.
    const bool always_hash = true;
#else
    const bool always_hash = false;
#endif
    unsigned long long hash = hash_;
    if (size >= 0 && (changed || always_hash || cns != ctime_ns_ || ino != ino_))
    {
        hash = hash_file(path_);
        if (!changed && hash != hash_)
            changed = true;
    }
    stamp_ns_ = ns;
    size_ = size;
    ctime_ns_ = cns;
    ino_ = ino;
    hash_ = hash;
    return changed;
}

bool FileWatcher::wait_changed(int timeout_ms)
{
#ifdef __linux__
    if (fd_ >= 0)
    {
        pollfd pfd{fd_, POLLIN, 0};
        if (poll(&pfd, 1, timeout_ms) <= 0)
            return false;
        bool mine = false;
        alignas(inotify_event) char buf[4096];
        for (ssize_t n; (n = read(fd_, buf, sizeof buf)) > 0;)
            for (char* at = buf; at < buf + n;)
            {
                const auto* ev = reinterpret_cast<const inotify_event*>(at);
                if (ev->len && name_ == ev->name)
                    mine = true;
                at += sizeof(inotify_event) + ev->len;
            }
        return mine;
    }
#endif
    // Polling: check at once, then every 10 ms until the timeout.
    using clock = std::chrono::steady_clock;
    const auto end = clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        if (poll_stamp())
            return true;
        if (clock::now() >= end)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace harness
