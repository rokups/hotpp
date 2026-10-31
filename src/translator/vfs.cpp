// The disk file system of hot::FileSystem.
#include "hot/vfs.h"

#include <sys/stat.h>

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
        struct ::stat st;
        VfsStat r;
        if (::stat(path.c_str(), &st) != 0)
            return r;
        r.exists = true;
        const uint64_t mtime_ns = static_cast<uint64_t>(st.st_mtim.tv_sec) * 1000000000ull
            + static_cast<uint64_t>(st.st_mtim.tv_nsec);
        r.version = mtime_ns ^ static_cast<uint64_t>(st.st_size);
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
