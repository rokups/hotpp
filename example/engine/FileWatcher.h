// A watcher of one file for the harness (design 8.1).
// On Linux it uses inotify on the parent directory. This also sees an editor that
// writes a new file and renames it over the old one. Without inotify it polls the
// modification time, the size, the inode and the ctime of the file. When only the inode or the
// ctime changes, it compares the hash of the content, so an edit that restores the old
// modification time with the same size is seen.
#pragma once
#include <ctime>
#include <string>

namespace harness {

class FileWatcher {
public:
    enum class Mode { Auto, Polling };  // Polling forces the fallback (used by the tests)

    explicit FileWatcher(std::string path, Mode mode = Mode::Auto);
    ~FileWatcher();
    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    bool uses_inotify() const { return fd_ >= 0; }

    // Waits up to "timeout_ms" for a change of the file. Returns true when the file
    // changed since the last call. Zero checks and returns at once.
    bool wait_changed(int timeout_ms);

private:
    bool poll_stamp();  // true when the stamp of the file differs from the last stamp

    std::string path_, name_;
    int fd_ = -1;  // the inotify descriptor, or -1
    long long stamp_ns_ = -1;
    long long size_ = -1;
    long long ctime_ns_ = -1;
    unsigned long long ino_ = 0;
    unsigned long long hash_ = 0;  // the hash of the content at the last stamp
};

}  // namespace harness
