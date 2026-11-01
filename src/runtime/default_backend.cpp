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
#include "hot/scratch.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>
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
#ifdef HOT_PORTABLE
    // The work directory of a portable host is in a shared place. Refuse an unsafe one.
    const std::string work_error = check_work_dir(paths.work_dir);
    if (!work_error.empty())
        throw std::runtime_error(work_error);
#endif
    // One work directory for each process, so parallel test processes do not share files.
    o.work_dir = paths.work_dir + "/p" + std::to_string(getpid());
    std::error_code ec;
    std::filesystem::remove_all(o.work_dir, ec);
    // Only this p<pid> directory goes. The parent directory is shared with other processes.
    o.remove_work_dir_at_exit = !keep_temp();
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
