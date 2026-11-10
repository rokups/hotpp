// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// make_default_backend(): the backend that the program chooses at run time.
//
// CMake defines HOT_WITH_CLANG_FILES and HOT_WITH_JIT for the backends that the build has.
// BackendKind::Auto reads the environment variable HOT_BACKEND ("jit" or "files").
// A list value uses "|" between the items.
#include "hot/backend.h"
#include "hot/paths.h"
#include "hot/runtime/process.h"
#include "hot/scratch.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef _WIN32
    #include <signal.h>
    #include <sys/types.h>
#endif
#if defined(HOT_WITH_CLANG_FILES)
    #include "hot/clang_files_backend.h"
#endif
#if defined(HOT_WITH_JIT)
    #include "hot/jit_backend.h"
#endif
#if !defined(HOT_WITH_CLANG_FILES) && !defined(HOT_WITH_JIT)
    #error "Define HOT_WITH_CLANG_FILES or HOT_WITH_JIT."
#endif

namespace hot
{

namespace
{

constexpr bool kWithClangFiles =
#if defined(HOT_WITH_CLANG_FILES)
    true;
#else
    false;
#endif

constexpr bool kWithJit =
#if defined(HOT_WITH_JIT)
    true;
#else
    false;
#endif

// The kind that make_default_backend makes. It never returns Auto.
BackendKind resolve_kind(BackendKind kind)
{
    if (kind != BackendKind::Auto)
        return kind;
    const char* env = std::getenv("HOT_BACKEND");
    if (env && *env)
    {
        std::string value = env;
        for (char& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (value == "jit")
            return BackendKind::Jit;
        // "clang_files" and "clang-files" are old spellings of "files".
        if (value == "files" || value == "clang_files" || value == "clang-files")
            return BackendKind::ClangFiles;
        throw std::runtime_error("The environment variable HOT_BACKEND is \"" + std::string(env) +
                                 "\". Use \"jit\" or \"files\".");
    }
    return kWithClangFiles ? BackendKind::ClangFiles : BackendKind::Jit;
}

#if defined(HOT_WITH_CLANG_FILES)
#ifndef _WIN32
// True when a process with this id does not exist. The call kill(pid, 0) sends no signal: it is
// only a test. Any other result (also EPERM) means the process can exist.
bool process_is_gone(long long pid)
{
    if (pid <= 0 || pid > 2147483647LL)
        return false;
    return ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH;
}
#endif

// Removes the directories "p<pid>" of "base" whose process does not run any more. The other
// names stay. A running process is not a candidate. Windows: no cleanup.
void remove_stale_work_dirs(const std::filesystem::path& base)
{
#ifndef _WIN32
    std::error_code ec;
    const long long own = static_cast<long long>(hot::detail::process_id());
    std::vector<std::filesystem::path> stale;
    for (std::filesystem::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code type_ec;
        if (!it->is_directory(type_ec))
            continue;
        const std::string name = it->path().filename().string();
        if (name.size() < 2 || name.size() > 11 || name[0] != 'p')
            continue;
        if (!std::all_of(name.begin() + 1, name.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
            continue;
        const long long pid = std::stoll(name.substr(1));
        if (pid != own && process_is_gone(pid))
            stale.push_back(it->path());
    }
    for (const std::filesystem::path& dir : stale)
        std::filesystem::remove_all(dir, ec);
#else
    (void)base;
#endif
}

std::unique_ptr<Backend> make_clang_files(const DefaultBackendOptions& options)
{
    ClangFilesOptions o;
    const DefaultPaths paths = default_paths();
    o.compiler = paths.compiler;
    o.include_dirs = paths.include_dirs;
    o.extra_flags = paths.cxxflags;
    o.include_dirs.insert(o.include_dirs.end(), options.include_dirs.begin(), options.include_dirs.end());
    o.extra_flags.insert(o.extra_flags.end(), options.extra_flags.begin(), options.extra_flags.end());
    o.resource_dir = paths.resource_dir;
    o.fs = options.fs;
    o.preludes = options.preludes;
    o.std_version = options.std_version;
    o.target_triple = paths.target_triple;
    o.sysroot = paths.sysroot;
    // Windows: each module DLL links the import library of the host (hot/runtime/hidden.h).
    o.host_import_library = paths.host_import_library;
#ifdef HOT_PORTABLE
    // The work directory of a portable host is in a shared place. Refuse an unsafe one.
    const std::string work_error = check_work_dir(options.work_dir.empty() ? paths.work_dir : options.work_dir);
    if (!work_error.empty())
        throw std::runtime_error(work_error);
#endif
    // One work directory for each process, so parallel test processes do not share files.
    const std::string work_base = options.work_dir.empty() ? paths.work_dir : options.work_dir;
    o.work_dir = work_base + "/p" + std::to_string(hot::detail::process_id());
    // The directories of the dead processes stay otherwise (a crash, or a kill, skips the cleanup).
    // The environment variable HOT_KEEP_TEMP keeps them on purpose.
    if (!keep_temp())
        remove_stale_work_dirs(work_base);
    std::error_code ec;
    std::filesystem::remove_all(o.work_dir, ec);
    // Only this p<pid> directory goes. The parent directory is shared with other processes.
    o.remove_work_dir_at_exit = !keep_temp();
    // The environment decides over the option. The backend makes "<cache_dir>/src" the generated_dir.
    o.cache_dir = options.cache_dir;
    if (const char* env = std::getenv("HOT_CACHE_DIR"); env && *env)
        o.cache_dir = env;
    if (!ObjectCache::enabled_by_env())
        o.cache_dir.clear();
    // The variable decides over the option, also when it is empty (empty: no database).
    if (const char* env = std::getenv("HOT_COMPILE_COMMANDS_DIR"))
        o.compile_commands_dir = env;
    else
        o.compile_commands_dir = options.compile_commands_dir;
    return std::make_unique<ClangFilesBackend>(std::move(o));
}
#endif

#if defined(HOT_WITH_JIT)
std::unique_ptr<Backend> make_jit(const DefaultBackendOptions& options)
{
    JitOptions o;
    const DefaultPaths paths = default_paths();
    o.compiler = paths.compiler;
    o.include_dirs = paths.include_dirs;
    o.extra_flags = paths.cxxflags;
    o.include_dirs.insert(o.include_dirs.end(), options.include_dirs.begin(), options.include_dirs.end());
    o.extra_flags.insert(o.extra_flags.end(), options.extra_flags.begin(), options.extra_flags.end());
    o.resource_dir = paths.resource_dir;
    o.fs = options.fs;
    o.preludes = options.preludes;
    o.std_version = options.std_version;
    return std::make_unique<JitBackend>(std::move(o));
}
#endif

} // namespace

bool backend_available(BackendKind kind)
{
    switch (kind)
    {
    case BackendKind::ClangFiles:
        return kWithClangFiles;
    case BackendKind::Jit:
        return kWithJit;
    case BackendKind::Auto:
        break;
    }
    return kWithClangFiles || kWithJit;
}

std::unique_ptr<Backend> make_default_backend()
{
    return make_default_backend(DefaultBackendOptions{});
}

std::unique_ptr<Backend> make_default_backend(const DefaultBackendOptions& options)
{
    const BackendKind kind = resolve_kind(options.kind);
    if (kind == BackendKind::Jit)
    {
#if defined(HOT_WITH_JIT)
        return make_jit(options);
#else
        throw std::runtime_error("The JIT backend is not in this build. Configure Hot++ with "
                                 "HOT_ENABLE_JIT=ON (it needs the LLVM and Clang packages).");
#endif
    }
#if defined(HOT_WITH_CLANG_FILES)
    return make_clang_files(options);
#else
    throw std::runtime_error("The clang files backend is not in this build. Configure Hot++ with "
                             "HOT_ENABLE_CLANG_FILES=ON.");
#endif
}

} // namespace hot
