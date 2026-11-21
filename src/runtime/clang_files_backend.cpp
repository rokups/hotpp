// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "hot/clang_files_backend.h"
#include "parallel_probe.h"

#include "hot/config.h"
#include "hot/scratch.h"
#include "hot/hotc_lib.h"
#include "hot/paths.h"
#include "hot/runtime/process.h"

#if defined(__linux__)
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if !defined(__linux__)
// The ELF symbol readers return early here; the constants only keep them compiling.
constexpr int STB_LOCAL = 0, STT_OBJECT = 1, STT_FUNC = 2, STT_TLS = 6;
#endif
#include <algorithm>
#include <atomic>
#include <map>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <sstream>
#include <set>
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

namespace
{
// HOT_TIMING: prints the time of a serial phase when the scope ends. The scope does nothing
// when the switch is off.
class PhaseTimer
{
public:
    PhaseTimer(const char* name, bool on)
        : name_(name)
        , on_(on)
        , t0_(std::chrono::steady_clock::now())
    {
    }
    explicit PhaseTimer(const char* name)
        : PhaseTimer(name, std::getenv("HOT_TIMING") != nullptr)
    {
    }
    ~PhaseTimer()
    {
        if (!on_)
            return;
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0_);
        std::fprintf(stderr, "hot: timing %s %lld ms\n", name_, static_cast<long long>(us.count() / 1000));
    }
    PhaseTimer(const PhaseTimer&) = delete;
    PhaseTimer& operator=(const PhaseTimer&) = delete;

private:
    const char* name_;
    bool on_;
    std::chrono::steady_clock::time_point t0_;
};
} // namespace

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
// "mtime" and "size" (optional) get the stamp of the new file. The stamp is from the temp file
// before the rename: a write of an other process after the rename has no effect on it.
bool write_text_replace(const std::string& path, const std::string& text, int64_t* mtime = nullptr,
    uintmax_t* size = nullptr)
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
    if (mtime && size)
    {
        std::error_code e1, e2;
        const fs::file_time_type t = fs::last_write_time(tmp, e1);
        *size = fs::file_size(tmp, e2);
        *mtime = static_cast<int64_t>(t.time_since_epoch().count());
        if (e1 || e2)
            *size = static_cast<uintmax_t>(-1); // no stamp: the next flush reads the file
    }
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

// A line that sets a macro of the script mode (a ".cpp" script or optimized AOT code). The unit has
// the line before its includes, so the PCH must have it too. No other "#define" line is accepted.
bool is_script_macro_line(const std::string& line)
{
    return line == "#define __HOTSCRIPT__ 1" || line == "#define HOT_AOT_OPTIMIZED 1";
}

// True when the PCH text has a script macro line. Such a unit uses only its exact PCH: the shared
// PCH of the other units has no such macro.
bool has_script_macros(const std::string& prefix)
{
    for (size_t pos = 0; pos < prefix.size();)
    {
        const size_t nl = prefix.find('\n', pos);
        const size_t next = nl == std::string::npos ? prefix.size() : nl + 1;
        std::string line = prefix.substr(pos, next - pos);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (is_script_macro_line(line))
            return true;
        pos = next;
    }
    return false;
}

// The start of the translated text up to the last "#include" line of the start. The start has
// only "#include" and "#line" lines, the script macro lines, empty lines and "//" comments. The PCH has this exact
// text, so a compile with the exact PCH of the unit sees the same declarations and macros as a
// compile without it. The shared PCH has more: it adds the declarations and macros of the headers
// of other units. The include lines in the .cpp find their include guards. Empty: no include.
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
        else if (!(line.empty() || line.rfind("//", 0) == 0 || line.rfind("#line", 0) == 0
                     || is_script_macro_line(line)))
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
        std::string line = prefix.substr(pos, next - pos);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (prefix.compare(pos, 8, "#include") == 0 || is_script_macro_line(line))
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

// The options with a fixed fmt choice: a PCH build uses the choice of its plan, also when an other
// job turned the own fmt copy on after the plan.
ClangFilesOptions with_fmt_choice(const ClangFilesOptions& opt, bool own_fmt)
{
    ClangFilesOptions o = opt;
    o.fmt_header_only = std::make_shared<std::atomic<bool>>(own_fmt);
    return o;
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

std::string union_pch_header(const std::vector<std::string>& prefixes)
{
    std::set<std::string> seen;
    std::string r;
    for (const std::string& prefix : prefixes)
    {
        const std::string text = pch_header_text(prefix);
        for (size_t pos = 0; pos < text.size();)
        {
            const size_t nl = text.find('\n', pos);
            const size_t next = nl == std::string::npos ? text.size() : nl + 1;
            const std::string line = text.substr(pos, next - pos);
            if (seen.insert(line).second)
                r += line;
            pos = next;
        }
    }
    return r;
}

namespace
{

// The lines of a PCH header text.
std::set<std::string> pch_lines(const std::string& text)
{
    std::set<std::string> r;
    for (size_t pos = 0; pos < text.size();)
    {
        const size_t nl = text.find('\n', pos);
        const size_t next = nl == std::string::npos ? text.size() : nl + 1;
        r.insert(text.substr(pos, next - pos));
        pos = next;
    }
    return r;
}

// Writes a file in a way that a reader never sees half of it.
void write_text_atomic(const fs::path& path, const std::string& text)
{
    const fs::path tmp = path.string() + ".tmp" + std::to_string(hot::detail::process_id());
    std::error_code ec;
    if (write_text(tmp.string(), text))
        fs::rename(tmp, path, ec);
    if (ec || !fs::exists(path, ec))
        fs::remove(tmp, ec);
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
    if (!opt_.fmt_header_only)
        opt_.fmt_header_only = std::make_shared<std::atomic<bool>>(false);
    // The decision before the first compile: the flags ask for the fmt code of the host, but the host
    // does not export it (a host library loaded RTLD_LOCAL is also checked). The own copy is on then.
    // The check of the module after the link stays as the safety net.
    if (!fmt_header_only(opt_) && !(opt_.host_fmt_probe ? opt_.host_fmt_probe() : host_exports_fmt(opt_.host_import_library)))
    {
        opt_.fmt_header_only->store(true);
        if (std::getenv("HOT_TIMING"))
            std::fprintf(stderr, "hot: the host does not export the fmt code; the units use their own fmt code\n");
    }
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
    wait_for_prewarm(); // the prewarm thread uses the backend options
    wait_for_pch(); // the PCH thread uses the backend and the work directory
    {
        std::thread t;
        {
            std::lock_guard<std::mutex> lock(shape_mutex_);
            t = std::move(shape_thread_);
        }
        if (t.joinable())
            t.join(); // the shape thread uses the backend and the cache directory
    }
    {
        // A batch that never finished leaves entries that are not in the file.
        std::lock_guard<std::mutex> lock(mutex_);
        flush_compile_commands();
    }
    if (opt_.remove_work_dir_at_exit)
        remove_scratch(opt_.work_dir);
}

void lower_thread_priority(); // threads.cpp

void ClangFilesBackend::wait_for_prewarm()
{
    std::thread t;
    {
        std::lock_guard<std::mutex> lock(pw_mutex_);
        t = std::move(pw_thread_);
    }
    if (t.joinable())
        t.join();
}

void ClangFilesBackend::prewarm(const std::vector<ScriptSource>&)
{
    const char* off = std::getenv("HOT_PREWARM");
    if (off && std::string(off) == "0")
        return;
    if (opt_.compile_commands_dir.empty() || opt_.generated_dir.empty())
        return;
    std::lock_guard<std::mutex> lock(pw_mutex_);
    if (pw_thread_.joinable() || pw_reading_)
        return; // one prewarm for each backend
    pw_reading_ = true;
    try
    {
        pw_thread_ = std::thread(
            [this]
            {
                std::map<std::string, CompileCommandEntry> commands;
                std::map<std::string, std::string> rendered;
                int64_t mtime = 0;
                uintmax_t size = 0;
                bool ok = false;
                try
                {
                    lower_thread_priority();
                    // The stamp comes before the read: a write after it makes the stamp differ.
                    const fs::path db = fs::path(opt_.compile_commands_dir) / "compile_commands.json";
                    std::error_code e1, e2;
                    const fs::file_time_type t = fs::last_write_time(db, e1);
                    size = fs::file_size(db, e2);
                    mtime = static_cast<int64_t>(t.time_since_epoch().count());
                    if (!e1 && !e2)
                    {
                        std::string text;
                        commands = read_compile_commands(text, rendered);
                        ok = !rendered.empty() || !commands.empty();
                    }
                }
                catch (...)
                {
                    ok = false;
                }
                std::lock_guard<std::mutex> l(pw_mutex_);
                ++pw_reads_;
                if (ok)
                {
                    pw_commands_ = std::move(commands);
                    pw_rendered_ = std::move(rendered);
                    pw_mtime_ = mtime;
                    pw_size_ = size;
                    pw_ready_ = true;
                }
                pw_reading_ = false;
                pw_cv_.notify_all();
            });
    }
    catch (...)
    {
        pw_reading_ = false; // no thread: the first flush reads the file itself
    }
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

// The script uses its own copy of the fmt code (-DFMT_HEADER_ONLY)? Not when the host flags have
// -DFMT_SHARED: the host has the fmt code and exports it (libUrho3D.so does). The own copy costs
// about 300 ms of the compile time of a unit. Only ELF: other targets keep the own copy (the host
// import library of Windows must carry the fmt symbols; the loader of Mach-O has no check here).
// HOT_FMT_HEADER_ONLY=1 forces the own copy. A backend that found a missing fmt symbol sets the
// shared flag "opt.fmt_header_only": the own copy is on from then.
// True for a Linux target with 64 bit ELF files (the check of the fmt symbols reads only these).
static bool elf64_linux_target(const std::string& triple)
{
    if (triple.empty())
        return sizeof(void*) == 8;
    if (triple.find("linux") == std::string::npos)
        return false;
    for (const char* narrow : {"i386", "i486", "i586", "i686", "arm", "thumb", "mips-", "mipsel", "powerpc-", "riscv32"})
        if (triple.rfind(narrow, 0) == 0)
            return false;
    return true;
}

bool fmt_header_only(const ClangFilesOptions& opt)
{
#if defined(__linux__)
    if (const char* v = std::getenv("HOT_FMT_HEADER_ONLY"); v && std::strcmp(v, "1") == 0)
        return true;
    if (opt.fmt_header_only && opt.fmt_header_only->load())
        return true;
    // The own fmt copy stays for a target that the check of the fmt symbols cannot read.
    if (sizeof(void*) != 8 || !elf64_linux_target(opt.target_triple))
        return true;
    for (const std::string& f : opt.extra_flags)
        if (f == "-DFMT_SHARED" || f.compare(0, 13, "-DFMT_SHARED=") == 0)
            return false;
    return true;
#else
    (void)opt;
    return true;
#endif
}

#if defined(__linux__)
namespace
{

// A symbol of the host: the global scope, or the handle of the host library when it is already
// loaded (a host that loaded it with RTLD_LOCAL, for example a .NET P/Invoke).
bool host_has_symbol(const char* name, const std::string& host_library)
{
    if (dlsym(RTLD_DEFAULT, name))
        return true;
    if (!host_library.empty())
        if (void* handle = dlopen(host_library.c_str(), RTLD_LAZY | RTLD_NOLOAD))
        {
            const bool found = dlsym(handle, name) != nullptr;
            dlclose(handle);
            return found;
        }
    return false;
}

// Reads "len" bytes at "off" of the file. False when the range is outside the file.
bool read_range(int fd, uint64_t file_size, uint64_t off, uint64_t len, void* out)
{
    if (off > file_size || len > file_size - off)
        return false;
    char* dst = static_cast<char*>(out);
    while (len > 0)
    {
        const ssize_t n = pread(fd, dst, static_cast<size_t>(len), static_cast<off_t>(off));
        if (n <= 0)
            return false;
        dst += n;
        off += static_cast<uint64_t>(n);
        len -= static_cast<uint64_t>(n);
    }
    return true;
}

} // namespace
#endif

bool host_exports_fmt(const std::string& host_library)
{
#if defined(__linux__)
    // A symbol that each module with the fmt code of the host needs.
    // The inline namespace of fmt changes with the version of the headers: the probe tries the
    // versions 10, 11 and 12. Any of them means that the host has the code (a module that needs an
    // other one fails the check of its undefined symbols after the link, and then uses its own copy).
    for (const char* name : {"_ZN3fmt3v106detail11assert_failEPKciS3_", "_ZN3fmt3v116detail11assert_failEPKciS3_",
             "_ZN3fmt3v126detail11assert_failEPKciS3_"})
        if (host_has_symbol(name, host_library))
            return true;
    return false;
#else
    (void)host_library;
    return false;
#endif
}

// True when the module "path" has an undefined fmt symbol that the process does not export. The
// loader would fail on it. The function reads the dynamic symbol table of an ELF64 file. It reads
// only the headers and the two tables (pread), not the whole file.
bool has_missing_fmt_symbol(const std::string& path, std::string* first_name, const std::string& host_library)
{
#if defined(__linux__)
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct Closer
    {
        int fd;
        ~Closer() { close(fd); }
    } closer{fd};
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0)
        return false;
    const uint64_t size = static_cast<uint64_t>(st.st_size);
    Elf64_Ehdr eh;
    if (!read_range(fd, size, 0, sizeof eh, &eh) || std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0
        || eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_shoff == 0 || eh.e_shentsize != sizeof(Elf64_Shdr))
        return false;
    std::vector<Elf64_Shdr> sections(eh.e_shnum);
    if (!sections.empty() && !read_range(fd, size, eh.e_shoff, uint64_t(eh.e_shnum) * sizeof(Elf64_Shdr), sections.data()))
        return false;
    const uint64_t limit = 256ull << 20; // a dynamic table of more than 256 MB is not real
    for (const Elf64_Shdr& sh : sections)
    {
        if (sh.sh_type != SHT_DYNSYM || sh.sh_link >= sections.size() || sh.sh_size > limit)
            continue;
        const Elf64_Shdr& str = sections[sh.sh_link];
        if (str.sh_size > limit || str.sh_size == 0)
            continue;
        std::vector<Elf64_Sym> syms(static_cast<size_t>(sh.sh_size / sizeof(Elf64_Sym)));
        std::vector<char> names(static_cast<size_t>(str.sh_size) + 1, '\0'); // ends with a zero
        if (!read_range(fd, size, sh.sh_offset, syms.size() * sizeof(Elf64_Sym), syms.data())
            || !read_range(fd, size, str.sh_offset, str.sh_size, names.data()))
            continue;
        for (const Elf64_Sym& sym : syms)
        {
            if (sym.st_shndx != SHN_UNDEF || ELF64_ST_BIND(sym.st_info) == STB_WEAK || sym.st_name == 0
                || sym.st_name >= str.sh_size)
                continue;
            const char* name = names.data() + sym.st_name;
            if (!std::strstr(name, "3fmt3v") || host_has_symbol(name, host_library))
                continue;
            if (first_name)
                *first_name = name;
            return true;
        }
    }
    return false;
#else
    (void)path;
    (void)first_name;
    (void)host_library;
    return false;
#endif
}

// The split of the flags between the compile and the link, for each target:
// - ELF: the compile has the code flags (-fPIC, -fno-plt, -fvisibility). The link has -shared and -fPIC.
// - MinGW: the compile has the libstdc++ headers. The link has lld, the static runtime and -L.
// - Darwin: "-undefined dynamic_lookup" is for the link only. MSVC: the runtime flag goes to both.
// Target, sysroot, resource dir and extra flags go to both, so the driver finds the same libraries.
std::vector<std::string> compile_object_command(const ClangFilesOptions& opt, const std::string& cpp_path,
    const std::string& obj_path, const std::string& depfile_path, const std::string& script_dir,
    const std::string& mirror_root)
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
        "-c", "-Werror=return-type",
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
    if (opt.debug_info)
        cmd.insert(std::find(cmd.begin(), cmd.end(), "-c") + 1, "-g");
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
    const std::string mirror = mirror_root.empty() ? opt.work_dir + "/vfs/" : mirror_root;
    if (opt.fs && !script_dir.empty())
        cmd.push_back("-I" + mirror + vfs_relative(script_dir).value_or(std::string()));
    for (const std::string& d : opt.include_dirs)
    {
        if (opt.fs)
            cmd.push_back("-I" + mirror + vfs_relative(d).value_or(std::string()));
        cmd.push_back("-I" + d);
    }
    for (const std::string& f : format_compile_flags(fmt_header_only(opt)))
        cmd.push_back(f);
    for (const std::string& f : checks_compile_flags())
        cmd.push_back(f);
    for (const std::string& f : thread_safe_compile_flags())
        cmd.push_back(f);
    for (const std::string& f : traceback_compile_flags())
        cmd.push_back(f);
    // The host flags can have -DFMT_HEADER_ONLY (the compile database of rbfx has it). It makes the
    // unit hold the whole fmt engine: the unit then needs about 300 ms more. This flag comes after all
    // other flags, so it removes the macro that any earlier flag sets.
    if (!fmt_header_only(opt))
        cmd.push_back("-UFMT_HEADER_ONLY");
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

// The words of a line that "clang -###" prints: each word is in double quotes, with a backslash
// before a quote or a backslash.
static std::vector<std::string> parse_driver_line(const std::string& line)
{
    std::vector<std::string> words;
    for (size_t i = 0; i < line.size();)
    {
        if (line[i] != '"')
        {
            ++i;
            continue;
        }
        std::string w;
        for (++i; i < line.size() && line[i] != '"'; ++i)
        {
            if (line[i] == '\\' && i + 1 < line.size())
                ++i;
            w += line[i];
        }
        ++i;
        words.push_back(std::move(w));
    }
    return words;
}

// The linker command that the driver would run for a link with lld (an ELF host): the driver costs
// about 17 ms for each link, and the command is the same for each module. One "-###" run gives it
// and the next links run it directly. The result holds for the options (the key). Empty: no direct
// link (an other linker, an other target, more than one command, or HOT_DIRECT_LINK=0). The caller
// runs the driver when the direct link fails.
static std::vector<std::string> direct_link_command(
    const ClangFilesOptions& opt, const std::string& obj, const std::string& so)
{
#ifdef _WIN32
    (void)opt;
    (void)obj;
    (void)so;
    return {};
#else
    const char* off = std::getenv("HOT_DIRECT_LINK");
    if ((off && std::strcmp(off, "0") == 0) || opt.linker_flag.empty() || darwin_target(opt)
        || is_mingw_triple(opt.target_triple)
        || !(opt.target_triple.empty() || opt.target_triple.find("linux") != std::string::npos))
        return {};
    static std::mutex mutex;
    static std::map<std::string, std::vector<std::string>> known;
    const std::string placeholder = "/dev/null"; // the driver wants an input that exists
    std::vector<std::string> probe = link_module_command(opt, placeholder, "@so@");
    std::string key;
    for (const std::string& w : probe)
        key += w + '\n';
    std::vector<std::string> command;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = known.find(key);
        if (it != known.end())
            command = it->second;
        else
        {
            probe.push_back("-###");
            std::string text;
            if (run_command(probe, text) == 0)
            {
                std::vector<std::string> lines;
                std::stringstream stream(text);
                for (std::string line; std::getline(stream, line);)
                {
                    const size_t first = line.find_first_not_of(" \t");
                    if (first != std::string::npos && line[first] == '"')
                        lines.push_back(line.substr(first));
                }
                if (lines.size() == 1)
                {
                    command = parse_driver_line(lines[0]);
                    const bool linker = !command.empty() && fs::path(command[0]).filename().string().find("ld") == 0;
                    if (!linker || std::count(command.begin(), command.end(), placeholder) != 1
                        || std::count(command.begin(), command.end(), std::string("@so@")) != 1)
                        command.clear();
                }
            }
            known[key] = command;
        }
    }
    for (std::string& w : command)
        if (w == "/dev/null")
            w = obj;
        else if (w == "@so@")
            w = so;
    return command;
#endif
}

// Runs the link of a module. When the link with the linker flag (lld) fails, it runs once more
// with the default linker, so a broken lld never fails a reload. HOT_TIMING logs the retry.
static int run_link(const ClangFilesOptions& opt, const std::string& obj, const std::string& so,
    std::string& output, bool timing)
{
    // The linker alone (without the driver) when its command is known. A failure runs the driver.
    int rc = 1;
    const std::vector<std::string> direct = direct_link_command(opt, obj, so);
    if (timing)
        std::fprintf(stderr, "hot: direct link %s\n", direct.empty() ? "off" : direct[0].c_str());
    if (!direct.empty())
    {
        rc = run_command(direct, output);
        if (rc != 0)
        {
            if (timing)
                std::fprintf(stderr, "hot: the direct link failed; the driver links\n");
            output.clear();
        }
    }
    if (rc != 0)
        rc = run_command(link_module_command(opt, obj, so), output);
    if (rc != 0 && !opt.linker_flag.empty())
    {
        ClangFilesOptions plain = opt;
        plain.linker_flag.clear();
        std::string retry_output;
        const int retry_rc = run_command(link_module_command(plain, obj, so), retry_output);
        if (timing)
            std::fprintf(stderr, "hot: the link with %s failed; the link with the default linker %s\n",
                opt.linker_flag.c_str(), retry_rc == 0 ? "works" : "fails too");
        if (retry_rc == 0)
        {
            output.clear();
            return 0;
        }
        output = retry_output;
        return retry_rc;
    }
    return rc;
}

// True for an ELF target of a host that is not Windows. HOT_LINKER=default turns lld off.
static bool use_lld_by_default(const ClangFilesOptions& opt)
{
#ifdef _WIN32
    (void)opt;
    return false;
#else
    const char* env = std::getenv("HOT_LINKER");
    if (env && std::string(env) == "default")
        return false;
    if (darwin_target(opt) || is_mingw_triple(opt.target_triple))
        return false;
    return opt.target_triple.empty() || opt.target_triple.find("linux") != std::string::npos;
#endif
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
    if (!opt.linker_flag.empty())
        cmd.push_back(opt.linker_flag);
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

// The text of one entry, without the separator after it.
static std::string compile_command_entry_json(const CompileCommandEntry& e)
{
    std::string r = "  {\n    \"directory\": ";
    r += json_string(e.directory);
    r += ",\n    \"file\": ";
    r += json_string(e.file);
    r += ",\n    \"arguments\": [";
    for (size_t k = 0; k < e.arguments.size(); ++k)
    {
        if (k)
            r += ", ";
        r += json_string(e.arguments[k]);
    }
    r += "],\n    \"output\": ";
    r += json_string(e.output);
    r += "\n  }";
    return r;
}

std::string compile_commands_json(const std::vector<CompileCommandEntry>& entries)
{
    std::string r = "[\n";
    for (size_t i = 0; i < entries.size(); ++i)
    {
        r += compile_command_entry_json(entries[i]);
        r += i + 1 < entries.size() ? ",\n" : "\n";
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

    // Parses one object. The text has nothing else.
    bool parse_single(CompileCommandEntry& e, bool& has_arguments)
    {
        if (!parse_object(e, has_arguments))
            return false;
        skip_space();
        return i_ == s_.size();
    }
    // Decodes one JSON string with its quotes.
    static bool decode_string(const std::string& quoted, std::string& out)
    {
        JsonReader r(quoted);
        return r.parse_string(out) && r.i_ == quoted.size();
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

namespace
{

// One top-level object of the database: the range in the text and the value of "file".
struct DbSlice
{
    size_t begin = 0;
    size_t end = 0; // after the "}"
    std::string file;
};

// Cuts the top-level array into objects and checks the syntax of each object (strings, numbers,
// literals, nested arrays and objects, the commas and colons). It reads only the "file" value of
// each object. An object with no "file" string is not in the list. It returns false for any
// syntax error: the full parser then handles the file.
class DbScanner
{
public:
    explicit DbScanner(const std::string& s)
        : s_(s)
    {
    }

    bool run(std::vector<DbSlice>& out)
    {
        if (!eat('['))
            return false;
        space();
        if (i_ < s_.size() && s_[i_] == ']')
        {
            ++i_;
            return end();
        }
        for (;;)
        {
            space();
            DbSlice slice;
            slice.begin = i_;
            bool have_file = false;
            std::string file_text;
            if (!object(1, have_file, file_text))
                return false;
            slice.end = i_;
            if (have_file && !JsonReader::decode_string(file_text, slice.file))
                return false;
            if (have_file && !slice.file.empty())
                out.push_back(std::move(slice));
            space();
            if (i_ < s_.size() && s_[i_] == ',')
            {
                ++i_;
                continue;
            }
            return eat(']') && end();
        }
    }

private:
    void space()
    {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t'))
            ++i_;
    }
    bool eat(char c)
    {
        space();
        if (i_ >= s_.size() || s_[i_] != c)
            return false;
        ++i_;
        return true;
    }
    bool end()
    {
        space();
        return i_ == s_.size();
    }
    // A string. "escape" tells that the string has a backslash.
    bool string(size_t& begin, bool& escape)
    {
        if (!eat('"'))
            return false;
        begin = i_ - 1;
        escape = false;
        for (;;)
        {
            i_ = s_.find_first_of("\"\\", i_);
            if (i_ == std::string::npos)
            {
                i_ = s_.size();
                return false;
            }
            if (s_[i_] == '"')
                break;
            escape = true;
            ++i_;
            if (i_ >= s_.size() || !std::strchr("\"\\/bfnrtu", s_[i_]) || s_[i_] == '\0')
                return false;
            ++i_;
        }
        if (i_ >= s_.size())
            return false;
        ++i_;
        return true;
    }
    bool value(int depth)
    {
        if (depth > 32)
            return false;
        space();
        if (i_ >= s_.size())
            return false;
        const char c = s_[i_];
        size_t b;
        bool esc;
        if (c == '"')
            return string(b, esc);
        if (c == '{')
        {
            bool have = false;
            std::string ignore;
            return object(depth + 1, have, ignore);
        }
        if (c == '[')
        {
            ++i_;
            space();
            if (i_ < s_.size() && s_[i_] == ']')
            {
                ++i_;
                return true;
            }
            for (;;)
            {
                if (!value(depth + 1))
                    return false;
                space();
                if (i_ < s_.size() && s_[i_] == ',')
                {
                    ++i_;
                    continue;
                }
                return eat(']');
            }
        }
        const size_t start = i_;
        while (i_ < s_.size() && s_[i_] != '\0' && std::strchr("-+.eE0123456789abcdefghijklmnopqrstuvwxyz", s_[i_]))
            ++i_;
        if (i_ == start)
            return false;
        if (c >= 'a' && c <= 'z')
        {
            const std::string word = s_.substr(start, i_ - start);
            return word == "true" || word == "false" || word == "null";
        }
        return is_json_number(s_.substr(start, i_ - start));
    }
    // The number grammar of JSON: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?
    static bool is_json_number(const std::string& w)
    {
        size_t k = 0;
        const auto digits = [&]
        {
            const size_t b = k;
            while (k < w.size() && w[k] >= '0' && w[k] <= '9')
                ++k;
            return k - b;
        };
        if (k < w.size() && w[k] == '-')
            ++k;
        if (k < w.size() && w[k] == '0')
            ++k;
        else if (digits() == 0)
            return false;
        if (k < w.size() && w[k] == '.')
        {
            ++k;
            if (digits() == 0)
                return false;
        }
        if (k < w.size() && (w[k] == 'e' || w[k] == 'E'))
        {
            ++k;
            if (k < w.size() && (w[k] == '+' || w[k] == '-'))
                ++k;
            if (digits() == 0)
                return false;
        }
        return k == w.size();
    }
    // An object. At depth 1 it takes the text of the "file" value (it must be a string).
    bool object(int depth, bool& have_file, std::string& file_text)
    {
        if (!eat('{'))
            return false;
        space();
        if (i_ < s_.size() && s_[i_] == '}')
        {
            ++i_;
            return true;
        }
        for (;;)
        {
            size_t kb, ke;
            bool escape;
            if (!string(kb, escape))
                return false;
            ke = i_;
            if (!eat(':'))
                return false;
            space();
            if (depth == 1 && escape)
                return false; // an escaped key: leave it to the full parser
            if (depth == 1 && s_.compare(kb, ke - kb, "\"file\"") == 0)
            {
                size_t vb;
                bool vesc;
                if (!string(vb, vesc))
                    return false;
                file_text = s_.substr(vb, i_ - vb);
                have_file = true;
            }
            else if (!value(depth))
                return false;
            space();
            if (i_ < s_.size() && s_[i_] == ',')
            {
                ++i_;
                continue;
            }
            return eat('}');
        }
    }

    const std::string& s_;
    size_t i_ = 0;
};

bool scan_compile_db(const std::string& s, std::vector<DbSlice>& out)
{
    return DbScanner(s).run(out);
}

} // namespace

std::map<std::string, CompileCommandEntry> ClangFilesBackend::read_compile_commands(
    std::string& text, std::map<std::string, std::string>& foreign) const
{
    std::map<std::string, CompileCommandEntry> result;
    text.clear();
    foreign.clear();
    const std::string path = (fs::path(opt_.compile_commands_dir) / "compile_commands.json").string();
    // HOT_TIMING: the split of the read, in microseconds.
    using Clock = std::chrono::steady_clock;
    const bool timing = std::getenv("HOT_TIMING") != nullptr;
    const auto us_since = [](Clock::time_point t) {
        return static_cast<long long>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t).count());
    };
    long long t_read = 0, t_scan = 0, t_exists = 0, t_copy = 0, t_parse = 0;
    size_t n_own = 0, n_foreign = 0, n_missing = 0;
    auto t = Clock::now();
    if (!read_text(path, text))
        return result;
    t_read = us_since(t);
    const auto report = [&]
    {
        if (timing)
            std::fprintf(stderr,
                "hot: timing compile db read %lld us scan %lld us exists %lld us copy %lld us parse-own %lld us; "
                "bytes %zu own %zu foreign %zu missing %zu\n",
                t_read, t_scan, t_exists, t_copy, t_parse, text.size(), n_own, n_foreign, n_missing);
    };
    // An entry of this backend has a file in the generated directory. The backend parses these
    // entries. It keeps the text of each other entry and writes it back as it is.
    std::string prefix = absolute_path(opt_.generated_dir).string();
    while (!prefix.empty() && (prefix.back() == '/' || prefix.back() == '\\'))
        prefix.pop_back();
    const auto ours = [&prefix](const std::string& file)
    {
        return file.size() > prefix.size() && file.compare(0, prefix.size(), prefix) == 0 &&
            (file[prefix.size()] == '/' || file[prefix.size()] == '\\');
    };
    std::error_code ec;
    std::vector<DbSlice> slices;
    t = Clock::now();
    bool scanned = !prefix.empty() && scan_compile_db(text, slices);
    t_scan = us_since(t);
    if (scanned)
    {
        for (const DbSlice& sl : slices)
        {
            t = Clock::now();
            const bool exists = fs::exists(sl.file, ec);
            t_exists += us_since(t);
            if (!exists)
            {
                ++n_missing;
                continue;
            }
            if (!ours(sl.file))
            {
                t = Clock::now();
                foreign[sl.file] = text.substr(sl.begin, sl.end - sl.begin);
                t_copy += us_since(t);
                ++n_foreign;
                continue;
            }
            t = Clock::now();
            CompileCommandEntry e;
            bool has_arguments = false;
            if (!JsonReader(text.substr(sl.begin, sl.end - sl.begin)).parse_single(e, has_arguments))
            {
                scanned = false;
                break;
            }
            if (has_arguments && !e.file.empty())
                result[e.file] = std::move(e);
            t_parse += us_since(t);
            ++n_own;
        }
        if (scanned)
        {
            report();
            return result;
        }
        result.clear();
        foreign.clear();
    }
    // The scan failed (a damaged file): the full parse as before.
    std::vector<CompileCommandEntry> old;
    if (!JsonReader(text).parse_entries(old))
        return result; // a damaged file: start empty
    for (CompileCommandEntry& e : old)
    {
        if (e.file.empty() || !fs::exists(e.file, ec))
            continue;
        if (!prefix.empty() && !ours(e.file))
        {
            foreign[e.file] = compile_command_entry_json(e);
            continue;
        }
        result[e.file] = std::move(e);
    }
    return result;
}

void ClangFilesBackend::record_compile_command(
    const std::string& unit, const std::string& script_dir, const std::string& object, bool defer)
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
    // The mirror directories "<work_dir>/vfs" and "<work_dir>/vfs.job" belong to this process.
    // The IDE reads the real directories ("-I<dir>") that follow.
    const std::string mirror_arg = "-I" + opt_.work_dir + "/vfs/";
    e.arguments.erase(std::remove_if(e.arguments.begin(), e.arguments.end(),
                          [&](const std::string& a) { return a.compare(0, mirror_arg.size(), mirror_arg) == 0; }),
        e.arguments.end());
    commands_[e.file] = std::move(e);
    compile_db_dirty_ = true;
    if (!defer)
        flush_compile_commands();
}

void ClangFilesBackend::flush_compile_commands()
{
    const PhaseTimer timer("flush_compile_commands");
    if (!compile_db_dirty_ || opt_.compile_commands_dir.empty() || opt_.generated_dir.empty())
        return;
    compile_db_dirty_ = false;
    // HOT_TIMING: the time of each phase, in microseconds, in one line.
    using Clock = std::chrono::steady_clock;
    const bool timing = std::getenv("HOT_TIMING") != nullptr;
    auto tp = Clock::now();
    long long t_exists = 0, t_check = 0, t_read = 0, t_merge = 0, t_text = 0, t_write = 0;
    const auto lap = [&](long long& acc)
    {
        const auto now = Clock::now();
        acc += std::chrono::duration_cast<std::chrono::microseconds>(now - tp).count();
        tp = now;
    };
    struct Report
    {
        bool on;
        long long &a, &b, &c, &d, &e, &g;
        size_t commands;
        ~Report()
        {
            if (on)
                std::fprintf(stderr,
                    "hot: timing flush split own-exists %lld us check %lld us read-db %lld us merge-render %lld us "
                    "text-compare %lld us write %lld us; own entries %zu\n",
                    a, b, c, d, e, g, commands);
        }
    } report{timing, t_exists, t_check, t_read, t_merge, t_text, t_write, commands_.size()};
    std::error_code ec;
    // An entry whose generated file is gone (a removed unit) goes.
    for (auto it = commands_.begin(); it != commands_.end();)
        it = fs::exists(it->first, ec) ? std::next(it) : commands_.erase(it);
    lap(t_exists);
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
    if (!disk_known_)
    {
        // The prewarm read is a result only when the file has the same stamp. The wait is for the
        // read only: the thread never takes "mutex_".
        std::unique_lock<std::mutex> l(pw_mutex_);
        pw_cv_.wait(l, [this] { return !pw_reading_; });
        if (pw_ready_)
        {
            pw_ready_ = false;
            if (stamp(mtime, size) && mtime == pw_mtime_ && size == pw_size_)
            {
                disk_commands_ = std::move(pw_commands_);
                disk_rendered_ = std::move(pw_rendered_);
                disk_mtime_ = pw_mtime_;
                disk_size_ = pw_size_;
                disk_known_ = true;
            }
            pw_commands_.clear();
            pw_rendered_.clear();
        }
    }
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
        for (auto it = disk_rendered_.begin(); same && it != disk_rendered_.end(); ++it)
            same = fs::exists(it->first, ec);
        if (same)
            return;
    }
    // The file is the same as at the last write, but an entry changed. The text of the other
    // entries is the same: the backend keeps it, so it does not read and parse the file again.
    lap(t_check);
    std::map<std::string, CompileCommandEntry> merged;
    std::map<std::string, std::string> rendered;
    std::string old_text;
    if (disk_known_ && stamp(mtime, size) && mtime == disk_mtime_ && size == disk_size_)
    {
        merged = std::move(disk_commands_);
        rendered = std::move(disk_rendered_);
        for (auto it = rendered.begin(); it != rendered.end();)
        {
            if (fs::exists(it->first, ec))
                ++it;
            else
            {
                merged.erase(it->first);
                it = rendered.erase(it);
            }
        }
    }
    else
    {
        // Another process can write the file after the last read. The merge keeps its entries.
        // The window between the read and the rename is short; a lost entry comes back at its
        // next build.
        merged = read_compile_commands(old_text, rendered);
        ++compile_db_reads_;
    }
    lap(t_read);
    disk_known_ = false;
    disk_commands_.clear();
    disk_rendered_.clear();
    for (const auto& [file, entry] : commands_)
    {
        const auto m = merged.find(file);
        if (m != merged.end() && same_entry(m->second, entry) && rendered.count(file))
            continue;
        merged[file] = entry;
        rendered[file] = compile_command_entry_json(entry);
    }
    lap(t_merge);
    std::string text = "[\n";
    size_t n = 0;
    for (const auto& [file, entry] : merged)
        if (!rendered.count(file))
            rendered.emplace(file, compile_command_entry_json(entry));
    for (const auto& [file, entry_text] : rendered) // the map order is the order of "file"
    {
        text += entry_text;
        text += ++n < rendered.size() ? ",\n" : "\n";
    }
    text += "]\n";
    // "old_text" is empty after a fast start: the text of the file is not known, but the stamp is.
    const bool same_as_disk = !old_text.empty() && text == old_text;
    lap(t_text);
    bool stamped = false;
    if (!same_as_disk)
    {
        fs::create_directories(dir, ec);
        // A write failure has no effect on the build. The next build tries again. The write is
        // atomic (a temporary file and a rename).
        if (!write_text_replace(db.string(), text, &disk_mtime_, &disk_size_))
        {
            std::fprintf(stderr, "[script] WARNING: cannot write %s\n", db.string().c_str());
            return;
        }
        ++compile_db_writes_;
        stamped = disk_size_ != static_cast<uintmax_t>(-1);
    }
    lap(t_write);
    // The file now has the text of "merged" and "rendered".
    if (stamped || (same_as_disk && stamp(disk_mtime_, disk_size_)))
    {
        disk_commands_ = std::move(merged);
        disk_rendered_ = std::move(rendered);
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
    const PhaseTimer timer("prepare_cache");
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
    // lld links a module faster than GNU ld (about 50 ms against 150 ms). The probe runs once. The
    // linker version is part of the compiler id, so a new linker makes new module keys. The flag is
    // in the link command, which is in the module key too. Without lld the link stays as before.
    if (opt_.linker_flag.empty() && use_lld_by_default(opt_) && version.find("clang") != std::string::npos)
    {
        std::string ld_out;
        std::vector<std::string> probe = {opt_.compiler};
        if (!opt_.target_triple.empty())
            probe.push_back("--target=" + opt_.target_triple);
        if (!opt_.sysroot.empty())
            probe.push_back("--sysroot=" + opt_.sysroot);
        for (const char* a : {"-fuse-ld=lld", "-Wl,--version", "-x", "c", "/dev/null", "-o", "/dev/null"})
            probe.push_back(a);
        const int ld_rc = run_command(probe, ld_out);
        const size_t at = ld_out.find("LLD ");
        if (ld_rc == 0 && at != std::string::npos)
        {
            opt_.linker_flag = "-fuse-ld=lld";
            compiler_id_ += "|ld:" + ld_out.substr(at, ld_out.find('\n', at) - at);
        }
    }
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

namespace
{

// The file name of an include line ("#include \"x\"" or "#include <x>"). Empty: no name.
std::string include_line_name(const std::string& line)
{
    const size_t open = line.find_first_of("\"<", 8);
    if (open == std::string::npos)
        return std::string();
    const size_t close = line.find(line[open] == '<' ? '>' : '"', open + 1);
    return close == std::string::npos ? std::string() : line.substr(open + 1, close - open - 1);
}

// True when the include line of a unit has a file in the per-directory mirror path of the unit
// ("-I<work_dir>/vfs/<script_dir>", only with a host file system). The shared PCH build has no
// such path, so such a line stays out of the shared union and the unit uses its exact PCH.
bool resolves_in_unit_dir(const ClangFilesOptions& opt, const std::string& script_dir, const std::string& line)
{
    if (!opt.fs || script_dir.empty())
        return false;
    const std::string name = include_line_name(line);
    const std::optional<std::string> rel = vfs_relative(script_dir);
    if (name.empty() || !rel)
        return false;
    std::error_code ec;
    return fs::exists(fs::path(opt.work_dir) / "vfs" / *rel / name, ec);
}

// The prefix without the include lines that resolve in the directory of the unit.
std::string without_unit_dir_lines(const ClangFilesOptions& opt, const std::string& script_dir, const std::string& prefix)
{
    if (!opt.fs || script_dir.empty())
        return prefix;
    std::string r;
    for (size_t pos = 0; pos < prefix.size();)
    {
        const size_t nl = prefix.find('\n', pos);
        const size_t next = nl == std::string::npos ? prefix.size() : nl + 1;
        const std::string line = prefix.substr(pos, next - pos);
        if (line.compare(0, 8, "#include") != 0 || !resolves_in_unit_dir(opt, script_dir, line))
            r += line;
        pos = next;
    }
    return r;
}

} // namespace

// A new random salt: a random nonce and the time. A deleted cache directory cannot reuse an
// old salt.
static std::string new_shared_pch_salt()
{
    std::random_device rd;
    const uint64_t a = (uint64_t(rd()) << 32) ^ rd();
    const uint64_t t = uint64_t(std::chrono::system_clock::now().time_since_epoch().count());
    char buf[64];
    std::snprintf(buf, sizeof buf, "%016llx%016llx", (unsigned long long)a, (unsigned long long)t);
    return buf;
}

// The salt of the shared PCH key: the text in "shared.salt". Each reset of the shared PCH writes a
// new random value, so an old shared entry with the same union text does not hit again. The
// caller holds "pch_mutex_" or reads only.
std::string ClangFilesBackend::shared_pch_salt() const
{
    std::string salt;
    read_text((fs::path(opt_.cache_dir) / "pch" / "shared.salt").string(), salt);
    return salt;
}

ClangFilesBackend::PchPlan ClangFilesBackend::make_pch_plan(
    const std::string& text, const std::string& script_dir_in, uint64_t shadow_hash, bool shared) const
{
    // The shared key does not depend on the script directory: the shared build has no per-directory
    // include path, so one shared PCH serves the units of all directories. It has a reset salt.
    const std::string script_dir = shared ? std::string() : script_dir_in;
    // The key has the header text, the normalized build flags, the compiler id and the shadow
    // hash. The header stamps of the manifest do not see a new header earlier in the include
    // path: with the same include guard, the old PCH would hide it. The shadow hash sees it.
    ObjectCache::KeyInputs inputs;
    inputs.cpp_text = "pch\n" + (shared ? "salt " + shared_pch_salt() + "\n" : std::string()) + text;
    inputs.args = ObjectCache::normalize_args(
        pch_build_command(opt_, "@hdr@", "@pch@", "@dep@", script_dir), opt_.work_dir);
    inputs.compiler_id = compiler_id_;
    inputs.shadow_hash = shadow_hash;
    PchPlan plan;
    plan.own_fmt = fmt_header_only(opt_);
    plan.key = ObjectCache::make_key(inputs);
    plan.prefix = text;
    plan.header = pch_marker(plan.key, ".h");
    return plan;
}

bool ClangFilesBackend::pch_is_bad(const PchPlan& plan, const char* ext) const
{
    std::error_code ec;
    const fs::path bad = pch_marker(plan.key, ext);
    if (!fs::exists(bad, ec))
        return false;
    const auto t = fs::last_write_time(bad, ec);
    if (!ec && fs::file_time_type::clock::now() - t < std::chrono::hours(24))
        return true;
    fs::remove(bad, ec);
    return false;
}

// The first miss of an include set builds the PCH next to its compile, in a thread. The compile
// does not use a PCH and does not wait for the build. The miss after that uses the PCH. A PCH
// that did not work has a "bad" marker: no PCH for one day. The build writes the "seen" marker.
// The shared PCH (prewarm_pch) comes second: when the exact PCH of the unit is missing and every
// include line of the unit is in "<cache_dir>/pch/shared.inc", the shared PCH serves the unit.
// A unit with a line outside the union gives the line to the caller (PchPlan::extra_add). The
// caller adds it to "shared.extra" after the unit compiled. A unit that failed with the shared PCH
// has a "<exact key>.noshared" marker: it does not use the shared PCH for one day.
std::optional<ClangFilesBackend::PchPlan> ClangFilesBackend::plan_pch(
    const std::string& cpp, const std::string& script_dir, bool mirror_files, uint64_t shadow_hash)
{
    // A mirror header of the host file system is in the work directory. Each job writes it
    // again, and it has no stamp. So a compile with mirror files uses no PCH.
    if (mirror_files)
        return std::nullopt;
    // The version line of the driver must name clang ("-include-pch" is a clang flag).
    if (!pch_cache_ || !pch_enabled_by_env() || compiler_id_.find("clang") == std::string::npos)
        return std::nullopt;
    const std::string prefix = pch_header_text(include_prefix(cpp));
    if (prefix.empty())
        return std::nullopt;
    PchPlan plan = make_pch_plan(prefix, script_dir, shadow_hash);
    plan.unit_key = plan.key;
    plan.extra_epoch = pch_extra_epoch_; // the caller holds "pch_mutex_"
    const bool exact_bad = pch_is_bad(plan);
    if (!exact_bad)
    {
        if (const std::optional<ObjectCache::Hit> hit = pch_cache_->lookup(plan.key))
        {
            plan.pch = hit->object.string();
            return plan;
        }
    }
    // A unit with script macros (a .cpp script) uses only its exact PCH. The shared PCH has no
    // such macro, and the unit adds no line to the union.
    if (has_script_macros(prefix))
    {
        plan.why = "unit has script macros: exact pch only";
        if (exact_bad)
            plan.why += "; exact pch has the bad marker";
        else
            plan.build = true;
        return plan;
    }
    std::string shared;
    if (read_text((fs::path(opt_.cache_dir) / "pch" / "shared.inc").string(), shared) && !shared.empty())
    {
        const std::set<std::string> have = pch_lines(shared);
        // The shared key has no shadow hash. prewarm_pch tests the probe paths of the batches
        // once for each batch (shared.shadow): a file at one of them drops the shared PCH. A
        // header that is gone changes a stamp of the manifest.
        std::vector<std::string> outside;
        bool unit_dir = false;
        for (const std::string& line : pch_lines(prefix))
        {
            // A line that resolves in the directory of the unit stays out of the union.
            if (resolves_in_unit_dir(opt_, script_dir, line))
                unit_dir = true;
            else if (!have.count(line))
                outside.push_back(line);
        }
        if (outside.empty() && unit_dir)
        {
            plan.why = "include line resolves in the directory of the unit";
        }
        else if (outside.empty())
        {
            PchPlan sp = make_pch_plan(shared, std::string(), 0, true);
            sp.unit_key = plan.key;
            sp.shared = true;
            sp.extra_epoch = plan.extra_epoch;
            if (pch_is_bad(sp))
                plan.why = "shared pch has the bad marker";
            else if (pch_is_bad(plan, ".noshared"))
                plan.why = "noshared marker";
            else if (const std::optional<ObjectCache::Hit> hit = pch_cache_->lookup(sp.key))
            {
                sp.pch = hit->object.string();
                return sp;
            }
            else
                plan.why = "shared entry missing";
        }
        else
        {
            for (const std::string& line : outside)
                plan.extra_add += line;
            std::string first = outside.front();
            if (!first.empty() && first.back() == '\n')
                first.pop_back();
            plan.why = "union lacks " + std::to_string(outside.size()) + " lines (first: " + first + ")";
        }
    }
    else
        plan.why = "no shared union (shared.inc missing)";
    if (exact_bad)
    {
        plan.why += (plan.why.empty() ? "" : "; ") + std::string("exact pch has the bad marker");
        // No PCH build. The plan carries the extra lines and the reason.
        return plan;
    }
    plan.build = true;
    return plan;
}

namespace
{

// Adds the lines to the file shared.extra, without duplicates. The caller holds "pch_mutex_".
void append_shared_extra(const fs::path& dir, const std::string& lines)
{
    const fs::path path = dir / "shared.extra";
    std::string extra;
    read_text(path.string(), extra);
    const std::set<std::string> known = pch_lines(extra);
    std::string add;
    for (size_t pos = 0; pos < lines.size();)
    {
        const size_t nl = lines.find('\n', pos);
        const size_t next = nl == std::string::npos ? lines.size() : nl + 1;
        const std::string line = lines.substr(pos, next - pos);
        if (!known.count(line))
            add += line;
        pos = next;
    }
    if (add.empty())
        return;
    std::error_code ec;
    fs::create_directories(dir, ec);
    write_text_atomic(path, extra + add);
}

// The lines of a text without the new line, in order, without duplicates.
std::vector<std::string> text_lines(const std::string& text)
{
    std::vector<std::string> r;
    std::set<std::string> seen;
    for (size_t pos = 0; pos < text.size();)
    {
        const size_t nl = text.find('\n', pos);
        const size_t end = nl == std::string::npos ? text.size() : nl;
        std::string line = text.substr(pos, end - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        if (!line.empty() && seen.insert(line).second)
            r.push_back(std::move(line));
    }
    return r;
}

} // namespace

// The shared startup PCH. The units of a batch have different include sets. The first edit
// after the start can have an include set that no unit of the batch has. So one PCH has the
// union of the include lines. The union only grows: it has the old "shared.inc", the lines of the
// batch and the lines of "shared.extra" (lines of units that compiled), in first-seen order. The
// union is in "shared.inc". plan_pch reads it. The shared key has the text of the union only.
// A batch that the union covers writes nothing and builds nothing. A bigger union builds a new
// PCH. A good build writes shared.inc, so the old PCH serves until then. The old PCH stays until
// trim_pch removes it. No PCH when a build runs, when HOT_PCH is "0", or for mirror files.
// The shared key has no shadow hash. A file that appears later at a probe path (a path that did
// not exist at parse time) can hide behind the old PCH: the include guard of the old header stays
// in the PCH, and the unit compiles with no error against the old header. So the file
// "shared.shadow" keeps all probe paths of all batches (it only grows). Once for each batch,
// prewarm_pch tests whether a listed path has a file now. If one has, the shared PCH entry, shared.inc
// and shared.extra go, the union and the list restart from the lines of the batch, and the PCH is
// built again by the normal rules. A growth of the list alone builds no PCH.
// Startup rule: the first prewarm_pch call of the backend is the startup batch. It starts its
// build at once. A later call (an edit batch) queues the build in "pch_pending_". The last unit
// of the batch starts it (start_pending_pch), so the build does not slow the compiles of the batch.
// The probe paths of a batch are in a set, so the list has each path once.
// Limit: only begin_load_many (prewarm_pch) tests the probe paths. A load() or build() call does
// not test them. The rbfx-script host uses begin_load_many only. A file at a probe path then shows
// at the next begin_load_many batch.
// The shared key does not depend on the script directory (the shared build has no per-directory
// include path). A line that resolves in the per-directory mirror path of a unit stays out of the
// union. The unit with such a line uses its own exact PCH.
// Each reset adds one to "shared.salt", and the shared key has the salt: an older shared entry
// with the same union text does not hit again.
void ClangFilesBackend::snapshot_pch_shadow(std::vector<std::string>& paths, uint64_t& generation)
{
    if (!pch_shadow_loaded_)
    {
        pch_shadow_loaded_ = true;
        std::string text;
        read_text((fs::path(opt_.cache_dir) / "pch" / "shared.shadow").string(), text);
        for (const std::string& line : text_lines(text))
            pch_shadow_.insert(line);
    }
    paths.assign(pch_shadow_.begin(), pch_shadow_.end());
    generation = pch_shadow_gen_;
}

bool ClangFilesBackend::apply_pch_shadow(
    const std::vector<std::string>& batch, bool exists, const std::set<std::string>& batch_existing)
{
    const size_t old_size = pch_shadow_.size();
    if (exists)
        pch_shadow_.clear();
    for (const std::string& path : batch)
        if (!exists || !batch_existing.count(path)) // after a reset: a path with a file is no probe
            pch_shadow_.insert(path);
    if (exists || pch_shadow_.size() != old_size)
    {
        ++pch_shadow_gen_;
        if (!exists) // after a reset the caller writes the file last
            write_pch_shadow();
    }
    return exists;
}

void ClangFilesBackend::write_pch_shadow()
{
    std::string text;
    for (const std::string& path : pch_shadow_)
        text += path + "\n";
    const fs::path file = fs::path(opt_.cache_dir) / "pch" / "shared.shadow";
    std::error_code dir_ec;
    fs::create_directories(file.parent_path(), dir_ec);
    write_text_atomic(file, text);
}

void ClangFilesBackend::prewarm_pch(const std::vector<const BuildJob*>& jobs, bool timing)
{
    const PhaseTimer timer("prewarm_pch", timing);
    if (jobs.empty() || !pch_cache_ || !pch_enabled_by_env() || compiler_id_.find("clang") == std::string::npos)
        return;
    std::vector<std::string> prefixes;
    std::vector<std::string> batch_shadow;
    for (const BuildJob* job : jobs)
    {
        if (opt_.fs && !job->tr.virtual_files.empty())
            return;
        const std::string prefix = include_prefix(job->tr.cpp);
        // A unit with script macros has only its exact PCH (see plan_pch).
        if (has_script_macros(pch_header_text(prefix)))
            continue;
        prefixes.push_back(without_unit_dir_lines(opt_, job->script_dir, prefix));
        batch_shadow.insert(batch_shadow.end(), job->tr.shadow_paths.begin(), job->tr.shadow_paths.end());
    }
    if (prefixes.empty())
        return;
    const fs::path dir = fs::path(opt_.cache_dir) / "pch";
    // The probe path test does the stat calls (many) without "pch_mutex_". A change of the list
    // while they run (the generation) means a new test. The epoch changes only below, under the lock.
    std::unique_lock<std::mutex> lock(pch_mutex_);
    bool shadow_exists = false;
    std::set<std::string> batch_existing;
    for (;;)
    {
        std::vector<std::string> listed;
        uint64_t generation = 0;
        snapshot_pch_shadow(listed, generation);
        lock.unlock();
        std::error_code ec;
        shadow_exists = false;
        batch_existing.clear();
        shadow_exists = detail::find_any_parallel(listed, [](const std::string& path) {
            std::error_code probe_ec;
            return fs::exists(path, probe_ec);
        }) != listed.size();
        if (shadow_exists)
            for (const std::string& path : batch_shadow)
                if (fs::exists(path, ec))
                    batch_existing.insert(path);
        lock.lock();
        if (generation == pch_shadow_gen_)
            break;
    }
    const bool startup = !pch_startup_done_;
    pch_startup_done_ = true;
    pch_defer_ = !startup;
    if (pch_thread_.joinable() && pch_done_)
    {
        pch_thread_.join();
        pch_running_ = false;
    }
    std::string old_text;
    std::string extra;
    read_text((dir / "shared.inc").string(), old_text);
    read_text((dir / "shared.extra").string(), extra);
    if (apply_pch_shadow(batch_shadow, shadow_exists, batch_existing))
    {
        // A file is at a probe path of an old batch. The shared PCH can hide it. Drop the
        // union, its entry and the extra lines. A running shared build does not write shared.inc
        // (the epoch changed) and removes its entry.
        // Order: the new salt first, then the entry, shared.inc and shared.extra, and the list
        // last. A crash between two steps never leaves an old entry that can hit with the new
        // state: the old key has the old salt.
        std::error_code ec;
        std::optional<PchPlan> old_plan;
        if (!old_text.empty())
            old_plan = make_pch_plan(old_text, std::string(), 0, true);
        fs::create_directories(dir, ec);
        write_text_atomic(dir / "shared.salt", new_shared_pch_salt());
        if (old_plan)
            pch_cache_->remove(old_plan->key);
        fs::remove(dir / "shared.inc", ec);
        fs::remove(dir / "shared.extra", ec);
        write_pch_shadow();
        ++pch_extra_epoch_;
        pch_pending_.erase(std::remove_if(pch_pending_.begin(), pch_pending_.end(),
                               [](const PendingPch& p) { return p.plan.shared; }),
            pch_pending_.end());
        old_text.clear();
        extra.clear();
        if (timing)
            std::fprintf(stderr, "hot: a file is at a probe path of the shared PCH: the shared PCH is reset\n");
    }
    if (pch_running_)
        return; // the running build owns the shared key: do nothing
    // Old lines first: the order of the union stays the same, so the key stays the same. A
    // queued plan of an earlier batch has lines that the file does not have yet.
    prefixes.insert(prefixes.begin(), old_text);
    for (const PendingPch& p : pch_pending_)
        if (p.plan.shared)
            prefixes.insert(prefixes.begin() + 1, p.plan.prefix);
    prefixes.push_back(extra);
    const std::string text = union_pch_header(prefixes);
    if (text.empty())
        return;
    PchPlan plan = make_pch_plan(text, std::string(), 0, true);
    plan.shared = true;
    const bool same = text == old_text;
    if (pch_is_bad(plan))
        return;
    if (pch_cache_->lookup(plan.key))
    {
        if (!same)
        {
            std::error_code ec;
            fs::create_directories(dir, ec);
            write_text_atomic(dir / "shared.inc", text);
        }
        return;
    }
    plan.build = true;
    if (startup)
    {
        start_pch_build(plan, std::string(), "shared", timing);
        return;
    }
    // A newer shared plan replaces an older one: it has the old lines too.
    pch_pending_.erase(std::remove_if(pch_pending_.begin(), pch_pending_.end(),
                           [](const PendingPch& p) { return p.plan.shared; }),
        pch_pending_.end());
    pch_pending_.insert(pch_pending_.begin(), PendingPch{plan, std::string(), "shared", timing});
}

void ClangFilesBackend::start_pending_pch()
{
    std::lock_guard<std::mutex> lock(pch_mutex_);
    pch_defer_ = false;
    if (pch_pending_.empty())
        return;
    if (pch_thread_.joinable() && pch_done_)
    {
        pch_thread_.join();
        pch_running_ = false;
    }
    if (pch_running_)
        return; // the plans stay: the next batch end tries again
    PendingPch p = std::move(pch_pending_.front());
    pch_pending_.clear();
    // The caller can be a noexcept destructor: no exception leaves this function. A thread that
    // does not start (or bad_alloc) means no PCH build.
    try
    {
        start_pch_build(p.plan, p.script_dir, p.unit, p.timing);
    }
    catch (const std::exception& e)
    {
        pch_running_ = false;
        pch_done_ = true;
        pch_pending_.clear();
        std::fprintf(stderr, "hot: %s has no PCH: cannot start the PCH build: %s\n", p.unit.c_str(), e.what());
    }
    catch (...)
    {
        pch_running_ = false;
        pch_done_ = true;
        pch_pending_.clear();
        std::fprintf(stderr, "hot: %s has no PCH: cannot start the PCH build\n", p.unit.c_str());
    }
}

namespace
{

// The banner of the translator before the function bodies of a .hot unit.
const char* const kBodiesBanner = "// ---------------------------------------------------------------- function bodies\n";


// ---- two objects for a .hot unit (design D)
//
// The text of a unit has four parts: P (everything before the bodies, without the record and entry
// ranges), R (the record and entry ranges: slots, record, _create, _make_N, constructor table),
// B (the function bodies) and T (script_register). The shape PCH has P. One compile does B and one
// does R and T, both with the shape PCH: only the shape object defines the record, and each object
// emits only the static and inline code that it uses. A linker -r step joins the two objects.
// The text edits make the unit state shared between the two objects:
//  - GEN, hot_live, *_slot, *_fields and *_layout become "inline" variables (one copy in the module);
//  - a function that P declares and B defines gets external linkage (HOT_UNIT_FN / HOT_UNIT_INLINE),
//    so R and T can call it from the other object.
// Any text that does not have the expected form makes the split invalid: the unit then uses the
// shape PCH with one object, or the whole unit.
// The version of the split transform. It is in the object key and in the shape PCH key: a change of
// the transform makes the old joined objects invalid.
const char* const kSplitVersion = "hot-split=2obj-v1";

// The tag in the object key: the version with the two object decision.
std::string split_object_tag(bool two_objects)
{
    return two_objects ? std::string(kSplitVersion) : std::string("hot-split=off");
}

struct UnitSplit
{
    std::string header; // the shape header: P with the macros
    std::string main; // B
    std::string shape; // R and T
    std::vector<std::string> fns; // the functions that got external linkage
};

bool starts_with(const std::string& s, const char* prefix)
{
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

// The name of a function prototype or definition line: the identifier before the first '('.
std::string function_name_of(const std::string& line, size_t from)
{
    const size_t paren = line.find('(', from);
    if (paren == std::string::npos || paren == 0)
        return std::string();
    size_t b = paren;
    while (b > from && (std::isalnum(static_cast<unsigned char>(line[b - 1])) || line[b - 1] == '_'))
        --b;
    return line.substr(b, paren - b);
}

bool split_unit_into_two(const std::string& text, const std::string& name, UnitSplit& out)
{
    const std::string bodies_banner = kBodiesBanner;
    const std::string record_banner = "// ------------------------------------------------------ record and entry\n";
    const std::string script_register = "extern \"C\" void script_register(hot::Registry& reg)\n";
    std::vector<std::string> lines;
    for (size_t at = 0; at < text.size();)
    {
        size_t e = text.find('\n', at);
        e = e == std::string::npos ? text.size() : e + 1;
        lines.push_back(text.substr(at, e - at));
        at = e;
    }
    size_t bodies = lines.size(), reg = lines.size();
    std::vector<std::pair<size_t, size_t>> records; // [begin, end) of each record and entry range
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (lines[i] == bodies_banner)
        {
            if (bodies != lines.size())
                return false; // two banners
            bodies = i;
        }
        else if (lines[i] == script_register)
        {
            if (reg != lines.size())
                return false;
            reg = i;
        }
        else if (lines[i] == record_banner)
        {
            size_t j = i + 1;
            while (j < lines.size() && !starts_with(lines[j], "// ----------------"))
                ++j;
            records.emplace_back(i, j);
            i = j - 1;
        }
    }
    if (bodies == lines.size() || reg == lines.size() || reg < bodies || records.empty() || records.back().second > bodies)
        return false;
    // Prototypes of P that have a definition in B.
    const char* const kinds[] = {"[[maybe_unused]] HOT_HIDDEN static ", "static "};
    auto kind_of = [&](const std::string& l) -> int {
        for (int k = 0; k < 2; ++k)
            if (starts_with(l, kinds[k]))
            {
                if (k == 1)
                    for (const char* skip : {"const ", "void*", "uint32_t", "constexpr", "inline ", "assert"})
                        if (l.compare(std::strlen(kinds[k]), std::strlen(skip), skip) == 0)
                            return -1;
                return k;
            }
        return -1;
    };
    // A prototype ends with ");" (a comment after it does not matter).
    auto is_proto = [](const std::string& l) {
        std::string t = l.substr(0, l.find("//"));
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back())))
            t.pop_back();
        return t.size() > 2 && t.compare(t.size() - 2, 2, ");") == 0;
    };
    // The signature of a function line: the text after the kind up to the last ')' (the full
    // prototype, so overloads stay apart; a comment and the end of the line do not count).
    auto sig_of = [&](const std::string& l, int k) {
        const std::string t = l.substr(0, l.find("//"));
        const size_t close = t.rfind(')');
        return close == std::string::npos ? std::string() : t.substr(std::strlen(kinds[k]), close + 1 - std::strlen(kinds[k]));
    };
    std::set<std::pair<std::string, int>> protos;
    std::vector<std::string> p_lines;
    std::vector<size_t> p_orig; // the line number (0 based) of each line of P in the unit
    size_t rec = 0;
    for (size_t i = 0; i < bodies; ++i)
    {
        if (rec < records.size() && i == records[rec].first)
        {
            i = records[rec].second - 1;
            ++rec;
            continue;
        }
        p_lines.push_back(lines[i]);
        p_orig.push_back(i);
    }
    // The key of a function is its full prototype: overloads are different functions. Two equal
    // prototypes make the split invalid.
    for (const std::string& l : p_lines)
    {
        const int k = kind_of(l);
        if (k >= 0 && is_proto(l) && !protos.insert({sig_of(l, k), k}).second)
            return false;
    }
    std::set<std::pair<std::string, int>> defined;
    for (size_t i = bodies; i < reg; ++i)
    {
        const int k = kind_of(lines[i]);
        if (k < 0 || is_proto(lines[i]) || lines[i].find(");") != std::string::npos)
            continue;
        const auto key = std::make_pair(sig_of(lines[i], k), k);
        if (protos.count(key) && !defined.insert(key).second)
            return false; // two definitions of one prototype
    }
    // A definition in P of a function that B defines is not split.
    for (const std::string& l : p_lines)
    {
        const int k = kind_of(l);
        if (k >= 0 && !is_proto(l) && l.find('(') != std::string::npos && defined.count({sig_of(l, k), k}))
            return false;
    }
    auto convert = [&](std::string& l, bool in_p) {
        const int k = kind_of(l);
        if (k < 0 || (in_p != is_proto(l)))
            return;
        const std::string n = function_name_of(l, std::strlen(kinds[k]));
        if (defined.count({sig_of(l, k), k}))
        {
            l = (k == 0 ? "[[maybe_unused]] HOT_UNIT_INLINE " : "HOT_UNIT_FN ") + l.substr(std::strlen(kinds[k]));
            if (in_p)
                out.fns.push_back(n);
        }
    };
    int gen = 0, live = 0;
    size_t macro_at = p_lines.size();
    for (size_t i = 0; i < p_lines.size(); ++i)
    {
        std::string& l = p_lines[i];
        if (starts_with(l, "[[maybe_unused]] static uint32_t GEN = "))
        {
            l = "[[maybe_unused]] HOT_UNIT_VAR " + l.substr(std::strlen("[[maybe_unused]] static "));
            ++gen;
            macro_at = std::min(macro_at, i);
        }
        else if (starts_with(l, "static uint32_t hot_live = "))
        {
            l = "HOT_UNIT_VAR " + l.substr(7);
            ++live;
        }
        else if (starts_with(l, "static void* ") && l.find("_slot = ") != std::string::npos)
            l = "HOT_UNIT_VAR " + l.substr(7);
        else if ((starts_with(l, "static const hot::FieldInfo ") && l.find("_fields[] =") != std::string::npos)
            || (starts_with(l, "static const hot::StateLayout ") && l.find("_layout =") != std::string::npos))
            l = "HOT_UNIT_VAR const " + l.substr(std::strlen("static const "));
        else
            convert(l, true);
    }
    if (gen != 1 || live != 1)
        return false;
    {
        std::vector<std::string> fns = std::move(out.fns);
        out = UnitSplit();
        out.fns = std::move(fns);
    }
    out.header = "#define HOT_SPLIT_UNIT 1\n#line 1 \"" + name + "\"\n";
    for (size_t i = 0; i < p_lines.size(); ++i)
    {
        bool reline = i > 0 && p_orig[i] != p_orig[i - 1] + 1; // a range was removed before this line
        if (i == macro_at)
        {
            out.header += "#ifdef HOT_SPLIT_UNIT\n#define HOT_UNIT_VAR inline\n#define HOT_UNIT_FN\n"
                          "#define HOT_UNIT_INLINE HOT_NODEBUG\n#else\n#define HOT_UNIT_VAR static\n"
                          "#define HOT_UNIT_FN static\n#define HOT_UNIT_INLINE HOT_HIDDEN static\n#endif\n";
            reline = true;
        }
        // The line numbers of P stay the ones of the unit (__LINE__, messages).
        if (reline)
            out.header += "#line " + std::to_string(p_orig[i] + 1) + " \"" + name + "\"\n";
        out.header += p_lines[i];
    }
    out.main = "#line " + std::to_string(bodies + 1) + " \"" + name + "\"\n";
    for (size_t i = bodies; i < reg; ++i)
    {
        std::string l = lines[i];
        convert(l, false);
        out.main += l;
    }
    for (const auto& range : records)
    {
        out.shape += "#line " + std::to_string(range.first + 1) + " \"" + name + "\"\n";
        for (size_t i = range.first; i < range.second; ++i)
            out.shape += lines[i];
    }
    out.shape += "#line " + std::to_string(reg + 1) + " \"" + name + "\"\n";
    for (size_t i = reg; i < lines.size(); ++i)
        out.shape += lines[i];
    return true;
}

struct ElfSym
{
    std::string name;
    unsigned char bind = 0;
    unsigned char type = 0;
    bool defined = false;
    bool writable = false; // defined in a section with SHF_WRITE (.data, .bss, .tdata, .tbss)
    uint16_t shndx = 0;
    uint64_t value = 0;
    uint64_t size = 0;
    bool used = false; // a relocation of code or data (not of a dynamic initializer) names the symbol
};

// All named symbols of the .symtab of an ELF64 file. False when the file is not an ELF64 file.
bool elf_symbols(const std::string& path, std::vector<ElfSym>& syms)
{
#if defined(__linux__)
    std::ifstream in(path, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Elf64_Ehdr eh;
    if (data.size() < sizeof eh)
        return false;
    std::memcpy(&eh, data.data(), sizeof eh);
    if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_shoff == 0
        || eh.e_shentsize != sizeof(Elf64_Shdr) || eh.e_shoff > data.size()
        || uint64_t(eh.e_shnum) * sizeof(Elf64_Shdr) > data.size() - eh.e_shoff)
        return false;
    auto section = [&](size_t i) {
        Elf64_Shdr sh;
        std::memcpy(&sh, data.data() + eh.e_shoff + i * sizeof(Elf64_Shdr), sizeof sh);
        return sh;
    };
    auto in_file = [&](uint64_t off, uint64_t len) { return off <= data.size() && len <= data.size() - off; };
    for (size_t i = 0; i < eh.e_shnum; ++i)
    {
        const Elf64_Shdr sh = section(i);
        if (sh.sh_type != SHT_SYMTAB || sh.sh_link >= eh.e_shnum || !in_file(sh.sh_offset, sh.sh_size))
            continue;
        const Elf64_Shdr str = section(sh.sh_link);
        if (!in_file(str.sh_offset, str.sh_size))
            continue;
        for (uint64_t off = 0; off + sizeof(Elf64_Sym) <= sh.sh_size; off += sizeof(Elf64_Sym))
        {
            Elf64_Sym sym;
            std::memcpy(&sym, data.data() + sh.sh_offset + off, sizeof sym);
            ElfSym e; // an unnamed symbol stays in the list (the index of a relocation) with an empty name
            if (sym.st_name != 0 && sym.st_name < str.sh_size)
                e.name.assign(data.c_str() + str.sh_offset + sym.st_name,
                    strnlen(data.c_str() + str.sh_offset + sym.st_name, str.sh_size - sym.st_name));
            e.bind = ELF64_ST_BIND(sym.st_info);
            e.type = ELF64_ST_TYPE(sym.st_info);
            e.defined = sym.st_shndx != SHN_UNDEF;
            e.shndx = sym.st_shndx;
            e.value = sym.st_value;
            e.size = sym.st_size;
            e.writable = e.defined && sym.st_shndx < eh.e_shnum && (section(sym.st_shndx).sh_flags & SHF_WRITE) != 0;
            syms.push_back(std::move(e));
        }
        // The relocations: which symbols the code and the data of the object use. The sections of the
        // dynamic initializers (.text.startup, .init_array), the debug data and the unwind tables do
        // not count.
        const size_t first_sym = syms.size() - sh.sh_size / sizeof(Elf64_Sym);
        // The data symbols of each section. The assembler writes a relocation against a local variable
        // as "section symbol + offset", so a use is found by the offset.
        std::map<uint16_t, std::vector<size_t>> data_in_section; // sorted by the value
        std::map<uint16_t, uint64_t> longest; // the biggest symbol of each section
        for (size_t k = first_sym; k < syms.size(); ++k)
            if (syms[k].bind == STB_LOCAL && (syms[k].type == STT_OBJECT || syms[k].type == STT_TLS) && syms[k].defined)
            {
                data_in_section[syms[k].shndx].push_back(k);
                longest[syms[k].shndx] = std::max<uint64_t>(longest[syms[k].shndx], std::max<uint64_t>(syms[k].size, 1));
            }
        for (auto& entry : data_in_section)
            std::sort(entry.second.begin(), entry.second.end(),
                [&](size_t a, size_t b) { return syms[a].value < syms[b].value; });
        if (eh.e_shstrndx >= eh.e_shnum)
            continue;
        const Elf64_Shdr names_section = section(eh.e_shstrndx);
        if (!in_file(names_section.sh_offset, names_section.sh_size))
            continue;
        for (size_t k = 0; k < eh.e_shnum; ++k)
        {
            const Elf64_Shdr rel = section(k);
            if (rel.sh_type != SHT_RELA || rel.sh_link != i || rel.sh_info >= eh.e_shnum
                || !in_file(rel.sh_offset, rel.sh_size))
                continue;
            const Elf64_Shdr target = section(rel.sh_info);
            if (target.sh_name >= names_section.sh_size)
                continue;
            const std::string tname(data.c_str() + names_section.sh_offset + target.sh_name,
                strnlen(data.c_str() + names_section.sh_offset + target.sh_name, names_section.sh_size - target.sh_name));
            if (starts_with(tname, ".text.startup") || starts_with(tname, ".init_array") || starts_with(tname, ".debug")
                || starts_with(tname, ".eh_frame") || starts_with(tname, ".gcc_except") || starts_with(tname, ".llvm")
                || starts_with(tname, ".fini_array") || starts_with(tname, ".ctors"))
                continue;
            for (uint64_t off = 0; off + sizeof(Elf64_Rela) <= rel.sh_size; off += sizeof(Elf64_Rela))
            {
                Elf64_Rela ra;
                std::memcpy(&ra, data.data() + rel.sh_offset + off, sizeof ra);
                const size_t idx = first_sym + ELF64_R_SYM(ra.r_info);
                if (idx >= syms.size())
                    continue;
                if (syms[idx].type != STT_SECTION)
                {
                    syms[idx].used = true;
                    continue;
                }
                const auto in_section = data_in_section.find(syms[idx].shndx);
                if (in_section == data_in_section.end())
                    continue;
                // x86-64. The target of a pc relative relocation is S + A + 4 + (the size of the immediate
                // that follows the displacement: 0, 1, 2 or 4), so the offset is in [A + 4, A + 8]. All
                // types except the absolute ones use this interval; this covers the TLS general and local
                // dynamic types (19, 20) and the initial exec type (22) too. An absolute type (64, 32, 32S,
                // DTPOFF32, TPOFF32) and PC64 name the offset itself. A symbol is used when its range
                // meets the interval: a neighbour can be marked too, which only refuses a split. Other
                // machines: every data symbol of the section counts as used.
                const uint32_t type = ELF64_R_TYPE(ra.r_info);
                const bool exact = type == 1 || type == 10 || type == 11 || type == 21 || type == 23 || type == 24;
                const std::vector<size_t>& sorted = in_section->second;
                if (eh.e_machine != EM_X86_64)
                {
                    for (size_t k : sorted)
                        syms[k].used = true;
                    continue;
                }
                const int64_t qlo = exact ? ra.r_addend : ra.r_addend + 4;
                const int64_t qhi = exact ? ra.r_addend : ra.r_addend + 8;
                const int64_t reach = static_cast<int64_t>(longest[syms[idx].shndx]);
                // The first symbol that starts after the interval; the ones before it can meet it.
                auto it = std::upper_bound(sorted.begin(), sorted.end(), qhi,
                    [&](int64_t v, size_t k) { return v < static_cast<int64_t>(syms[k].value); });
                while (it != sorted.begin())
                {
                    --it;
                    const int64_t lo = static_cast<int64_t>(syms[*it].value);
                    if (lo + reach <= qlo)
                        break; // no symbol before this one reaches the interval
                    if (lo + static_cast<int64_t>(std::max<uint64_t>(syms[*it].size, 1)) > qlo)
                        syms[*it].used = true;
                }
            }
        }
    }
    return true;
#else
    (void)path;
    (void)syms;
    return false;
#endif
}

// The names of the local data symbols of an ELF64 object that are shared state of the unit. P is in
// BOTH objects, and the record object runs user code too (the constructor of a shell and the field
// initializers). A mutable local variable that both objects define is a second copy: before the
// split the unit had one copy, so the split is wrong then. Looked at:
//  - the unit state by name: GEN, hot_live, *_slot, *_fields, *_layout (also with "_ZL<n>");
//  - each local object symbol in a writable section (SHF_WRITE) that the object uses: a static or anonymous-namespace
//    variable of a header, a static local of a static function (_ZZ), its guard (_ZGV), a thread
//    local variable.
// Not looked at (harmless, because the copies are equal or they are only caches):
//  - a symbol that no code or data of the object uses (a relocation of .text or .data; a dynamic
//    initializer does not count): its copy is dead. This is the case for a "static const" object
//    of a header that this object does not read, for example a string constant;
//  - symbols in a read only section (constant tables and strings of P: equal copies, nothing writes);
//  - "__site" symbols (FieldSite and FnSite caches: each object may fill its own, the epoch checks
//    them) and the "__construct_keys" arrays (see is_cache_symbol);
//  - weak and global symbols (inline variables and template statics: the linker keeps one);
//  - local functions: a function that both objects define is a copy of the same code. The limit:
//    the address of such a function can differ between the objects (no check of relocations).
// False when the file is not an ELF64 file.
bool is_cache_symbol(const std::string& name)
{
    // "__construct_keys": a "static const char* const" array that the translator writes with
    // typeid(..).name(). The compiler fills it at load time (so it is in .bss), but it is const and
    // each object gets the same strings: the record uses one copy, and no code writes to it.
    const std::string keys = "__construct_keys";
    return name.find("__site") != std::string::npos
        || (name.size() >= keys.size() && name.compare(name.size() - keys.size(), keys.size(), keys) == 0);
}

bool unit_state_symbols(const std::string& path, std::set<std::string>& names)
{
    std::vector<ElfSym> syms;
    if (!elf_symbols(path, syms))
        return false;
    for (const ElfSym& e : syms)
    {
        if (e.name.empty() || e.bind != STB_LOCAL || (e.type != STT_OBJECT && e.type != STT_TLS))
            continue;
        std::string base = e.name;
        if (starts_with(base, "_ZL"))
        {
            size_t d = 3;
            while (d < base.size() && std::isdigit(static_cast<unsigned char>(base[d])))
                ++d;
            base = base.substr(d);
        }
        auto ends = [&](const char* suffix) {
            const size_t n = std::strlen(suffix);
            return base.size() >= n && base.compare(base.size() - n, n, suffix) == 0;
        };
        const bool by_name = base == "GEN" || base == "hot_live" || ends("_slot") || ends("_fields") || ends("_layout");
        if (by_name || (e.writable && e.used && !is_cache_symbol(e.name)))
            names.insert(e.name);
    }
    return true;
}

// The check of the joined object. A problem text is returned (empty: the object is right):
//  - a function that P declares with external linkage (fns) must have a defined symbol;
//  - no undefined symbol may have the name of a local function or variable that the object defines
//    (the other object used a name that stayed internal in this one: the link would fail);
// "_ZL<n>name" is the internal name of "_Z<n>name". Needs an ELF64 object.
std::string merged_object_problem(const std::string& path, const std::vector<std::string>& fns)
{
    std::vector<ElfSym> syms;
    if (!elf_symbols(path, syms))
        return "cannot read the symbols of the joined object";
    std::set<std::string> undefined, local_external_names;
    std::vector<std::string> defined_global;
    for (const ElfSym& e : syms)
    {
        if (e.name.empty())
            continue;
        if (!e.defined)
        {
            undefined.insert(e.name);
            continue;
        }
        if (e.bind == STB_LOCAL && (e.type == STT_FUNC || e.type == STT_OBJECT))
        {
            local_external_names.insert(e.name);
            if (starts_with(e.name, "_ZL"))
                local_external_names.insert("_Z" + e.name.substr(3));
        }
        else if (e.bind != STB_LOCAL)
            defined_global.push_back(e.name);
    }
    for (const std::string& n : local_external_names)
        if (undefined.count(n))
            return "the symbol " + n + " is internal in one object and used from the other";
    for (const std::string& fn : fns)
    {
        const std::string mangled = std::to_string(fn.size()) + fn;
        bool found = false;
        for (const std::string& d : defined_global)
            if (d == fn || d.find(mangled) != std::string::npos)
            {
                found = true;
                break;
            }
        if (!found)
            return "the function " + fn + " has no definition in the joined object";
    }
    return std::string();
}

// The flags that stop the join step: LTO makes bitcode objects, and a linker other than lld may
// not know "-r" the same way.
bool two_object_flags_ok(const std::vector<std::string>& args)
{
    for (const std::string& a : args)
    {
        if (starts_with(a, "-flto"))
            return false;
        if (starts_with(a, "-fuse-ld=") && a != "-fuse-ld=lld")
            return false;
    }
    return true;
}

// The linker of the join step: "ld.lld" next to the compiler, else in the PATH. Empty: none.
std::string find_lld(const std::string& compiler)
{
    std::error_code ec;
    // HOT_LLD names the tool (a test uses it for a tool that fails).
    if (const char* forced = std::getenv("HOT_LLD"); forced && *forced)
        return forced;
    const fs::path beside = fs::path(compiler).parent_path() / "ld.lld";
    if (!fs::path(compiler).parent_path().empty() && fs::exists(beside, ec))
        return beside.string();
    if (const char* path = std::getenv("PATH"))
    {
        std::stringstream ss(path);
        std::string dir;
        while (std::getline(ss, dir, ':'))
            if (!dir.empty() && fs::exists(fs::path(dir) / "ld.lld", ec))
                return (fs::path(dir) / "ld.lld").string();
    }
    return std::string();
}

std::atomic<unsigned> g_running_compiles{0};

// Gives the place back when it ends (also on an exception).
struct PlaceGuard
{
    bool held = true;
    ~PlaceGuard()
    {
        if (held)
            --g_running_compiles;
    }
};

// Joins a thread when it ends: a joinable thread that is destroyed ends the program.
struct JoinGuard
{
    std::thread& thread;
    ~JoinGuard()
    {
        if (thread.joinable())
            thread.join();
    }
};

int run_counted(const std::vector<std::string>& cmd, std::string& output)
{
    ++g_running_compiles;
    PlaceGuard place;
    return run_command(cmd, output);
}

// Takes one place for a compile that runs next to others. False: the job limit is reached.
bool reserve_compile_place()
{
    const unsigned before = g_running_compiles.fetch_add(1);
    if (before + 1 < build_jobs())
        return true;
    --g_running_compiles;
    return false;
}

// Removes the files when it ends (also on an exception).
struct TempFiles
{
    std::vector<std::string> files;
    ~TempFiles()
    {
        std::error_code ec;
        for (const std::string& f : files)
            fs::remove(f, ec);
    }
};

std::string shape_hash(const std::string& text)
{
    char buf[40];
    std::snprintf(buf, sizeof buf, "%016llx%016llx", static_cast<unsigned long long>(hotc::text_hash("shape1\n" + text)),
        static_cast<unsigned long long>(hotc::text_hash(text + "\nshape2")));
    return buf;
}

} // namespace

void evict_shape_files(const std::string& dir_name, const std::string& unit_id, size_t keep_unit, size_t keep_all)
{
    const fs::path dir(dir_name);
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    // An explicit loop: the increment of a directory iterator must not throw in a thread.
    fs::directory_iterator it(dir, ec), end;
    for (; !ec && it != end; it.increment(ec))
    {
        const fs::path& path = it->path();
        const std::string ext = path.extension().string();
        std::error_code time_ec;
        const auto mtime = it->last_write_time(time_ec);
        if (time_ec)
            continue;
        const auto age = now - mtime;
        if ((ext == ".tmp" && age > std::chrono::hours(1)) || (ext == ".nosplit" && age > std::chrono::hours(24)))
            fs::remove(path, time_ec); // a build that died, or an old marker
        else if (ext == ".pch")
            files.emplace_back(mtime, path);
    }
    // The newest first. A hit sets the time of its PCH, so this is the order of the last use.
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    size_t unit_seen = 0;
    for (size_t i = 0; i < files.size(); ++i)
    {
        const bool mine = !unit_id.empty() && files[i].second.filename().string().rfind(unit_id + "-", 0) == 0;
        if ((mine && ++unit_seen > keep_unit) || i >= keep_all)
            for (const char* ext : {".pch", ".h", ".nosplit"}) // unlink: a reader keeps its handle on ELF
                fs::remove(fs::path(files[i].second).replace_extension(ext), ec);
    }
}

std::string split_object_tag_of(bool two_objects)
{
    return split_object_tag(two_objects);
}

bool two_object_flags_ok_of(const std::vector<std::string>& args)
{
    return two_object_flags_ok(args);
}

std::string merged_object_problem_of(const std::string& path, const std::vector<std::string>& fns)
{
    return merged_object_problem(path, fns);
}

std::string find_lld_of(const std::string& compiler)
{
    return find_lld(compiler);
}

bool split_unit_for_test(const std::string& text, const std::string& name, std::string& header, std::string& main,
    std::string& shape, std::vector<std::string>& fns)
{
    UnitSplit u;
    if (!split_unit_into_two(text, name, u))
        return false;
    header = u.header;
    main = u.main;
    shape = u.shape;
    fns = u.fns;
    return true;
}

bool unit_state_symbols_of(const std::string& object_path, std::set<std::string>& names)
{
    return unit_state_symbols(object_path, names);
}

ClangFilesBackend::ShapeSplit ClangFilesBackend::plan_shape(const std::string& cpp_text, const std::string& unit_cpp,
    const std::string& base_pch, const std::string& base_key, const std::vector<std::string>& args, bool is_hot,
    const std::string& script_dir, bool two_objects) const
{
    ShapeSplit s;
    const char* off = std::getenv("HOT_SHAPE_PCH");
    if (!is_hot || (off && std::strcmp(off, "0") == 0) || opt_.cache_dir.empty() || base_pch.empty())
        return s;
    // The translator writes the banner once. A second banner (in a raw string or a comment of the
    // user) makes the place unsafe: the unit then compiles whole.
    const std::string banner = kBodiesBanner;
    const size_t at = cpp_text.find(banner);
    if (at == std::string::npos || at == 0 || cpp_text[at - 1] != '\n' || cpp_text.find(banner, at + 1) != std::string::npos)
        return s;
    const std::string shape = cpp_text.substr(0, at);
    UnitSplit two;
    if (two_objects && !split_unit_into_two(cpp_text, unit_cpp, two))
        return s; // the text has not the expected form: the caller tries the split with one object
    std::error_code ec;
    const uintmax_t size = fs::file_size(base_pch, ec);
    const auto mtime = fs::last_write_time(base_pch, ec).time_since_epoch().count();
    if (ec)
        return s;
    std::string material = base_pch + "\n" + base_key + "\n" + std::to_string(size) + "\n" + std::to_string(mtime) + "\n";
    for (const std::string& a : args)
        material += a + "\n";
    // The header has "#line 1 <unit_cpp>" and the build uses the script dir: both belong to the key.
    material += "dir " + script_dir + "\nunit " + unit_cpp + "\n";
    if (two_objects)
        material += std::string(kSplitVersion) + "\n";
    s.own_fmt = fmt_header_only(opt_);
    s.key = shape_hash(material + (two_objects ? "two objects\n" + two.header : shape));
    // The file stem has the unit id: the eviction finds the files of one script by it.
    s.unit_id = shape_hash(unit_cpp).substr(0, 12);
    s.pch = (fs::path(opt_.cache_dir) / "pch" / "shape" / (s.unit_id + "-" + s.key + ".pch")).string();
    // A marker of a split that failed for this key (the whole unit compiled): it holds for one day.
    const fs::path marker = fs::path(s.pch).replace_extension(".nosplit");
    const auto mt = fs::last_write_time(marker, ec);
    if (!ec)
    {
        if (fs::file_time_type::clock::now() - mt < std::chrono::hours(24))
        {
            s.key.clear();
            return s;
        }
        fs::remove(marker, ec);
    }
    size_t lines = 1;
    for (char c : shape)
        if (c == '\n')
            ++lines;
    if (two_objects)
    {
        s.header_text = std::move(two.header);
        s.main_text = std::move(two.main);
        s.second_text = std::move(two.shape);
        s.fns = std::move(two.fns);
        return s;
    }
    s.header_text = "#line 1 \"" + unit_cpp + "\"\n" + shape;
    s.main_text = "#line " + std::to_string(lines) + " \"" + unit_cpp + "\"\n" + cpp_text.substr(at);
    return s;
}

void ClangFilesBackend::start_shape_build(const ShapeSplit& split, const std::string& base_pch,
    const std::string& script_dir, const std::string& unit, bool timing)
{
    std::thread old;
    {
        std::lock_guard<std::mutex> lock(shape_mutex_);
        if (shape_building_)
            return; // one build at a time
        if (shape_thread_.joinable())
            old = std::move(shape_thread_);
        shape_building_ = true;
    }
    if (old.joinable())
        old.join(); // the last build ended (shape_building_ was false)
    std::lock_guard<std::mutex> lock(shape_mutex_);
    shape_thread_ = std::thread([this, split, base_pch, script_dir, unit, timing] {
      try {
        const auto t0 = std::chrono::steady_clock::now();
        const fs::path dir = fs::path(opt_.cache_dir) / "pch" / "shape";
        std::error_code ec;
        fs::create_directories(dir, ec);
        static std::atomic<unsigned> shape_counter{0};
        const std::string tag = split.key + "." + std::to_string(hot::detail::process_id()) + "." + std::to_string(shape_counter++);
        // The PCH names its header file: the header stays next to the PCH (same key, same text).
        const std::string hdr = fs::path(split.pch).replace_extension(".h").string();
        const std::string tmp = (dir / (tag + ".pch.tmp")).string();
        const std::string dep = (dir / (tag + ".d.tmp")).string();
        // The PCH records the time of its header: never rewrite the header when it is the same
        // (an other process can have written it). A new file goes through a temp file and a rename.
        std::string have;
        bool ok = (read_text(hdr, have) && have == split.header_text) || write_text_replace(hdr, split.header_text);
        std::string output;
        if (ok)
        {
            ok = run_command(with_pch(pch_build_command(with_fmt_choice(opt_, split.own_fmt), hdr, tmp, dep, script_dir), base_pch), output) == 0;
            if (ok)
            {
                fs::rename(tmp, split.pch, ec);
                ok = !ec;
            }
        }
        fs::remove(tmp, ec);
        fs::remove(dep, ec);
        if (!ok && !fs::exists(split.pch, ec))
            fs::remove(hdr, ec); // a failed build is no reason for a marker. Keep the header of a good PCH.
        else
        {
            // Keep the 2 newest shape PCH files of this script and the 32 newest in all. An
            // other process can read an old file: unlink is safe on ELF (the reader keeps its
            // handle). Windows refuses the removal of an open file: the file stays for a later run.
            evict_shape_files(dir.string(), split.unit_id, 2, 32);
        }
        if (timing)
            std::fprintf(stderr, "hot: %s shape pch %s in %lld ms (key %.8s)\n", unit.c_str(), ok ? "built" : "failed",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count()), split.key.c_str());
      } catch (...) {
        // A failure of the file system (an exception) never ends the process: no shape PCH then.
      }
        std::lock_guard<std::mutex> done(shape_mutex_);
        shape_building_ = false;
    });
}

bool ClangFilesBackend::build_pch(const PchPlan& plan, const std::string& script_dir, std::string& error)
{
    const fs::file_time_type started = fs::file_time_type::clock::now();
    // The first build of a cache has no "pch" directory yet.
    std::error_code dir_ec;
    fs::create_directories(fs::path(pch_marker(plan.key, ".seen")).parent_path(), dir_ec);
    // A new time of the "seen" marker: the trim of a process does not remove the header now.
    // The marker files and the header are under "pch_mutex_". The lock ends before the compiler runs.
    bool wrote = false;
    {
        std::lock_guard<std::mutex> lock(pch_mutex_);
        write_text(pch_marker(plan.key, ".seen"), std::string());
        wrote = write_pch_header(plan.header, plan.prefix);
    }
    if (!wrote)
    {
        error = "cannot write " + plan.header;
        return false;
    }
    if (pch_build_hook_)
        pch_build_hook_();
    const std::string base = (fs::path(opt_.work_dir) / plan.key.hex()).string();
    const std::string tmp = base + ".pch.tmp" + std::to_string(hot::detail::process_id());
    const std::string dep = base + ".pchd.tmp" + std::to_string(hot::detail::process_id());
    std::error_code ec;
    std::string output;
    const int rc = run_command(pch_build_command(with_fmt_choice(opt_, plan.own_fmt), plan.header, tmp, dep, script_dir), output);
    std::string dep_text;
    if (rc == 0)
        read_text(dep, dep_text);
    fs::remove(dep, ec);
    bool ok = rc == 0;
    // A compile error of the PCH header is final for this key. A compiler that did not start or that
    // a signal ended (run_command gives -1) can work the next time: no "bad" marker for it.
    bool bad = !ok && rc != -1;
    if (!ok)
        error = rc == -1 ? "the PCH build did not run or a signal ended it: " + output.substr(0, 400)
                         : "the PCH build failed (exit code " + std::to_string(rc) + "): " + output.substr(0, 400);
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
    {
        std::lock_guard<std::mutex> lock(pch_mutex_);
        write_text(pch_marker(plan.key, ".bad"), error);
    }
    // A PCH is 10 to 100 MB: a new one can put the PCH entries above the limit.
    if (ok)
    {
        trim_pch();
        // The trim removes the oldest entries first. A new PCH that is gone alone is larger than
        // the limit: a rebuild is useless, so it gets the "bad" marker.
        if (!fs::exists(pch_cache_->object_path(plan.key), ec))
        {
            error = "the PCH is larger than the size limit of the cache";
            std::lock_guard<std::mutex> lock(pch_mutex_);
            write_text(pch_marker(plan.key, ".bad"), error);
            return false;
        }
    }
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
// A "<hex>.noshared" file is a marker of a unit: it goes after the retention like a ".bad" file.
// A "shared.*.tmp<pid>" file older than the retention goes: a crash left it.
void ClangFilesBackend::trim_pch()
{
    if (!pch_cache_)
        return;
    pch_cache_->trim();
    try
    {
        std::lock_guard<std::mutex> lock(pch_mutex_); // the marker files are under the mutex
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
            if (p.filename().string().rfind("shared.", 0) == 0 && ext.rfind(".tmp", 0) == 0)
            {
                const auto t = fs::last_write_time(p, fec);
                if (!fec && now - t > kPchMarkerRetention)
                    fs::remove(p, fec);
                continue;
            }
            if (ext != ".seen" && ext != ".bad" && ext != ".noshared" && ext != ".h")
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
    const uint64_t epoch = pch_extra_epoch_;
    // The caller holds "pch_mutex_". The flag is true only when the thread starts.
    // If the thread constructor throws, the flag stays false and later PCH builds can start.
    pch_thread_ = std::thread(
        [this, plan, script_dir, unit, timing, epoch]
        {
            const auto s = std::chrono::steady_clock::now();
            std::string error;
            bool ok = false;
            // An exception must not end the process: it means no PCH.
            try
            {
                ok = build_pch(plan, script_dir, error);
            }
            catch (const std::exception& e)
            {
                error = std::string("exception in the PCH build: ") + e.what();
            }
            catch (...)
            {
                error = "unknown exception in the PCH build";
            }
            const long long ms = static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s).count());
            if (plan.shared)
            {
                const fs::path dir = fs::path(opt_.cache_dir) / "pch";
                std::lock_guard<std::mutex> lock(pch_mutex_);
                std::error_code ec;
                if (ok && epoch != pch_extra_epoch_)
                {
                    // The union dropped while the build ran (a new file at a probe path). The PCH
                    // can have an old header: it does not stay.
                    pch_cache_->remove(plan.key);
                }
                else if (ok)
                {
                    // The new union is now the saved union.
                    write_text_atomic(dir / "shared.inc", plan.prefix);
                }
                else if (pch_is_bad(plan))
                {
                    // A bad line must not kill the shared PCH. A deleted header must not poison
                    // the union for good. A unit that planned before this point must not add its
                    // lines to shared.extra again: the epoch tells it.
                    fs::remove(dir / "shared.extra", ec);
                    fs::remove(dir / "shared.inc", ec);
                    ++pch_extra_epoch_;
                }
            }
            if (!ok)
                std::fprintf(stderr, "hot: %s has no PCH: %s\n", unit.c_str(), error.c_str());
            else if (timing && plan.shared)
                std::fprintf(stderr, "hot: %s pch build %lld ms (key %s, union %zu lines)\n", unit.c_str(), ms,
                    plan.key.hex().substr(0, 8).c_str(), static_cast<size_t>(std::count(plan.prefix.begin(), plan.prefix.end(), '\n')));
            else if (timing)
                std::fprintf(stderr, "hot: %s pch build %lld ms\n", unit.c_str(), ms);
            pch_done_ = true;
        });
    pch_running_ = true;
}

void ClangFilesBackend::wait_for_pch()
{
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock(pch_mutex_);
        thread = std::move(pch_thread_);
    }
    if (thread.joinable())
    {
        thread.join();
        // "pch_running_" stays true during the join: a build call starts no second PCH build.
        std::lock_guard<std::mutex> lock(pch_mutex_);
        pch_running_ = false;
    }
}

bool ClangFilesBackend::build(const ScriptSource& source, uint32_t generation, const std::string& out_so,
    std::string& error, std::vector<std::string>* dependencies)
{
    return build_impl(source, generation, LoadMode::Reload, out_so, error, nullptr, dependencies);
}

// "deferred_slots" null: write the slots file when the compile succeeds (the tool hotc).
// Else give the new content of the slots file back, and leave the file as it is.
// The serial build holds "mutex_" for the three stages.
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
    BuildJob job;
    // An exception in translate_stage must not leave the mirror directory.
    struct MirrorCleanup
    {
        BuildJob& job;
        ~MirrorCleanup() { remove_mirror(job); }
    } mirror_cleanup{job};
    job.use_module_cache = use_module_cache;
    const bool translated =
        translate_stage(job, source, generation, out_so, deferred_slots != nullptr, make_translate_inputs(mode));
    if (dependencies)
        *dependencies = job.tr.dependencies; // also after a failure
    if (hashes)
    {
        hashes->iface_hash = job.tr.iface_hash;
        hashes->import_hashes = job.tr.import_hashes;
        hashes->iface_info = job.tr.iface_info;
        hashes->use_info = job.tr.use_info;
    }
    if (!translated || !compile_stage(job))
    {
        error = std::move(job.error);
        return false;
    }
    if (cached_module)
        *cached_module = job.cached_module;
    record_stage(job);
    if (deferred_slots)
    {
        *deferred_slots = job.tr.slots_out + job.tr.sigs_out;
        return true;
    }
    if (!write_text(opt_.slots_file, job.tr.slots_out + job.tr.sigs_out))
    {
        error = "cannot write " + opt_.slots_file;
        return false;
    }
    return true;
}

void ClangFilesBackend::remove_mirror(BuildJob& job)
{
    if (!job.mirror_dir.empty())
    {
        std::error_code ec;
        fs::remove_all(job.mirror_dir, ec);
    }
    job.mirror_dir.clear();
    job.mirror_root.clear();
}

std::string ClangFilesBackend::unit_name_of(const ScriptSource& source)
{
    std::string unit = source.unit.empty() ? fs::path(source.name).stem().string() : source.unit;
    if (unit.empty())
        unit = "script";
    return unit;
}

bool ClangFilesBackend::translate_stage(BuildJob& job, const ScriptSource& source, uint32_t generation,
    const std::string& out_so, bool deferred, const TranslateInputs& in)
{
    job.seq = job_seq_++;
    job.source_name = source.name;
    job.source_text = source.text;
    job.out_so = out_so;
    std::error_code ec;
    const fs::path so_path(out_so);
    if (so_path.has_parent_path())
        fs::create_directories(so_path.parent_path(), ec);
    fs::create_directories(
        fs::path(opt_.slots_file).parent_path().empty() ? fs::path(".") : fs::path(opt_.slots_file).parent_path(), ec);
    // The .o and the depfile go next to the .so.
    job.obj_path = fs::path(so_path).replace_extension(".o").string();
    job.dep_path = fs::path(so_path).replace_extension(".d").string();
    std::string& unit = job.unit;
    unit = unit_name_of(source);
    // A load gives the .cpp a stable name: "<generated_dir>/<unit>.cpp", else "<work_dir>/<unit>.cpp".
    // It is also the "#line" name, so the text of the .cpp has no part of the .so name (no process
    // id, generation, epoch or sequence). Equal script text then gives equal text at each generation.
    // A build of a named .so (the tool hotc, "deferred" false) without a generated_dir puts
    // the .cpp next to the .so: the caller chose that name.
    // A unit name can have directories ("Player/Health"). They become directories of the .cpp.
    // A unit name can be an absolute path (the launcher). Its root goes away, so the .cpp stays
    // in the directory and never replaces a file next to the script.
    const fs::path unit_cpp = fs::path(unit + ".cpp").relative_path();
    const bool stable_cpp = !opt_.generated_dir.empty();
    job.line_name = stable_cpp ? (fs::path(opt_.generated_dir) / unit_cpp).string()
        : deferred ? (fs::path(opt_.work_dir) / unit_cpp).string()
                   : fs::path(so_path).replace_extension(".cpp").string();
    const std::string& line_name = job.line_name;
    fs::create_directories(fs::path(line_name).parent_path(), ec);
    // With the cache the compile reads a private copy "<work_dir>/<unit>.cpp". Two processes then
    // never change the input of a compile of the other. The stable file is for the IDE only.
    job.use_cache = in.use_cache;
    const bool use_cache = job.use_cache;
    std::string& cpp_path = job.cpp_path;
    cpp_path = use_cache ? (fs::path(opt_.work_dir) / unit_cpp).string() : line_name;

    hotc::TranslateOptions to;
    to.script_text = source.text;
    to.modules = source.modules;
    to.script_name = source.name;
    to.unit_name = source.unit;
    to.output_name = line_name;
    to.generation = static_cast<int>(generation);
    // A build of a named .so (the tool hotc, "deferred" false) names its generation. A load
    // does not: its text stays free of the generation.
    to.export_generation = !deferred;
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
    // The slots text is empty for the first generation and for a fresh load.
    to.slots_in = in.slots_in;
    // HOT_TIMING prints the time of the translate and of the clang run of each unit.
    job.timing = in.timing;
    job.t0 = std::chrono::steady_clock::now();
    // The translate cache gives the result of an earlier translate with the same inputs. A hit
    // continues as a translate: also when the object cache then misses, the compile uses the
    // cached text. Its "#line" lines and the source map give the script places of the errors.
    std::optional<hotc::TranslateResult> cached;
    TranslateCache* const tr_cache = use_cache ? tr_cache_.get() : nullptr;
    if (tr_cache)
        cached = tr_cache->lookup(to);
    job.translate_hit = cached.has_value();
    job.mark("lookup");
    job.translate_start = fs::file_time_type::clock::now();
    hotc::TranslateResult& tr = job.tr;
    tr = job.translate_hit ? std::move(*cached) : hotc::translate(to, *opt_.session);
    job.mark("translate");
    if (tr_cache && !job.translate_hit && tr.ok && tr.diagnostics.empty())
    {
        std::string store_error;
        if (!tr_cache->store(to, tr, job.translate_start, store_error) && job.timing)
            std::fprintf(stderr, "hot: %s has no translate cache entry: %s\n", unit.c_str(), store_error.c_str());
    }
    job.mark("store");
    job.t1 = std::chrono::steady_clock::now();
    if (!tr.ok)
    {
        job.error = opt_.raw_diagnostics
            ? tr.diagnostics
            : hotc::rewrite_translator_diagnostic(tr.diagnostics, hotc::script_texts(source.name, source.text, tr));
        return false;
    }
    if (use_cache)
    {
        fs::create_directories(fs::path(cpp_path).parent_path(), ec);
        if (!write_text(cpp_path, tr.cpp))
        {
            job.error = "cannot write " + cpp_path;
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
            job.error = "cannot write " + cpp_path;
            return false;
        }
    }
    else if (!stable_cpp && !write_text(cpp_path, tr.cpp))
    {
        job.error = "cannot write " + cpp_path;
        return false;
    }

    // The headers of the host file system go to disk files, because the external compiler reads
    // the disk. Each job writes them to its own directory "<work_dir>/vfs.job/<pid>.<seq>". So jobs
    // that compile at the same time do not share files. compile_stage removes the directory.
    if (opt_.fs && !tr.virtual_files.empty())
    {
        job.mirror_dir = (fs::path(opt_.work_dir) / "vfs.job" /
                           (std::to_string(hot::detail::process_id()) + "." + std::to_string(job.seq)))
                              .string();
        job.mirror_root = job.mirror_dir + "/";
        fs::remove_all(job.mirror_dir, ec);
        for (const auto& [vpath, vtext] : tr.virtual_files)
        {
            const std::optional<std::string> rel = vfs_relative(vpath);
            if (!rel)
            {
                job.error = "the virtual file '" + vpath + "' is outside the mirror directory\n";
                remove_mirror(job);
                return false;
            }
            const fs::path target = fs::path(job.mirror_dir) / *rel;
            fs::create_directories(target.parent_path(), ec);
            if (!write_text(target.string(), vtext))
            {
                job.error = "cannot write " + target.string();
                remove_mirror(job);
                return false;
            }
        }
    }
    const size_t script_slash = source.name.rfind('/');
    job.script_dir = script_slash == std::string::npos ? std::string(".") : source.name.substr(0, script_slash);
    const std::string sysroot_error = mingw_sysroot_error(opt_.target_triple, opt_.sysroot);
    if (!sysroot_error.empty())
    {
        job.error = sysroot_error + "\n";
        remove_mirror(job);
        return false;
    }
    return true;
}

bool ClangFilesBackend::compile_stage(BuildJob& job)
{
    // The units use the fmt code of the host. If a module needs an fmt symbol that the host does not
    // export, the module would not load. Then this backend turns the own fmt copy on and compiles again.
    const bool host_fmt = !fmt_header_only(opt_);
    const bool ok = compile_stage_once(job, host_fmt);
    std::string missing;
    if (!host_fmt || !ok || !(opt_.missing_fmt_symbol ? opt_.missing_fmt_symbol(job.out_so, &missing) : has_missing_fmt_symbol(job.out_so, &missing, opt_.host_import_library)))
    {
        if (host_fmt)
            remove_mirror(job);
        return ok;
    }
    if (opt_.fmt_header_only)
        opt_.fmt_header_only->store(true);
    const std::string msg = "hot: " + job.unit + " needs the fmt symbol " + missing
        + " that the host does not export; the units use their own fmt code from now";
    std::fprintf(stderr, "%s\n", msg.c_str());
    return compile_stage_once(job, false);
}

bool ClangFilesBackend::compile_stage_once(BuildJob& job, bool keep_mirror)
{
    std::error_code ec;
    const fs::path so_path(job.out_so);
    const std::string& out_so = job.out_so;
    const std::string& unit = job.unit;
    const std::string& line_name = job.line_name;
    const std::string& cpp_path = job.cpp_path;
    const std::string& obj_path = job.obj_path;
    const std::string& dep_path = job.dep_path;
    const std::string& script_dir = job.script_dir;
    const hotc::TranslateResult& tr = job.tr;
    const bool timing = job.timing;
    const auto translate_start = job.translate_start;
    // The mirror of this job is not needed after the compile, on each return path.
    struct MirrorGuard
    {
        BuildJob& job;
        bool keep;
        ~MirrorGuard()
        {
            if (!keep)
                remove_mirror(job);
        }
    } mirror_guard{job, keep_mirror};
    fs::remove(so_path, ec);
    std::string output;
    // The diagnostics can name the "#line" name or the path of the file (before the first "#line").
    hotc::ScriptTexts generated = {{line_name, tr.cpp}};
    if (cpp_path != line_name)
        generated.push_back({cpp_path, tr.cpp});
    const auto fail = [&](const char* what, int code)
    {
        if (!opt_.raw_diagnostics)
            output = hotc::rewrite_diagnostic(
                output, {{job.source_name, job.source_text}}, generated, &tr.source_map, &tr.indirect_names);
        job.error = output + "the " + what + " of " + job.source_name + " failed (exit code " + std::to_string(code)
            + ")\n";
        return false;
    };
    int link_rc = 0;
    job.built_object = obj_path; // the object file of the compile, for the compile database
    job.cached_module.reset();
    const char* cache_note = "";
    std::string pch_note; // ", pch" when the compile used the PCH
    auto t2 = std::chrono::steady_clock::now();
    if (!job.use_cache)
    {
        fs::remove(obj_path, ec);
        const int rc = run_command(compile_object_command(opt_, cpp_path, obj_path, dep_path, script_dir, job.mirror_root), output);
        t2 = std::chrono::steady_clock::now();
        if (rc != 0)
            return fail("compile", rc);
        link_rc = run_link(opt_, obj_path, out_so, output, timing);
    }
    else
    {
        // The key has the translated text, the compile arguments (the paths of this build are
        // fixed names, also the root of the mirror: the root of the job is not in the key), the compiler and the mirror files that the compile can read.
        job.mark("write_cpp");
        ObjectCache::KeyInputs inputs;
        inputs.cpp_text = tr.cpp;
        inputs.args = ObjectCache::normalize_args(
            compile_object_command(opt_, "@cpp@", "@obj@", "@dep@", script_dir), opt_.work_dir);
        inputs.compiler_id = compiler_id_;
        // The split decision is in the key: an object that the two object split made (joined) must not
        // serve a build without the split, and a new version of the transform makes new keys.
        const bool hot_unit_key = job.source_name.size() > 4
            && job.source_name.compare(job.source_name.size() - 4, 4, ".hot") == 0;
        const char* two_env = std::getenv("HOT_SPLIT_UNIT_OBJECTS");
        const std::string lld = find_lld(opt_.compiler);
#if defined(__linux__)
        const bool elf_target = elf64_linux_target(opt_.target_triple);
#else
        const bool elf_target = false;
#endif
        const bool two_allowed = hot_unit_key && elf_target && !lld.empty() && two_object_flags_ok(inputs.args)
            && !(two_env && std::strcmp(two_env, "0") == 0);
        if (hot_unit_key)
            inputs.args.push_back(split_object_tag(two_allowed));
        if (opt_.fs)
            inputs.virtual_files = tr.virtual_files;
        // A sum of the path hashes: the order of the paths is different after a translate cache hit.
        for (const std::string& p : tr.shadow_paths)
            inputs.shadow_hash += hotc::text_hash(p);
        const ObjectCache::Key key = ObjectCache::make_key(inputs);
        job.mark("key");
        std::optional<ObjectCache::Hit> hit = cache_->lookup(key);
        job.mark("obj_lookup");
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
        if (module_key && job.use_module_cache && !hit)
            cache_->count_module_miss();
        if (module_key && job.use_module_cache && hit)
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
                    job.built_object = cache_->object_path(key).string();
                    t2 = std::chrono::steady_clock::now();
                    job.cached_module = module_key;
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
                // The number of the job is in the names: parallel jobs with the same key do not collide.
                const std::string base = (fs::path(opt_.work_dir) / key.hex()).string();
                const std::string suffix = std::to_string(hot::detail::process_id()) + "." + std::to_string(job.seq);
                const std::string tmp = base + ".o.tmp" + suffix;
                const std::string dep = base + ".d.tmp" + suffix;
                fs::remove(tmp, ec);
                fs::remove(dep, ec);
                const std::vector<std::string> plain = compile_object_command(opt_, cpp_path, tmp, dep, script_dir, job.mirror_root);
                std::optional<PchPlan> pch;
                std::string extra_add; // include lines for shared.extra after a good compile
                uint64_t extra_epoch = 0; // the epoch of the union at plan time
                std::string no_pch_why; // for HOT_TIMING: why the compile has no shared PCH
                {
                    std::lock_guard<std::mutex> pch_lock(pch_mutex_);
                    pch = plan_pch(tr.cpp, script_dir, opt_.fs && !tr.virtual_files.empty(), inputs.shadow_hash);
                    if (pch)
                    {
                        extra_add = pch->extra_add;
                        extra_epoch = pch->extra_epoch;
                        no_pch_why = pch->why;
                    }
                    else
                        no_pch_why = "no PCH plan (HOT_PCH=0, no clang, mirror files or no include lines)";
                    // The PCH build runs next to the compile and after the end of this call: the
                    // compile, the link and the load do not wait for it. One PCH build at a time: a
                    // plan to build while the last build runs uses no PCH.
                    if (pch_thread_.joinable() && pch_done_)
                    {
                        pch_thread_.join();
                        pch_running_ = false;
                    }
                    if (pch && pch->build && pch_running_)
                    {
                        no_pch_why = "shared pch building" + (no_pch_why.empty() ? "" : "; " + no_pch_why);
                        pch.reset();
                    }
                    if (pch && pch->build && pch_defer_)
                    {
                        // An edit batch: the build starts when the last unit of the batch ends.
                        // The first edit after a cold start has no exact PCH and no shared entry,
                        // so its plan waits here, and its own compile has no PCH. The next
                        // edits use the PCH. A build in the batch would slow the compiles of the
                        // batch.
                        no_pch_why = "deferred" + (no_pch_why.empty() ? "" : "; " + no_pch_why);
                        const bool queued = std::any_of(pch_pending_.begin(), pch_pending_.end(),
                            [&](const PendingPch& p) { return p.plan.key.hex() == pch->key.hex(); });
                        if (!queued)
                            pch_pending_.push_back(PendingPch{*pch, script_dir, unit, timing});
                        pch->build = false;
                    }
                    else if (pch && pch->build)
                        start_pch_build(*pch, script_dir, unit, timing);
                }
                job.mark("plan_pch");
                int rc = 0;
                bool used_pch = false;
                std::vector<std::string> shape_deps; // cache files of a shape PCH hit: not object inputs
                if (pch && !pch->pch.empty())
                {
                    // The shape PCH: on a hit the compile gets the rest of the unit after the PCH of
                    // the shape. On a miss it compiles the whole unit and builds the shape PCH later.
                    const std::string& sn = job.source_name;
                    const bool is_hot_unit = sn.size() > 4 && sn.compare(sn.size() - 4, 4, ".hot") == 0;
                    // A stable name: the work dir of a process has its id, and the key must not have it.
                    std::string stable_name = line_name;
                    const std::string work_prefix = opt_.work_dir;
                    if (!work_prefix.empty() && stable_name.compare(0, work_prefix.size(), work_prefix) == 0)
                        stable_name = "@work@" + stable_name.substr(work_prefix.size());
                    // Two objects (design D): a compile of the bodies and a compile of the record and the
                    // entry, in parallel, joined by "ld.lld -r". HOT_SPLIT_UNIT_OBJECTS=0 turns it off.
                    ShapeSplit shape = plan_shape(tr.cpp, stable_name, pch->pch, pch->key.hex(), inputs.args,
                        is_hot_unit, script_dir, two_allowed);
                    if (shape.key.empty() && two_allowed)
                        shape = plan_shape(tr.cpp, stable_name, pch->pch, pch->key.hex(), inputs.args, is_hot_unit,
                            script_dir, false);
                    bool shape_hit = false;
                    bool split_failed = false;
                    bool split_io_error = false; // a file write failed: no marker, the next build tries again
                    std::string split_out;
                    shape_deps.clear();
                    const fs::path shape_pch_path(shape.pch);
                    if (!shape.key.empty() && fs::exists(shape_pch_path, ec))
                    {
                        const std::string main_path = tmp + ".main.cpp";
                        std::vector<std::string> cmd = plain;
                        cmd[cmd.size() - 3] = main_path; // the input file
                        cmd = with_pch(std::move(cmd), shape.pch);
                        if (!shape.second_text.empty() && write_text(main_path, shape.main_text))
                        {
                            // Two objects: bodies (main) and record and entry (shape file).
                            const std::string second_path = tmp + ".shape.cpp";
                            const std::string obj_main = tmp + ".main.o";
                            const std::string obj_shape = tmp + ".shape.o";
                            const std::string dep_shape = dep + ".shape";
                            cmd[cmd.size() - 1] = obj_main;
                            std::vector<std::string> cmd2 = plain;
                            cmd2[cmd2.size() - 3] = second_path;
                            cmd2[cmd2.size() - 1] = obj_shape;
                            cmd2[cmd2.size() - 4] = dep_shape;
                            cmd2 = with_pch(std::move(cmd2), shape.pch);
                            std::string out_a, out_b;
                            int rc_a = 1, rc_b = 1;
                            bool io_error = false;
                            TempFiles temp{{obj_main, obj_shape, dep_shape, second_path}};
                            if (write_text(second_path, shape.second_text))
                            {
                                // The second compile runs next to the first one only when a place is free
                                // (the job limit, HOT_JOBS); else one after the other.
                                std::thread second;
                                if (reserve_compile_place())
                                {
                                    try
                                    {
                                        second = std::thread([&] {
                                            PlaceGuard place; // the place of the reservation
                                            rc_b = run_command(cmd2, out_b);
                                        });
                                    }
                                    catch (const std::system_error&)
                                    {
                                        --g_running_compiles; // no thread: the compile runs here
                                    }
                                }
                                JoinGuard join_second{second};
                                rc_a = run_counted(cmd, out_a);
                                if (second.joinable())
                                    second.join();
                                else
                                    rc_b = run_counted(cmd2, out_b);
                            }
                            else
                                io_error = true;
                            std::string why = io_error ? "cannot write the second file" : std::string();
                            rc = rc_a != 0 ? rc_a : rc_b;
                            if (rc == 0)
                            {
                                // The guard: no copy of the unit state in both objects.
                                const auto guard_start = std::chrono::steady_clock::now();
                                std::set<std::string> state_a, state_b;
                                if (!unit_state_symbols(obj_main, state_a) || !unit_state_symbols(obj_shape, state_b))
                                    why = "cannot read the symbols of the objects";
                                else
                                    for (const std::string& n : state_a)
                                        if (state_b.count(n))
                                            why = "the unit state " + n + " is in both objects";
                                if (why.empty())
                                {
                                    std::string link_out;
                                    if (run_command({lld, "-r", obj_shape, obj_main, "-o", tmp}, link_out) != 0)
                                        why = "the join of the objects failed: " + link_out;
                                    else
                                        why = merged_object_problem(tmp, shape.fns);
                                }
                                if (timing)
                                    std::fprintf(stderr, "hot: %s split guard and join %.1f ms\n", unit.c_str(),
                                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - guard_start).count());
                                if (!why.empty())
                                    rc = 1;
                                else
                                {
                                    std::string dep_a, dep_b;
                                    read_text(dep, dep_a);
                                    read_text(dep_shape, dep_b);
                                    write_text(dep, dep_a + "\n" + dep_b);
                                }
                            }
                            split_io_error = io_error;
                            split_out = out_a + out_b + why;
                            output = split_out;
                            shape_hit = rc == 0;
                            for (const std::string& f : {obj_main, obj_shape, dep_shape, second_path})
                                fs::remove(f, ec);
                            if (shape_hit)
                            {
                                fs::last_write_time(shape_pch_path, fs::file_time_type::clock::now(), ec);
                                if (timing)
                                    std::fprintf(stderr, "hot: %s shape pch hit, two objects (key %.8s)\n",
                                        unit.c_str(), shape.key.c_str());
                            }
                            else
                            {
                                if (timing)
                                    std::fprintf(stderr, "hot: %s two objects refused: %.300s\n", unit.c_str(),
                                        split_out.c_str());
                                split_failed = true;
                                fs::remove(tmp, ec);
                                fs::remove(dep, ec);
                            }
                        }
                        else if (write_text(main_path, shape.main_text))
                        {
                            rc = run_command(cmd, split_out);
                            shape_hit = rc == 0;
                            if (shape_hit)
                            {
                                output = split_out;
                                fs::last_write_time(shape_pch_path, fs::file_time_type::clock::now(), ec); // last use
                                if (timing)
                                    std::fprintf(stderr, "hot: %s shape pch hit (key %.8s)\n", unit.c_str(),
                                        shape.key.c_str());
                            }
                            else
                            {
                                split_failed = true;
                                fs::remove(tmp, ec);
                                fs::remove(dep, ec);
                            }
                        }
                        fs::remove(main_path, ec);
                    }
                    if (shape_hit)
                        shape_deps = {shape.pch, fs::path(shape.pch).replace_extension(".h").string()};
                    if (!shape_hit)
                        rc = run_command(with_pch(plain, pch->pch), output);
                    if (split_failed && rc == 0)
                    {
                        // The whole unit works, so the split is the cause. A damaged or changed
                        // PCH (or a PCH that an other job removed) only needs a new build.
                        // Other errors make a marker: the key uses the whole unit for one day.
                        const bool pch_problem = !fs::exists(shape_pch_path, ec) ||
                            split_out.find("malformed or corrupted") != std::string::npos ||
                            split_out.find("PCH file") != std::string::npos ||
                            split_out.find("AST file") != std::string::npos ||
                            split_out.find("has been modified") != std::string::npos ||
                            split_out.find("could not find file") != std::string::npos ||
                            split_out.find("precompiled header") != std::string::npos ||
                            split_out.find("defined in the PCH") != std::string::npos;
                        if (pch_problem)
                        {
                            fs::remove(shape_pch_path, ec);
                            fs::remove(fs::path(shape.pch).replace_extension(".h"), ec);
                        }
                        else if (!split_io_error)
                            write_text(fs::path(shape.pch).replace_extension(".nosplit").string(), split_out);
                        std::fprintf(stderr, "hot: %s does not compile with its shape PCH; the whole unit is used\n",
                            unit.c_str());
                        if (timing)
                            std::fprintf(stderr, "hot: split output: %.600s\n", split_out.c_str());
                    }
                    used_pch = rc == 0;
                    if (rc == 0 && !shape_hit && !shape.key.empty() && !fs::exists(shape.pch, ec))
                        start_shape_build(shape, pch->pch, script_dir, unit, timing);
                    if (rc != 0)
                    {
                        // The PCH must never fail a build: compile again without it. When that
                        // compile works, the PCH is bad (damaged, or a header without a guard).
                        const std::string pch_output = output;
                        // A trim can remove the PCH during the compile: that is no bad PCH.
                        const bool pch_exists = fs::exists(pch->pch, ec);
                        fs::remove(tmp, ec);
                        fs::remove(dep, ec);
                        rc = run_command(plain, output);
                        if (rc == 0 && pch->shared)
                        {
                            // One unit fails with the shared PCH. The shared PCH stays for the
                            // other units. Only this unit (its exact key) skips it.
                            // A damaged shared PCH file: remove the entry, so the next prewarm
                            // builds it again. No marker: the unit is not the cause.
                            const bool damaged = pch_output.find("malformed or corrupted") != std::string::npos ||
                                pch_output.find("PCH file") != std::string::npos ||
                                pch_output.find("AST file") != std::string::npos;
                            if (pch_exists && damaged)
                            {
                                pch_cache_->remove(pch->key);
                                pch_note = ", pch failed";
                                std::fprintf(stderr, "hot: %s: the shared PCH file is damaged; it is removed\n",
                                    unit.c_str());
                            }
                            else if (pch_exists)
                            {
                                {
                                    std::lock_guard<std::mutex> pch_lock(pch_mutex_);
                                    write_text(pch_marker(pch->unit_key, ".noshared"), pch_output);
                                }
                                pch_note = ", pch failed";
                                std::fprintf(stderr,
                                    "hot: %s does not compile with the shared PCH; the shared PCH is off for this unit "
                                    "for one day\n",
                                    unit.c_str());
                            }
                        }
                        else if (rc == 0)
                        {
                            pch_cache_->remove(pch->key);
                            if (pch_exists)
                            {
                                {
                                    std::lock_guard<std::mutex> pch_lock(pch_mutex_);
                                    write_text(pch_marker(pch->key, ".bad"), pch_output);
                                }
                                pch_note = ", pch failed";
                                std::fprintf(stderr,
                                    "hot: %s does not compile with its PCH; the PCH is off for one day\n", unit.c_str());
                            }
                        }
                    }
                }
                else
                {
                    if (timing && !no_pch_why.empty())
                        std::fprintf(stderr, "hot: %s no shared pch: %s\n", unit.c_str(), no_pch_why.c_str());
                    rc = run_command(plain, output);
                }
                if (used_pch)
                    pch_note = ", pch";
                t2 = std::chrono::steady_clock::now();
                job.mark("clang");
                std::string dep_text;
                if (rc == 0)
                    read_text(dep, dep_text);
                fs::remove(dep, ec);
                // The object depends on the headers of the PCH too. Clang lists them in the
                // depfile of a compile with "-include-pch". The rule keeps them for a clang
                // that does not. parse_depfile reads each rule.
                // The shape PCH and its header are files of the cache: the key has their content. Their
                // time is new after a build, so the store would refuse them as changed headers.
                for (const std::string& d : shape_deps)
                    for (size_t at = dep_text.find(d); at != std::string::npos; at = dep_text.find(d, at))
                        dep_text.erase(at, d.size());
                if (used_pch)
                    dep_text += "\n" + depfile_rule(pch_cache_->headers(pch->key));
                if (rc != 0)
                {
                    fs::remove(tmp, ec);
                    return fail("compile", rc);
                }
                if (!extra_add.empty())
                {
                    std::lock_guard<std::mutex> pch_lock(pch_mutex_);
                    // The union dropped since the plan: the lines can be the cause. Discard them.
                    if (extra_epoch == pch_extra_epoch_)
                        append_shared_extra(fs::path(opt_.cache_dir) / "pch", extra_add);
                }
                std::string store_error;
                // The start of the translate: the translated text depends on the headers too.
                const bool stored = cache_->store(key, tmp, dep_text, opt_.work_dir, translate_start, store_error);
                job.mark("obj_store");
                if (stored)
                    object = cache_->object_path(key).string();
                else
                {
                    std::fprintf(stderr, "hot: cannot store %s in the object cache: %s\n", unit.c_str(),
                        store_error.c_str());
                    object = tmp;
                    temp_object = tmp;
                }
            }
            link_rc = run_link(opt_, object, out_so, output, timing);
            job.built_object = temp_object.empty() ? object : obj_path;
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
            job.translate_hit ? std::string("cached") : std::to_string(ms(job.t0, job.t1)) + " ms";
        // The detail of a translate miss comes after the old fields, so a reader of the old fields works.
        std::string translate_detail;
        if (!job.translate_hit)
        {
            const hotc::TranslateTiming& tt = job.tr.timing;
            const auto tms = [](int64_t ns) { return static_cast<long long>(ns / 1000000); };
            char buf[200];
            std::snprintf(buf, sizeof buf, " [translate detail: parse %lld ms x%lld, wait %lld, front %lld, lower %lld, check %lld, emit %lld]",
                tms(tt.parse_ns), static_cast<long long>(tt.parses), tms(tt.parse_wait_ns), tms(tt.front_ns),
                tms(tt.lower_ns), tms(tt.check_ns), tms(tt.emit_ns));
            translate_detail = buf;
        }
        // The parts of the translate (from t0 to t1) and of the compile (from t1 to t2). Each part
        // is the time from the earlier mark. "tail" is the rest to the end of the phase.
        std::string parts;
        {
            const auto us = [](auto x, auto y) {
                return static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(y - x).count());
            };
            auto prev = job.t0;
            std::string tp, cp;
            bool in_compile = false;
            for (const auto& m : job.marks)
            {
                std::string& dst = in_compile || std::string(m.first) == "write_cpp" ? cp : tp;
                if (&dst == &cp && !in_compile)
                {
                    in_compile = true;
                    tp += " tail " + std::to_string(us(prev, job.t1) / 1000.0).substr(0, 5);
                    prev = job.t1;
                }
                char b2[64];
                std::snprintf(b2, sizeof b2, "%s%s %.1f", dst.empty() ? "" : ", ", m.first, us(prev, m.second) / 1000.0);
                dst += b2;
                prev = m.second;
            }
            if (!tp.empty() || !cp.empty())
                parts = " [translate parts ms: " + tp + "] [compile parts ms: " + cp + "]";
        }
        std::fprintf(stderr, "hot: %s translate %s, compile %lld ms, link %lld ms%s%s%s%s\n", unit.c_str(),
            translate_note.c_str(), ms(job.t1, t2), ms(t2, t3), cache_note, pch_note.c_str(),
            translate_detail.c_str(), parts.c_str());
    }
    if (link_rc != 0)
        return fail("link", link_rc);
    return true;
}

void ClangFilesBackend::record_stage(const BuildJob& job, bool defer)
{
    record_compile_command(job.unit, job.script_dir, job.built_object, defer);
}

LoadResult ClangFilesBackend::load(const ScriptSource& source, uint32_t generation, LoadMode mode)
{
    return load_impl(source, generation, mode, true);
}

std::string ClangFilesBackend::library_path(const ScriptSource& source, uint32_t generation, LoadMode mode)
{
    std::string stem = fs::path(source.name).stem().string();
    if (stem.empty())
        stem = "script";
    // A fresh load starts a new epoch. The epoch is in the name, so a fresh library never has
    // the path of a library that the process still maps.
    const unsigned epoch = mode == LoadMode::Fresh ? ++epoch_ : epoch_.load();
    // Two backends of one process can share a work directory and have the same epoch and
    // sequence. The process-wide number "u" makes each name unique, so dlopen never returns the
    // handle of an old library that has the same path.
    static std::atomic<unsigned> unique{0};
    return opt_.work_dir + "/" + stem + ".g" + std::to_string(generation) + ".e" + std::to_string(epoch) + "."
        + std::to_string(sequence_++) + ".u" + std::to_string(unique++) + module_suffix();
}

LoadResult ClangFilesBackend::load_impl(
    const ScriptSource& source, uint32_t generation, LoadMode mode, bool use_module_cache)
{
    const std::string so = library_path(source, generation, mode);
    LoadResult r;
    std::string slots;
    std::optional<ObjectCache::Key> cached_module;
    if (!build_impl(source, generation, mode, so, r.error, &slots, &r.dependencies, &r, &cached_module,
            use_module_cache))
    {
        r.reason = ReloadReason::CompileError;
        return r;
    }
    return complete_load(source, generation, mode, so, std::move(slots), std::move(cached_module), std::move(r));
}

LoadResult ClangFilesBackend::complete_load(const ScriptSource& source, uint32_t generation, LoadMode mode,
    const std::string& so, std::string slots, std::optional<ObjectCache::Key> cached_module, LoadResult r)
{
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
    IfaceInfo iface_info = std::move(r.iface_info);
    UseInfo use_info = std::move(r.use_info);
    const auto open_start = std::chrono::steady_clock::now();
    r = module_from_library(so);
    open_us_ += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - open_start)
                    .count();
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
    r.iface_info = std::move(iface_info);
    r.use_info = std::move(use_info);
    if (r.module)
    {
        r.module->generation = generation;
        r.module->iface_hash = iface_hash;
        r.module->imports = r.import_hashes;
        r.module->iface_info = r.iface_info;
        r.module->use_info = r.use_info;
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

// A load in two phases. The constructor side (begin_load) ran the translate stage.
class ClangFilesBackend::TwoPhaseLoad : public PendingLoad
{
public:
    TwoPhaseLoad(ClangFilesBackend& backend, ScriptSource source, uint32_t generation, LoadMode mode)
        : backend_(backend)
        , source_(std::move(source))
        , generation_(generation)
        , mode_(mode)
    {
    }

    ~TwoPhaseLoad() override
    {
        // The first phase failed or finish did not run: the library file is not in use.
        if (!finished_ && !job_.out_so.empty())
        {
            std::error_code ec;
            fs::remove(job_.out_so, ec);
        }
        remove_mirror(job_);
        batch_done();
    }

    // The batch of one begin_load_many call. A null pointer means no batch (not set yet).
    void set_batch(std::shared_ptr<size_t> batch) { batch_ = std::move(batch); }

    void compile() override
    {
        if (!ok)
            return;
        compiled_ = backend_.compile_stage(job_);
    }

    LoadResult finish() override
    {
        finished_ = true;
        // The unit counts as done when this function ends (or at once for a failure).
        struct Done
        {
            TwoPhaseLoad& self;
            ~Done() { self.batch_done(); }
        } done{*this};
        if (!ok)
            return std::move(head);
        LoadResult r = std::move(head);
        if (!compiled_)
        {
            r.error = job_.error;
            r.reason = ReloadReason::CompileError;
            return r;
        }
        using Clock = std::chrono::steady_clock;
        const auto us = [](Clock::time_point a, Clock::time_point b)
        { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count(); };
        const auto t0 = Clock::now();
        {
            std::lock_guard<std::mutex> lock(backend_.mutex_);
            backend_.record_stage(job_, true);
        }
        const auto t1 = Clock::now();
        const int64_t open_before = backend_.open_us_;
        LoadResult out = backend_.complete_load(source_, generation_, mode_, job_.out_so,
            job_.tr.slots_out + job_.tr.sigs_out, job_.cached_module, std::move(r));
        const auto t2 = Clock::now();
        if (job_.timing)
        {
            backend_.record_us_ += us(t0, t1);
            backend_.complete_us_ += us(t1, t2);
            std::fprintf(stderr,
                "hot: %s finish record_stage %lld ms, complete_load %lld ms, dlopen %lld ms "
                "[sum: record_stage %lld ms, complete_load %lld ms, dlopen %lld ms]\n",
                job_.unit.c_str(), static_cast<long long>(us(t0, t1) / 1000),
                static_cast<long long>(us(t1, t2) / 1000), static_cast<long long>((backend_.open_us_ - open_before) / 1000),
                static_cast<long long>(backend_.record_us_ / 1000), static_cast<long long>(backend_.complete_us_ / 1000),
                static_cast<long long>(backend_.open_us_ / 1000));
        }
        return out;
    }

    // Marks the first phase as failed, without a translate.
    void fail(std::string error)
    {
        job_.error = error;
        head.error = std::move(error);
        head.reason = ReloadReason::CompileError;
        ok = false;
    }
    void set_so(std::string so) { so_ = std::move(so); }
    const BuildJob& job() const { return job_; }

    // The first phase. The caller holds "mutex_" (or gave the work to threads that it waits for).
    // It runs on any thread: it writes only this object and the files of its unit.
    void start(const TranslateInputs& in)
    {
        bool translated = false;
        try
        {
            translated = backend_.translate_stage(job_, source_, generation_, so_, true, in);
            head.dependencies = job_.tr.dependencies; // also after a failure
            head.iface_hash = job_.tr.iface_hash;
            head.import_hashes = job_.tr.import_hashes;
            // The queue of the compiles sorts by this cost.
            cost = job_.tr.cpp.empty() ? job_.source_text.size() : job_.tr.cpp.size();
            head.iface_info = job_.tr.iface_info;
            head.use_info = job_.tr.use_info;
        }
        catch (const std::exception& e)
        {
            translated = false;
            job_.error = e.what()[0] ? e.what() : "the translate failed";
        }
        catch (...)
        {
            translated = false;
            job_.error = "the translate failed";
        }
        if (!translated)
        {
            head.error = job_.error;
            head.reason = ReloadReason::CompileError;
            ok = false;
        }
    }

private:
    // The unit leaves the batch once. The last unit writes the compile database. This runs on
    // the caller thread. It takes "mutex_".
    void batch_done()
    {
        if (!batch_)
            return;
        const PhaseTimer timer("BuildBatch end");
        std::shared_ptr<size_t> batch = std::move(batch_);
        bool last = false;
        {
            std::lock_guard<std::mutex> lock(backend_.mutex_);
            last = --*batch == 0;
            if (last)
                backend_.flush_compile_commands();
        }
        // The compiles of the batch ended: a queued PCH build can start. No other lock is held.
        // This runs in a noexcept destructor: a throw ends the process. The PCH is optional.
        if (last)
        {
            try
            {
                backend_.start_pending_pch();
            }
            catch (...)
            {
            }
        }
    }

    ClangFilesBackend& backend_;
    std::shared_ptr<size_t> batch_;
    ScriptSource source_;
    uint32_t generation_;
    LoadMode mode_;
    std::string so_;
    BuildJob job_;
    bool compiled_ = false;
    bool finished_ = false;
};

ClangFilesBackend::TranslateInputs ClangFilesBackend::make_translate_inputs(LoadMode mode)
{
    TranslateInputs in;
    in.use_cache = prepare_cache();
    // A missing file means the first generation. A fresh generation starts with no slots.
    if (mode == LoadMode::Reload)
        read_text(opt_.slots_file, in.slots_in);
    in.timing = std::getenv("HOT_TIMING") != nullptr;
    return in;
}

std::unique_ptr<PendingLoad> ClangFilesBackend::begin_load(
    const ScriptSource& source, uint32_t generation, LoadMode mode)
{
#ifdef _WIN32
    return Backend::begin_load(source, generation, mode);
#else
    // One code path: a wave of one unit. It starts no thread.
    std::vector<std::unique_ptr<PendingLoad>> many = begin_load_many({source}, generation, mode);
    return std::move(many.front());
#endif
}

std::vector<std::unique_ptr<PendingLoad>> ClangFilesBackend::begin_load_many(
    const std::vector<ScriptSource>& sources, uint32_t generation, LoadMode mode)
{
#ifdef _WIN32
    return Backend::begin_load_many(sources, generation, mode);
#else
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::unique_ptr<PendingLoad>> result;
    std::vector<TwoPhaseLoad*> loads;
    result.reserve(sources.size());
    // The serial part: the names of the libraries, the cache setup and the slots file.
    std::optional<PhaseTimer> serial_timer;
    serial_timer.emplace("begin_load_many serial part");
    const TranslateInputs in = make_translate_inputs(mode);
    std::set<std::string> units;
    for (const ScriptSource& source : sources)
    {
        auto pending = std::make_unique<TwoPhaseLoad>(*this, source, generation, mode);
        pending->set_so(library_path(source, generation, mode));
        if (!units.insert(unit_name_of(source)).second)
        {
            // Two units with one name would write one .cpp file.
            pending->fail("the unit name '" + unit_name_of(source) + "' is the same unit name as an earlier source");
        }
        else
            loads.push_back(pending.get());
        result.push_back(std::move(pending));
    }
    serial_timer.reset();
    // The parallel part: the same thread pattern as run_compiles. HOT_JOBS=N gives N threads in total.
    std::atomic<size_t> next{0};
    const auto work = [&]
    {
        for (size_t i = next.fetch_add(1); i < loads.size(); i = next.fetch_add(1))
            loads[i]->start(in); // catches each exception
    };
    const size_t count = std::min<size_t>(build_jobs(), loads.size());
    std::vector<std::thread> threads;
    if (count > 1)
    {
        try
        {
            for (size_t i = 1; i < count; ++i)
                threads.emplace_back(work);
        }
        catch (...)
        {
            // A thread did not start: the started threads and this thread do the work.
        }
    }
    // The guard joins every started thread, also when "work" throws on this thread.
    struct Joiner
    {
        std::vector<std::thread>& threads;
        ~Joiner()
        {
            for (std::thread& t : threads)
                if (t.joinable())
                    t.join();
        }
    } joiner{threads};
    work();
    for (std::thread& t : threads)
        if (t.joinable())
            t.join();
    // The shared startup PCH starts before the compile jobs. It never fails the load.
    if (in.use_cache)
    {
        try
        {
            std::vector<const BuildJob*> jobs;
            for (TwoPhaseLoad* load : loads)
                if (load->ok)
                    jobs.push_back(&load->job());
            prewarm_pch(jobs, in.timing);
        }
        catch (...)
        {
        }
    }
    // The batch starts after the threads end: a throw above leaves the loads with no batch.
    // The units write the compile database once, when the last of them ends. The caller holds
    // "mutex_" until the return, so no unit ends before then.
    auto batch = std::make_shared<size_t>(result.size());
    for (auto& p : result)
        static_cast<TwoPhaseLoad*>(p.get())->set_batch(batch);
    return result;
#endif
}

} // namespace hot
