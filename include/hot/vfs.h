// A file system that a host supplies for the imports of a script. The translator reads a
// script import ("using "x.hot";") and a native header ("using "x.h";") from this file system
// first. It reads the disk when the host file system has no such file.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace hot
{

struct VfsStat
{
    bool exists = false;
    // A number that changes when the content changes. 0 means unknown: the translator then
    // hashes the content.
    uint64_t version = 0;
};

// The host file system. The paths are the same strings that the translator builds for the
// disk: "dir/relative". An implementation must be thread safe. The translator can call it from
// more than one thread at the same time.
class FileSystem
{
public:
    FileSystem() : id_(next_id()) {}
    // A copy is another file system, so it gets a new id.
    FileSystem(const FileSystem&) : id_(next_id()) {}
    FileSystem& operator=(const FileSystem&) { return *this; }
    virtual ~FileSystem() = default;
    // A number that no other file system of this process has, also not a freed one. The
    // translator cache uses it to tell two file systems apart.
    uint64_t id() const { return id_; }
    // Reads the file into "out". Gives false when there is no such file.
    virtual bool read(const std::string& path, std::string& out) = 0;
    virtual VfsStat stat(const std::string& path) = 0;

private:
    // Defined in the translator library, so that every module shares one counter.
    static uint64_t next_id();
    uint64_t id_;
};

// The file system of the disk. It is the default. The version is mtime_ns ^ size.
std::shared_ptr<FileSystem> disk_file_system();

} // namespace hot
