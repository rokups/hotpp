// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The disk file system of hot::FileSystem.
#include "hot/vfs.h"

#include "file_time.h"

#include <atomic>
#include <fstream>
#include <sstream>

namespace hot
{

uint64_t FileSystem::next_id()
{
    static std::atomic<uint64_t> counter{0};
    return ++counter;
}

namespace
{

class DiskFileSystem final : public FileSystem
{
public:
    bool read(const std::string& path, std::string& out) override
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return false;
        std::ostringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }

    VfsStat stat(const std::string& path) override
    {
        VfsStat r;
        const hotc::FileTimes t = hotc::file_times(path);
        if (!t.exists)
            return r;
        r.exists = true;
        r.version = t.mtime_ns ^ t.size;
        return r;
    }
};

} // namespace

std::shared_ptr<FileSystem> disk_file_system()
{
    static const std::shared_ptr<FileSystem> fs = std::make_shared<DiskFileSystem>();
    return fs;
}

} // namespace hot
