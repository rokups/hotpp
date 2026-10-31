#include "hot/jit_backend.h"

#include "hot/config.h"
#include "hot/hotc_lib.h"
#include "hot/paths.h"

#include <clang/Basic/Diagnostic.h>
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Driver/CreateInvocationFromArgs.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <llvm/ExecutionEngine/Orc/Debugging/DebuggerSupport.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <chrono>
#include <filesystem>
#include <mutex>

namespace hot
{

namespace
{

// A path that does not exist on disk. The compile reads the file from memory.
constexpr const char* kVirtualDir = "/hot-jit-virtual";

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

std::string error_text(llvm::Error e)
{
    return llvm::toString(std::move(e));
}

void init_llvm_once()
{
    static std::once_flag flag;
    std::call_once(flag, []
    {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
    });
}

} // namespace

// The LLJIT of the session. The Modules keep a copy of the pointer to this object.
struct JitState
{
    std::unique_ptr<llvm::orc::LLJIT> jit;
};

namespace
{

// What one generation owns in the JIT. The destructor runs the static
// destructors and removes the code (ResourceTracker::remove).
struct Generation
{
    std::shared_ptr<JitState> impl;
    llvm::orc::JITDylib* dylib = nullptr;
    llvm::orc::ResourceTrackerSP tracker;
    bool initialized = false;

    ~Generation()
    {
        if (initialized)
            llvm::consumeError(impl->jit->deinitialize(*dylib));
        if (tracker)
            llvm::consumeError(tracker->remove());
        if (dylib)
            llvm::consumeError(impl->jit->getExecutionSession().removeJITDylib(*dylib));
    }
};

struct Compiled
{
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::LLVMContext> context;
    std::string diagnostics;
};

// Builds the compile command like the driver does and parses it into a
// CompilerInvocation. "path" is the only input file. With a non-null "text" the
// file exists only in memory.
struct CompileSetup
{
    // The invocation keeps pointers into the arguments (for example for the
    // debug flags), so the strings must live as long as the invocation.
    std::vector<std::string> args;
    llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> vfs;
    std::unique_ptr<clang::CompilerInvocation> invocation;
};

// "virtual_files" (path, text) are the headers that only the file system of the host holds. They
// go into the memory file system under their exact paths.
CompileSetup make_invocation(const JitOptions& opt, const std::string& path, const std::string* text, bool as_pch,
    std::string& diagnostics, const std::vector<std::pair<std::string, std::string>>* virtual_files = nullptr,
    const std::string& script_dir = std::string())
{
    CompileSetup s;
    auto memory = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
    if (text)
        memory->addFile(path, 0, llvm::MemoryBuffer::getMemBufferCopy(*text, path));
    // The memory file system needs absolute normal paths, so the paths of the files and the paths
    // of the "-I" dirs are made absolute and normal when a host file system is set.
    auto normal = [](const std::string& p) { return std::filesystem::absolute(p).lexically_normal().string(); };
    if (virtual_files)
        for (const auto& [vpath, vtext] : *virtual_files)
        {
            const std::string full = normal(vpath);
            memory->addFile(full, 0, llvm::MemoryBuffer::getMemBufferCopy(vtext, full));
        }
    s.vfs = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(llvm::vfs::getRealFileSystem());
    s.vfs->pushOverlay(memory);

    std::vector<std::string>& args = s.args;
    args = {opt.compiler, "-std=" + (opt.std_version.empty() ? std::string(cxx_std_name()) : opt.std_version), "-O1",
        "-fPIC", "-Werror=return-type"};
    if (!opt.debug_flag.empty())
        args.push_back(opt.debug_flag);
    if (!opt.resource_dir.empty())
        args.push_back("-resource-dir=" + opt.resource_dir);
    for (const std::string& f : opt.extra_flags)
        args.push_back(f);
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
    for (const std::string& f : traceback_compile_flags())
        args.push_back(f);
    if (!as_pch && !opt.pch_path.empty())
    {
        args.push_back("-include-pch");
        args.push_back(opt.pch_path);
    }
    args.push_back(path);
    std::vector<const char*> cargs;
    for (const std::string& a : args)
        cargs.push_back(a.c_str());

    llvm::raw_string_ostream os(diagnostics);
    clang::DiagnosticOptions diag_opts;
    clang::TextDiagnosticPrinter printer(os, diag_opts);
    llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> diags =
        clang::CompilerInstance::createDiagnostics(*s.vfs, diag_opts, &printer, false);
    clang::CreateInvocationOptions cio;
    cio.Diags = diags;
    cio.VFS = s.vfs;
    s.invocation = clang::createInvocation(cargs, cio);
    return s;
}

Compiled compile(const JitOptions& opt, const std::string& path, const std::string& text,
    const std::vector<std::pair<std::string, std::string>>& virtual_files, const std::string& script_dir)
{
    Compiled out;
    CompileSetup s = make_invocation(opt, path, &text, false, out.diagnostics, &virtual_files, script_dir);
    if (!s.invocation)
    {
        out.diagnostics += "cannot build the compile command\n";
        return out;
    }
    llvm::raw_string_ostream os(out.diagnostics);
    clang::DiagnosticOptions print_opts = s.invocation->getDiagnosticOpts();
    // ExecuteAction writes "N errors generated." to stderr when ShowCarets is on.
    s.invocation->getDiagnosticOpts().ShowCarets = false;
    clang::TextDiagnosticPrinter printer(os, print_opts);
    clang::CompilerInstance ci(std::shared_ptr<clang::CompilerInvocation>(std::move(s.invocation)));
    ci.setVirtualFileSystem(s.vfs);
    ci.createDiagnostics(&printer, false);
    clang::EmitLLVMOnlyAction action;
    const bool ok = ci.ExecuteAction(action);
    os.flush();
    if (!ok || ci.getDiagnostics().hasErrorOccurred())
        return out;
    out.module = action.takeModule();
    out.context.reset(action.takeLLVMContext());
    if (!out.module)
        out.diagnostics += "the compile made no module\n";
    return out;
}

} // namespace

namespace
{

// Adds the module to a new JITDylib "id", runs the static initializers and finds
// script_register. The Module owns the JITDylib.
LoadResult add_to_jit(const std::shared_ptr<JitState>& impl, const std::string& id, llvm::orc::ThreadSafeModule tsm)
{
    LoadResult r;
    auto holder = std::make_unique<Generation>();
    holder->impl = impl;
    llvm::orc::LLJIT& jit = *impl->jit;
    auto dylib = jit.createJITDylib(id);
    if (!dylib)
    {
        r.error = "cannot make a JITDylib: " + error_text(dylib.takeError()) + "\n";
        return r;
    }
    holder->dylib = &*dylib;
    holder->tracker = holder->dylib->createResourceTracker();
    if (llvm::Error e = jit.addIRModule(holder->tracker, std::move(tsm)))
    {
        r.error = "cannot add the module: " + error_text(std::move(e)) + "\n";
        return r; // "holder" removes the dylib
    }
    // Static initializers run here: for example the dynamic initializer of the class record.
    if (llvm::Error e = jit.initialize(*holder->dylib))
    {
        r.error = "cannot run the static initializers: " + error_text(std::move(e)) + "\n";
        return r;
    }
    holder->initialized = true;
    auto addr = jit.lookup(*holder->dylib, "script_register");
    if (!addr)
    {
        r.error = "script_register not found or not linkable: " + error_text(addr.takeError()) + "\n";
        return r;
    }
    Module m;
    m.register_fn = addr->toPtr<Module::RegisterFn>();
    m.keep_alive = std::shared_ptr<void>(holder.release(), [](void* p) { delete static_cast<Generation*>(p); });
    m.origin = "jit:" + id;
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
    init_llvm_once();
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
    const std::string cpp_path = std::string(kVirtualDir) + "/" + id + ".cpp";

    timings_ = {};
    Clock::time_point t0 = Clock::now();
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
    to.strict_borrowed = opt_.strict_borrowed;
    to.format_lib = format_lib_name();
    to.fs = opt_.fs;
    // A fresh load ignores the slot state of the earlier generations.
    if (mode == LoadMode::Reload)
    {
        std::lock_guard<std::mutex> slock(slots_->mutex);
        to.slots_in = slots_->text;
    }
    hotc::TranslateResult tr = hotc::translate(to, *opt_.session);
    timings_.translate_ms = ms_since(t0);
    if (!tr.ok)
    {
        r.error = opt_.raw_diagnostics
            ? tr.diagnostics
            : hotc::rewrite_translator_diagnostic(tr.diagnostics, hotc::script_texts(source.name, source.text, tr));
        r.reason = tr.slots_removed ? ReloadReason::SlotsRemoved : ReloadReason::CompileError;
        return r;
    }

    t0 = Clock::now();
    const size_t script_slash = source.name.rfind('/');
    const std::string script_dir =
        script_slash == std::string::npos ? std::string(".") : source.name.substr(0, script_slash);
    Compiled c = compile(opt_, cpp_path, tr.cpp, tr.virtual_files, script_dir);
    timings_.compile_ms = ms_since(t0);
    if (!c.module)
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
        auto jit = llvm::orc::LLJITBuilder().create();
        if (!jit)
        {
            r.error = "cannot make the JIT: " + error_text(jit.takeError()) + "\n";
            return r;
        }
        impl->jit = std::move(*jit);
        // The debugger sees the JIT code: the plugin registers each linked object through the
        // GDB JIT interface (__jit_debug_descriptor). GDB and LLDB read it. The object has the
        // line table of "-gline-tables-only", so the debugger shows the .hot files and lines.
        // A failure is not fatal: the code runs, only without debugger support.
        if (opt_.debugger_support)
        {
            if (llvm::Error e = llvm::orc::enableDebuggerSupport(*impl->jit))
                debugger_error_ = error_text(std::move(e));
            else
                debugger_support_ = true;
        }
        // The executable links with -rdynamic: the engine, the runtime and libstdc++ resolve here.
        auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            impl->jit->getDataLayout().getGlobalPrefix());
        if (!gen)
        {
            r.error = "cannot search the process symbols: " + error_text(gen.takeError()) + "\n";
            return r;
        }
        impl->jit->getMainJITDylib().addGenerator(std::move(*gen));
        impl_ = std::move(impl);
    }

    // The core instance (module.h) needs a copy of the IR: the JIT consumes the module.
    auto tsc = llvm::orc::ThreadSafeContext(std::move(c.context));
    auto core_ir = std::make_shared<llvm::orc::ThreadSafeModule>(llvm::CloneModule(*c.module), tsc);
    LoadResult added = add_to_jit(impl_, id, llvm::orc::ThreadSafeModule(std::move(c.module), tsc));
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
        if (!*core_ir)
        {
            LoadResult e;
            e.error = "the core instance of jit:" + id + " is made already";
            return e;
        }
        return add_to_jit(impl, id + ".core", std::move(*core_ir));
    };
    return added;
}

bool JitBackend::make_pch(const std::string& header_path, const std::string& out_pch, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    init_llvm_once();
    error.clear();
    CompileSetup s = make_invocation(opt_, header_path, nullptr, true, error);
    if (!s.invocation)
    {
        error += "cannot build the compile command\n";
        return false;
    }
    s.invocation->getFrontendOpts().OutputFile = out_pch;
    llvm::raw_string_ostream os(error);
    clang::DiagnosticOptions print_opts = s.invocation->getDiagnosticOpts();
    s.invocation->getDiagnosticOpts().ShowCarets = false;
    clang::TextDiagnosticPrinter printer(os, print_opts);
    clang::CompilerInstance ci(std::shared_ptr<clang::CompilerInvocation>(std::move(s.invocation)));
    ci.setVirtualFileSystem(s.vfs);
    ci.createDiagnostics(&printer, false);
    clang::GeneratePCHAction action;
    const bool ok = ci.ExecuteAction(action);
    os.flush();
    return ok && !ci.getDiagnostics().hasErrorOccurred();
}

} // namespace hot
