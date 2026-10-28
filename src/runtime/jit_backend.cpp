#include "spp/jit_backend.h"

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

#include "spp/sppc_lib.h"

namespace spp {

namespace {

// A path that does not exist on disk. The compile reads the file from memory.
constexpr const char* kVirtualDir = "/spp-jit-virtual";

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

std::string error_text(llvm::Error e) { return llvm::toString(std::move(e)); }

void init_llvm_once()
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
    });
}

}  // namespace

// The LLJIT of the session. The Modules keep a copy of the pointer to this object.
struct JitState {
    std::unique_ptr<llvm::orc::LLJIT> jit;
};

namespace {

// What one generation owns in the JIT. The destructor runs the static
// destructors and removes the code (ResourceTracker::remove).
struct Generation {
    std::shared_ptr<JitState> impl;
    llvm::orc::JITDylib* dylib = nullptr;
    llvm::orc::ResourceTrackerSP tracker;
    bool initialized = false;

    ~Generation()
    {
        if (initialized) llvm::consumeError(impl->jit->deinitialize(*dylib));
        if (tracker) llvm::consumeError(tracker->remove());
        if (dylib) llvm::consumeError(impl->jit->getExecutionSession().removeJITDylib(*dylib));
    }
};

struct Compiled {
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::LLVMContext> context;
    std::string diagnostics;
};

// Builds the compile command like the driver does and parses it into a
// CompilerInvocation. "path" is the only input file. With a non-null "text" the
// file exists only in memory.
struct CompileSetup {
    // The invocation keeps pointers into the arguments (for example for the
    // debug flags), so the strings must live as long as the invocation.
    std::vector<std::string> args;
    llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> vfs;
    std::unique_ptr<clang::CompilerInvocation> invocation;
};

CompileSetup make_invocation(const JitOptions& opt, const std::string& path, const std::string* text,
                             bool as_pch, std::string& diagnostics)
{
    CompileSetup s;
    auto memory = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
    if (text) memory->addFile(path, 0, llvm::MemoryBuffer::getMemBufferCopy(*text, path));
    s.vfs = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(llvm::vfs::getRealFileSystem());
    s.vfs->pushOverlay(memory);

    std::vector<std::string>& args = s.args;
    args = {opt.compiler, "-std=c++20", "-O1", "-fPIC", "-Werror=return-type"};
    if (!opt.debug_flag.empty()) args.push_back(opt.debug_flag);
    if (!opt.resource_dir.empty()) args.push_back("-resource-dir=" + opt.resource_dir);
    for (const std::string& f : opt.extra_flags) args.push_back(f);
    for (const std::string& d : opt.include_dirs) args.push_back("-I" + d);
    if (!as_pch && !opt.pch_path.empty()) {
        args.push_back("-include-pch");
        args.push_back(opt.pch_path);
    }
    args.push_back(path);
    std::vector<const char*> cargs;
    for (const std::string& a : args) cargs.push_back(a.c_str());

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

Compiled compile(const JitOptions& opt, const std::string& path, const std::string& text)
{
    Compiled out;
    CompileSetup s = make_invocation(opt, path, &text, false, out.diagnostics);
    if (!s.invocation) {
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
    if (!ok || ci.getDiagnostics().hasErrorOccurred()) return out;
    out.module = action.takeModule();
    out.context.reset(action.takeLLVMContext());
    if (!out.module) out.diagnostics += "the compile made no module\n";
    return out;
}

}  // namespace

namespace {

// Adds the module to a new JITDylib "id", runs the static initializers and finds
// script_register. The Module owns the JITDylib.
LoadResult add_to_jit(const std::shared_ptr<JitState>& impl, const std::string& id, llvm::orc::ThreadSafeModule tsm)
{
    LoadResult r;
    auto holder = std::make_unique<Generation>();
    holder->impl = impl;
    llvm::orc::LLJIT& jit = *impl->jit;
    auto dylib = jit.createJITDylib(id);
    if (!dylib) {
        r.error = "cannot make a JITDylib: " + error_text(dylib.takeError()) + "\n";
        return r;
    }
    holder->dylib = &*dylib;
    holder->tracker = holder->dylib->createResourceTracker();
    if (llvm::Error e = jit.addIRModule(holder->tracker, std::move(tsm))) {
        r.error = "cannot add the module: " + error_text(std::move(e)) + "\n";
        return r;  // "holder" removes the dylib
    }
    // Static initializers run here: for example the dynamic initializer of the class record.
    if (llvm::Error e = jit.initialize(*holder->dylib)) {
        r.error = "cannot run the static initializers: " + error_text(std::move(e)) + "\n";
        return r;
    }
    holder->initialized = true;
    auto addr = jit.lookup(*holder->dylib, "script_register");
    if (!addr) {
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

}  // namespace

JitBackend::JitBackend(JitOptions options) : opt_(std::move(options))
{
    if (!opt_.session) opt_.session = std::make_shared<sppc::TranslatorSession>();
}

JitBackend::~JitBackend() = default;

LoadResult JitBackend::load(const ScriptSource& source, uint32_t generation)
{
    std::lock_guard<std::mutex> lock(mutex_);
    LoadResult r;
    init_llvm_once();
    namespace fs = std::filesystem;
    std::string stem = fs::path(source.name).stem().string();
    if (stem.empty()) stem = "script";
    const std::string id = stem + ".g" + std::to_string(generation) + "." + std::to_string(sequence_++);
    const std::string cpp_path = std::string(kVirtualDir) + "/" + id + ".cpp";

    timings_ = {};
    Clock::time_point t0 = Clock::now();
    sppc::TranslateOptions to;
    to.script_text = source.text;
    to.script_name = source.name;
    to.output_name = cpp_path;
    to.generation = static_cast<int>(generation);
    to.include_dirs = opt_.include_dirs;
    to.resource_dir = opt_.resource_dir;
    to.strict_borrowed = opt_.strict_borrowed;
    to.format_lib = opt_.format_lib;
    to.slots_in = slots_;
    sppc::TranslateResult tr = sppc::translate(to, *opt_.session);
    timings_.translate_ms = ms_since(t0);
    if (!tr.ok) {
        r.error = tr.diagnostics;
        return r;
    }

    t0 = Clock::now();
    Compiled c = compile(opt_, cpp_path, tr.cpp);
    timings_.compile_ms = ms_since(t0);
    if (!c.module) {
        if (!opt_.raw_diagnostics)
            c.diagnostics = sppc::rewrite_diagnostic(c.diagnostics, {{source.name, source.text}}, {{cpp_path, tr.cpp}});
        r.error = c.diagnostics + "the compile of " + source.name + " failed\n";
        return r;
    }

    t0 = Clock::now();
    if (!impl_) {
        auto impl = std::make_shared<JitState>();
        auto jit = llvm::orc::LLJITBuilder().create();
        if (!jit) {
            r.error = "cannot make the JIT: " + error_text(jit.takeError()) + "\n";
            return r;
        }
        impl->jit = std::move(*jit);
        // The debugger sees the JIT code: the plugin registers each linked object through the
        // GDB JIT interface (__jit_debug_descriptor). GDB and LLDB read it. The object has the
        // line table of "-gline-tables-only", so the debugger shows the .spp files and lines.
        // A failure is not fatal: the code runs, only without debugger support.
        if (opt_.debugger_support) {
            if (llvm::Error e = llvm::orc::enableDebuggerSupport(*impl->jit))
                debugger_error_ = error_text(std::move(e));
            else
                debugger_support_ = true;
        }
        // The executable links with -rdynamic: the engine, the runtime and libstdc++ resolve here.
        auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            impl->jit->getDataLayout().getGlobalPrefix());
        if (!gen) {
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
    if (!added) {
        r.error = added.error;
        return r;
    }
    timings_.jit_ms = ms_since(t0);
    slots_ = tr.slots_out + tr.sigs_out;  // the slot order changes only when the whole load succeeds
    // The core gets its own JITDylib. The loader calls make_core at most one time.
    std::shared_ptr<JitState> impl = impl_;
    added.module->make_core = [impl, id, core_ir]() {
        if (!*core_ir) {
            LoadResult e;
            e.error = "the core instance of jit:" + id + " is made already";
            return e;
        }
        return add_to_jit(impl, id + ".core", std::move(*core_ir));
    };
    return added;
}

bool JitBackend::make_pch(const std::string& header_path, const std::string& out_pch,
                          std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    init_llvm_once();
    error.clear();
    CompileSetup s = make_invocation(opt_, header_path, nullptr, true, error);
    if (!s.invocation) {
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

}  // namespace spp
