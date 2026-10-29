// The host files of spp-ls (step 2a).
//
// A host is a program that runs scripts. The CMake function spp_script_host writes one JSON
// file for each host: "<hosts-dir>/<name>.json". The file tells the compiler, the include
// directories and the defines of the host. It also tells which scripts the host runs.
// HostSet loads these files. match() finds the best host for a script path.
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace spp::ls {

struct Host {
    std::string name, binary_dir, compiler, std_version, resource_dir;
    std::vector<std::string> script_dirs, scripts, include_dirs, defines, flags;

    // The flags, then "-D<define>" for each define. The JSON file has no "-D" prefix.
    std::vector<std::string> cxx_flags() const;
};

class HostSet {
public:
    // Loads each "*.json" file in dir. It replaces the previous set. It skips a file with
    // invalid JSON or a version that is not 1. When require_cache is true, it also skips a host
    // when "<binaryDir>/CMakeCache.txt" does not exist (the build directory is gone).
    // It remembers the modification time of dir.
    void load(const std::string& dir, bool require_cache = true);

    // Returns true and loads again when the modification time of the directory changed.
    bool refresh();

    // Gives the best host for an absolute script path, or nullptr. An exact entry in "scripts"
    // scores 1000000. Else the longest "scriptDirs" entry that contains the path scores its
    // length. A directory contains a path only at a path component border. On a tie, the first
    // name in sorted order wins.
    std::shared_ptr<const Host> match(const std::string& script_path) const;

    size_t size() const { return hosts_.size(); }

private:
    std::string dir_;
    bool require_cache_ = true;
    bool loaded_ = false;
    std::filesystem::file_time_type mtime_{};
    std::vector<std::shared_ptr<const Host>> hosts_;  // sorted by name
};

}  // namespace spp::ls
