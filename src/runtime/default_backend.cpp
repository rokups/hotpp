// make_default_backend(): the backend that the CMake option SPP_BACKEND selected.
//
// CMake defines SPP_BACKEND_CLANG_FILES or SPP_BACKEND_JIT.
// A list value uses "|" between the items.
#include <unistd.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "spp/backend.h"
#if defined(SPP_BACKEND_CLANG_FILES)
#include "spp/clang_files_backend.h"
#elif defined(SPP_BACKEND_JIT)
#include "spp/jit_backend.h"
#else
#error "Define SPP_BACKEND_CLANG_FILES or SPP_BACKEND_JIT."
#endif

namespace spp {

#if defined(SPP_BACKEND_CLANG_FILES) || defined(SPP_BACKEND_JIT)
namespace {
std::vector<std::string> split_list(const std::string& s)
{
    std::vector<std::string> v;
    std::stringstream ss(s);
    for (std::string item; std::getline(ss, item, '|');)
        if (!item.empty()) v.push_back(item);
    return v;
}
}  // namespace
#endif

std::unique_ptr<Backend> make_default_backend()
{
#if defined(SPP_BACKEND_CLANG_FILES)
    ClangFilesOptions o;
    o.compiler = SPP_DEFAULT_CXX;
    o.include_dirs = split_list(SPP_DEFAULT_INCLUDES);
    o.extra_flags = split_list(SPP_DEFAULT_CXXFLAGS);
    o.resource_dir = SPP_DEFAULT_RESOURCE_DIR;
    // One work directory for each process, so parallel test processes do not share files.
    o.work_dir = std::string(SPP_DEFAULT_WORK_DIR) + "/p" + std::to_string(getpid());
    std::error_code ec;
    std::filesystem::remove_all(o.work_dir, ec);
    return std::make_unique<ClangFilesBackend>(std::move(o));
#elif defined(SPP_BACKEND_JIT)
    JitOptions o;
    o.compiler = SPP_DEFAULT_CXX;
    o.include_dirs = split_list(SPP_DEFAULT_INCLUDES);
    o.extra_flags = split_list(SPP_DEFAULT_CXXFLAGS);
    o.resource_dir = SPP_DEFAULT_RESOURCE_DIR;
    return std::make_unique<JitBackend>(std::move(o));
#endif
}

}  // namespace spp
