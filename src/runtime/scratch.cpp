#include "spp/scratch.h"

#include <cstdlib>
#include <filesystem>

// The CMake option SPP_KEEP_TEMP sets this define.
#ifndef SPP_KEEP_TEMP_DEFAULT
    #define SPP_KEEP_TEMP_DEFAULT 0
#endif

namespace spp
{

namespace fs = std::filesystem;

bool keep_temp()
{
    const char* env = std::getenv("SPP_KEEP_TEMP");
    if (env)
        return env[0] != '\0' && std::string(env) != "0";
    return SPP_KEEP_TEMP_DEFAULT != 0;
}

bool is_removable_scratch_path(const std::string& path)
{
    if (path.empty())
        return false;
    const fs::path p(path);
    if (!p.is_absolute())
        return false;
    for (const fs::path& part : p)
    {
        if (part == ".." || part == ".")
            return false;
    }
    const fs::path n = p.lexically_normal();
    return n.has_relative_path() && n.has_parent_path() && n != n.root_path();
}

void remove_scratch(const std::string& path)
{
    if (!is_removable_scratch_path(path))
        return;
    std::error_code ec;
    fs::remove_all(fs::path(path), ec);
}

ScratchDir::ScratchDir(std::string path)
    : path_(std::move(path))
{
    remove_scratch(path_);
    std::error_code ec;
    fs::create_directories(path_, ec);
}

ScratchDir::~ScratchDir()
{
    if (!keep_temp())
        remove_scratch(path_);
}

} // namespace spp
