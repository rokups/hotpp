// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "hot/launcher.h"

#include "hot/backend.h"
#include "hot/runtime/loader.h"
#include "hot/runtime/scheduler.h"
#include "hot/runtime/threads.h"

#include <cstdint>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>
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

// One script unit of the program: the entry script or an imported script. The unit name follows
// the rule in doc/module-level.md: the entry script has its absolute path without ".hot" as its
// unit name, and the file of an import unit is the unit name with ".hot".
struct UnitState
{
    std::string path; // the file to read
    uint64_t iface_hash = 0;
    std::vector<std::pair<std::string, uint64_t>> imports;
    std::vector<std::string> dependencies;
};

// The result of a batch build on the worker thread.
struct BuildOutput
{
    bool ok = false;
    std::string error; // the text of the first failure
    std::vector<hot::LoadResult> results; // one module for each unit of the batch
    std::map<std::string, UnitState> units; // the table of all units after the build
    std::vector<std::string> dependencies; // the union of the dependencies of all units
    std::set<std::string> attempted; // the units that the build tried
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

    // The build on the worker thread. One worker runs at a time: "worker.joinable()" is the guard.
    // The worker runs only Backend::load. It writes "build_result" and "build_done" under "mutex"
    // and does not touch the other members. The main thread reads them under "mutex", joins the
    // worker, and then handles the result. All Loader calls stay on the main thread.
    std::thread worker;
    std::mutex mutex;
    bool build_done = false;
    BuildOutput build_result;
    std::set<std::string> build_todo; // the units of the running build
    // The script units. "units" is the table of the live modules. "pending_units" is the table of a
    // pending reload; the commit makes it live. "retry" holds the units of a build that failed or
    // that the loader rejected: the next build includes them again.
    std::string main_unit;
    std::map<std::string, UnitState> units;
    std::map<std::string, UnitState> pending_units;
    std::set<std::string> retry;
    // What the main thread remembers about the running build. Only the main thread uses them.
    std::map<std::string, Stamp> build_pre; // the stamps from before the build read the files
    uint32_t build_generation = 0;          // the generation number of the build
    hot::LoadMode build_mode = hot::LoadMode::Reload;

    // The full restart. Only the main thread uses these members.
    std::function<void()> request_quit;
    // A Fresh build gave a module. The restart starts when "main" returns.
    bool restart_pending = false;
    bool quit_requested = false;  // the launcher called request_quit for the restart
    bool no_quit_logged = false;  // the log has the "no quit hook" line
    std::string restart_reason;   // why the restart is needed
    // A full restart is needed and not yet built. Each build is Fresh until a Fresh build works
    // or the program restarts. A failed Fresh build does not make the change live.
    bool fresh_needed = false;
    // The stamps of the native dependencies that the log reported as changed. One line each stamp.
    std::map<std::string, Stamp> native_reported;
    std::optional<BuildOutput> fresh; // the Fresh build for the restart

    // Waits for the worker. The worker uses the backend, so the join must come before the members
    // go. A build can take seconds, so the destructor can wait for that time.
    ~Impl()
    {
        if (worker.joinable())
            worker.join();
    }
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

// The union of the dependencies of all units.
std::vector<std::string> all_dependencies(const std::map<std::string, UnitState>& units)
{
    std::set<std::string> seen;
    std::vector<std::string> out;
    for (const auto& [name, u] : units)
        for (const std::string& d : u.dependencies)
            if (seen.insert(d).second)
                out.push_back(d);
    return out;
}

// Builds the units in "todo" as one batch (design: modules, step 4). Each unit is its own module. A
// unit that the batch does not know yet (a new import) joins the batch. When the interface hash of a
// unit changes, its importers join the batch too, until no hash changes. "known" is the table of
// the live units. "texts" has the text of files that the caller read before the build (by path):
// the build uses that text, so an edit during the build is a change for the next build. It runs on
// the worker thread and uses only its arguments.
BuildOutput build_batch(hot::Backend& backend, const std::string& script_path, const std::vector<std::string>& include_dirs,
                        const std::string& main_unit, std::map<std::string, UnitState> known, std::set<std::string> todo,
                        uint32_t generation, hot::LoadMode mode, const std::map<std::string, std::string>& texts = {})
{
    BuildOutput out;
    std::map<std::string, hot::LoadResult> built;
    const auto fail = [&](std::string error, const std::vector<std::string>& deps)
    {
        out.error = std::move(error);
        out.units = known;
        out.dependencies = all_dependencies(known);
        for (const std::string& d : deps)
            out.dependencies.push_back(d);
        return out;
    };
    while (!todo.empty())
    {
        const std::string name = *todo.begin();
        todo.erase(todo.begin());
        if (built.count(name))
            continue;
        out.attempted.insert(name);
        const auto old = known.find(name);
        const std::string path = name == main_unit ? script_path
                                 : old != known.end() ? old->second.path
                                                      : resolve_path(script_path, include_dirs, name + ".hot");
        std::string text;
        if (const auto t = texts.find(path); t != texts.end())
            text = t->second;
        else if (!read_file(path, text))
            return fail("cannot read the script '" + path + "'", {});
        hot::LoadResult r = backend.load(hot::ScriptSource{path, std::move(text), {}, name}, generation, mode);
        if (!r)
            return fail(r.error.empty() ? "the script '" + path + "' did not compile" : r.error, r.dependencies);
        const bool changed = old == known.end() || old->second.iface_hash != r.iface_hash;
        known[name] = UnitState{path, r.iface_hash, r.import_hashes, r.dependencies};
        for (const auto& [dep, hash] : r.import_hashes)
            if (!known.count(dep) && !built.count(dep))
                todo.insert(dep); // a new import
        if (changed)
            for (const auto& [unit, u] : known)
                for (const auto& [dep, hash] : u.imports)
                    if (dep == name && !built.count(unit) && unit != name)
                        todo.insert(unit); // an importer of the changed interface
        built.emplace(name, std::move(r));
    }
    for (auto& [name, r] : built)
        out.results.push_back(std::move(r));
    out.units = std::move(known);
    out.dependencies = all_dependencies(out.units);
    out.ok = true;
    return out;
}

// True for a dependency that is not a ".hot" file (a native header). The entry script is a script.
bool is_native(const std::string& script_path, const std::string& path)
{
    if (path == script_path)
        return false;
    return !(path.size() >= 4 && path.compare(path.size() - 4, 4, ".hot") == 0);
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
        impl->main_unit = fs::absolute(script_path, ec).lexically_normal().replace_extension().generic_string();
        BuildOutput built = build_batch(*impl->backend, script_path, options.include_dirs, impl->main_unit, {},
                                        {impl->main_unit}, 1, hot::LoadMode::Reload);
        impl->dependencies = built.dependencies;
        if (!built.ok)
        {
            error = built.error.empty() ? "the script did not compile" : built.error;
            return nullptr;
        }
        std::vector<hot::Module> modules;
        for (hot::LoadResult& r : built.results)
        {
            // Read the pointer before the move of the module.
            if (r.module->unit == impl->main_unit)
                impl->entry_main = r.module->entry_main;
            modules.push_back(std::move(*r.module));
        }
        impl->units = std::move(built.units);
        const hot::ReloadResult loaded = impl->loader.load(std::move(modules));
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
    impl->request_quit = options.request_quit;
    impl->include_dirs = options.include_dirs;
    impl->stamps = adopt_stamps(impl->script_path, impl->include_dirs, pre, impl->dependencies, false);
    impl->seen = impl->stamps;
    return std::unique_ptr<Program>(new Program(std::move(impl)));
}

namespace
{

// The restart of run_main, after "main" returned. It stops the tasks, resets the loader, loads the
// fresh module and takes its "main". It gives false when the restart cannot go on. The caller
// logs nothing more.
// A template, because Program::Impl is private: the compiler deduces the type.
template <class ImplT>
bool restart_program(ImplT& impl)
{
    // The worker cannot run here, because poll starts no build after the restart is pending.
    // Join it anyway and drop its result.
    if (impl.worker.joinable())
    {
        impl.worker.join();
        const std::lock_guard<std::mutex> lock(impl.mutex);
        impl.build_result = BuildOutput{};
        impl.build_done = false;
    }
    // The tasks pin their generation, and Loader::reset fails while a pin exists. Tasks need C++20
    // coroutines: the Scheduler type exists only when this file builds with them.
#if defined(HOT_HAS_TASK)
    hot::scheduler().cancel_all();
#endif
    std::string error;
    if (!impl.loader.reset(error))
    {
        impl.log("[hot] full restart failed: " + (error.empty() ? "the loader cannot reset" : error));
        return false;
    }
    // After the reset the old "main" pointer dangles.
    impl.entry_main = nullptr;
    BuildOutput fresh = std::move(*impl.fresh);
    impl.fresh.reset();
    hot::Module::EntryMainFn entry_main = nullptr;
    std::vector<hot::Module> modules;
    for (hot::LoadResult& r : fresh.results)
    {
        // Read the pointer before the move of the module.
        if (r.module->unit == impl.main_unit)
            entry_main = r.module->entry_main;
        modules.push_back(std::move(*r.module));
    }
    impl.units = std::move(fresh.units);
    impl.pending_units.clear();
    impl.retry.clear();
    const hot::ReloadResult loaded = impl.loader.load(std::move(modules));
    if (!loaded)
    {
        impl.log("[hot] full restart failed: " + (loaded.error.empty() ? "the loader rejected the module" : loaded.error));
        return false;
    }
    impl.entry_main = entry_main;
    impl.active_generation = 1;
    impl.main_generation = 1;
    impl.next_generation = 2;
    impl.pending_generation = 0;
    impl.busy_logged = false;
    impl.dirty = false;
    impl.build_pre.clear();
    impl.build_generation = 0;
    impl.restart_pending = false;
    impl.fresh_needed = false;
    impl.quit_requested = false;
    impl.no_quit_logged = false;
    impl.log("[hot] full restart: " + impl.restart_reason);
    impl.restart_reason.clear();
    return true;
}

} // namespace

int run_main(Program& program, const std::vector<std::string>& args)
{
    Program::Impl& impl = *program.impl_;
    std::vector<const char*> argv;
    argv.push_back(impl.script_path.c_str());
    for (const std::string& a : args)
        argv.push_back(a.c_str());
    for (;;)
    {
        if (!impl.entry_main)
        {
            impl.log("the script has no 'main'");
            return 2;
        }
        int code = 1;
        // The frame of "main" stays on the stack during the whole run, and "main" calls the code of
        // its generation directly (hot_fn_main). Loader::frame_boundary does not look at the native
        // call frames. After a reload, the generation of "main" is retired, and frame_boundary
        // could unload its library while "main" still runs in it. A pin keeps a generation loaded:
        // frame_boundary skips a pinned generation. The pin takes the generation number.
        // A reload does not refresh entry_main, because "main" does not run again. Only the
        // restart below takes a new one. The pin ends before the restart.
        try
        {
            const PinGuard pin(impl.main_generation);
            code = impl.entry_main(static_cast<int32_t>(argv.size()), argv.data());
        }
        catch (const std::exception& e)
        {
            impl.log(std::string("the script threw an exception: ") + e.what());
            return 1;
        }
        catch (...)
        {
            impl.log("the script threw an unknown exception");
            return 1;
        }
        // The restart needs the fresh build and the quit request of the launcher. Without a quit
        // request, "main" ended by itself (for example the user closed the window): do not restart.
        if (!impl.restart_pending || !impl.quit_requested)
            return code;
        // The restart keeps the stamps of the Fresh build. An edit that came during the wait is a
        // change for the next poll.
        try
        {
            if (!restart_program(impl))
                return 1;
        }
        catch (const std::exception& e)
        {
            impl.log(std::string("[hot] full restart failed: ") + e.what());
            return 1;
        }
        catch (...)
        {
            impl.log("[hot] full restart failed: unknown exception");
            return 1;
        }
    }
}

bool Program::restart_pending() const
{
    return impl_->restart_pending;
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

    // 0. A restart is pending: a Fresh build gave the new module, and the program must quit. The
    // launcher takes no new file change until the restart ends.
    if (impl.restart_pending)
    {
        impl.loader.frame_boundary();
        return PollStatus::RestartPending;
    }

    // 1. A build runs on the worker thread. The scan waits until the result is handled.
    // A finished build is joined and handled below.
    bool have_result = false;
    if (impl.worker.joinable())
    {
        {
            const std::lock_guard<std::mutex> lock(impl.mutex);
            have_result = impl.build_done;
        }
        if (!have_result)
        {
            impl.loader.frame_boundary();
            return PollStatus::Building;
        }
    }

    // 2. The scan. "seen" holds the stamps of the last scan: a difference from "seen" restarts the
    // debounce time. A difference from the stamps of the last build makes the program dirty.
    if (!have_result && (!impl.scanned_once || elapsed_ms(impl.last_scan) >= static_cast<long long>(impl.poll_interval_ms)))
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
        // A header that is back at its build stamp ends the pause. Its next change gets a log line.
        for (auto it = impl.native_reported.begin(); it != impl.native_reported.end();)
        {
            const auto now_stamp = current.find(it->first);
            const auto build_stamp = impl.stamps.find(it->first);
            if (now_stamp == current.end() || build_stamp == impl.stamps.end() || now_stamp->second == build_stamp->second)
                it = impl.native_reported.erase(it);
            else
                ++it;
        }
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
            impl.units = std::move(impl.pending_units);
            impl.pending_units.clear();
            impl.retry.clear();
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
        for (const auto& [name, u] : impl.pending_units)
            impl.retry.insert(name);
        impl.pending_units.clear();
        impl.log("[hot] reload failed: " + (committed.error.empty() ? "the commit failed" : committed.error));
        return PollStatus::Failed;
    };

    // Takes the stamps of a build. "pre" holds the stamps from before the build read the files.
    const auto take_stamps = [&](const std::map<std::string, Stamp>& pre, const std::vector<std::string>& dependencies,
                                 bool keep_known)
    {
        impl.stamps = adopt_stamps(impl.script_path, impl.include_dirs, pre, dependencies, keep_known);
        impl.seen = impl.stamps;
    };

    // Starts the build on the worker thread. "pre" holds the stamps from before the build reads the
    // files. A Fresh build has the generation 1. It gives Building, or Failed with a log line.
    const auto start_build = [&](hot::LoadMode mode, std::map<std::string, Stamp> pre) -> PollStatus
    {
        const char* const prefix = mode == hot::LoadMode::Fresh ? "[hot] full restart build failed: " : "[hot] reload failed: ";
        std::string text;
        if (!read_file(impl.script_path, text))
        {
            impl.log(std::string(prefix) + "cannot read the script '" + impl.script_path + "'");
            // Take the new stamps. The missing file is a change only once.
            take_stamps(pre, {}, true);
            return PollStatus::Failed;
        }
        // The units to build. A Fresh build starts from the entry script and builds all units. A
        // Reload builds the units whose file changed, and the units of an earlier failure. A
        // changed script that is not a unit makes the build include all units.
        std::set<std::string> todo;
        std::map<std::string, UnitState> known;
        if (mode == hot::LoadMode::Fresh)
            todo.insert(impl.main_unit);
        else
        {
            known = impl.units;
            todo = impl.retry;
            for (const auto& [path, stamp] : pre)
            {
                const auto old = impl.stamps.find(path);
                if (old != impl.stamps.end() && old->second == stamp)
                    continue;
                bool mapped = false;
                for (const auto& [name, u] : known)
                    if (u.path == path)
                        todo.insert(name), mapped = true;
                if (!mapped && !is_native(impl.script_path, path))
                    for (const auto& [name, u] : known)
                        todo.insert(name);
            }
            if (todo.empty())
                todo.insert(impl.main_unit);
        }
        impl.build_todo = todo;
        // The texts of the entry script and of the known units, read now (see build_batch).
        std::map<std::string, std::string> texts;
        texts[impl.script_path] = std::move(text);
        for (const auto& [name, u] : impl.units)
            if (std::string t; !texts.count(u.path) && read_file(u.path, t))
                texts[u.path] = std::move(t);
        // The worker gets copies of the values and a pointer to the Impl. The Impl is on the
        // heap, and its destructor joins the worker, so the pointer stays valid.
        impl.build_pre = std::move(pre);
        impl.build_generation = mode == hot::LoadMode::Fresh ? 1 : impl.next_generation;
        impl.build_mode = mode;
        impl.build_done = false;
        try
        {
            impl.worker = std::thread(
                [state = &impl, backend = impl.backend.get(), script_path = impl.script_path,
                 include_dirs = impl.include_dirs, main_unit = impl.main_unit, known = std::move(known),
                 todo = std::move(todo), texts = std::move(texts), generation = impl.build_generation,
                 mode = impl.build_mode]()
                {
                    BuildOutput result;
                    try
                    {
                        result = build_batch(*backend, script_path, include_dirs, main_unit, known, todo, generation, mode, texts);
                    }
                    catch (const std::exception& e)
                    {
                        result = BuildOutput{};
                        result.error = e.what();
                    }
                    catch (...)
                    {
                        result = BuildOutput{};
                        result.error = "unknown exception";
                    }
                    const std::lock_guard<std::mutex> lock(state->mutex);
                    state->build_result = std::move(result);
                    state->build_done = true;
                });
            return PollStatus::Building;
        }
        catch (const std::system_error& e)
        {
            // The thread did not start. Treat it as a failed build.
            impl.log(std::string(prefix) + "cannot start the build thread: " + e.what());
            take_stamps(impl.build_pre, {}, true);
            impl.build_pre.clear();
            return PollStatus::Failed;
        }
    };

    // Starts the Fresh build of a full restart. "reason" says why the restart is needed.
    const auto start_restart = [&](const std::string& reason) -> PollStatus
    {
        impl.fresh_needed = true;
        impl.restart_reason = reason;
        impl.log("[hot] full restart needed: " + reason);
        // The stamps from before the second build reads the files.
        return start_build(hot::LoadMode::Fresh, stat_known(impl.stamps));
    };

    PollStatus status = PollStatus::Idle;
    if (have_result)
    {
        // 2a. The worker finished. Join it and handle the result. The worker is gone, so the
        // result and the remembered values belong to this thread again.
        impl.worker.join();
        BuildOutput built;
        {
            const std::lock_guard<std::mutex> lock(impl.mutex);
            built = std::move(impl.build_result);
            impl.build_result = BuildOutput{};
            impl.build_done = false;
        }
        const std::set<std::string> todo = std::move(impl.build_todo);
        impl.build_todo.clear();
        const std::map<std::string, Stamp> pre = std::move(impl.build_pre);
        impl.build_pre.clear();
        const uint32_t generation = impl.build_generation;
        const hot::LoadMode mode = impl.build_mode;
        const char* const prefix = mode == hot::LoadMode::Fresh ? "[hot] full restart build failed: " : "[hot] reload failed: ";
        std::string restart_needed; // not empty: a Reload build shows that a full restart is needed
        status = PollStatus::Failed;
        try
        {
            if (mode == hot::LoadMode::Fresh)
            {
                if (!built.ok)
                {
                    // "fresh_needed" stays: the next build is Fresh again.
                    take_stamps(pre, built.dependencies, true);
                    impl.log(std::string(prefix) + (built.error.empty() ? "the script did not compile" : built.error));
                }
                else
                {
                    impl.dependencies = built.dependencies;
                    take_stamps(pre, impl.dependencies, false);
                    impl.fresh = std::move(built);
                    impl.restart_pending = true;
                    impl.fresh_needed = false;
                    status = PollStatus::RestartPending;
                    if (impl.request_quit)
                    {
                        impl.quit_requested = true;
                        impl.log("[hot] full restart: waiting for the program to quit");
                        try
                        {
                            impl.request_quit();
                        }
                        catch (const std::exception& e)
                        {
                            impl.quit_requested = false;
                            impl.log(std::string("[hot] full restart: the quit hook threw an exception: ") + e.what());
                        }
                        catch (...)
                        {
                            impl.quit_requested = false;
                            impl.log("[hot] full restart: the quit hook threw an unknown exception");
                        }
                    }
                    else if (!impl.no_quit_logged)
                    {
                        impl.no_quit_logged = true;
                        impl.log("[hot] restart needed, but the engine has no quit hook: the old code goes on");
                    }
                }
            }
            else if (!built.ok)
            {
                take_stamps(pre, built.dependencies, true);
                impl.retry.insert(todo.begin(), todo.end());
                impl.retry.insert(built.attempted.begin(), built.attempted.end());
                impl.log(std::string(prefix) + (built.error.empty() ? "the script did not compile" : built.error));
            }
            else
            {
                impl.dependencies = built.dependencies;
                take_stamps(pre, impl.dependencies, false);
                std::map<std::string, UnitState> next_units = std::move(built.units);
                const hot::ReloadResult requested = impl.loader.request_reload(std::move(built.results));
                if (requested.status != hot::ReloadStatus::Pending)
                    impl.retry.insert(built.attempted.begin(), built.attempted.end());
                if (requested.status == hot::ReloadStatus::Pending)
                {
                    impl.pending_units = std::move(next_units);
                    impl.pending_generation = generation;
                    status = finish_commit();
                }
                else if (requested.status == hot::ReloadStatus::FullReloadRequired)
                {
                    restart_needed = requested.error.empty() ? "the change needs a full restart" : requested.error;
                }
                else
                {
                    impl.log(std::string(prefix) +
                             (requested.error.empty() ? "the loader rejected the module" : requested.error));
                }
            }
            if (!restart_needed.empty())
                status = start_restart(restart_needed);
        }
        catch (const std::exception& e)
        {
            impl.log(std::string(prefix) + e.what());
            take_stamps(pre, {}, true);
            status = PollStatus::Failed;
        }
        catch (...)
        {
            impl.log(std::string(prefix) + "unknown exception");
            take_stamps(pre, {}, true);
            status = PollStatus::Failed;
        }
    }
    else if (impl.loader.reload_pending())
    {
        // 2b. A reload from an earlier Busy result. The dirty flag keeps its value, so a new edit
        // builds in a later call.
        status = finish_commit();
    }
    else if (impl.dirty && elapsed_ms(impl.change_seen) >= static_cast<long long>(impl.debounce_ms))
    {
        // 2c. The build, after the debounce time.
        impl.dirty = false;
        bool paused = false;
        // The stamps from before the build reads the files.
        std::map<std::string, Stamp> pre = stat_known(impl.stamps);
        // A changed file that is not a ".hot" file (a native header) pauses the reload. Such a change
        // usually needs a rebuild of the engine, and a build against the new header can give a
        // layout mismatch. No build starts, also when a ".hot" file changed too. The entry script
        // always counts as a script. The log has one line for each new stamp of the header.
        for (const auto& entry : pre)
        {
            const std::string& path = entry.first;
            if (is_native(impl.script_path, path) && !(entry.second == impl.stamps[path]))
            {
                const auto reported = impl.native_reported.find(path);
                if (reported == impl.native_reported.end() || !(reported->second == entry.second))
                {
                    impl.native_reported[path] = entry.second;
                    impl.log("[hot] native dependency changed: " + path + ". Rebuild the native dependencies. Reload is paused.");
                    status = PollStatus::Failed;
                }
                paused = true;
            }
        }
        // After an earlier restart trigger, each build stays Fresh until a Fresh build works.
        if (!paused && impl.fresh_needed)
        {
            impl.log("[hot] full restart needed: " + impl.restart_reason);
            status = start_build(hot::LoadMode::Fresh, std::move(pre));
        }
        else if (!paused)
            status = start_build(hot::LoadMode::Reload, std::move(pre));
    }

    // 3. Unload the retired generations. This also runs after a commit of this call.
    impl.loader.frame_boundary();
    return status;
}

} // namespace hot::launcher
