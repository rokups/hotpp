---
layout: default
title: File system of the host
nav_order: 33
group: Embedding
---

## Virtual file system

A host can give the script imports and the headers from memory. Use this when the files are in a database, in an archive, or in an editor buffer. The default is the disk.

Set the option `fs` to a `std::shared_ptr<hot::FileSystem>` (`include/hot/vfs.h`). The option is in `DefaultBackendOptions`, `ClangFilesOptions` and `JitOptions`. The file system serves these files:

- A script import: `using "x.hot";`.
- A native header: `using "x.h";`.
- A header that a virtual header includes with `#include "..."` or `#include <...>`.

For each directory, the backend asks your file system first. Then it reads the disk. The directories are, in order: the script folder, then the `include_dirs`. A path is `dir + "/" + name`. The folder does not need to exist on the disk. A file on the disk stops the search for that name.

An interface with two functions:

| Function | Meaning |
|---|---|
| `bool read(path, out)` | Put the file text in `out`. Give `false` when there is no such file. |
| `VfsStat stat(path)` | Give `exists` and a `version`. |

The `version` is a number that changes when the file changes. The translator caches the parse of the headers. A new version makes it parse again. The version `0` means "unknown". The translator then reads the file at each load and compares the content. Use a real version when the read is expensive.

**Thread safety:** the file system must be thread safe. The backend can call it from more than one thread.

**Example:**

```cpp
#include "hot/backend.h"
#include "hot/vfs.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>

class MemoryFs final : public hot::FileSystem
{
public:
    bool read(const std::string& path, std::string& out) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = files_.find(path);
        if (it == files_.end())
            return false;
        out = it->second.text;
        return true;
    }
    hot::VfsStat stat(const std::string& path) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = files_.find(path);
        if (it == files_.end())
            return {};
        return {true, it->second.version};
    }
    void set(const std::string& path, std::string text, uint64_t version)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        files_[path] = {std::move(text), version};
    }

private:
    struct File { std::string text; uint64_t version = 0; };
    std::mutex mutex_;
    std::map<std::string, File> files_;
};

auto fs = std::make_shared<MemoryFs>();
fs->set("/game/scripts/stats.h", "inline int base_hp() { return 10; }\n", 1);

hot::DefaultBackendOptions options;
options.fs = fs;
auto backend = hot::make_default_backend(options);
// A script named "/game/scripts/boss.hot" can now use: using "stats.h";
```

To reload, call `fs->set` with a new text and a new version. Then call `load` again.

**Rules:**

- `hot::disk_file_system()` gives the default file system. You can use it as a base for your own.
- System headers (`using <vector>;`) stay on the disk. The header `hot/script.h` stays on the disk.
- The `.so` files, the generated `.cpp` files and the slots file stay on the disk.
- The `CLANG_FILES` backend writes your virtual files to `<work_dir>/vfs/` at each load, because the compiler reads the disk. The backend makes each path absolute and removes the `..` parts first, so a file never goes outside `vfs`.
- The `JIT` backend gives the files to the compiler in memory. It writes nothing.
- A folder in `include_dirs` does not need to exist on the disk. A relative folder or a folder with `..` is allowed.
- Give the host file system the path as the search builds it. For a header with `..` in the path, the file system gets the path with `..` first, then the normal path.
- A virtual header that you remove makes the next load fail with a "`file not found`" error.
