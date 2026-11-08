// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "hot/jit_backend.h"

#include "hot/command_line.h"
#include "hot/config.h"
#include "hot/hotc_lib.h"
#include "hot/paths.h"
#include "hot/runtime/errors.h"

#include "jit_engine.h"
#include "jit_msvc_symbols.h"


#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

namespace hot
{

namespace
{

// A path that does not exist on disk. The compile reads the file from memory. Use
// jit_virtual_path() for a path in this dir: on Windows the path needs a drive.
constexpr const char* kVirtualDir = "/hot-jit-virtual";

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

#ifdef __APPLE__
// The path of the macOS SDK: $SDKROOT, else the output of "xcrun --show-sdk-path". Empty when
// both fail: then the Clang driver uses its default sysroot. The query runs one time.
const std::string& apple_sdk_path()
{
    static const std::string path = [] {
        if (const char* env = std::getenv("SDKROOT"); env && *env)
            return std::string(env);
        std::string out;
        if (FILE* pipe = popen("xcrun --show-sdk-path 2>/dev/null", "r"))
        {
            char buf[1024];
            while (std::fgets(buf, sizeof buf, pipe))
                out += buf;
            if (pclose(pipe) != 0)
                out.clear();
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
            out.pop_back();
        return out;
    }();
    return path;
}
#endif

// The error text with a hint for a refused signature or refused executable memory (macOS only).
std::string with_platform_hint(std::string text)
{
#ifdef __APPLE__
    text += macos_entitlement_hint(text);
#endif
    return text;
}

// The text of an engine string. The engine allocated it, so the engine releases it.
std::string take(char* text)
{
    std::string out = text ? text : "";
    hot_jit_string_free(text);
    return out;
}

// The pointers of the strings, for the C interface of the engine.
std::vector<const char*> c_strings(const std::vector<std::string>& v)
{
    std::vector<const char*> out;
    for (const std::string& x : v)
        out.push_back(x.c_str());
    return out;
}

} // namespace

// The JIT engine of the session. The Modules keep a copy of the pointer to this object.
struct JitState
{
    hot_jit_engine* engine = nullptr;
    ~JitState() { hot_jit_engine_free(engine); }
};

namespace
{

// What one generation owns in the JIT. The destructor runs the static
// destructors and removes the code.
struct Generation
{
    std::shared_ptr<JitState> impl;
    hot_jit_dylib* dylib = nullptr;

    ~Generation() { hot_jit_remove(dylib); }
};

// The engine owns the IR. Use only with the mutex of the backend or with one owner.
struct IrHandle
{
    hot_jit_ir* ir = nullptr;
    ~IrHandle() { hot_jit_ir_free(ir); }
};

struct Compiled
{
    std::unique_ptr<IrHandle> ir;
    std::string diagnostics;
};

// The compile command like the driver has it, and the files of the memory file system.
// "path" is the only input file and the last argument.
struct CompileArgs
{
    std::vector<std::string> args;
    std::vector<std::string> paths;
    std::vector<std::string> texts;
};

// "virtual_files" (path, text) are the headers that only the file system of the host holds. They
// go into the memory file system under their absolute normal paths.
CompileArgs make_args(const JitOptions& opt, const std::string& path, const std::string* text, bool as_pch,
    const std::vector<std::pair<std::string, std::string>>* virtual_files = nullptr,
    const std::string& script_dir = std::string())
{
    CompileArgs s;
    if (text)
    {
        s.paths.push_back(path);
        s.texts.push_back(*text);
    }
    // The memory file system needs absolute normal paths, so the paths of the files and the paths
    // of the "-I" dirs are made absolute and normal when a host file system is set.
    auto normal = [](const std::string& p) { return std::filesystem::absolute(p).lexically_normal().string(); };
    if (virtual_files)
        for (const auto& [vpath, vtext] : *virtual_files)
        {
            s.paths.push_back(normal(vpath));
            s.texts.push_back(vtext);
        }

    std::vector<std::string>& args = s.args;
    args = {opt.compiler, "-std=" + (opt.std_version.empty() ? std::string(cxx_std_name()) : opt.std_version), "-O1",
#ifndef _WIN32
        "-fPIC",
#endif
        "-Werror=return-type",
        // A signed overflow wraps in a script. The compiler must not use it as undefined behavior.
        "-fwrapv",
#ifdef _WIN32
        // Experimental. HOT_EXPORT becomes dllimport: the code takes the runtime symbols from the host.
        // A Windows target has no -fPIC and no ELF visibility.
        "-DHOT_MODULE_BUILD=1"
#elif defined(__APPLE__)
        "-fvisibility=hidden", "-fvisibility-inlines-hidden",
        // Mach-O has no PLT, so no -fno-plt. JITLink registers the DWARF unwind data (__eh_frame)
        // of the code, not the compact unwind data. Without DWARF unwind data for each function,
        // an exception cannot pass a JIT frame. Untested (doc/macos.md).
        "-femit-dwarf-unwind=always"
#else
        "-fvisibility=hidden", "-fvisibility-inlines-hidden",
        // A call to the host reads the address from the GOT. A stub would add one jump to each call.
        "-fno-plt"
#endif
    };
#if HOT_ELF_TLS_DEPTH
    // The JIT links its code without TLS relocations. On ELF the code reads the call depth of the
    // host through the thread pointer and an absolute symbol (errors.h). Other targets call
    // hot::detail::call_depth_slot().
    args.push_back("-DHOT_JIT_TLS_OFFSET");
#endif
    // An MSVC host: the JIT code uses the C runtime DLL of the host, as in the files backend. The
    // JIT target is the host, so the triple is empty. The precompiled header gets the same flag.
    for (const std::string& f : host_ms_runtime_lib_flags(std::string()))
        args.push_back(f);
    if (!opt.debug_flag.empty())
        args.push_back(opt.debug_flag);
    if (!opt.resource_dir.empty())
        args.push_back("-resource-dir=" + opt.resource_dir);
    for (const std::string& f : opt.extra_flags)
        args.push_back(f);
#ifdef __APPLE__
    // The Clang library has no xcrun and reads no clang configuration file, so it can miss the
    // macOS SDK (the C and C++ headers).
    for (const std::string& f : darwin_sysroot_flags(args, apple_sdk_path()))
        args.push_back(f);
#endif
    // With a host file system, a header in the directory of the script is a virtual file there.
    if (opt.fs && !script_dir.empty())
        args.push_back("-I" + normal(script_dir));
    for (const std::string& d : opt.include_dirs)
    {
        if (opt.fs)
            args.push_back("-I" + normal(d));
        args.push_back("-I" + d);
    }
    for (const std::string& f : format_compile_flags())
        args.push_back(f);
    for (const std::string& f : checks_compile_flags())
        args.push_back(f);
    for (const std::string& f : thread_safe_compile_flags())
        args.push_back(f);
    for (const std::string& f : traceback_compile_flags())
        args.push_back(f);
    if (!as_pch && !opt.pch_path.empty())
    {
        args.push_back("-include-pch");
        args.push_back(opt.pch_path);
    }
    args.push_back(path);
    return s;
}

Compiled compile(const JitOptions& opt, const std::string& path, const std::string& text,
    const std::vector<std::pair<std::string, std::string>>& virtual_files, const std::string& script_dir)
{
    Compiled out;
    const CompileArgs s = make_args(opt, path, &text, false, &virtual_files, script_dir);
    const std::vector<const char*> args = c_strings(s.args);
    const std::vector<const char*> paths = c_strings(s.paths);
    const std::vector<const char*> texts = c_strings(s.texts);
    hot_jit_ir* ir = nullptr;
    char* diagnostics = nullptr;
    const int ok =
        hot_jit_compile(args.data(), args.size(), paths.data(), texts.data(), paths.size(), &ir, &diagnostics);
    out.diagnostics = take(diagnostics);
    if (ok)
    {
        out.ir = std::make_unique<IrHandle>();
        out.ir->ir = ir;
    }
    return out;
}

} // namespace

namespace
{

// Adds the module to a new JITDylib "id", runs the static initializers and finds
// script_register. The Module owns the JITDylib.
LoadResult add_to_jit(const std::shared_ptr<JitState>& impl, const std::string& id, hot_jit_ir* ir)
{
    LoadResult r;
    auto holder = std::make_unique<Generation>();
    holder->impl = impl;
    std::vector<const char*> abs_names;
    std::vector<uint64_t> abs_values;
#if HOT_ELF_TLS_DEPTH
    // The JIT code reads the call depth with the thread pointer and this offset (errors.h). The
    // address of the absolute symbol is the offset. The offset is the same in each thread.
    abs_names.push_back("hot_call_depth_tp_offset");
    abs_values.push_back(static_cast<uint64_t>(detail::call_depth_tp_offset()));
#endif
    // MSVC: the engine DLL (/MT) cannot find the static part of the host C runtime.
    jit::add_msvc_host_symbols(abs_names, abs_values);
    char* error = nullptr;
    holder->dylib =
        hot_jit_add(impl->engine, id.c_str(), ir, abs_names.data(), abs_values.data(), abs_names.size(), &error);
    if (!holder->dylib)
    {
        r.error = with_platform_hint(take(error));
        return r;
    }
    void* reg = hot_jit_lookup(holder->dylib, "script_register", &error);
    if (!reg)
    {
        r.error = with_platform_hint("script_register not found or not linkable: " + take(error) + "\n");
        return r;
    }
    void* entry = hot_jit_lookup(holder->dylib, "hot_main", &error);
    take(error); // a missing "hot_main" is not an error: the module has no entry point
    Module m;
    m.register_fn = reinterpret_cast<Module::RegisterFn>(reg);
    m.keep_alive = std::shared_ptr<void>(holder.release(), [](void* p) { delete static_cast<Generation*>(p); });
    m.origin = "jit:" + id;
    if (entry)
        m.entry_main = reinterpret_cast<Module::EntryMainFn>(entry);
    r.module = std::move(m);
    return r;
}

} // namespace

JitBackend::JitBackend(JitOptions options)
    : opt_(std::move(options))
{
    if (!opt_.session)
        opt_.session = std::make_shared<hotc::TranslatorSession>();
}

JitBackend::~JitBackend() = default;

void JitBackend::seed_slots(const std::string& text)
{
    std::lock_guard<std::mutex> lock(slots_->mutex);
    if (!slots_cover(slots_->text, text)) // else the state has every line of the text and more
        slots_->text = text;
}

void JitBackend::revert_slots(const std::string& text)
{
    std::lock_guard<std::mutex> lock(slots_->mutex);
    slots_->text = slots_revert(slots_->text, text);
}

LoadResult JitBackend::load(const ScriptSource& source, uint32_t generation, LoadMode mode)
{
    std::lock_guard<std::mutex> lock(mutex_);
    LoadResult r;
    namespace fs = std::filesystem;
    std::string stem = fs::path(source.name).stem().string();
    if (stem.empty())
        stem = "script";
    // A fresh load starts a new epoch. The epoch is in the name of the JITDylib, so a fresh
    // generation never has the name of a JITDylib that the process still uses.
    if (mode == LoadMode::Fresh)
        ++epoch_;
    const std::string id =
        stem + ".g" + std::to_string(generation) + ".e" + std::to_string(epoch_) + "." + std::to_string(sequence_++);
    // On Windows the path gets the drive of the working directory, as Clang reads it.
    std::error_code cwd_ec;
    const std::string cpp_path =
        jit_virtual_path(kVirtualDir, id + ".cpp", std::filesystem::current_path(cwd_ec).string());

    timings_ = {};
    Clock::time_point t0 = Clock::now();
    hotc::TranslateOptions to;
    to.script_text = source.text;
    to.modules = source.modules;
    to.script_name = source.name;
    to.unit_name = source.unit;
    to.output_name = cpp_path;
    to.generation = static_cast<int>(generation);
    to.include_dirs = opt_.include_dirs;
    // The translator needs the resource dir for the native headers. A portable build finds it at run time.
    to.resource_dir = opt_.resource_dir.empty() ? default_paths().resource_dir : opt_.resource_dir;
    to.std_version = opt_.std_version.empty() ? cxx_std_name() : opt_.std_version;
    // The parse of the native headers sees the macros of the compile.
    for (const std::string& f : opt_.extra_flags)
        if (f.rfind("-D", 0) == 0 || f.rfind("-U", 0) == 0)
            to.defines.push_back(f);
    to.strict_borrowed = opt_.strict_borrowed;
    to.null_warnings = opt_.null_warnings;
    to.format_lib = format_lib_name();
    to.view_type = view_type_name();
    to.fs = opt_.fs;
    to.preludes = opt_.preludes;
    // A fresh load ignores the slot state of the earlier generations.
    if (mode == LoadMode::Reload)
    {
        std::lock_guard<std::mutex> slock(slots_->mutex);
        to.slots_in = slots_->text;
    }
    hotc::TranslateResult tr = hotc::translate(to, *opt_.session);
    timings_.translate_ms = ms_since(t0);
    r.dependencies = tr.dependencies; // also after a failure
    r.iface_hash = tr.iface_hash;
    r.import_hashes = tr.import_hashes;
    if (!tr.ok)
    {
        r.error = opt_.raw_diagnostics
            ? tr.diagnostics
            : hotc::rewrite_translator_diagnostic(tr.diagnostics, hotc::script_texts(source.name, source.text, tr));
        r.reason = ReloadReason::CompileError;
        return r;
    }

    t0 = Clock::now();
    const size_t script_slash = source.name.rfind('/');
    const std::string script_dir =
        script_slash == std::string::npos ? std::string(".") : source.name.substr(0, script_slash);
    Compiled c = compile(opt_, cpp_path, tr.cpp, tr.virtual_files, script_dir);
    timings_.compile_ms = ms_since(t0);
    if (!c.ir)
    {
        if (!opt_.raw_diagnostics)
            c.diagnostics = hotc::rewrite_diagnostic(
                c.diagnostics, {{source.name, source.text}}, {{cpp_path, tr.cpp}}, &tr.source_map);
        r.error = c.diagnostics + "the compile of " + source.name + " failed\n";
        r.reason = ReloadReason::CompileError;
        return r;
    }

    t0 = Clock::now();
    if (!impl_)
    {
        auto impl = std::make_shared<JitState>();
        char* error = nullptr;
        char* debugger_error = nullptr;
        int debugger_on = 0;
        impl->engine = hot_jit_engine_create(opt_.debugger_support ? 1 : 0, &debugger_on, &error, &debugger_error);
        debugger_error_ = take(debugger_error);
        debugger_support_ = debugger_on != 0;
        if (!impl->engine)
        {
            r.error = take(error);
            return r;
        }
        take(error);
        impl_ = std::move(impl);
    }

    // The core instance (module.h) needs a copy of the IR: the JIT consumes the module.
    auto core_ir = std::make_shared<IrHandle>();
    core_ir->ir = hot_jit_ir_clone(c.ir->ir);
    hot_jit_ir* main_ir = c.ir->ir;
    c.ir->ir = nullptr;
    LoadResult added = add_to_jit(impl_, id, main_ir);
    if (!added)
    {
        r.error = added.error;
        r.reason = ReloadReason::BadModule;
        return r;
    }
    timings_.jit_ms = ms_since(t0);
    // The slot order changes only when the Loader accepts the module. The callback holds the
    // shared slot state, not the backend: the module can outlive the backend.
    added.module->on_accept = [state = slots_, text = tr.slots_out + tr.sigs_out]
    {
        std::lock_guard<std::mutex> slock(state->mutex);
        state->text = text;
    };
    // The core gets its own JITDylib. The loader calls make_core at most one time.
    std::shared_ptr<JitState> impl = impl_;
    added.module->make_core = [impl, id, core_ir]()
    {
        if (!core_ir->ir)
        {
            LoadResult e;
            e.error = "the core instance of jit:" + id + " is made already";
            return e;
        }
        hot_jit_ir* ir = core_ir->ir;
        core_ir->ir = nullptr;
        return add_to_jit(impl, id + ".core", ir);
    };
    added.dependencies = std::move(r.dependencies);
    added.iface_hash = r.iface_hash;
    added.import_hashes = r.import_hashes;
    added.module->iface_hash = r.iface_hash;
    added.module->imports = std::move(r.import_hashes);
    if (!source.unit.empty())
        added.module->unit = source.unit;
    return added;
}

bool JitBackend::make_pch(const std::string& header_path, const std::string& out_pch, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const CompileArgs s = make_args(opt_, header_path, nullptr, true);
    const std::vector<const char*> args = c_strings(s.args);
    char* diagnostics = nullptr;
    const int ok = hot_jit_make_pch(args.data(), args.size(), out_pch.c_str(), &diagnostics);
    error = take(diagnostics);
    return ok != 0;
}

} // namespace hot
