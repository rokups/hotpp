#include "FileWatcher.h"

#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <thread>

#ifdef __linux__
#include <sys/inotify.h>
#endif

namespace harness {

FileWatcher::FileWatcher(std::string path, Mode mode) : path_(std::move(path))
{
    namespace fs = std::filesystem;
    const fs::path p = fs::absolute(path_);
    name_ = p.filename().string();
    poll_stamp();  // the first stamp: a later change differs from it
#ifdef __linux__
    if (mode == Mode::Auto) {
        fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd_ >= 0 &&
            inotify_add_watch(fd_, p.parent_path().c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE) < 0) {
            close(fd_);
            fd_ = -1;  // use the polling fallback
        }
    }
#else
    (void)mode;
#endif
}

FileWatcher::~FileWatcher()
{
    if (fd_ >= 0) close(fd_);
}

bool FileWatcher::poll_stamp()
{
    struct stat st {};
    long long ns = -1, size = -1;
    if (stat(path_.c_str(), &st) == 0) {
        ns = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
        size = st.st_size;
    }
    const bool changed = ns != stamp_ns_ || size != size_;
    stamp_ns_ = ns;
    size_ = size;
    return changed;
}

bool FileWatcher::wait_changed(int timeout_ms)
{
#ifdef __linux__
    if (fd_ >= 0) {
        pollfd pfd{fd_, POLLIN, 0};
        if (poll(&pfd, 1, timeout_ms) <= 0) return false;
        bool mine = false;
        alignas(inotify_event) char buf[4096];
        for (ssize_t n; (n = read(fd_, buf, sizeof buf)) > 0;)
            for (char* at = buf; at < buf + n;) {
                const auto* ev = reinterpret_cast<const inotify_event*>(at);
                if (ev->len && name_ == ev->name) mine = true;
                at += sizeof(inotify_event) + ev->len;
            }
        return mine;
    }
#endif
    // Polling: check at once, then every 10 ms until the timeout.
    using clock = std::chrono::steady_clock;
    const auto end = clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (poll_stamp()) return true;
        if (clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}  // namespace harness
