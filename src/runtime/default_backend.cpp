// make_default_backend(): the backend that the CMake option HOT_BACKEND selected.
//
// CMake defines HOT_BACKEND_CLANG_FILES or HOT_BACKEND_JIT.
// A list value uses "|" between the items.
#include "hot/backend.h"
#include "hot/paths.h"
#include "hot/scratch.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>
#if defined(HOT_BACKEND_CLANG_FILES)
    #include "hot/clang_files_backend.h"
#elif defined(HOT_BACKEND_JIT)
    #include "hot/jit_backend.h"
#else
    #error "Define HOT_BACKEND_CLANG_FILES or HOT_BACKEND_JIT."
#endif

namespace hot
{

std::unique_ptr<Backend> make_default_backend()
{
    return make_default_backend(DefaultBackendOptions{});
}

std::unique_ptr<Backend> make_default_backend(const DefaultBackendOptions& options)
{
#if defined(HOT_BACKEND_CLANG_FILES)
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
#elif defined(HOT_BACKEND_JIT)
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
#endif
}

} // namespace hot
