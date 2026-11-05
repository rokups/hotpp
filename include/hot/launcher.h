// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The launcher: it compiles an entry script with its imports, loads it, and runs its "main".
//
// The library does not ship a runner program. Each project builds its own runner: it links its
// engine and the library hot_launcher, calls load, and then calls run_main. Usually the "main"
// of the script calls native engine code that runs a blocking loop. The launcher has no frame
// loop. The program ends when "main" returns.
//
// The runner executable must export its symbols (CMake: ENABLE_EXPORTS ON, hot_export_host),
// because the script module resolves the symbols of the host from it.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hot::launcher
{

// The result of Program::poll.
//   Idle:           nothing happened.
//   Building:       a build runs (reserved for the worker thread of a later step).
//   Applied:        a reload was applied. The new code runs from now.
//   Failed:         the build or the reload failed. The old code keeps running.
//   RestartPending: a full restart is needed (reserved for a later step).
enum class PollStatus
{
    Idle,
    Building,
    Applied,
    Failed,
    RestartPending
};

struct Options
{
    std::vector<std::string> include_dirs; // for the imports and the native headers
    std::vector<std::string> extra_flags;  // added to the compile command
    std::string std_version;               // for example "c++20". Empty: the standard of the host
    std::string backend;                   // "" (the default choice), "files" or "jit"
    std::string work_dir;                  // empty: default_cache_dir(script_path)
    // Receives the diagnostics and the exception messages. Empty: write to stderr.
    std::function<void(const std::string&)> log;
    // Program::poll scans the files at most once in this time.  0: scan on each call.
    unsigned poll_interval_ms = 200;
    // Program::poll waits this time after the last seen change before it builds. 0: build at once.
    unsigned debounce_ms = 100;
    // Reserved for the full restart of a later step. The launcher does not use it yet.
    std::function<void()> request_quit;
};

// The loaded program. It owns the backend, the loader and the loaded generation. The library
// of the script stays loaded while the object lives. It can move. It cannot copy.
class Program
{
public:
    ~Program();
    Program(Program&&) noexcept;
    Program& operator=(Program&&) noexcept;
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;

    // The files that the build read, other than the entry script: the native headers and the
    // imported scripts. The later reload milestone watches them.
    const std::vector<std::string>& dependencies() const;
    // The path of the entry script, as the caller gave it.
    const std::string& script_path() const;

    // Applies the edits of the entry script and its dependencies to the running program.
    // Call it on the main thread, once for each frame, at the frame boundary. A scan finds a
    // changed file (a different write time or size, or a missing file). Then poll builds the new
    // generation on the calling thread and commits it. Each call also runs Loader::frame_boundary.
    // A failed build keeps the old code and gives Failed. It does not try again until the next
    // file change.
    PollStatus poll();
    // The number of the active generation. It is 1 after load and grows with each applied reload.
    uint32_t generation() const;

private:
    struct Impl;
    explicit Program(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend std::unique_ptr<Program> load(const Options&, const std::string&, std::string&);
    friend int run_main(Program&, const std::vector<std::string>&);
};

// Compiles the entry script with all imports and loads the module. On an error, the result is
// null and "error" holds the diagnostics. Relative import paths are relative to the folder of
// the entry script.
std::unique_ptr<Program> load(const Options& options, const std::string& script_path, std::string& error);

// Calls the "main" of the script with argc and argv. argv[0] is the script path, and then come
// "args". Returns the result of "main" (0 for a "void main"). Returns 1 when the script throws a
// C++ exception: the message goes to Options::log. Returns 2 when the script has no "main".
int run_main(Program& program, const std::vector<std::string>& args);

// The default work directory of a script: "$XDG_CACHE_HOME/hot/<hash>", or "~/.cache/hot/<hash>"
// on Linux, and "%LOCALAPPDATA%\hot\<hash>" on Windows. The hash is the hash of the absolute path
// of the script. The function creates the directory.
std::string default_cache_dir(const std::string& script_path);

} // namespace hot::launcher
