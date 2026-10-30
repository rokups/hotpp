// make_default_backend(): the backend that the CMake option HOT_BACKEND selected.
//
// CMake defines HOT_BACKEND_CLANG_FILES or HOT_BACKEND_JIT.
// A list value uses "|" between the items.
#include "hot/backend.h"
#include "hot/scratch.h"

#include <filesystem>
#include <sstream>
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

#if defined(HOT_BACKEND_CLANG_FILES) || defined(HOT_BACKEND_JIT)
namespace
{
std::vector<std::string> split_list(const std::string& s)
{
    std::vector<std::string> v;
    std::stringstream ss(s);
    for (std::string item; std::getline(ss, item, '|');)
        if (!item.empty())
            v.push_back(item);
    return v;
}
} // namespace
#endif

std::unique_ptr<Backend> make_default_backend()
{
    return make_default_backend(DefaultBackendOptions{});
}

std::unique_ptr<Backend> make_default_backend(const DefaultBackendOptions& options)
{
#if defined(HOT_BACKEND_CLANG_FILES)
    ClangFilesOptions o;
    o.compiler = HOT_DEFAULT_CXX;
    o.include_dirs = split_list(HOT_DEFAULT_INCLUDES);
    o.extra_flags = split_list(HOT_DEFAULT_CXXFLAGS);
    o.include_dirs.insert(o.include_dirs.end(), options.include_dirs.begin(), options.include_dirs.end());
    o.extra_flags.insert(o.extra_flags.end(), options.extra_flags.begin(), options.extra_flags.end());
    o.resource_dir = HOT_DEFAULT_RESOURCE_DIR;
    // One work directory for each process, so parallel test processes do not share files.
    o.work_dir = std::string(HOT_DEFAULT_WORK_DIR) + "/p" + std::to_string(getpid());
    std::error_code ec;
    std::filesystem::remove_all(o.work_dir, ec);
    // Only this p<pid> directory goes. The parent directory is shared with other processes.
    o.remove_work_dir_at_exit = !keep_temp();
    return std::make_unique<ClangFilesBackend>(std::move(o));
#elif defined(HOT_BACKEND_JIT)
    JitOptions o;
    o.compiler = HOT_DEFAULT_CXX;
    o.include_dirs = split_list(HOT_DEFAULT_INCLUDES);
    o.extra_flags = split_list(HOT_DEFAULT_CXXFLAGS);
    o.include_dirs.insert(o.include_dirs.end(), options.include_dirs.begin(), options.include_dirs.end());
    o.extra_flags.insert(o.extra_flags.end(), options.extra_flags.begin(), options.extra_flags.end());
    o.resource_dir = HOT_DEFAULT_RESOURCE_DIR;
    return std::make_unique<JitBackend>(std::move(o));
#endif
}

} // namespace hot
