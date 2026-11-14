// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "hot/jit_backend.h"

#include "hot/command_line.h"
#include "hot/config.h"
#include "hot/hotc_lib.h"
#include "hot/paths.h"
#include "hot/runtime/errors.h"
#include "hot/runtime/process.h"

#include "jit_engine.h"
#include "jit_msvc_symbols.h"


#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
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

#if !defined(_WIN32) && !defined(__APPLE__)
// The object cache is on Linux only. The capture and the load of an object are proved for ELF
// only (jit_object_tests.cpp). On Windows a COFF object goes through RuntimeDyld and the memory
// manager of the unwind tables. The macOS JIT is untested (doc/macos.md).
constexpr bool kObjectCacheSupported = true;
#else
constexpr bool kObjectCacheSupported = false;
#endif

// The file of a cached object: this magic, the size and the hash (Hash64) of the object, both
// as 8 bytes little endian, and then the object. The cache holds machine code that the process
// runs, so a load checks the hash: a damaged file is a miss, not a crash.
constexpr char kObjectMagic[8] = {'H', 'O', 'T', 'J', 'I', 'T', 'O', '1'};
constexpr size_t kObjectHeaderSize = 24;

void put_u64(std::string& out, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

uint64_t get_u64(const char* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
    return v;
}

uint64_t object_hash(const char* data, size_t size)
{
    Hash64 h;
    h.add_bytes(data, size);
    return h.value();
}

// Reads a cached object and checks its header. On a failure it returns false and sets "error".
bool read_object_file(const std::filesystem::path& path, std::string& object, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof())
    {
        error = "cannot read " + path.string();
        return false;
    }
    if (text.size() < kObjectHeaderSize || text.compare(0, 8, kObjectMagic, 8) != 0)
    {
        error = "no object header";
        return false;
    }
    const uint64_t size = get_u64(text.data() + 8);
    if (size != text.size() - kObjectHeaderSize)
    {
        error = "the size is not correct";
        return false;
    }
    if (object_hash(text.data() + kObjectHeaderSize, size) != get_u64(text.data() + 16))
    {
        error = "the hash is not correct";
        return false;
    }
    object = text.substr(kObjectHeaderSize);
    return true;
}

// A path in a depfile (Make syntax, ObjectCache::parse_depfile).
std::string make_escape(const std::string& path)
{
    std::string out;
    for (const char c : path)
    {
        if (c == ' ' || c == '#' || c == '\\')
            out.push_back('\\');
        if (c == '$')
            out.push_back('$');
        out.push_back(c);
    }
    return out;
}

// Adds the object to the cache. "deps" are the files on disk that the compile read. On a
// failure it returns false and sets "error".
bool store_object(ObjectCache& cache, const ObjectCache::Key& key, const std::string& object,
    const std::vector<std::string>& deps, std::filesystem::file_time_type started, std::string& error)
{
    namespace fs = std::filesystem;
    const fs::path path = cache.object_path(key);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec)
    {
        error = "cannot create " + path.parent_path().string() + ": " + ec.message();
        return false;
    }
    // The name has ".tmp": ObjectCache::trim removes an old one. ".jit" keeps it apart from the
    // temporary files of ObjectCache.
    static std::atomic<unsigned> counter{0};
    const fs::path tmp = path.string() + ".tmp" + std::to_string(detail::process_id()) + "." +
        std::to_string(counter.fetch_add(1)) + ".jit";
    {
        std::string header(kObjectMagic, 8);
        put_u64(header, object.size());
        put_u64(header, object_hash(object.data(), object.size()));
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(header.data(), static_cast<std::streamsize>(header.size()));
        out.write(object.data(), static_cast<std::streamsize>(object.size()));
        out.close();
        if (!out)
        {
            fs::remove(tmp, ec);
            error = "cannot write " + tmp.string();
            return false;
        }
    }
    std::string depfile = make_escape(path.string()) + ":";
    for (const std::string& d : deps)
        depfile += " \\\n " + make_escape(d);
    depfile += "\n";
    if (cache.store(key, tmp, depfile, std::string(), started, error))
        return true;
    fs::remove(tmp, ec);
    return false;
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
    std::vector<std::string> deps; // the files that the compile read, one path in each item
};

// The compile command like the driver has it, and the files of the memory file system.
// "path" is the only input file and the last argument.
struct CompileArgs
{
    std::vector<std::string> args;
    std::vector<std::string> paths;
    std::vector<std::string> texts;
    // More text for the object cache key only: the absolute "-I" dirs.
    std::vector<std::string> key_args;
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
#ifdef NDEBUG
    // The host has NDEBUG, so the JIT code has it too. Else an inline function of a runtime header
    // has another body in the JIT code, for example in_script_tick() (threads.h). The optimization
    // level does not change.
    args.push_back("-DNDEBUG");
#endif
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
        // The cache key needs the place of a relative dir: another working directory can have
        // another header with the same name.
        s.key_args.push_back("-I" + normal(d));
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

// The compiles that run now and the most that ran at the same time (peak_parallel_compiles).
std::atomic<unsigned> g_active_compiles{0};
std::atomic<unsigned> g_peak_compiles{0};

// Counts one compile for peak_parallel_compiles.
struct CompileCount
{
    CompileCount()
    {
        const unsigned now = g_active_compiles.fetch_add(1) + 1;
        unsigned peak = g_peak_compiles.load();
        while (now > peak && !g_peak_compiles.compare_exchange_weak(peak, now))
        {
        }
    }
    ~CompileCount() { g_active_compiles.fetch_sub(1); }
};

// The Clang compile to IR. It is self-contained (hot_jit_compile), so many calls can run at the
// same time.
Compiled clang_compile(const CompileArgs& s)
{
    const CompileCount count;
    Compiled out;
    const std::vector<const char*> args = c_strings(s.args);
    const std::vector<const char*> paths = c_strings(s.paths);
    const std::vector<const char*> texts = c_strings(s.texts);
    hot_jit_ir* ir = nullptr;
    char* diagnostics = nullptr;
    char* deps = nullptr;
    const int ok = hot_jit_compile(
        args.data(), args.size(), paths.data(), texts.data(), paths.size(), &ir, &diagnostics, &deps);
    out.diagnostics = take(diagnostics);
    const std::string dep_text = take(deps);
    for (size_t start = 0; start < dep_text.size();)
    {
        size_t end = dep_text.find('\n', start);
        if (end == std::string::npos)
            end = dep_text.size();
        if (end > start)
            out.deps.push_back(dep_text.substr(start, end - start));
        start = end + 1;
    }
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
// script_register. The Module owns the JITDylib. The code is "ir" or, when "ir" is null, the
// cached object "object". With "captured", the call copies the object that ORC made from "ir"
// into it (empty when the engine has none). The call always releases the copy of the engine.
// "add_failed" is true when the add itself fails. Then no static initializer ran.
LoadResult add_to_jit(const std::shared_ptr<JitState>& impl, const std::string& id, hot_jit_ir* ir,
    const std::string* object, JitTimings* timings = nullptr, std::string* captured = nullptr,
    bool* add_failed = nullptr)
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
    holder->dylib = ir
        ? hot_jit_add(impl->engine, id.c_str(), ir, abs_names.data(), abs_values.data(), abs_names.size(), &error)
        : hot_jit_add_object(impl->engine, id.c_str(), object->data(), object->size(), abs_names.data(),
              abs_values.data(), abs_names.size(), &error);
    if (!holder->dylib)
    {
        if (add_failed)
            *add_failed = true;
        r.error = with_platform_hint(take(error));
        return r;
    }
    if (timings)
        hot_jit_add_timing(holder->dylib, &timings->orc_ms, &timings->init_ms);
    const Clock::time_point t_lookup = Clock::now();
    void* reg = hot_jit_lookup(holder->dylib, "script_register", &error);
    if (!reg)
    {
        r.error = with_platform_hint("script_register not found or not linkable: " + take(error) + "\n");
        return r;
    }
    void* entry = hot_jit_lookup(holder->dylib, "hot_main", &error);
    take(error); // a missing "hot_main" is not an error: the module has no entry point
    if (timings)
        timings->lookup_ms = ms_since(t_lookup);
    // The static initializers ran and the lookups found the code: ORC made the whole object.
    if (captured)
    {
        const void* data = nullptr;
        size_t size = 0;
        captured->clear();
        if (hot_jit_dylib_object(holder->dylib, &data, &size))
            captured->assign(static_cast<const char*>(data), size);
    }
    // The engine captures each object. Nobody else needs the copy, so it does not stay for the
    // life of the module.
    hot_jit_dylib_release_object(holder->dylib);
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
    // The registration of the LLVM targets writes global data with no lock. It runs here, before
    // the parallel translates and compiles of the loads: libclang first, then the engine.
    hotc::register_llvm_targets();
    hot_jit_init();
    if (!opt_.session)
        opt_.session = std::make_shared<hotc::TranslatorSession>();
    if (!opt_.cache_dir.empty() && TranslateCache::enabled_by_env())
    {
        // The JIT output has the virtual output name and the in-memory flags of this backend. The
        // text can differ from the text of the files backend, so "extra" keeps the entries apart.
        tr_cache_ = std::make_unique<TranslateCache>(opt_.cache_dir);
        tr_cache_->set_extra("jit");
    }
    // The object cache follows the rules of the files backend: HOT_CACHE=0 turns it off and
    // HOT_CACHE_MAX_MB gives the size limit.
    if (kObjectCacheSupported && !opt_.cache_dir.empty() && ObjectCache::enabled_by_env())
    {
        uint64_t max_bytes = opt_.cache_max_bytes;
        if (const char* env = std::getenv("HOT_CACHE_MAX_MB"); env && *env)
        {
            char* end = nullptr;
            const unsigned long long mb = std::strtoull(env, &end, 10);
            if (end && *end == '\0')
                max_bytes = static_cast<uint64_t>(mb) << 20;
        }
        obj_cache_ = std::make_unique<ObjectCache>(std::filesystem::path(opt_.cache_dir) / "jit", max_bytes);
        obj_cache_->trim();
        compiler_id_ = take(hot_jit_target_id()) + opt_.cache_id_extra;
    }
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

// A load in three phases (PendingLoad). The phases that touch the backend run on the caller
// thread with "mutex_". compile touches only this object, so many compiles run at the same time.
class JitBackend::Pending final : public PendingLoad
{
public:
    Pending(JitBackend& backend, const ScriptSource& source, uint32_t generation, LoadMode mode)
        : backend_(backend)
        , source_(source)
        , generation_(generation)
        , mode_(mode)
    {
    }

    // Phase 1, serial, with "mutex_": the name of the JITDylib, the epoch and the slot text.
    void prepare()
    {
        namespace fs = std::filesystem;
        const JitOptions& opt = backend_.opt_;
        stem_ = fs::path(source_.name).stem().string();
        if (stem_.empty())
            stem_ = "script";
        unit_label_ = source_.unit.empty() ? stem_ : source_.unit;
        // A fresh load starts a new epoch. The epoch is in the name of the JITDylib, so a fresh
        // generation never has the name of a JITDylib that the process still uses.
        if (mode_ == LoadMode::Fresh)
            ++backend_.epoch_;
        id_ = stem_ + ".g" + std::to_string(generation_) + ".e" + std::to_string(backend_.epoch_) + "." +
            std::to_string(backend_.sequence_++);
        // The virtual .cpp has the name of the unit, not the id: the "#line" name is in the text,
        // and equal script text must give equal text at each generation.
        // On Windows the path gets the drive of the working directory, as Clang reads it.
        std::error_code cwd_ec;
        cpp_path_ = jit_virtual_path(kVirtualDir, unit_label_ + ".cpp", fs::current_path(cwd_ec).string());

        to_.script_text = source_.text;
        to_.modules = source_.modules;
        to_.script_name = source_.name;
        to_.unit_name = source_.unit;
        to_.output_name = cpp_path_;
        to_.generation = static_cast<int>(generation_);
        to_.include_dirs = opt.include_dirs;
        // The translator needs the resource dir for the native headers. A portable build finds it at run time.
        to_.resource_dir = opt.resource_dir.empty() ? default_paths().resource_dir : opt.resource_dir;
        to_.std_version = opt.std_version.empty() ? cxx_std_name() : opt.std_version;
        // The parse of the native headers sees the macros of the compile.
        for (const std::string& f : opt.extra_flags)
            if (f.rfind("-D", 0) == 0 || f.rfind("-U", 0) == 0)
                to_.defines.push_back(f);
        to_.strict_borrowed = opt.strict_borrowed;
        to_.null_warnings = opt.null_warnings;
        to_.format_lib = format_lib_name();
        to_.view_type = view_type_name();
        to_.fs = opt.fs;
        to_.preludes = opt.preludes;
        // A fresh load ignores the slot state of the earlier generations.
        if (mode_ == LoadMode::Reload)
        {
            std::lock_guard<std::mutex> slock(backend_.slots_->mutex);
            to_.slots_in = backend_.slots_->text;
        }
    }

    // Phase 1, on any thread: the translate (with the translate cache), the compile command and
    // the key of the object. It writes only this object. The session and the translate cache are
    // thread safe. It catches each exception: the entry fails.
    void translate()
    {
        try
        {
            translate_unit();
        }
        catch (const std::exception& e)
        {
            fail(e.what()[0] ? e.what() : "the translate failed");
        }
        catch (...)
        {
            fail("the translate failed");
        }
    }

    // Phase 1, serial, with "mutex_": the lookup in the object cache.
    void lookup()
    {
        if (!ok || !key_)
            return;
        if (std::optional<ObjectCache::Hit> hit = backend_.obj_cache_->lookup(*key_))
            hit_path_ = hit->object;
    }

    // Phase 2, on any thread, with no lock: the read of the cached object, or the Clang compile.
    void compile() override
    {
        if (!ok)
            return;
        if (hit_path_)
        {
            auto bytes = std::make_shared<std::string>();
            std::string why;
            if (read_object_file(*hit_path_, *bytes, why))
            {
                object_ = std::move(bytes);
                return;
            }
            std::fprintf(stderr, "hot: %s: the cached object is not valid (%s), so it compiles again\n",
                unit_label_.c_str(), why.c_str());
            remove_entry_ = true; // finish removes the entry: the cache is not thread safe
        }
        run_compile();
    }

    // Phase 3, serial, with "mutex_", on the caller thread: the add to ORC, the static
    // initializers, the store in the object cache and the timings.
    LoadResult finish() override
    {
        std::lock_guard<std::mutex> lock(backend_.mutex_);
        // The destructor runs before the lock goes: the timings of the backend have this load.
        struct KeepTimings
        {
            JitBackend& backend;
            const JitTimings& timings;
            ~KeepTimings() { backend.timings_ = timings; }
        } keep{backend_, timings_};
        if (!ok)
            return std::move(head);
        LoadResult r = std::move(head);
        if (remove_entry_)
            backend_.obj_cache_->remove(*key_);

        JitTimings jt;
        LoadResult added;
        if (object_)
        {
            if (!start_engine(r))
                return r;
            bool add_failed = false;
            added = add_to_jit(backend_.impl_, id_, nullptr, object_.get(), &jt, nullptr, &add_failed);
            if (!added && add_failed)
            {
                // An error of hot_jit_add_object comes before the first static initializer
                // (jit_engine.h), so a compile and a second load are safe. The compile runs here,
                // on the caller thread.
                std::fprintf(stderr, "hot: %s: the cached object does not load, so it compiles again: %s\n",
                    unit_label_.c_str(), added.error.c_str());
                backend_.obj_cache_->remove(*key_);
                object_.reset();
            }
            else if (!added)
            {
                // The initializers ran already: a second load would run them again.
                r.error = added.error;
                r.reason = ReloadReason::BadModule;
                return r;
            }
        }
        std::shared_ptr<IrHandle> core_ir;
        if (object_)
            ++backend_.object_hits_;
        else
        {
            if (backend_.obj_cache_)
                ++backend_.object_misses_;
            if (!compiled_done_)
                run_compile();
            if (!compiled_.ir)
            {
                if (!backend_.opt_.raw_diagnostics)
                    compiled_.diagnostics = hotc::rewrite_diagnostic(
                        compiled_.diagnostics, {{source_.name, source_.text}}, {{cpp_path_, tr_.cpp}}, &tr_.source_map);
                r.error = compiled_.diagnostics + "the compile of " + source_.name + " failed\n";
                r.reason = ReloadReason::CompileError;
                return r;
            }
            if (!start_engine(r))
                return r;
            // The core instance (module.h) needs a copy of the IR: the JIT consumes the module.
            core_ir = std::make_shared<IrHandle>();
            core_ir->ir = hot_jit_ir_clone(compiled_.ir->ir);
            hot_jit_ir* main_ir = compiled_.ir->ir;
            compiled_.ir->ir = nullptr;
            std::string captured;
            added = add_to_jit(backend_.impl_, id_, main_ir, nullptr, &jt, key_ ? &captured : nullptr);
            if (!added)
            {
                r.error = added.error;
                r.reason = ReloadReason::BadModule;
                return r;
            }
            if (key_ && captured.empty())
                std::fprintf(
                    stderr, "hot: %s has no object cache entry: the engine kept no object\n", unit_label_.c_str());
            else if (key_)
                store(captured);
        }
        timings_.orc_ms = jt.orc_ms;
        timings_.init_ms = jt.init_ms;
        timings_.lookup_ms = jt.lookup_ms;
        timings_.jit_ms = jt.orc_ms + jt.init_ms + jt.lookup_ms;
        // "total" is the sum of the phases: in a batch, a load also waits for the other units.
        if (std::getenv("HOT_TIMING"))
            std::fprintf(stderr,
                "hot: %s jit translate %.0f ms, compile %.0f ms, orc %.0f ms, init %.0f ms, lookup %.0f ms, total "
                "%.0f ms%s\n",
                unit_label_.c_str(), timings_.translate_ms, timings_.compile_ms, timings_.orc_ms, timings_.init_ms,
                timings_.lookup_ms, timings_.translate_ms + timings_.compile_ms + timings_.jit_ms,
                !backend_.obj_cache_ ? "" : (object_ ? ", object cache hit" : ", object cache miss"));
        added.module->generation = generation_;
        // The slot order changes only when the Loader accepts the module. The callback holds the
        // shared slot state, not the backend: the module can outlive the backend.
        added.module->on_accept = [state = backend_.slots_, text = tr_.slots_out + tr_.sigs_out]
        {
            std::lock_guard<std::mutex> slock(state->mutex);
            state->text = text;
        };
        // The core gets its own JITDylib. The loader calls make_core at most one time. After a
        // cache hit there is no IR: the core gets the same object a second time.
        std::shared_ptr<JitState> impl = backend_.impl_;
        added.module->make_core = [impl, id = id_, core_ir, object = object_]() mutable
        {
            if (object)
            {
                const std::shared_ptr<const std::string> bytes = std::move(object);
                return add_to_jit(impl, id + ".core", nullptr, bytes.get());
            }
            if (!core_ir || !core_ir->ir)
            {
                LoadResult e;
                e.error = "the core instance of jit:" + id + " is made already";
                return e;
            }
            hot_jit_ir* ir = core_ir->ir;
            core_ir->ir = nullptr;
            return add_to_jit(impl, id + ".core", ir, nullptr);
        };
        added.dependencies = std::move(r.dependencies);
        added.iface_hash = r.iface_hash;
        added.import_hashes = r.import_hashes;
        added.iface_info = r.iface_info;
        added.use_info = r.use_info;
        added.module->iface_hash = r.iface_hash;
        added.module->imports = std::move(r.import_hashes);
        added.module->iface_info = std::move(r.iface_info);
        added.module->use_info = std::move(r.use_info);
        if (!source_.unit.empty())
            added.module->unit = source_.unit;
        return added;
    }

    // Marks the entry as failed.
    void fail(std::string error)
    {
        head.error = std::move(error);
        head.reason = ReloadReason::CompileError;
        ok = false;
    }

private:
    void translate_unit()
    {
        const JitOptions& opt = backend_.opt_;
        const Clock::time_point t0 = Clock::now();
        // The translate cache gives the result of an earlier translate with the same inputs.
        std::optional<hotc::TranslateResult> cached;
        if (backend_.tr_cache_)
            cached = backend_.tr_cache_->lookup(to_);
        translate_start_ = std::filesystem::file_time_type::clock::now();
        const bool translate_hit = cached.has_value();
        tr_ = translate_hit ? std::move(*cached) : hotc::translate(to_, *opt.session);
        if (backend_.tr_cache_ && !translate_hit && tr_.ok && tr_.diagnostics.empty())
        {
            std::string store_error;
            if (!backend_.tr_cache_->store(to_, tr_, translate_start_, store_error))
                std::fprintf(
                    stderr, "hot: %s has no translate cache entry: %s\n", source_.name.c_str(), store_error.c_str());
        }
        timings_.translate_ms = ms_since(t0);
        head.dependencies = tr_.dependencies; // also after a failure
        head.iface_hash = tr_.iface_hash;
        head.import_hashes = tr_.import_hashes;
        head.iface_info = tr_.iface_info;
        head.use_info = tr_.use_info;
        if (!tr_.ok)
        {
            fail(opt.raw_diagnostics ? tr_.diagnostics
                                     : hotc::rewrite_translator_diagnostic(
                                           tr_.diagnostics, hotc::script_texts(source_.name, source_.text, tr_)));
            return;
        }
        const size_t script_slash = source_.name.rfind('/');
        const std::string script_dir =
            script_slash == std::string::npos ? std::string(".") : source_.name.substr(0, script_slash);
        args_ = make_args(opt, cpp_path_, &tr_.cpp, false, &tr_.virtual_files, script_dir);
        // The key of the object has all the inputs of the compile and of the code generation, but
        // no name of this load (generation, epoch, sequence): each load gives the dylib a new name.
        if (backend_.obj_cache_)
        {
            ObjectCache::KeyInputs inputs;
            inputs.cpp_text = tr_.cpp;
            inputs.args = args_.args;
            inputs.args.insert(inputs.args.end(), args_.key_args.begin(), args_.key_args.end());
            inputs.compiler_id = backend_.compiler_id_;
            if (opt.fs)
                inputs.virtual_files = tr_.virtual_files;
            // A sum of the path hashes: the order of the paths is different after a translate cache hit.
            for (const std::string& p : tr_.shadow_paths)
                inputs.shadow_hash += hotc::text_hash(p);
            key_ = ObjectCache::make_key(inputs);
        }
    }

    void run_compile()
    {
        const Clock::time_point t0 = Clock::now();
        ++backend_.compiles_;
        compiled_ = clang_compile(args_);
        timings_.compile_ms = ms_since(t0);
        compiled_done_ = true;
    }

    // The engine starts at the first load that has code to add.
    bool start_engine(LoadResult& r)
    {
        if (backend_.impl_)
            return true;
        auto impl = std::make_shared<JitState>();
        char* error = nullptr;
        char* debugger_error = nullptr;
        int debugger_on = 0;
        const std::vector<const void*> host_runtime = jit::host_runtime_addresses();
        impl->engine = hot_jit_engine_create(backend_.opt_.debugger_support ? 1 : 0, host_runtime.data(),
            host_runtime.size(), &debugger_on, &error, &debugger_error);
        backend_.debugger_error_ = take(debugger_error);
        backend_.debugger_support_ = debugger_on != 0;
        if (!impl->engine)
        {
            r.error = take(error);
            return false;
        }
        take(error);
        if (backend_.obj_cache_)
            hot_jit_capture_objects(impl->engine, 1);
        backend_.impl_ = std::move(impl);
        return true;
    }

    // Adds the captured object to the cache. The manifest has the files on disk. The files of the
    // memory file system are in the key.
    void store(const std::string& captured)
    {
        namespace fs = std::filesystem;
        const std::set<std::string> in_memory(args_.paths.begin(), args_.paths.end());
        std::set<std::string> seen;
        std::vector<std::string> deps;
        const auto add_dep = [&](const std::string& d)
        {
            std::error_code ec;
            const std::string p = fs::absolute(d, ec).lexically_normal().string();
            if (!ec && !in_memory.count(p) && p.rfind(kVirtualDir, 0) != 0 && seen.insert(p).second)
                deps.push_back(p);
        };
        for (const std::string& d : compiled_.deps)
            add_dep(d);
        if (!backend_.opt_.pch_path.empty())
            add_dep(backend_.opt_.pch_path);
        std::string store_error;
        if (!store_object(*backend_.obj_cache_, *key_, captured, deps, translate_start_, store_error))
            std::fprintf(stderr, "hot: %s has no object cache entry: %s\n", unit_label_.c_str(), store_error.c_str());
    }

    JitBackend& backend_;
    ScriptSource source_;
    uint32_t generation_;
    LoadMode mode_;
    std::string stem_;
    std::string unit_label_;
    std::string id_;
    std::string cpp_path_;
    hotc::TranslateOptions to_;
    hotc::TranslateResult tr_;
    std::filesystem::file_time_type translate_start_{};
    CompileArgs args_;
    std::optional<ObjectCache::Key> key_;
    std::optional<std::filesystem::path> hit_path_; // the file of an object cache hit
    std::shared_ptr<const std::string> object_; // the object of a cache hit
    bool remove_entry_ = false; // the cached object is not valid
    Compiled compiled_;
    bool compiled_done_ = false;
    JitTimings timings_;
};

LoadResult JitBackend::load(const ScriptSource& source, uint32_t generation, LoadMode mode)
{
    // The same phases as a wave of one unit, all on this thread.
    Pending p(*this, source, generation, mode);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        p.prepare();
        p.translate();
        p.lookup();
    }
    p.compile();
    return p.finish();
}

std::unique_ptr<PendingLoad> JitBackend::begin_load(const ScriptSource& source, uint32_t generation, LoadMode mode)
{
#ifdef _WIN32
    // The JIT on Windows links with RuntimeDyld. It keeps the serial load of the default.
    return Backend::begin_load(source, generation, mode);
#else
    // One code path: a wave of one unit. It starts no thread.
    std::vector<std::unique_ptr<PendingLoad>> many = begin_load_many({source}, generation, mode);
    return std::move(many.front());
#endif
}

std::vector<std::unique_ptr<PendingLoad>> JitBackend::begin_load_many(
    const std::vector<ScriptSource>& sources, uint32_t generation, LoadMode mode)
{
#ifdef _WIN32
    return Backend::begin_load_many(sources, generation, mode);
#else
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::unique_ptr<PendingLoad>> result;
    std::vector<Pending*> loads;
    result.reserve(sources.size());
    // The serial part: the names, the epochs and the slot text, in the order of the sources.
    for (const ScriptSource& source : sources)
    {
        auto pending = std::make_unique<Pending>(*this, source, generation, mode);
        try
        {
            pending->prepare();
            loads.push_back(pending.get());
        }
        catch (const std::exception& e)
        {
            pending->fail(e.what()[0] ? e.what() : "the load failed");
        }
        result.push_back(std::move(pending));
    }
    // The parallel part: the same thread pattern as run_compiles. HOT_JOBS=N gives N threads in total.
    std::atomic<size_t> next{0};
    const auto work = [&]
    {
        for (size_t i = next.fetch_add(1); i < loads.size(); i = next.fetch_add(1))
            loads[i]->translate(); // catches each exception
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
    {
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
    }
    // The serial part: the object cache is not thread safe.
    for (Pending* p : loads)
        p->lookup();
    return result;
#endif
}

unsigned JitBackend::peak_parallel_compiles(bool reset)
{
    return reset ? g_peak_compiles.exchange(0) : g_peak_compiles.load();
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
