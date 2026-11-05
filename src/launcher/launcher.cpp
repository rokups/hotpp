// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "hot/launcher.h"

#include "hot/backend.h"
#include "hot/runtime/loader.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace hot::launcher
{

namespace fs = std::filesystem;

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
    try
    {
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

} // namespace hot::launcher
