// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "hot/clang_files_backend.h"

#include "hot/config.h"
#include "hot/scratch.h"
#include "hot/hotc_lib.h"
#include "hot/paths.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

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

std::vector<std::string> compile_command(const ClangFilesOptions& opt, const std::string& cpp_path,
    const std::string& out_so, const std::string& script_dir)
{
    const bool mingw = is_mingw_triple(opt.target_triple);
    // A MinGW link must use the C++ driver, so it links libstdc++ (hot/command_line.h).
    std::vector<std::string> cmd = {mingw ? cxx_driver(opt.compiler) : opt.compiler,
        "-std=" + (opt.std_version.empty() ? std::string(cxx_std_name()) : opt.std_version),
#ifndef _WIN32
        // The shared library needs position independent code. A Windows DLL does not.
        "-fPIC",
#endif
        "-shared", "-g", "-Werror=return-type",
        // A module symbol must not bind to a host symbol with the same mangled name. The entry
        // point script_register has default visibility (hot/runtime/registry.h).
        "-fvisibility=hidden", "-fvisibility-inlines-hidden",
#ifdef _WIN32
        // HOT_EXPORT becomes dllimport: the module takes the runtime symbols from the host.
        "-DHOT_MODULE_BUILD=1",
#else
        // A call to the host (a native method, the runtime) reads the address from the GOT. A PLT
        // stub adds one jump to each call. The module loads with RTLD_NOW, so no symbol is lazy.
        "-fno-plt",
#endif
    };
    if (!opt.target_triple.empty())
        cmd.push_back("--target=" + opt.target_triple);
    if (!opt.sysroot.empty())
        cmd.push_back("--sysroot=" + opt.sysroot);
    for (const std::string& f : mingw_target_flags(opt.target_triple, opt.sysroot))
        cmd.push_back(f);
#if defined(_MSC_VER)
    // An MSVC host: the module uses the same C runtime DLL as the host (/MD or /MDd). The
    // default of clang++ is the static runtime. Two runtimes do not share the STL debug levels.
    if (!mingw)
    {
    #ifdef _DEBUG
        cmd.push_back("-fms-runtime-lib=dll_dbg");
    #else
        cmd.push_back("-fms-runtime-lib=dll");
    #endif
    }
#endif
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
    cmd.push_back(cpp_path);
    // The library goes after the source, so the link resolves the module's host references.
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

bool ClangFilesBackend::build(
    const ScriptSource& source, uint32_t generation, const std::string& out_so, std::string& error)
{
    return build_impl(source, generation, LoadMode::Reload, out_so, error, nullptr, nullptr);
}

// "deferred_slots" null: write the slots file when the compile succeeds (the tool hotc).
// Else give the new content of the slots file back, and leave the file as it is.
bool ClangFilesBackend::build_impl(const ScriptSource& source, uint32_t generation, LoadMode mode,
    const std::string& out_so, std::string& error, std::string* deferred_slots, bool* slots_removed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    if (slots_removed)
        *slots_removed = false;
    std::error_code ec;
    const fs::path so_path(out_so);
    if (so_path.has_parent_path())
        fs::create_directories(so_path.parent_path(), ec);
    fs::create_directories(
        fs::path(opt_.slots_file).parent_path().empty() ? fs::path(".") : fs::path(opt_.slots_file).parent_path(), ec);
    const std::string cpp_path = fs::path(so_path).replace_extension(".cpp").string();

    hotc::TranslateOptions to;
    to.script_text = source.text;
    to.modules = source.modules;
    to.script_name = source.name;
    to.output_name = cpp_path;
    to.generation = static_cast<int>(generation);
    to.include_dirs = opt_.include_dirs;
    // The translator needs the resource dir for the native headers. A portable build finds it at run time.
    to.resource_dir = opt_.resource_dir.empty() ? default_paths().resource_dir : opt_.resource_dir;
    to.std_version = opt_.std_version.empty() ? cxx_std_name() : opt_.std_version;
    // The parse uses the target of the modules, so the native headers match the compile.
    to.target = opt_.target_triple;
    to.sysroot = opt_.sysroot;
    to.strict_borrowed = opt_.strict_borrowed;
    to.format_lib = format_lib_name();
    to.fs = opt_.fs;
    // A missing file means the first generation. A fresh generation starts with no slots.
    if (mode == LoadMode::Reload)
        read_text(opt_.slots_file, to.slots_in);
    hotc::TranslateResult tr = hotc::translate(to, *opt_.session);
    if (!tr.ok)
    {
        if (slots_removed)
            *slots_removed = tr.slots_removed;
        error = opt_.raw_diagnostics
            ? tr.diagnostics
            : hotc::rewrite_translator_diagnostic(tr.diagnostics, hotc::script_texts(source.name, source.text, tr));
        return false;
    }
    if (!write_text(cpp_path, tr.cpp))
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
    const std::vector<std::string> cmd = compile_command(opt_, cpp_path, out_so, script_dir);
    fs::remove(so_path, ec);
    std::string output;
    const int rc = run_command(cmd, output);
    if (rc != 0)
    {
        if (!opt_.raw_diagnostics)
            output =
                hotc::rewrite_diagnostic(output, {{source.name, source.text}}, {{cpp_path, tr.cpp}}, &tr.source_map);
        error = output + "the compile of " + source.name + " failed (exit code " + std::to_string(rc) + ")\n";
        return false;
    }
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
    bool slots_removed = false;
    if (!build_impl(source, generation, mode, so, r.error, &slots, &slots_removed))
    {
        r.reason = slots_removed ? ReloadReason::SlotsRemoved : ReloadReason::CompileError;
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
    r = module_from_library(so);
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
