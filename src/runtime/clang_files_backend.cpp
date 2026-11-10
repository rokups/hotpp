// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "hot/clang_files_backend.h"

#include "hot/config.h"
#include "hot/scratch.h"
#include "hot/hotc_lib.h"
#include "hot/paths.h"
#include "hot/runtime/process.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <spawn.h>
    #include <sys/wait.h>
    #include <unistd.h>

extern char** environ;
#endif

namespace hot
{

namespace fs = std::filesystem;

#ifdef _WIN32
int run_command(const std::vector<std::string>& argv, std::string& output)
{
    output.clear();
    if (argv.empty())
        return -1;
    const auto fail = [&](const char* what)
    {
        output = "cannot run '" + argv[0] + "': " + what + " failed (error " + std::to_string(GetLastError()) + ")\n";
        return -1;
    };
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0))
        return fail("CreatePipe");
    // Only the child gets the write end. The parent keeps the read end for itself.
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    // The child inherits only the write end, not the inheritable handles of other threads. Else a
    // second child can hold the write end of this pipe open, and the read loop does not end.
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size))
    {
        CloseHandle(read_end);
        CloseHandle(write_end);
        return fail("InitializeProcThreadAttributeList");
    }
    HANDLE inherit[1] = {write_end};
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);
    STARTUPINFOEXA si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nullptr;
    si.StartupInfo.hStdOutput = write_end;
    si.StartupInfo.hStdError = write_end;
    si.lpAttributeList = attrs;
    std::string cmdline = windows_command_line(argv); // CreateProcessA can change the buffer
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &si.StartupInfo, &pi);
    const DWORD start_error = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    // Close the copy of the write end in the parent before the read loop. Else ReadFile never
    // sees the end of the pipe, and the loop does not end.
    CloseHandle(write_end);
    if (!started)
    {
        CloseHandle(read_end);
        SetLastError(start_error);
        return fail("CreateProcess");
    }
    CloseHandle(pi.hThread);
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(read_end, buf, sizeof buf, &n, nullptr) && n > 0)
        output.append(buf, n);
    CloseHandle(read_end);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    const BOOL have_code = GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return have_code ? static_cast<int>(code) : -1;
}
#else
int run_command(const std::vector<std::string>& argv, std::string& output)
{
    output.clear();
    if (argv.empty())
        return -1;
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    std::vector<char*> cargv;
    for (const std::string& a : argv)
        cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, cargv[0], &fa, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0)
    {
        close(fds[0]);
        output = "cannot run '" + argv[0] + "': " + std::strerror(rc) + "\n";
        return -1;
    }
    char buf[4096];
    for (;;)
    {
        ssize_t n = read(fds[0], buf, sizeof buf);
        if (n > 0)
            output.append(buf, static_cast<size_t>(n));
        else if (n < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

namespace
{

bool read_text(const std::string& path, std::string& text)
{
    std::ifstream f(path);
    if (!f)
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    text = ss.str();
    return true;
}

bool write_text(const std::string& path, const std::string& text)
{
    std::ofstream f(path, std::ios::binary); // binary: LF line ends on each platform
    if (!f)
        return false;
    f << text;
    return static_cast<bool>(f);
}

// Writes the file through a temp file and a rename, so a reader never sees a part of the text.
// The temp name has the process id: two processes do not write the same temp file. Windows
// refuses the rename while an other process has the target open, so it tries 3 times.
bool write_text_replace(const std::string& path, const std::string& text)
{
    static std::atomic<unsigned> counter{0};
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    const std::string tmp = path + "." + std::to_string(pid) + "." + std::to_string(counter++) + ".tmp";
    if (!write_text(tmp, text))
        return false;
    std::error_code ec;
#ifdef _WIN32
    const int tries = 3;
#else
    const int tries = 1;
#endif
    for (int i = 0; i < tries; ++i)
    {
        if (i > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        fs::rename(tmp, path, ec);
        if (!ec)
            return true;
    }
    fs::remove(tmp, ec);
    return false;
}

} // namespace

void ClangFilesBackend::seed_slots(const std::string& text)
{
    std::error_code ec; // the work dir can be new: the first load makes it
    std::filesystem::create_directories(std::filesystem::path(opt_.slots_file).parent_path(), ec);
    std::string have;
    if (read_text(opt_.slots_file, have) && slots_cover(have, text))
        return; // the file has every line of the text and more: it has newer state, keep it
    if (!write_text(opt_.slots_file, text))
        std::fprintf(stderr, "[script] WARNING: cannot write %s\n", opt_.slots_file.c_str());
}

void ClangFilesBackend::revert_slots(const std::string& text)
{
    std::string have;
    if (!read_text(opt_.slots_file, have))
        return;
    if (!write_text(opt_.slots_file, slots_revert(have, text)))
        std::fprintf(stderr, "[script] WARNING: cannot write %s\n", opt_.slots_file.c_str());
}

ClangFilesBackend::ClangFilesBackend(ClangFilesOptions options)
    : opt_(std::move(options))
{
    if (!opt_.session)
        opt_.session = std::make_shared<hotc::TranslatorSession>();
    if (opt_.slots_file.empty())
        opt_.slots_file = opt_.work_dir + "/slots.txt";
    if (!opt_.cache_dir.empty() && ObjectCache::enabled_by_env())
    {
        uint64_t max_bytes = opt_.cache_max_bytes;
        if (const char* env = std::getenv("HOT_CACHE_MAX_MB"); env && *env)
        {
            char* end = nullptr;
            const unsigned long long mb = std::strtoull(env, &end, 10);
            if (end && *end == '\0')
                max_bytes = static_cast<uint64_t>(mb) << 20;
        }
        cache_ = std::make_unique<ObjectCache>(opt_.cache_dir, max_bytes);
        cache_->trim();
        if (opt_.generated_dir.empty())
            opt_.generated_dir = (fs::path(opt_.cache_dir) / "src").string();
    }
}

ClangFilesBackend::~ClangFilesBackend()
{
    if (opt_.remove_work_dir_at_exit)
        remove_scratch(opt_.work_dir);
}

namespace
{

// The place of a virtual path under "<work_dir>/vfs". The path becomes absolute and lexically
// normal first, so the ".." parts of "../shared" or of "a/../b" are resolved. The result has no
// root and no leading slash. It gives no value when the result would leave the mirror root.
std::optional<std::string> vfs_relative(const std::string& path)
{
    const std::string rel = fs::absolute(fs::path(path)).lexically_normal().relative_path().string();
    if (rel == ".." || rel.compare(0, 3, "../") == 0)
        return std::nullopt;
    return rel;
}

} // namespace

namespace
{

// A Darwin target: the triple says so, or no triple on a macOS host.
bool darwin_target(const ClangFilesOptions& opt)
{
#ifdef __APPLE__
    return opt.target_triple.empty() || is_darwin_triple(opt.target_triple);
#else
    return is_darwin_triple(opt.target_triple);
#endif
}

// The MinGW flags of the compile or of the link. The compile gets "-nostdinc++" and the
// "-isystem <dir>" pairs (the libstdc++ headers). The link gets the other flags (the linker,
// the static runtime and the "-L" of libgcc).
std::vector<std::string> mingw_flags(const ClangFilesOptions& opt, bool compile)
{
    std::vector<std::string> r;
    const std::vector<std::string> all = mingw_target_flags(opt.target_triple, opt.sysroot);
    for (size_t i = 0; i < all.size(); ++i)
    {
        const bool header = all[i] == "-nostdinc++" || all[i] == "-isystem";
        if (header == compile)
            r.push_back(all[i]);
        if (all[i] == "-isystem" && i + 1 < all.size())
        {
            ++i;
            if (compile)
                r.push_back(all[i]);
        }
    }
    return r;
}

} // namespace

// The split of the flags between the compile and the link, for each target:
// - ELF: the compile has the code flags (-fPIC, -fno-plt, -fvisibility). The link has -shared and -fPIC.
// - MinGW: the compile has the libstdc++ headers. The link has lld, the static runtime and -L.
// - Darwin: "-undefined dynamic_lookup" is for the link only. MSVC: the runtime flag goes to both.
// Target, sysroot, resource dir and extra flags go to both, so the driver finds the same libraries.
std::vector<std::string> compile_object_command(const ClangFilesOptions& opt, const std::string& cpp_path,
    const std::string& obj_path, const std::string& depfile_path, const std::string& script_dir)
{
    const bool mingw = is_mingw_triple(opt.target_triple);
    const bool darwin = darwin_target(opt);
    // A MinGW build must use the C++ driver, so it links libstdc++ (hot/command_line.h).
    std::vector<std::string> cmd = {mingw ? cxx_driver(opt.compiler) : opt.compiler,
        "-std=" + (opt.std_version.empty() ? std::string(cxx_std_name()) : opt.std_version),
#ifndef _WIN32
        // The shared library needs position independent code. A Windows DLL does not.
        "-fPIC",
#endif
        "-c", "-g", "-Werror=return-type",
        // A signed overflow wraps in a script. The compiler must not use it as undefined behavior.
        "-fwrapv",
        // A module symbol must not bind to a host symbol with the same mangled name. The entry
        // point script_register has default visibility (hot/runtime/registry.h).
        "-fvisibility=hidden", "-fvisibility-inlines-hidden",
#ifdef _WIN32
        // HOT_EXPORT becomes dllimport: the module takes the runtime symbols from the host.
        "-DHOT_MODULE_BUILD=1",
#endif
    };
#ifndef _WIN32
    // A call to the host (a native method, the runtime) reads the address from the GOT. A PLT
    // stub adds one jump to each call. The module loads with RTLD_NOW, so no symbol is lazy.
    // The flag is for ELF only. Mach-O has no PLT.
    if (!darwin)
        cmd.push_back("-fno-plt");
#endif
    if (!opt.target_triple.empty())
        cmd.push_back("--target=" + opt.target_triple);
    if (!opt.sysroot.empty())
        cmd.push_back("--sysroot=" + opt.sysroot);
    for (const std::string& f : mingw_flags(opt, true))
        cmd.push_back(f);
    // An MSVC host: the module uses the same C runtime DLL as the host (/MD or /MDd). The
    // default of clang++ is the static runtime. Two runtimes do not share the STL debug levels.
    for (const std::string& f : host_ms_runtime_lib_flags(opt.target_triple))
        cmd.push_back(f);
    if (!opt.resource_dir.empty())
        cmd.push_back("-resource-dir=" + opt.resource_dir);
    for (const std::string& f : opt.extra_flags)
        cmd.push_back(f);
    const std::string mirror = opt.work_dir + "/vfs/";
    if (opt.fs && !script_dir.empty())
        cmd.push_back("-I" + mirror + vfs_relative(script_dir).value_or(std::string()));
    for (const std::string& d : opt.include_dirs)
    {
        if (opt.fs)
            cmd.push_back("-I" + mirror + vfs_relative(d).value_or(std::string()));
        cmd.push_back("-I" + d);
    }
    for (const std::string& f : format_compile_flags())
        cmd.push_back(f);
    for (const std::string& f : checks_compile_flags())
        cmd.push_back(f);
    for (const std::string& f : thread_safe_compile_flags())
        cmd.push_back(f);
    for (const std::string& f : traceback_compile_flags())
        cmd.push_back(f);
    // The depfile names the headers of the compile (the input of an object cache).
    if (!depfile_path.empty())
    {
        cmd.push_back("-MD");
        cmd.push_back("-MF");
        cmd.push_back(depfile_path);
    }
    cmd.push_back(cpp_path);
    cmd.push_back("-o");
    cmd.push_back(obj_path);
    return cmd;
}

std::vector<std::string> link_module_command(
    const ClangFilesOptions& opt, const std::string& obj_path, const std::string& out_so)
{
    const bool mingw = is_mingw_triple(opt.target_triple);
    std::vector<std::string> cmd = {mingw ? cxx_driver(opt.compiler) : opt.compiler, "-shared",
#ifndef _WIN32
        "-fPIC",
#endif
        "-g"};
    if (!opt.target_triple.empty())
        cmd.push_back("--target=" + opt.target_triple);
    if (!opt.sysroot.empty())
        cmd.push_back("--sysroot=" + opt.sysroot);
    for (const std::string& f : mingw_flags(opt, false))
        cmd.push_back(f);
    for (const std::string& f : darwin_target_flags(darwin_target(opt)))
        cmd.push_back(f);
    for (const std::string& f : host_ms_runtime_lib_flags(opt.target_triple))
        cmd.push_back(f);
    if (!opt.resource_dir.empty())
        cmd.push_back("-resource-dir=" + opt.resource_dir);
    for (const std::string& f : opt.extra_flags)
        cmd.push_back(f);
    // The extra flags can have compile flags (-D, -O2). The link does not use them.
    cmd.push_back("-Wno-unused-command-line-argument");
    cmd.push_back(obj_path);
    // The library goes after the object, so the link resolves the module's host references.
    if (!opt.host_import_library.empty())
        cmd.push_back(opt.host_import_library);
    cmd.push_back("-o");
    cmd.push_back(out_so);
    return cmd;
}

namespace
{

std::string json_string(const std::string& s)
{
    std::string r = "\"";
    for (char ch : s)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c)
        {
        case '"': r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        case '\n': r += "\\n"; break;
        case '\r': r += "\\r"; break;
        case '\t': r += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                r += buf;
            }
            else
                r += ch;
        }
    }
    return r + "\"";
}

} // namespace

std::string compile_commands_json(const std::vector<CompileCommandEntry>& entries)
{
    std::string r = "[\n";
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const CompileCommandEntry& e = entries[i];
        r += "  {\n    \"directory\": " + json_string(e.directory) + ",\n    \"file\": " + json_string(e.file)
            + ",\n    \"arguments\": [";
        for (size_t k = 0; k < e.arguments.size(); ++k)
            r += (k ? ", " : "") + json_string(e.arguments[k]);
        r += "],\n    \"output\": " + json_string(e.output) + "\n  }" + (i + 1 < entries.size() ? "," : "") + "\n";
    }
    return r + "]\n";
}

namespace
{

// The path of the compiler program: a name with a directory, or a search of PATH.
std::string find_program(const std::string& name)
{
    std::error_code ec;
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
        return fs::exists(name, ec) ? name : std::string();
    const char* path_env = std::getenv("PATH");
    if (!path_env)
        return std::string();
#ifdef _WIN32
    const char sep = ';';
    const std::vector<std::string> names = {name, name + ".exe"};
#else
    const char sep = ':';
    const std::vector<std::string> names = {name};
#endif
    const std::string path = path_env;
    size_t start = 0;
    while (start <= path.size())
    {
        size_t end = path.find(sep, start);
        if (end == std::string::npos)
            end = path.size();
        const std::string dir = path.substr(start, end - start);
        for (const std::string& n : names)
        {
            const fs::path candidate = (dir.empty() ? fs::path(".") : fs::path(dir)) / n;
            if (fs::is_regular_file(candidate, ec))
                return candidate.string();
        }
        start = end + 1;
    }
    return std::string();
}

} // namespace

bool ClangFilesBackend::prepare_cache()
{
    if (!cache_)
        return false;
    if (compiler_id_done_)
        return !compiler_id_.empty();
    compiler_id_done_ = true;
    std::string output;
    const int rc = run_command({opt_.compiler, "--version"}, output);
    const std::string program = find_program(opt_.compiler);
    std::error_code ec1;
    std::error_code ec2;
    uint64_t size = 0;
    long long mtime = 0;
    if (!program.empty())
    {
        size = static_cast<uint64_t>(fs::file_size(program, ec1));
        mtime = static_cast<long long>(fs::last_write_time(program, ec2).time_since_epoch().count());
    }
    const std::string version = output.substr(0, output.find('\n'));
    if (rc != 0 || version.empty() || program.empty() || ec1 || ec2)
    {
        std::fprintf(stderr, "hot: the object cache is off: cannot identify the compiler '%s'\n",
            opt_.compiler.c_str());
        cache_.reset();
        return false;
    }
    compiler_id_ = program + "|" + version + "|" + std::to_string(mtime) + "|" + std::to_string(size);
    return true;
}

bool ClangFilesBackend::build(const ScriptSource& source, uint32_t generation, const std::string& out_so,
    std::string& error, std::vector<std::string>* dependencies)
{
    return build_impl(source, generation, LoadMode::Reload, out_so, error, nullptr, dependencies);
}

// "deferred_slots" null: write the slots file when the compile succeeds (the tool hotc).
// Else give the new content of the slots file back, and leave the file as it is.
bool ClangFilesBackend::build_impl(const ScriptSource& source, uint32_t generation, LoadMode mode,
    const std::string& out_so, std::string& error, std::string* deferred_slots,
    std::vector<std::string>* dependencies, LoadResult* hashes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    if (dependencies)
        dependencies->clear();
    std::error_code ec;
    const fs::path so_path(out_so);
    if (so_path.has_parent_path())
        fs::create_directories(so_path.parent_path(), ec);
    fs::create_directories(
        fs::path(opt_.slots_file).parent_path().empty() ? fs::path(".") : fs::path(opt_.slots_file).parent_path(), ec);
    // The .o and the depfile go next to the .so.
    const std::string obj_path = fs::path(so_path).replace_extension(".o").string();
    const std::string dep_path = fs::path(so_path).replace_extension(".d").string();
    std::string unit = source.unit.empty() ? fs::path(source.name).stem().string() : source.unit;
    if (unit.empty())
        unit = "script";
    // With a generated_dir the .cpp has a stable name. It is also the "#line" name, so the text
    // of the .cpp has no part of the .so name (no process id, generation, epoch or sequence).
    // A unit name can have directories ("Player/Health"). They become directories of generated_dir.
    const bool stable_cpp = !opt_.generated_dir.empty();
    const std::string line_name = stable_cpp ? (fs::path(opt_.generated_dir) / (unit + ".cpp")).string()
                                             : fs::path(so_path).replace_extension(".cpp").string();
    if (stable_cpp)
        fs::create_directories(fs::path(line_name).parent_path(), ec);
    // With the cache the compile reads a private copy "<work_dir>/<unit>.cpp". Two processes then
    // never change the input of a compile of the other. The stable file is for the IDE only.
    const bool use_cache = prepare_cache();
    std::string cpp_path = use_cache ? (fs::path(opt_.work_dir) / (unit + ".cpp")).string() : line_name;

    hotc::TranslateOptions to;
    to.script_text = source.text;
    to.modules = source.modules;
    to.script_name = source.name;
    to.unit_name = source.unit;
    to.output_name = line_name;
    to.generation = static_cast<int>(generation);
    to.include_dirs = opt_.include_dirs;
    // The translator needs the resource dir for the native headers. A portable build finds it at run time.
    to.resource_dir = opt_.resource_dir.empty() ? default_paths().resource_dir : opt_.resource_dir;
    to.std_version = opt_.std_version.empty() ? cxx_std_name() : opt_.std_version;
    // The parse of the native headers sees the macros of the compile.
    for (const std::string& f : opt_.extra_flags)
        if (f.rfind("-D", 0) == 0 || f.rfind("-U", 0) == 0)
            to.defines.push_back(f);
    // The parse uses the target of the modules, so the native headers match the compile.
    to.target = opt_.target_triple;
    to.sysroot = opt_.sysroot;
    to.strict_borrowed = opt_.strict_borrowed;
    to.null_warnings = opt_.null_warnings;
    to.format_lib = format_lib_name();
    to.view_type = view_type_name();
    to.fs = opt_.fs;
    to.preludes = opt_.preludes;
    // A missing file means the first generation. A fresh generation starts with no slots.
    if (mode == LoadMode::Reload)
        read_text(opt_.slots_file, to.slots_in);
    // HOT_TIMING prints the time of the translate and of the clang run of each unit.
    const bool timing = std::getenv("HOT_TIMING") != nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    hotc::TranslateResult tr = hotc::translate(to, *opt_.session);
    const auto t1 = std::chrono::steady_clock::now();
    if (dependencies)
        *dependencies = tr.dependencies; // also after a failure
    if (hashes)
    {
        hashes->iface_hash = tr.iface_hash;
        hashes->import_hashes = tr.import_hashes;
    }
    if (!tr.ok)
    {
        error = opt_.raw_diagnostics
            ? tr.diagnostics
            : hotc::rewrite_translator_diagnostic(tr.diagnostics, hotc::script_texts(source.name, source.text, tr));
        return false;
    }
    if (use_cache)
    {
        fs::create_directories(fs::path(cpp_path).parent_path(), ec);
        if (!write_text(cpp_path, tr.cpp))
        {
            error = "cannot write " + cpp_path;
            return false;
        }
        // A failure here has no effect on the build. Skip the write when the text is the same, so
        // a file watcher of the IDE does not see a change.
        std::string have;
        if (!(read_text(line_name, have) && have == tr.cpp))
            write_text_replace(line_name, tr.cpp);
    }
    else if (stable_cpp && !write_text_replace(cpp_path, tr.cpp))
    {
        // The rename can fail on Windows while an other process reads the file. This build then
        // uses the work dir path. The "#line" name stays the same.
        cpp_path = fs::path(so_path).replace_extension(".cpp").string();
        if (!write_text(cpp_path, tr.cpp))
        {
            error = "cannot write " + cpp_path;
            return false;
        }
    }
    else if (!stable_cpp && !write_text(cpp_path, tr.cpp))
    {
        error = "cannot write " + cpp_path;
        return false;
    }

    // The headers of the host file system go to disk files, because the external compiler reads
    // the disk. Each load writes them again under "<work_dir>/vfs".
    if (opt_.fs)
    {
        const fs::path mirror = fs::path(opt_.work_dir) / "vfs";
        fs::remove_all(mirror, ec);
        for (const auto& [vpath, vtext] : tr.virtual_files)
        {
            const std::optional<std::string> rel = vfs_relative(vpath);
            if (!rel)
            {
                error = "the virtual file '" + vpath + "' is outside the mirror directory\n";
                return false;
            }
            const fs::path target = mirror / *rel;
            fs::create_directories(target.parent_path(), ec);
            if (!write_text(target.string(), vtext))
            {
                error = "cannot write " + target.string();
                return false;
            }
        }
    }
    const size_t script_slash = source.name.rfind('/');
    const std::string script_dir =
        script_slash == std::string::npos ? std::string(".") : source.name.substr(0, script_slash);
    const std::string sysroot_error = mingw_sysroot_error(opt_.target_triple, opt_.sysroot);
    if (!sysroot_error.empty())
    {
        error = sysroot_error + "\n";
        return false;
    }
    fs::remove(so_path, ec);
    std::string output;
    // The diagnostics can name the "#line" name or the path of the file (before the first "#line").
    hotc::ScriptTexts generated = {{line_name, tr.cpp}};
    if (cpp_path != line_name)
        generated.push_back({cpp_path, tr.cpp});
    const auto fail = [&](const char* what, int code)
    {
        if (!opt_.raw_diagnostics)
            output = hotc::rewrite_diagnostic(output, {{source.name, source.text}}, generated, &tr.source_map);
        error = output + "the " + what + " of " + source.name + " failed (exit code " + std::to_string(code) + ")\n";
        return false;
    };
    int link_rc = 0;
    const char* cache_note = "";
    auto t2 = std::chrono::steady_clock::now();
    if (!use_cache)
    {
        fs::remove(obj_path, ec);
        const int rc = run_command(compile_object_command(opt_, cpp_path, obj_path, dep_path, script_dir), output);
        t2 = std::chrono::steady_clock::now();
        if (rc != 0)
            return fail("compile", rc);
        link_rc = run_command(link_module_command(opt_, obj_path, out_so), output);
    }
    else
    {
        // The key has the translated text, the compile arguments (the paths of this build are
        // fixed names), the compiler and the mirror files that the compile can read.
        ObjectCache::KeyInputs inputs;
        inputs.cpp_text = tr.cpp;
        inputs.args = ObjectCache::normalize_args(
            compile_object_command(opt_, "@cpp@", "@obj@", "@dep@", script_dir), opt_.work_dir);
        inputs.compiler_id = compiler_id_;
        if (opt_.fs)
            inputs.virtual_files = tr.virtual_files;
        const ObjectCache::Key key = ObjectCache::make_key(inputs);
        std::optional<ObjectCache::Hit> hit = cache_->lookup(key);
        cache_note = hit ? ", cache hit" : ", cache miss";
        bool retried = false;
        for (;;)
        {
            std::string object; // the object file to link
            std::string temp_object; // an object that the cache did not take: delete it after the link
            if (hit)
                object = hit->object.string();
            else
            {
                const std::string base = (fs::path(opt_.work_dir) / key.hex()).string();
                const std::string tmp = base + ".o.tmp" + std::to_string(hot::detail::process_id());
                const std::string dep = base + ".d.tmp" + std::to_string(hot::detail::process_id());
                fs::remove(tmp, ec);
                fs::remove(dep, ec);
                const int rc = run_command(compile_object_command(opt_, cpp_path, tmp, dep, script_dir), output);
                t2 = std::chrono::steady_clock::now();
                std::string dep_text;
                if (rc == 0)
                    read_text(dep, dep_text);
                fs::remove(dep, ec);
                if (rc != 0)
                {
                    fs::remove(tmp, ec);
                    return fail("compile", rc);
                }
                std::string store_error;
                if (cache_->store(key, tmp, dep_text, opt_.work_dir, store_error))
                    object = cache_->object_path(key).string();
                else
                {
                    std::fprintf(stderr, "hot: cannot store %s in the object cache: %s\n", unit.c_str(),
                        store_error.c_str());
                    object = tmp;
                    temp_object = tmp;
                }
            }
            link_rc = run_command(link_module_command(opt_, object, out_so), output);
            if (!temp_object.empty())
                fs::remove(temp_object, ec);
            if (link_rc == 0)
                break;
            cache_->remove(key); // a bad object must not stay in the cache
            if (!hit || retried)
                break;
            // The cached object does not link (a damaged file). Compile it once more.
            hit.reset();
            retried = true;
        }
    }
    if (timing)
    {
        const auto t3 = std::chrono::steady_clock::now();
        const auto ms = [](auto a, auto b) {
            return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count());
        };
        std::fprintf(stderr, "hot: %s translate %lld ms, compile %lld ms, link %lld ms%s\n", unit.c_str(),
            ms(t0, t1), ms(t1, t2), ms(t2, t3), cache_note);
    }
    if (link_rc != 0)
        return fail("link", link_rc);
    if (deferred_slots)
    {
        *deferred_slots = tr.slots_out + tr.sigs_out;
        return true;
    }
    if (!write_text(opt_.slots_file, tr.slots_out + tr.sigs_out))
    {
        error = "cannot write " + opt_.slots_file;
        return false;
    }
    return true;
}

LoadResult ClangFilesBackend::load(const ScriptSource& source, uint32_t generation, LoadMode mode)
{
    std::string stem = fs::path(source.name).stem().string();
    if (stem.empty())
        stem = "script";
    // A fresh load starts a new epoch. The epoch is in the name, so a fresh library never has
    // the path of a library that the process still maps.
    const unsigned epoch = mode == LoadMode::Fresh ? ++epoch_ : epoch_.load();
    const std::string so = opt_.work_dir + "/" + stem + ".g" + std::to_string(generation) + ".e" + std::to_string(epoch)
        + "." + std::to_string(sequence_++) + module_suffix();
    LoadResult r;
    std::string slots;
    if (!build_impl(source, generation, mode, so, r.error, &slots, &r.dependencies, &r))
    {
        r.reason = ReloadReason::CompileError;
        return r;
    }
    // Check now that the slots file is writable: the write at on_accept time then rarely fails.
    // The open for append does not change a file that exists. A new empty file is removed again.
    {
        std::error_code ec;
        const bool existed = fs::exists(opt_.slots_file, ec);
        bool writable;
        {
            std::ofstream probe(opt_.slots_file, std::ios::app | std::ios::binary);
            writable = static_cast<bool>(probe);
        }
        if (!existed)
            fs::remove(opt_.slots_file, ec);
        if (!writable)
        {
            r.error = "cannot write " + opt_.slots_file;
            r.reason = ReloadReason::CompileError;
            return r;
        }
    }
    std::vector<std::string> dependencies = std::move(r.dependencies);
    const uint64_t iface_hash = r.iface_hash;
    std::vector<std::pair<std::string, uint64_t>> import_hashes = std::move(r.import_hashes);
    r = module_from_library(so);
    r.dependencies = std::move(dependencies);
    r.iface_hash = iface_hash;
    r.import_hashes = std::move(import_hashes);
    if (r.module)
    {
        r.module->iface_hash = iface_hash;
        r.module->imports = r.import_hashes;
        if (!source.unit.empty())
            r.module->unit = source.unit;
    }
    if (!r.module)
    {
        if (r.reason == ReloadReason::None)
            r.reason = ReloadReason::BadModule;
        return r;
    }
    // The slots file changes only when the module is live. The callback holds a copy
    // of the path and the text, not the backend: the module can outlive the backend.
    r.module->on_accept = [path = opt_.slots_file, slots = std::move(slots)]
    {
        if (!write_text(path, slots))
            std::fprintf(stderr, "[script] WARNING: cannot write %s\n", path.c_str());
    };
    return r;
}

} // namespace hot
