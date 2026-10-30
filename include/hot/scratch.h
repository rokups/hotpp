// Scratch files: the directories that the backends and the samples write at run time.
#pragma once

#include <string>

namespace hot
{

// True when the scratch files must stay. The environment variable SPP_KEEP_TEMP overrides
// the CMake option. The value "0" or an empty value means false.
bool keep_temp();

// A scratch directory. The constructor removes the old content and creates the directory.
// The destructor removes the directory, unless keep_temp() is true.
class ScratchDir
{
public:
    explicit ScratchDir(std::string path);
    ~ScratchDir();
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    const std::string& path() const { return path_; }
    // The path of a file in the directory.
    std::string file(const std::string& name) const { return path_ + "/" + name; }

private:
    std::string path_;
};

// True when remove_scratch may remove the path. The path must be absolute. It must have no ".."
// or "." component. After lexical normalization it must have a parent path and a relative part.
// So "/", "/tmp/.." and "rel" are not removable.
bool is_removable_scratch_path(const std::string& path);

// A safe recursive remove. It does nothing when is_removable_scratch_path() is false.
void remove_scratch(const std::string& path);

} // namespace hot
