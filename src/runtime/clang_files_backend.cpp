// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "hot/clang_files_backend.h"

#include "hot/config.h"
#include "hot/scratch.h"
#include "hot/hotc_lib.h"
#include "hot/paths.h"
#include "hot/runtime/process.h"

#include <algorithm>
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
    #include <fcntl.h>
    #include <spawn.h>
    #include <sys/stat.h>
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
    // Close-on-exec: a command of another thread must not get the write end. Else this read loop
    // waits for the end of that command too. The dup2 to 1 and 2 in the child clears the flag.
    int fds[2];
#ifdef __linux__
    if (pipe2(fds, O_CLOEXEC) != 0)
        return -1;
#else
    if (pipe(fds) != 0)
        return -1;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
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

namespace
{

// The PCH needs the clang driver of a POSIX host and a target with the same driver rules. It
// is off for a Windows host and a MinGW or MSVC target.
bool pch_target_supported(const ClangFilesOptions& opt)
{
#ifdef _WIN32
    (void)opt;
    return false;
#else
    const std::string& t = opt.target_triple;
    if (is_mingw_triple(t) || t.find("windows") != std::string::npos || t.find("msvc") != std::string::npos ||
        t.find("win32") != std::string::npos)
        return false;
    return true;
#endif
}

// False when the environment variable HOT_PCH is "0".
bool pch_enabled_by_env()
{
    const char* v = std::getenv("HOT_PCH");
    return !(v && std::strcmp(v, "0") == 0);
}

// The start of the translated text up to the last "#include" line of the start. The start has
// only "#include" and "#line" lines, empty lines and "//" comments. The PCH has this exact
// text, so a compile with the PCH sees the same declarations and macros as a compile without
// it. The include lines in the .cpp then find their include guards. Empty: no include.
std::string include_prefix(const std::string& cpp)
{
    size_t pos = 0;
    size_t end = 0;
    while (pos < cpp.size())
    {
        size_t nl = cpp.find('\n', pos);
        const size_t next = nl == std::string::npos ? cpp.size() : nl + 1;
        std::string line = cpp.substr(pos, next - pos);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (line.rfind("#include", 0) == 0)
            end = next;
        else if (!(line.empty() || line.rfind("//", 0) == 0 || line.rfind("#line", 0) == 0))
            break;
        pos = next;
    }
    std::string r = cpp.substr(0, end);
    if (!r.empty() && r.back() != '\n')
        r += '\n';
    return r;
}

// The text of the PCH header: the "#include" lines of the include prefix only. The first line
// of the prefix names the script and the generation, and each "#line" line names the script.
// Without them, the text and the PCH key depend only on the include set: all the units with the
// same include set share one PCH. The .cpp keeps its own "#line" lines, so the diagnostics of
// the unit do not change. The PCH header has no code, so it has no line for the diagnostics.
std::string pch_header_text(const std::string& prefix)
{
    std::string r;
    for (size_t pos = 0; pos < prefix.size();)
    {
        const size_t nl = prefix.find('\n', pos);
        const size_t next = nl == std::string::npos ? prefix.size() : nl + 1;
        if (prefix.compare(pos, 8, "#include") == 0)
        {
            r.append(prefix, pos, next - pos);
            if (r.back() != '\n')
                r += '\n';
        }
        pos = next;
    }
    return r;
}

// The PCH build: the compile command of the unit without "-c", with the header as a C++ header.
// The flags are the same as the flags of the unit, so clang accepts the PCH for the unit.
// "-fpch-instantiate-templates" does the template instantiations of the headers in the PCH.
std::vector<std::string> pch_build_command(const ClangFilesOptions& opt, const std::string& header,
    const std::string& out, const std::string& dep, const std::string& script_dir)
{
    std::vector<std::string> cmd = compile_object_command(opt, header, out, dep, script_dir);
    cmd.erase(std::find(cmd.begin(), cmd.end(), "-c"));
    // The command ends with "<input> -o <output>".
    cmd.insert(cmd.end() - 3, {"-fpch-instantiate-templates", "-x", "c++-header"});
    return cmd;
}

// The compile command of the unit with the PCH.
std::vector<std::string> with_pch(std::vector<std::string> cmd, const std::string& pch)
{
    cmd.insert(cmd.end() - 3, {"-include-pch", pch});
    return cmd;
}

// A make rule with the headers of the PCH entry, for the depfile of an object.
std::string depfile_rule(const std::vector<std::string>& headers)
{
    std::string r = "pch:";
    for (const std::string& h : headers)
    {
        r += ' ';
        for (const char c : h)
        {
            if (c == ' ' || c == '#' || c == '\\')
                r += '\\';
            if (c == '$')
                r += '$';
            r += c;
        }
    }
    return r + "\n";
}

// Sets the time of the file to a fixed old time (2020-01-01 00:00:00 UTC). Each writer of a PCH
// header gives it the same time and the same size, so a replaced file has the stamp of the old
// file. The time is old, so the store rule for young headers accepts it.
bool set_fixed_pch_time(const fs::path& path)
{
#ifdef _WIN32
    std::error_code ec; // no PCH on a Windows host
    fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(24), ec);
    return !ec;
#else
    struct timespec times[2];
    times[0].tv_sec = times[1].tv_sec = 1577836800;
    times[0].tv_nsec = times[1].tv_nsec = 0;
    return utimensat(AT_FDCWD, path.c_str(), times, 0) == 0;
#endif
}

// Writes the file once. A file with the same text stays as it is: clang checks the time and the
// size of each input of the PCH. The temp file has a name for each call: two backends of one
// process do not share it. A hard link does not replace the file of another writer. The fixed
// time makes a race of two writers safe: each file has the same stamp. A file system without
// hard links gets a copy.
bool write_pch_header(const fs::path& path, const std::string& text)
{
    static std::atomic<unsigned> counter{0};
    std::error_code ec;
    std::string have;
    if (read_text(path.string(), have) && have == text)
        return true;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp" + std::to_string(hot::detail::process_id()) + "." +
        std::to_string(counter++);
    if (!write_text(tmp.string(), text) || !set_fixed_pch_time(tmp))
    {
        fs::remove(tmp, ec);
        return false;
    }
    if (read_text(path.string(), have) && have != text)
        fs::remove(path, ec); // a damaged file
    fs::create_hard_link(tmp, path, ec);
    if (ec && !(read_text(path.string(), have) && have == text))
    {
        static std::atomic<bool> told{false};
        if (!told.exchange(true))
            std::fprintf(stderr, "hot: cannot make a hard link %s (%s); the PCH header is a copy\n",
                path.string().c_str(), ec.message().c_str());
        std::error_code cec;
        fs::copy_file(tmp, path, fs::copy_options::overwrite_existing, cec);
        if (!cec)
            set_fixed_pch_time(path);
    }
    fs::remove(tmp, ec);
    return read_text(path.string(), have) && have == text;
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
        // The PCH entries have their own limit: half the object limit (a PCH is 10 to 100 MB).
        if (pch_target_supported(opt_))
        {
            pch_cache_ = std::make_unique<ObjectCache>(fs::path(opt_.cache_dir) / "pch", max_bytes / 2);
            trim_pch();
        }
        link_cache_ = ObjectCache::module_cache_enabled_by_env();
        // The translate cache works with the object cache (prepare_cache gives its compiler id).
        if (TranslateCache::enabled_by_env())
            tr_cache_ = std::make_unique<TranslateCache>(opt_.cache_dir, opt_.translator_id);
        if (opt_.generated_dir.empty())
            opt_.generated_dir = (fs::path(opt_.cache_dir) / "src").string();
    }
}

ClangFilesBackend::~ClangFilesBackend()
{
    wait_for_pch(); // the PCH thread uses the backend and the work directory
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

// A small tolerant reader for compile_commands.json. It reads an array of objects with the string
// fields "directory", "file" and "output" and the string array "arguments". It skips each other
// value. Any syntax error makes the parse fail.
class JsonReader
{
public:
    explicit JsonReader(const std::string& text)
        : s_(text)
    {
    }

    bool parse_entries(std::vector<CompileCommandEntry>& out)
    {
        if (!expect('['))
            return false;
        if (peek() == ']')
            return true;
        for (;;)
        {
            CompileCommandEntry e;
            bool has_arguments = false;
            if (!parse_object(e, has_arguments))
                return false;
            if (has_arguments)
                out.push_back(std::move(e));
            if (peek() == ',')
            {
                ++i_;
                continue;
            }
            return expect(']');
        }
    }

private:
    void skip_space()
    {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t'))
            ++i_;
    }
    char peek()
    {
        skip_space();
        return i_ < s_.size() ? s_[i_] : '\0';
    }
    bool expect(char c)
    {
        if (peek() != c)
            return false;
        ++i_;
        return true;
    }
    static void append_utf8(std::string& r, unsigned cp)
    {
        if (cp < 0x80)
            r += static_cast<char>(cp);
        else if (cp < 0x800)
        {
            r += static_cast<char>(0xC0 | (cp >> 6));
            r += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            r += static_cast<char>(0xE0 | (cp >> 12));
            r += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            r += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            r += static_cast<char>(0xF0 | (cp >> 18));
            r += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            r += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            r += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned& v)
    {
        if (i_ + 4 > s_.size())
            return false;
        v = 0;
        for (int k = 0; k < 4; ++k)
        {
            const char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<unsigned>(c - 'A' + 10);
            else
                return false;
        }
        return true;
    }
    bool parse_string(std::string& r)
    {
        if (!expect('"'))
            return false;
        r.clear();
        while (i_ < s_.size())
        {
            const char c = s_[i_++];
            if (c == '"')
                return true;
            if (c != '\\')
            {
                r += c;
                continue;
            }
            if (i_ >= s_.size())
                return false;
            const char e = s_[i_++];
            switch (e)
            {
            case '"': r += '"'; break;
            case '\\': r += '\\'; break;
            case '/': r += '/'; break;
            case 'b': r += '\b'; break;
            case 'f': r += '\f'; break;
            case 'n': r += '\n'; break;
            case 'r': r += '\r'; break;
            case 't': r += '\t'; break;
            case 'u':
            {
                unsigned cp = 0;
                if (!hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp < 0xDC00 && i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u')
                {
                    i_ += 2;
                    unsigned lo = 0;
                    if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF)
                        return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                append_utf8(r, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool parse_string_array(std::vector<std::string>& r)
    {
        r.clear();
        if (!expect('['))
            return false;
        if (peek() == ']')
        {
            ++i_;
            return true;
        }
        for (;;)
        {
            std::string item;
            if (!parse_string(item))
                return false;
            r.push_back(std::move(item));
            if (peek() == ',')
            {
                ++i_;
                continue;
            }
            return expect(']');
        }
    }
    // Skips one value of any type. "depth" limits the nesting.
    bool skip_value(int depth)
    {
        if (depth > 32)
            return false;
        const char c = peek();
        if (c == '"')
        {
            std::string ignore;
            return parse_string(ignore);
        }
        if (c == '[' || c == '{')
        {
            const char close = c == '[' ? ']' : '}';
            ++i_;
            if (peek() == close)
            {
                ++i_;
                return true;
            }
            for (;;)
            {
                if (c == '{')
                {
                    std::string key;
                    if (!parse_string(key) || !expect(':'))
                        return false;
                }
                if (!skip_value(depth + 1))
                    return false;
                if (peek() == ',')
                {
                    ++i_;
                    continue;
                }
                return expect(close);
            }
        }
        const size_t start = i_;
        while (i_ < s_.size() && s_[i_] != '\0' && std::strchr("-+.eE0123456789abcdefghijklmnopqrstuvwxyz", s_[i_]))
            ++i_;
        return i_ > start;
    }
    bool parse_object(CompileCommandEntry& e, bool& has_arguments)
    {
        if (!expect('{'))
            return false;
        if (peek() == '}')
        {
            ++i_;
            return true;
        }
        for (;;)
        {
            std::string key;
            if (!parse_string(key) || !expect(':'))
                return false;
            bool ok;
            if (key == "directory")
                ok = peek() == '"' && parse_string(e.directory);
            else if (key == "file")
                ok = peek() == '"' && parse_string(e.file);
            else if (key == "output")
                ok = peek() == '"' && parse_string(e.output);
            else if (key == "arguments")
            {
                ok = parse_string_array(e.arguments);
                has_arguments = ok;
            }
            else
                ok = skip_value(0);
            if (!ok)
                return false;
            if (peek() == ',')
            {
                ++i_;
                continue;
            }
            return expect('}');
        }
    }

    const std::string& s_;
    size_t i_ = 0;
};

fs::path absolute_path(const std::string& path)
{
    std::error_code ec;
    fs::path p = fs::absolute(path, ec);
    return ec ? fs::path(path) : p.lexically_normal();
}

} // namespace

std::map<std::string, CompileCommandEntry> ClangFilesBackend::read_compile_commands(std::string& text) const
{
    std::map<std::string, CompileCommandEntry> result;
    text.clear();
    const std::string path = (fs::path(opt_.compile_commands_dir) / "compile_commands.json").string();
    if (!read_text(path, text))
        return result;
    std::vector<CompileCommandEntry> old;
    if (!JsonReader(text).parse_entries(old))
        return result; // a damaged file: start empty
    std::error_code ec;
    for (CompileCommandEntry& e : old)
        if (!e.file.empty() && fs::exists(e.file, ec))
            result[e.file] = std::move(e);
    return result;
}

void ClangFilesBackend::record_compile_command(
    const std::string& unit, const std::string& script_dir, const std::string& object)
{
    if (opt_.compile_commands_dir.empty() || opt_.generated_dir.empty())
        return;
    const fs::path stable = absolute_path((fs::path(opt_.generated_dir) / (unit + ".cpp")).string());
    std::error_code ec;
    if (!fs::exists(stable, ec))
        return; // the publish failed: the IDE has no file to read
    CompileCommandEntry e;
    e.directory = absolute_path(cache_ ? opt_.cache_dir : opt_.work_dir).string();
    e.file = stable.string();
    e.output = absolute_path(object).string();
    // No depfile: the IDE needs no "-MD -MF".
    e.arguments = compile_object_command(opt_, e.file, e.output, std::string(), script_dir);
    // The mirror directory "<work_dir>/vfs" belongs to this process and is gone after the exit.
    // The IDE reads the real directories ("-I<dir>") that follow.
    const std::string mirror_arg = "-I" + opt_.work_dir + "/vfs/";
    e.arguments.erase(std::remove_if(e.arguments.begin(), e.arguments.end(),
                          [&](const std::string& a) { return a.compare(0, mirror_arg.size(), mirror_arg) == 0; }),
        e.arguments.end());
    commands_[e.file] = std::move(e);
    // An entry whose generated file is gone (a removed unit) goes.
    for (auto it = commands_.begin(); it != commands_.end();)
        it = fs::exists(it->first, ec) ? std::next(it) : commands_.erase(it);
    const fs::path dir(opt_.compile_commands_dir);
    const fs::path db = dir / "compile_commands.json";
    // The stamp of the file. A different stamp means a write of another process.
    const auto stamp = [&db](int64_t& mtime, uintmax_t& size)
    {
        std::error_code e1, e2;
        const fs::file_time_type t = fs::last_write_time(db, e1);
        size = fs::file_size(db, e2);
        mtime = static_cast<int64_t>(t.time_since_epoch().count());
        return !e1 && !e2;
    };
    const auto same_entry = [](const CompileCommandEntry& a, const CompileCommandEntry& b)
    {
        return a.directory == b.directory && a.file == b.file && a.arguments == b.arguments && a.output == b.output;
    };
    int64_t mtime = 0;
    uintmax_t size = 0;
    // The file is the same as at the last read or write, and it has the entries of this backend:
    // no work. A warm start of many units otherwise reads and writes the file for each unit.
    if (disk_known_ && stamp(mtime, size) && mtime == disk_mtime_ && size == disk_size_)
    {
        bool same = true;
        for (auto it = commands_.begin(); same && it != commands_.end(); ++it)
        {
            const auto d = disk_commands_.find(it->first);
            same = d != disk_commands_.end() && same_entry(d->second, it->second);
        }
        for (auto it = disk_commands_.begin(); same && it != disk_commands_.end(); ++it)
            same = fs::exists(it->first, ec);
        if (same)
            return;
    }
    disk_known_ = false;
    // Another process can write the file after the last read. The merge keeps its entries. The
    // window between the read and the rename is short; a lost entry comes back at its next build.
    std::string old_text;
    std::map<std::string, CompileCommandEntry> merged = read_compile_commands(old_text);
    for (const auto& [file, entry] : commands_)
        merged[file] = entry;
    std::vector<CompileCommandEntry> list;
    for (const auto& [file, entry] : merged) // the map order is the order of "file"
        list.push_back(entry);
    const std::string text = compile_commands_json(list);
    if (text != old_text)
    {
        fs::create_directories(dir, ec);
        // A write failure has no effect on the build. The next build tries again. The write is
        // atomic (a temporary file and a rename).
        if (!write_text_replace(db.string(), text))
        {
            std::fprintf(stderr, "[script] WARNING: cannot write %s\n", db.string().c_str());
            return;
        }
    }
    // The file now has the text of "merged".
    if (stamp(disk_mtime_, disk_size_))
    {
        disk_commands_ = std::move(merged);
        disk_known_ = true;
    }
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

namespace
{

// The stamp of a library file for the module key: the path, the time and the size.
std::string library_stamp(const std::string& path)
{
    if (path.empty())
        return std::string();
    std::error_code ec1;
    std::error_code ec2;
    const auto mtime = fs::last_write_time(path, ec1).time_since_epoch().count();
    const uintmax_t size = fs::file_size(path, ec2);
    if (ec1 || ec2)
        return path + "|missing";
    return path + "|" + std::to_string(static_cast<long long>(mtime)) + "|" + std::to_string(size);
}

// The environment variables that the compiler driver reads. They can change the include path or
// the target, so they are part of the compiler id: "|name=length:value" for each set variable.
std::string compiler_environment()
{
    static const char* const names[] = {"CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
        "SDKROOT", "MACOSX_DEPLOYMENT_TARGET", "CCC_OVERRIDE_OPTIONS", "LIBRARY_PATH", "COMPILER_PATH",
        // The MSVC tools and the Windows SDK.
        "INCLUDE", "EXTERNAL_INCLUDE", "LIB", "VCToolsInstallDir", "VCINSTALLDIR", "WindowsSdkDir",
        "WindowsSDKVersion",
        // The Apple targets other than macOS.
        "IPHONEOS_DEPLOYMENT_TARGET", "TVOS_DEPLOYMENT_TARGET", "WATCHOS_DEPLOYMENT_TARGET",
        "XROS_DEPLOYMENT_TARGET"};
    std::string text;
    for (const char* name : names)
    {
        // An unset variable adds no text, so a new name in the list keeps the old keys.
        const char* value = std::getenv(name);
        if (!value)
            continue;
        text += '|';
        text += name;
        text += std::string("=") + std::to_string(std::char_traits<char>::length(value)) + ":" + value;
    }
    return text;
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
    compiler_id_ = program + "|" + version + "|" + std::to_string(mtime) + "|" + std::to_string(size) +
        compiler_environment();
    // The compiler id is in the key of the translate cache too: a new compiler often comes with
    // new system headers, and the dependency list of a translate has no system header.
    if (tr_cache_)
        tr_cache_->set_extra(compiler_id_);
    return true;
}

std::string ClangFilesBackend::pch_marker(const ObjectCache::Key& key, const char* ext) const
{
    return (fs::path(opt_.cache_dir) / "pch" / (key.hex() + ext)).string();
}

// The first miss of an include set writes the "seen" marker and uses no PCH: a cold build has
// no extra cost. A later miss builds the PCH next to its compile, and the miss after that uses
// the PCH. A PCH that did not work has a "bad" marker: no PCH for one day.
std::optional<ClangFilesBackend::PchPlan> ClangFilesBackend::plan_pch(
    const std::string& cpp, const std::string& script_dir, bool mirror_files, uint64_t shadow_hash)
{
    // A mirror header of the host file system is in the work directory. Each load writes it
    // again, and it has no stamp. So a compile with mirror files uses no PCH.
    if (mirror_files)
        return std::nullopt;
    // The version line of the driver must name clang ("-include-pch" is a clang flag).
    if (!pch_cache_ || !pch_enabled_by_env() || compiler_id_.find("clang") == std::string::npos)
        return std::nullopt;
    const std::string prefix = pch_header_text(include_prefix(cpp));
    if (prefix.empty())
        return std::nullopt;
    // The key has the header text, the normalized build flags, the compiler id and the shadow
    // hash. The header stamps of the manifest do not see a new header earlier in the include
    // path: with the same include guard, the old PCH would hide it. The shadow hash sees it.
    ObjectCache::KeyInputs inputs;
    inputs.cpp_text = "pch\n" + prefix;
    inputs.args = ObjectCache::normalize_args(
        pch_build_command(opt_, "@hdr@", "@pch@", "@dep@", script_dir), opt_.work_dir);
    inputs.compiler_id = compiler_id_;
    inputs.shadow_hash = shadow_hash;
    PchPlan plan;
    plan.key = ObjectCache::make_key(inputs);
    plan.prefix = prefix;
    plan.header = pch_marker(plan.key, ".h");
    std::error_code ec;
    const fs::path bad = pch_marker(plan.key, ".bad");
    if (fs::exists(bad, ec))
    {
        const auto t = fs::last_write_time(bad, ec);
        if (!ec && fs::file_time_type::clock::now() - t < std::chrono::hours(24))
            return std::nullopt;
        fs::remove(bad, ec);
    }
    if (const std::optional<ObjectCache::Hit> hit = pch_cache_->lookup(plan.key))
    {
        plan.pch = hit->object.string();
        return plan;
    }
    const std::string seen = pch_marker(plan.key, ".seen");
    if (fs::exists(seen, ec))
    {
        plan.build = true;
        return plan;
    }
    fs::create_directories(fs::path(seen).parent_path(), ec);
    write_text(seen, std::string());
    return std::nullopt;
}

bool ClangFilesBackend::build_pch(const PchPlan& plan, const std::string& script_dir, std::string& error)
{
    const fs::file_time_type started = fs::file_time_type::clock::now();
    // A new time of the "seen" marker: the trim of a process does not remove the header now.
    write_text(pch_marker(plan.key, ".seen"), std::string());
    if (!write_pch_header(plan.header, plan.prefix))
    {
        error = "cannot write " + plan.header;
        return false;
    }
    const std::string base = (fs::path(opt_.work_dir) / plan.key.hex()).string();
    const std::string tmp = base + ".pch.tmp" + std::to_string(hot::detail::process_id());
    const std::string dep = base + ".pchd.tmp" + std::to_string(hot::detail::process_id());
    std::error_code ec;
    std::string output;
    const int rc = run_command(pch_build_command(opt_, plan.header, tmp, dep, script_dir), output);
    std::string dep_text;
    if (rc == 0)
        read_text(dep, dep_text);
    fs::remove(dep, ec);
    bool ok = rc == 0;
    bool bad = !ok; // the PCH of this key can never work
    if (!ok)
        error = "the PCH build failed (exit code " + std::to_string(rc) + "): " + output.substr(0, 400);
    // A header in the work directory has no stamp: a PCH of it can be stale. No such PCH.
    std::vector<std::string> deps;
    if (ok && !ObjectCache::parse_depfile(dep_text, deps))
    {
        ok = false;
        bad = true;
        error = "cannot parse the depfile of the PCH";
    }
    const std::string work = fs::path(opt_.work_dir).lexically_normal().string();
    for (const std::string& d : deps)
        if (ok && fs::path(d).lexically_normal().string().rfind(work, 0) == 0)
        {
            ok = false;
            bad = true;
            error = "the PCH reads the file '" + d + "' of the work directory";
        }
    if (ok && !pch_cache_->store(plan.key, tmp, dep_text, opt_.work_dir, started, error))
        ok = false;
    fs::remove(tmp, ec);
    // A PCH that does not build or reads a work file gets the "bad" marker. A store failure (a
    // young header) does not.
    if (bad)
        write_text(pch_marker(plan.key, ".bad"), error);
    // A PCH is 10 to 100 MB: a new one can put the PCH entries above the limit.
    if (ok)
        trim_pch();
    return ok;
}

namespace
{

// The time that a marker file of a PCH stays after its PCH is gone.
constexpr auto kPchMarkerRetention = std::chrono::hours(24 * 7);

} // namespace

// The size trim of the PCH entries. Then the marker files in "<cache_dir>/pch" of a PCH that is
// not in the cache:
// - a "<hex>.seen" or "<hex>.bad" file older than the retention (7 days) goes;
// - a "<hex>.h" file goes when its "seen" marker is gone. Its time is a fixed old time, so the
//   age of the "seen" marker is its age. A PCH build writes the "seen" marker again first.
// The files of a PCH in the cache stay. The temp files of the headers stay: their time is old.
void ClangFilesBackend::trim_pch()
{
    if (!pch_cache_)
        return;
    pch_cache_->trim();
    try
    {
        const fs::path dir = fs::path(opt_.cache_dir) / "pch";
        const fs::path obj = dir / "obj";
        const auto now = fs::file_time_type::clock::now();
        std::vector<fs::path> headers;
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            std::error_code fec;
            if (!it->is_regular_file(fec))
                continue;
            const fs::path p = it->path();
            const std::string ext = p.extension().string();
            if (ext != ".seen" && ext != ".bad" && ext != ".h")
                continue;
            if (fs::exists(obj / (p.stem().string() + ".json"), fec))
                continue; // the PCH is in the cache
            if (ext == ".h")
            {
                headers.push_back(p);
                continue;
            }
            const auto t = fs::last_write_time(p, fec);
            if (!fec && now - t > kPchMarkerRetention)
                fs::remove(p, fec);
        }
        for (const fs::path& h : headers)
        {
            std::error_code fec;
            if (!fs::exists(fs::path(h).replace_extension(".seen"), fec) && !fec)
                fs::remove(h, fec);
        }
    }
    catch (...)
    {
    }
}

void ClangFilesBackend::start_pch_build(
    const PchPlan& plan, const std::string& script_dir, const std::string& unit, bool timing)
{
    pch_done_ = false;
    pch_thread_ = std::thread(
        [this, plan, script_dir, unit, timing]
        {
            const auto s = std::chrono::steady_clock::now();
            std::string error;
            const bool ok = build_pch(plan, script_dir, error);
            const long long ms = static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s).count());
            if (!ok)
                std::fprintf(stderr, "hot: %s has no PCH: %s\n", unit.c_str(), error.c_str());
            else if (timing)
                std::fprintf(stderr, "hot: %s pch build %lld ms\n", unit.c_str(), ms);
            pch_done_ = true;
        });
}

void ClangFilesBackend::wait_for_pch()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (pch_thread_.joinable())
        pch_thread_.join();
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
    std::vector<std::string>* dependencies, LoadResult* hashes, std::optional<ObjectCache::Key>* cached_module,
    bool use_module_cache)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (cached_module)
        cached_module->reset();
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
    // The translate cache gives the result of an earlier translate with the same inputs. A hit
    // continues as a translate: also when the object cache then misses, the compile uses the
    // cached text. Its "#line" lines and the source map give the script places of the errors.
    std::optional<hotc::TranslateResult> cached;
    TranslateCache* const tr_cache = use_cache ? tr_cache_.get() : nullptr;
    if (tr_cache)
        cached = tr_cache->lookup(to);
    const bool translate_hit = cached.has_value();
    const fs::file_time_type translate_start = fs::file_time_type::clock::now();
    hotc::TranslateResult tr = translate_hit ? std::move(*cached) : hotc::translate(to, *opt_.session);
    if (tr_cache && !translate_hit && tr.ok && tr.diagnostics.empty())
    {
        std::string store_error;
        if (!tr_cache->store(to, tr, translate_start, store_error) && timing)
            std::fprintf(stderr, "hot: %s has no translate cache entry: %s\n", unit.c_str(), store_error.c_str());
    }
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
    std::string built_object = obj_path; // the object file of the compile, for the compile database
    const char* cache_note = "";
    std::string pch_note; // ", pch" when the compile used the PCH
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
        // A sum of the path hashes: the order of the paths is different after a translate cache hit.
        for (const std::string& p : tr.shadow_paths)
            inputs.shadow_hash += hotc::text_hash(p);
        const ObjectCache::Key key = ObjectCache::make_key(inputs);
        std::optional<ObjectCache::Hit> hit = cache_->lookup(key);
        cache_note = hit ? ", cache hit" : ", cache miss";
        // The key of the linked module: the object key, the link command and the host library.
        std::optional<ObjectCache::Key> module_key;
        if (link_cache_)
            module_key = ObjectCache::make_module_key(key,
                ObjectCache::normalize_args(link_module_command(opt_, "@obj@", "@so@"), opt_.work_dir),
                compiler_id_, library_stamp(opt_.host_import_library));
        bool module_cached = false;
        // The module key has no header data. Only the object lookup checks the header stamps, so
        // a module hit needs an object hit. Else a changed header can load an old module.
        if (module_key && use_module_cache && !hit)
            cache_->count_module_miss();
        if (module_key && use_module_cache && hit)
        {
            // A copy: the loader can remove the file, and a trim can remove the cache file.
            if (const std::optional<fs::path> cached = cache_->lookup_module(*module_key))
            {
                std::error_code copy_ec;
                fs::copy_file(*cached, so_path, fs::copy_options::overwrite_existing, copy_ec);
                if (!copy_ec)
                {
                    module_cached = true;
                    cache_note = ", link cached";
                    built_object = cache_->object_path(key).string();
                    t2 = std::chrono::steady_clock::now();
                    if (cached_module)
                        *cached_module = module_key;
                }
                else
                {
                    fs::remove(so_path, copy_ec);
                    cache_->remove_module(*module_key);
                }
            }
        }
        bool retried = false;
        while (!module_cached)
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
                const std::vector<std::string> plain = compile_object_command(opt_, cpp_path, tmp, dep, script_dir);
                std::optional<PchPlan> pch =
                    plan_pch(tr.cpp, script_dir, opt_.fs && !tr.virtual_files.empty(), inputs.shadow_hash);
                // The PCH build runs next to the compile and after the end of this call: the
                // compile, the link and the load do not wait for it. One PCH build at a time: a
                // plan to build while the last build runs uses no PCH.
                if (pch_thread_.joinable() && pch_done_)
                    pch_thread_.join();
                if (pch && pch->build && pch_thread_.joinable())
                    pch.reset();
                if (pch && pch->build)
                    start_pch_build(*pch, script_dir, unit, timing);
                int rc = 0;
                bool used_pch = false;
                if (pch && !pch->pch.empty())
                {
                    rc = run_command(with_pch(plain, pch->pch), output);
                    used_pch = rc == 0;
                    if (rc != 0)
                    {
                        // The PCH must never fail a build: compile again without it. When that
                        // compile works, the PCH is bad (damaged, or a header without a guard).
                        const std::string pch_output = output;
                        fs::remove(tmp, ec);
                        fs::remove(dep, ec);
                        rc = run_command(plain, output);
                        if (rc == 0)
                        {
                            pch_cache_->remove(pch->key);
                            write_text(pch_marker(pch->key, ".bad"), pch_output);
                            pch_note = ", pch failed";
                            std::fprintf(stderr, "hot: %s does not compile with its PCH; the PCH is off for one day\n",
                                unit.c_str());
                        }
                    }
                }
                else
                    rc = run_command(plain, output);
                if (used_pch)
                    pch_note = ", pch";
                t2 = std::chrono::steady_clock::now();
                std::string dep_text;
                if (rc == 0)
                    read_text(dep, dep_text);
                fs::remove(dep, ec);
                // The object depends on the headers of the PCH too. Clang lists them in the
                // depfile of a compile with "-include-pch". The rule keeps them for a clang
                // that does not. parse_depfile reads each rule.
                if (used_pch)
                    dep_text += "\n" + depfile_rule(pch_cache_->headers(pch->key));
                if (rc != 0)
                {
                    fs::remove(tmp, ec);
                    return fail("compile", rc);
                }
                std::string store_error;
                // The start of the translate: the translated text depends on the headers too.
                if (cache_->store(key, tmp, dep_text, opt_.work_dir, translate_start, store_error))
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
            built_object = temp_object.empty() ? object : obj_path;
            if (!temp_object.empty())
                fs::remove(temp_object, ec);
            if (link_rc == 0)
            {
                // A module entry needs a valid object entry (see the module lookup above).
                if (module_key && temp_object.empty())
                {
                    std::string store_error;
                    if (!cache_->store_module(*module_key, so_path, store_error))
                        std::fprintf(stderr, "hot: cannot store %s in the module cache: %s\n", unit.c_str(),
                            store_error.c_str());
                }
                break;
            }
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
        const std::string translate_note =
            translate_hit ? std::string("cached") : std::to_string(ms(t0, t1)) + " ms";
        std::fprintf(stderr, "hot: %s translate %s, compile %lld ms, link %lld ms%s%s\n", unit.c_str(),
            translate_note.c_str(), ms(t1, t2), ms(t2, t3), cache_note, pch_note.c_str());
    }
    if (link_rc != 0)
        return fail("link", link_rc);
    record_compile_command(unit, script_dir, built_object);
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
    return load_impl(source, generation, mode, true);
}

LoadResult ClangFilesBackend::load_impl(
    const ScriptSource& source, uint32_t generation, LoadMode mode, bool use_module_cache)
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
    std::optional<ObjectCache::Key> cached_module;
    if (!build_impl(source, generation, mode, so, r.error, &slots, &r.dependencies, &r, &cached_module,
            use_module_cache))
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
    if (!r.module && cached_module && cache_)
    {
        // The cached .so does not load (a damaged file). Remove the entry and link once more.
        // The second call never reads the module cache, so there is no loop.
        cache_->remove_module(*cached_module);
        std::error_code rm_ec;
        fs::remove(so, rm_ec);
        return load_impl(source, generation, mode, false);
    }
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
