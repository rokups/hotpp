// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The JIT engine (jit_engine.h). This file uses only LLVM, Clang and the standard library. It
// must not use the hot runtime: on MSVC this code has another C runtime than the runtime.

#include "jit_engine.h"

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
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct hot_jit_ir
{
    llvm::orc::ThreadSafeContext context;
    std::unique_ptr<llvm::Module> module;
};

struct hot_jit_engine
{
    std::unique_ptr<llvm::orc::LLJIT> jit;
};

struct hot_jit_dylib
{
    hot_jit_engine* engine = nullptr;
    llvm::orc::JITDylib* dylib = nullptr;
    llvm::orc::ResourceTrackerSP tracker;
    bool initialized = false;
};

namespace
{

void put(char** out, const std::string& text)
{
    if (!out)
        return;
    char* p = static_cast<char*>(std::malloc(text.size() + 1));
    if (p)
        std::memcpy(p, text.c_str(), text.size() + 1);
    *out = p;
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

// The invocation keeps pointers into the file system, so the two stay together.
struct CompileSetup
{
    llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> vfs;
    std::unique_ptr<clang::CompilerInvocation> invocation;
};

// Parses the command line like the driver does. The files go into a memory file system.
CompileSetup make_invocation(const char* const* args, size_t n_args, const char* const* paths,
    const char* const* texts, size_t n_files, std::string& diagnostics)
{
    CompileSetup s;
    auto memory = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
    for (size_t i = 0; i < n_files; ++i)
        memory->addFile(paths[i], 0, llvm::MemoryBuffer::getMemBufferCopy(texts[i], paths[i]));
    s.vfs = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(llvm::vfs::getRealFileSystem());
    s.vfs->pushOverlay(memory);

    std::vector<const char*> cargs(args, args + n_args);
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

// Runs "action" on the invocation. The messages go to "diagnostics" without carets.
bool run_action(CompileSetup& s, clang::FrontendAction& action, std::string& diagnostics)
{
    llvm::raw_string_ostream os(diagnostics);
    clang::DiagnosticOptions print_opts = s.invocation->getDiagnosticOpts();
    // ExecuteAction writes "N errors generated." to stderr when ShowCarets is on.
    s.invocation->getDiagnosticOpts().ShowCarets = false;
    clang::TextDiagnosticPrinter printer(os, print_opts);
    clang::CompilerInstance ci(std::shared_ptr<clang::CompilerInvocation>(std::move(s.invocation)));
    ci.setVirtualFileSystem(s.vfs);
    ci.createDiagnostics(&printer, false);
    const bool ok = ci.ExecuteAction(action);
    os.flush();
    return ok && !ci.getDiagnostics().hasErrorOccurred();
}

// RuntimeDyld links the COFF code of the JIT. It gives each REL32 reference to an external symbol
// a jump stub, also a reference to data. The data reference then gets the stub address, not the
// address of the data. A dllimport declaration reads the address from a pointer cell
// ("__imp_" + name). RuntimeDyld makes the cell and writes the address of "name" into it. So the
// host symbols keep their names without the prefix. A function keeps its jump stub.
void import_coff_data(llvm::Module& m)
{
    if (!m.getTargetTriple().isOSBinFormatCOFF())
        return;
    std::vector<const llvm::GlobalVariable*> thread_vars;
    for (llvm::GlobalVariable& g : m.globals())
    {
        if (!g.isDeclaration() || g.getDLLStorageClass() != llvm::GlobalValue::DefaultStorageClass)
            continue;
        if (g.getName().starts_with("__imp_") || g.getName().starts_with("llvm."))
            continue;
        if (g.isThreadLocal())
        {
            thread_vars.push_back(&g);
            continue;
        }
        // A dllimport symbol must not be dso_local. Clang makes each COFF declaration dso_local.
        g.setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
        g.setDSOLocal(false);
    }
    // The JIT uses emulated TLS. The backend replaces a thread variable X with its control record
    // "__emutls_v.X" and keeps a declaration of the record that is already in the module. A
    // dllimport declaration here makes the record also go through the pointer cell.
    llvm::LLVMContext& c = m.getContext();
    llvm::Type* word = m.getDataLayout().getIntPtrType(c);
    llvm::Type* ptr = llvm::PointerType::getUnqual(c);
    llvm::StructType* control = llvm::StructType::get(c, {word, word, ptr, ptr});
    for (const llvm::GlobalVariable* g : thread_vars)
    {
        const std::string name = ("__emutls_v." + g->getName()).str();
        if (m.getNamedGlobal(name))
            continue;
        auto* record = new llvm::GlobalVariable(
            m, control, false, llvm::GlobalValue::ExternalLinkage, nullptr, name);
        record->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
    }
}

} // namespace

extern "C"
{

void hot_jit_string_free(char* text)
{
    std::free(text);
}

int hot_jit_compile(const char* const* args, size_t n_args, const char* const* paths, const char* const* texts,
    size_t n_files, hot_jit_ir** out_ir, char** out_diagnostics)
{
    init_llvm_once();
    std::string diagnostics;
    CompileSetup s = make_invocation(args, n_args, paths, texts, n_files, diagnostics);
    int ok = 0;
    if (!s.invocation)
        diagnostics += "cannot build the compile command\n";
    else
    {
        clang::EmitLLVMOnlyAction action;
        if (run_action(s, action, diagnostics))
        {
            std::unique_ptr<llvm::Module> module = action.takeModule();
            std::unique_ptr<llvm::LLVMContext> context(action.takeLLVMContext());
            if (!module)
                diagnostics += "the compile made no module\n";
            else
            {
                auto* ir = new hot_jit_ir{llvm::orc::ThreadSafeContext(std::move(context)), std::move(module)};
                if (out_ir)
                    *out_ir = ir;
                else
                    delete ir;
                ok = 1;
            }
        }
    }
    put(out_diagnostics, diagnostics);
    return ok;
}

int hot_jit_make_pch(const char* const* args, size_t n_args, const char* out_pch, char** out_diagnostics)
{
    init_llvm_once();
    std::string diagnostics;
    CompileSetup s = make_invocation(args, n_args, nullptr, nullptr, 0, diagnostics);
    int ok = 0;
    if (!s.invocation)
        diagnostics += "cannot build the compile command\n";
    else
    {
        s.invocation->getFrontendOpts().OutputFile = out_pch;
        clang::GeneratePCHAction action;
        ok = run_action(s, action, diagnostics) ? 1 : 0;
    }
    put(out_diagnostics, diagnostics);
    return ok;
}

hot_jit_ir* hot_jit_ir_clone(const hot_jit_ir* ir)
{
    return new hot_jit_ir{ir->context, llvm::CloneModule(*ir->module)};
}

void hot_jit_ir_free(hot_jit_ir* ir)
{
    delete ir;
}

char* hot_jit_ir_link_text(const hot_jit_ir* ir)
{
    std::unique_ptr<llvm::Module> copy = llvm::CloneModule(*ir->module);
    import_coff_data(*copy);
    std::string text;
    llvm::raw_string_ostream os(text);
    copy->print(os, nullptr);
    os.flush();
    char* out = nullptr;
    put(&out, text);
    return out;
}

hot_jit_engine* hot_jit_engine_create(
    int debugger_support, int* out_debugger_on, char** out_error, char** out_debugger_error)
{
    init_llvm_once();
    if (out_debugger_on)
        *out_debugger_on = 0;
    auto jit = llvm::orc::LLJITBuilder().create();
    if (!jit)
    {
        put(out_error, "cannot make the JIT: " + error_text(jit.takeError()) + "\n");
        return nullptr;
    }
    auto engine = std::make_unique<hot_jit_engine>();
    engine->jit = std::move(*jit);
    // The debugger sees the JIT code: the plugin registers each linked object through the
    // GDB JIT interface (__jit_debug_descriptor). GDB and LLDB read it. The object has the
    // line table of "-gline-tables-only", so the debugger shows the .hot files and lines.
    if (debugger_support)
    {
        if (llvm::Error e = llvm::orc::enableDebuggerSupport(*engine->jit))
            put(out_debugger_error, error_text(std::move(e)));
        else if (out_debugger_on)
            *out_debugger_on = 1;
    }
    // The executable links with -rdynamic: the engine, the runtime and libstdc++ resolve here.
    // On Windows the search reads the exports of each loaded module, for example the host and
    // the DLL C runtime. The static runtime of this DLL has no exports.
    auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        engine->jit->getDataLayout().getGlobalPrefix());
    if (!gen)
    {
        put(out_error, "cannot search the process symbols: " + error_text(gen.takeError()) + "\n");
        return nullptr;
    }
    engine->jit->getMainJITDylib().addGenerator(std::move(*gen));
    return engine.release();
}

void hot_jit_engine_free(hot_jit_engine* engine)
{
    delete engine;
}

hot_jit_dylib* hot_jit_add(hot_jit_engine* engine, const char* id, hot_jit_ir* ir, const char* const* abs_names,
    const uint64_t* abs_values, size_t n_abs, char** out_error)
{
    std::unique_ptr<hot_jit_ir> owned(ir);
    auto holder = std::make_unique<hot_jit_dylib>();
    holder->engine = engine;
    llvm::orc::LLJIT& jit = *engine->jit;
    auto dylib = jit.createJITDylib(id);
    if (!dylib)
    {
        put(out_error, "cannot make a JITDylib: " + error_text(dylib.takeError()) + "\n");
        return nullptr;
    }
    holder->dylib = &*dylib;
    if (n_abs > 0)
    {
        llvm::orc::SymbolMap symbols;
        for (size_t i = 0; i < n_abs; ++i)
            symbols[jit.mangleAndIntern(abs_names[i])] =
                llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr(abs_values[i]),
                    llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Absolute);
        if (llvm::Error e = holder->dylib->define(llvm::orc::absoluteSymbols(std::move(symbols))))
        {
            put(out_error, "cannot define the absolute symbols: " + error_text(std::move(e)) + "\n");
            hot_jit_remove(holder.release());
            return nullptr;
        }
    }
    import_coff_data(*owned->module);
    holder->tracker = holder->dylib->createResourceTracker();
    if (llvm::Error e = jit.addIRModule(
            holder->tracker, llvm::orc::ThreadSafeModule(std::move(owned->module), owned->context)))
    {
        put(out_error, "cannot add the module: " + error_text(std::move(e)) + "\n");
        hot_jit_remove(holder.release());
        return nullptr;
    }
    // Static initializers run here: for example the dynamic initializer of the class record.
    if (llvm::Error e = jit.initialize(*holder->dylib))
    {
        put(out_error, "cannot run the static initializers: " + error_text(std::move(e)) + "\n");
        hot_jit_remove(holder.release());
        return nullptr;
    }
    holder->initialized = true;
    return holder.release();
}

void* hot_jit_lookup(hot_jit_dylib* dylib, const char* name, char** out_error)
{
    auto addr = dylib->engine->jit->lookup(*dylib->dylib, name);
    if (!addr)
    {
        put(out_error, error_text(addr.takeError()));
        return nullptr;
    }
    return addr->toPtr<void*>();
}

void hot_jit_remove(hot_jit_dylib* dylib)
{
    if (!dylib)
        return;
    llvm::orc::LLJIT& jit = *dylib->engine->jit;
    if (dylib->initialized)
        llvm::consumeError(jit.deinitialize(*dylib->dylib));
    if (dylib->tracker)
        llvm::consumeError(dylib->tracker->remove());
    if (dylib->dylib)
        llvm::consumeError(jit.getExecutionSession().removeJITDylib(*dylib->dylib));
    delete dylib;
}

} // extern "C"
