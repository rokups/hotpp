// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The host files of hot-ls (step 2a).
//
// A host is a program that runs scripts. The CMake function hot_script_host writes one JSON
// file for each host: "<hosts-dir>/<name>.json". The file tells the compiler, the include
// directories and the defines of the host. It also tells which scripts the host runs.
// The JSON file can name three side files: "definesFile", "optionsFile" and "includesFile".
// They hold one define, one option or one include dir on each line, with no escape. They are
// relative to the directory of the JSON file. A relative include dir, script dir or script is
// relative to "sourceDir" in the JSON file. The loader makes it absolute and normalizes it.
// The include dirs have the order: the first entry of "includeDirs", the "includesFile" lines,
// the other entries of "includeDirs". A repeated dir is dropped. A path with a newline
// cannot be in a side file.
// HostSet loads these files. match() finds the best host for a script path.
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace hot::ls
{

struct Host
{
    std::string name, binary_dir, compiler, std_version, resource_dir;
    std::vector<std::string> script_dirs, scripts, include_dirs, defines, flags;

    // The flags, then "-D<define>" for each define. The JSON file has no "-D" prefix.
    std::vector<std::string> cxx_flags() const;
};

class HostSet
{
public:
    // Loads each "*.json" file in dir. It replaces the previous set. It skips a file with
    // invalid JSON or a version that is not 1. When require_cache is true, it also skips a host
    // when "<binaryDir>/CMakeCache.txt" does not exist (the build directory is gone).
    // It remembers the modification time of dir.
    void load(const std::string& dir, bool require_cache = true);

    // Returns true and loads again when the modification time of the directory changed.
    // Else, when require_cache is true, it checks "<binaryDir>/CMakeCache.txt" of each host.
    // A skipped host whose cache file now exists is added. A host whose cache file is gone
    // is dropped (and kept as skipped). It returns true when the set of hosts changed.
    bool refresh();

    // Marks the set for a full reload at the next refresh(), when file is a "*.json", "*.defines", "*.options" or
    // "*.includes" file in the directory of the set. The modification time of the directory does not change when a host
    // file changes in place. Returns true when the file belongs to the set.
    bool force_reload_for(const std::string& file);

    // Gives the best host for an absolute script path, or nullptr. An exact entry in "scripts"
    // scores 1000000. Else the longest "scriptDirs" entry that contains the path scores its
    // length. A directory contains a path only at a path component border. On a tie, the first
    // name in sorted order wins.
    std::shared_ptr<const Host> match(const std::string& script_path) const;

    // Gives the host whose script dirs and scripts are nearest to a script that no host matches,
    // or nullptr. The nearness is the count of leading path parts that the directory of the
    // script shares with a script dir (or with the directory of a script) of the host. "common"
    // gets the shared directory of the best host. On a tie, the first name in sorted order wins.
    std::shared_ptr<const Host> nearest(const std::string& script_path, std::filesystem::path& common) const;

    size_t size() const { return hosts_.size(); }

private:
    std::string dir_;
    bool require_cache_ = true;
    bool loaded_ = false;
    bool forced_ = false; // reload at the next refresh()
    std::filesystem::file_time_type mtime_{};
    std::vector<std::shared_ptr<const Host>> hosts_; // sorted by name
    std::vector<std::shared_ptr<const Host>> skipped_; // valid, but no CMakeCache.txt
};

} // namespace hot::ls
