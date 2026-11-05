// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "hot/launcher.h"

#include "hot/backend.h"
#include "hot/runtime/loader.h"
#include "hot/runtime/threads.h"

#include <cstdint>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

namespace hot::launcher
{

namespace fs = std::filesystem;

// The state of a file for the change check: the write time and the size.
struct Stamp
{
    fs::file_time_type write_time{};
    std::uintmax_t size = 0;
    bool exists = false;
    bool operator==(const Stamp& o) const
    {
        return exists == o.exists && write_time == o.write_time && size == o.size;
    }
};

struct Program::Impl
{
    // The loader is declared after the backend, so it goes first. The libraries unload before
    // the backend removes its work directory.
    std::unique_ptr<hot::Backend> backend;
    hot::Loader loader;
    std::vector<std::string> dependencies;
    std::string script_path;
    hot::Module::EntryMainFn entry_main = nullptr;
    std::function<void(const std::string&)> log;

    // The state of poll. All of it belongs to the main thread.
    using Clock = std::chrono::steady_clock;
    std::vector<std::string> include_dirs;
    // The stamps of the last build. The key is the resolved path. A file that differs from its
    // stamp needs a build.
    std::map<std::string, Stamp> stamps;
    // The stamps of the last scan. A change of "seen" restarts the debounce time.
    std::map<std::string, Stamp> seen;
    Clock::time_point last_scan{};
    Clock::time_point change_seen{};
    bool scanned_once = false;
    bool dirty = false;
    unsigned poll_interval_ms = 200;
    unsigned debounce_ms = 100;
    uint32_t next_generation = 2;    // the number of the next build
    uint32_t pending_generation = 0; // the number of a pending reload, 0: none
    bool busy_logged = false;        // the log has the Busy line for the pending reload
    uint32_t active_generation = 1; // the generation that new calls reach
    uint32_t main_generation = 1;   // the generation that holds the frame of "main"
};

Program::Program(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Program::~Program() = default;
Program::Program(Program&&) noexcept = default;
Program& Program::operator=(Program&&) noexcept = default;

const std::vector<std::string>& Program::dependencies() const
{
    return impl_->dependencies;
}

const std::string& Program::script_path() const
{
    return impl_->script_path;
}

namespace
{

std::function<void(const std::string&)> log_of(const Options& options)
{
    if (options.log)
        return options.log;
    return [](const std::string& text) { std::fprintf(stderr, "%s\n", text.c_str()); };
}

// FNV-1a: the same hash on each platform and each standard library.
std::string hash_text(const std::string& text)
{
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char c : text)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buffer[17];
    std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(h));
    return buffer;
}

bool read_file(const std::string& path, std::string& text)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
    return true;
}

Stamp stat_file(const std::string& path)
{
    Stamp stamp;
    std::error_code ec;
    const fs::file_time_type time = fs::last_write_time(path, ec);
    if (ec)
        return stamp;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec)
        return stamp;
    stamp.write_time = time;
    stamp.size = size;
    stamp.exists = true;
    return stamp;
}

// A relative dependency path is relative to the folder of the entry script, or to an include
// directory. Use the first candidate that exists. When none exists, use the path in the folder of
// the entry script, and its stamp says "not exists".
std::string resolve_path(const std::string& script_path, const std::vector<std::string>& include_dirs,
                         const std::string& dependency)
{
    const fs::path path(dependency);
    if (path.is_absolute())
        return dependency;
    const std::string first = (fs::path(script_path).parent_path() / path).string();
    std::error_code ec;
    if (fs::exists(first, ec))
        return first;
    for (const std::string& dir : include_dirs)
    {
        const std::string candidate = (fs::path(dir) / path).string();
        if (fs::exists(candidate, ec))
            return candidate;
    }
    return first;
}

// The stamps of the known files (the entry script and the dependencies). The caller takes them
// BEFORE a build reads the files. A file that changes during the build then differs from its
// stamp at the next scan.
std::map<std::string, Stamp> stat_known(const std::map<std::string, Stamp>& known)
{
    std::map<std::string, Stamp> now;
    for (const auto& entry : known)
        now[entry.first] = stat_file(entry.first);
    return now;
}

// Takes the stamps after a build. "pre" holds the stamps from before the build. A known path keeps
// its pre-build stamp. Only a new dependency gets a stamp now. "keep_known" keeps all paths of
// "pre": a failed build can give an incomplete list of dependencies.
std::map<std::string, Stamp> adopt_stamps(const std::string& script_path, const std::vector<std::string>& include_dirs,
                                          const std::map<std::string, Stamp>& pre,
                                          const std::vector<std::string>& dependencies, bool keep_known)
{
    std::map<std::string, Stamp> next;
    if (keep_known)
        next = pre;
    else if (const auto it = pre.find(script_path); it != pre.end())
        next[script_path] = it->second;
    for (const std::string& d : dependencies)
    {
        const std::string path = resolve_path(script_path, include_dirs, d);
        const auto it = pre.find(path);
        next[path] = it != pre.end() ? it->second : stat_file(path);
    }
    return next;
}

// Pins a generation while the object lives, also when an exception leaves the scope.
class PinGuard
{
public:
    explicit PinGuard(uint32_t generation) : generation_(generation) { hot::pin(generation_); }
    ~PinGuard() { hot::unpin(generation_); }
    PinGuard(const PinGuard&) = delete;
    PinGuard& operator=(const PinGuard&) = delete;

private:
    uint32_t generation_;
};

} // namespace

std::string default_cache_dir(const std::string& script_path)
{
    std::error_code ec;
    const fs::path absolute = fs::weakly_canonical(fs::absolute(script_path, ec), ec);
    fs::path base;
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
        base = fs::path(local);
    else
        base = fs::temp_directory_path(ec);
#else
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    if (xdg && *xdg)
        base = fs::path(xdg);
    else if (home && *home)
        base = fs::path(home) / ".cache";
    else
        base = fs::temp_directory_path(ec);
#endif
    const fs::path dir = base / "hot" / hash_text(absolute.generic_string());
    fs::create_directories(dir, ec);
    return dir.string();
}

std::unique_ptr<Program> load(const Options& options, const std::string& script_path, std::string& error)
{
    error.clear();
    std::string text;
    if (!read_file(script_path, text))
    {
        error = "cannot read the script '" + script_path + "'";
        return nullptr;
    }
    hot::DefaultBackendOptions backend_options;
    backend_options.include_dirs = options.include_dirs;
    backend_options.extra_flags = options.extra_flags;
    backend_options.std_version = options.std_version;
    if (options.backend.empty())
        backend_options.kind = hot::BackendKind::Auto;
    else if (options.backend == "files")
        backend_options.kind = hot::BackendKind::ClangFiles;
    else if (options.backend == "jit")
        backend_options.kind = hot::BackendKind::Jit;
    else
    {
        error = "unknown backend '" + options.backend + "'. Use 'files' or 'jit'";
        return nullptr;
    }
    // The stamp of the entry script, taken before the build reads it.
    const std::map<std::string, Stamp> pre = {{script_path, stat_file(script_path)}};
    std::unique_ptr<Program::Impl> impl;
    try
    {
        impl = std::make_unique<Program::Impl>();
        backend_options.work_dir = options.work_dir.empty() ? default_cache_dir(script_path) : options.work_dir;
        impl->backend = hot::make_default_backend(backend_options);
        std::error_code ec;
        const std::string name = fs::absolute(script_path, ec).string();
        hot::LoadResult result = impl->backend->load(hot::ScriptSource{name, text, {}}, 1);
        impl->dependencies = result.dependencies;
        if (!result)
        {
            error = result.error.empty() ? "the script did not compile" : result.error;
            return nullptr;
        }
        // Read the pointer before the move of the module.
        impl->entry_main = result.module->entry_main;
        const hot::ReloadResult loaded = impl->loader.load(std::move(*result.module));
        if (!loaded)
        {
            error = loaded.error.empty() ? "the loader rejected the module" : loaded.error;
            return nullptr;
        }
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return nullptr;
    }
    impl->script_path = script_path;
    impl->log = log_of(options);
    impl->poll_interval_ms = options.poll_interval_ms;
    impl->debounce_ms = options.debounce_ms;
    impl->include_dirs = options.include_dirs;
    impl->stamps = adopt_stamps(impl->script_path, impl->include_dirs, pre, impl->dependencies, false);
    impl->seen = impl->stamps;
    return std::unique_ptr<Program>(new Program(std::move(impl)));
}

int run_main(Program& program, const std::vector<std::string>& args)
{
    Program::Impl& impl = *program.impl_;
    if (!impl.entry_main)
    {
        impl.log("the script has no 'main'");
        return 2;
    }
    std::vector<const char*> argv;
    argv.push_back(impl.script_path.c_str());
    for (const std::string& a : args)
        argv.push_back(a.c_str());
    // The frame of "main" stays on the stack during the whole run, and "main" calls the code of
    // its generation directly (hot_fn_main). Loader::frame_boundary does not look at the native
    // call frames. After a reload, the generation of "main" is retired, and frame_boundary could
    // unload its library while "main" still runs in it. A pin keeps a generation loaded:
    // frame_boundary skips a pinned generation. The pin takes the generation number.
    // entry_main is not refreshed on a reload, because "main" does not run again.
    try
    {
        const PinGuard pin(impl.main_generation);
        return impl.entry_main(static_cast<int32_t>(argv.size()), argv.data());
    }
    catch (const std::exception& e)
    {
        impl.log(std::string("the script threw an exception: ") + e.what());
    }
    catch (...)
    {
        impl.log("the script threw an unknown exception");
    }
    return 1;
}

uint32_t Program::generation() const
{
    return impl_->active_generation;
}

PollStatus Program::poll()
{
    Impl& impl = *impl_;
    const Impl::Clock::time_point now = Impl::Clock::now();
    const auto elapsed_ms = [&](Impl::Clock::time_point since)
    { return std::chrono::duration_cast<std::chrono::milliseconds>(now - since).count(); };

    // 1. The scan. "seen" holds the stamps of the last scan: a difference from "seen" restarts the
    // debounce time. A difference from the stamps of the last build makes the program dirty.
    if (!impl.scanned_once || elapsed_ms(impl.last_scan) >= static_cast<long long>(impl.poll_interval_ms))
    {
        impl.scanned_once = true;
        impl.last_scan = now;
        const std::map<std::string, Stamp> current = stat_known(impl.stamps);
        if (!(current == impl.seen))
        {
            impl.seen = current;
            impl.change_seen = now;
        }
        impl.dirty = !(current == impl.stamps);
    }

    // Commits the pending reload. A reload stays pending only after a Busy result: the commit
    // cannot run now. With StopTheWorld the commit has no Busy result. With EntryCounters it gives
    // Busy when the thread is inside script code. The loader has no call that drops a pending
    // reload, so each later request_reload would give Busy. Because of this, poll commits first.
    // "next_generation" grows only when a generation was committed. A reload that is dropped
    // leaves the number free, and the next build reuses it.
    const auto finish_commit = [&]() -> PollStatus
    {
        const hot::ReloadResult committed = impl.loader.commit_pending();
        if (committed.status == hot::ReloadStatus::Applied)
        {
            impl.active_generation = impl.pending_generation;
            impl.next_generation = impl.pending_generation + 1;
            impl.pending_generation = 0;
            impl.busy_logged = false;
            impl.log("[hot] reload applied: generation " + std::to_string(impl.active_generation));
            return PollStatus::Applied;
        }
        if (impl.loader.reload_pending())
        {
            // Busy: the reload stays pending. A later poll commits it. In EntryCounters mode, a
            // poll() call from script code can see Busy. The log has one line for each reload.
            if (!impl.busy_logged)
            {
                impl.busy_logged = true;
                impl.log("[hot] reload pending: the commit is busy");
            }
            return PollStatus::Idle;
        }
        impl.pending_generation = 0;
        impl.busy_logged = false;
        impl.log("[hot] reload failed: " + (committed.error.empty() ? "the commit failed" : committed.error));
        return PollStatus::Failed;
    };

    PollStatus status = PollStatus::Idle;
    if (impl.loader.reload_pending())
    {
        // 2a. A reload from an earlier Busy result. The dirty flag keeps its value, so a new edit
        // builds in a later call.
        status = finish_commit();
    }
    else if (impl.dirty && elapsed_ms(impl.change_seen) >= static_cast<long long>(impl.debounce_ms))
    {
        // 2b. The build, after the debounce time.
        impl.dirty = false;
        status = PollStatus::Failed;
        // The stamps from before the build reads the files.
        const std::map<std::string, Stamp> pre = stat_known(impl.stamps);
        const auto take_stamps = [&](const std::vector<std::string>& dependencies, bool keep_known)
        {
            impl.stamps = adopt_stamps(impl.script_path, impl.include_dirs, pre, dependencies, keep_known);
            impl.seen = impl.stamps;
        };
        std::string text;
        if (!read_file(impl.script_path, text))
        {
            impl.log("[hot] reload failed: cannot read the script '" + impl.script_path + "'");
            // Take the new stamps. The missing file is a change only once.
            take_stamps({}, true);
        }
        else
        {
            try
            {
                std::error_code ec;
                const std::string name = fs::absolute(impl.script_path, ec).string();
                hot::LoadResult built =
                    impl.backend->load(hot::ScriptSource{name, text, {}}, impl.next_generation, hot::LoadMode::Reload);
                if (!built)
                {
                    take_stamps(built.dependencies, true);
                    impl.log("[hot] reload failed: " + (built.error.empty() ? "the script did not compile" : built.error));
                }
                else
                {
                    impl.dependencies = built.dependencies;
                    take_stamps(impl.dependencies, false);
                    const hot::ReloadResult requested = impl.loader.request_reload(std::move(built));
                    if (requested.status == hot::ReloadStatus::Pending)
                    {
                        impl.pending_generation = impl.next_generation;
                        status = finish_commit();
                    }
                    else if (requested.status == hot::ReloadStatus::FullReloadRequired)
                    {
                        impl.log("[hot] full restart needed: " + requested.error);
                    }
                    else
                    {
                        impl.log("[hot] reload failed: " +
                                 (requested.error.empty() ? "the loader rejected the module" : requested.error));
                    }
                }
            }
            catch (const std::exception& e)
            {
                impl.log(std::string("[hot] reload failed: ") + e.what());
                take_stamps({}, true);
            }
            catch (...)
            {
                impl.log("[hot] reload failed: unknown exception");
                take_stamps({}, true);
            }
        }
    }

    // 3. Unload the retired generations. This also runs after a commit of this call.
    impl.loader.frame_boundary();
    return status;
}

} // namespace hot::launcher
