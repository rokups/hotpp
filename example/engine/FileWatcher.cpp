#include "FileWatcher.h"

#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
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

namespace {

long long ns_of(const timespec& ts) { return static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec; }

// FNV-1a hash of the content of a file.
unsigned long long hash_file(const std::string& path)
{
    unsigned long long h = 14695981039346656037ULL;
    std::ifstream f(path, std::ios::binary);
    char buf[8192];
    while (f.read(buf, sizeof buf) || f.gcount() > 0) {
        for (std::streamsize i = 0; i < f.gcount(); ++i) {
            h ^= static_cast<unsigned char>(buf[i]);
            h *= 1099511628211ULL;
        }
    }
    return h;
}

}  // namespace

bool FileWatcher::poll_stamp()
{
    struct stat st {};
    long long ns = -1, size = -1, cns = -1;
    unsigned long long ino = 0;
    if (stat(path_.c_str(), &st) == 0) {
        ns = ns_of(st.st_mtim);
        cns = ns_of(st.st_ctim);
        size = st.st_size;
        ino = static_cast<unsigned long long>(st.st_ino);
    }
    bool changed = ns != stamp_ns_ || size != size_;
    // The time and the size are equal, but the inode or the ctime differ: an edit can have
    // set the old time back. The content decides.
    unsigned long long hash = hash_;
    if (size >= 0 && (changed || cns != ctime_ns_ || ino != ino_)) {
        hash = hash_file(path_);
        if (!changed && hash != hash_) changed = true;
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
