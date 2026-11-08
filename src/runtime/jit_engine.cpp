// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The JIT engine (jit_engine.h). This file uses only LLVM, Clang and the standard library. It
// must not use the hot runtime: on MSVC this code has another C runtime than the runtime.

#include "jit_engine.h"

#include "jit_coff_check.h"
#include "jit_coff_memory.h"
#include "jit_host_allocators.h"
#include "jit_search_order.h"

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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// The JIT links COFF code with RuntimeDyld. On Windows x64 the engine registers the unwind data
// of the code (.pdata) with the system, so an exception can pass a JIT frame.
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    #define HOT_JIT_COFF_UNWIND 1
    #include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>

    #include <psapi.h>
#elif defined(_MSC_VER)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>

    #include <psapi.h>
#endif

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

#ifdef HOT_JIT_COFF_UNWIND

using hot::jit::CoffMemoryManager;
using hot::jit::jit_image_base;

// The unwind table calls of the system for CoffMemoryManager.
bool add_function_table(void* table, uint32_t count, uint64_t base, void*)
{
    return RtlAddFunctionTable(static_cast<PRUNTIME_FUNCTION>(table), count, static_cast<DWORD64>(base)) != 0;
}

void delete_function_table(void* table, void*)
{
    RtlDeleteFunctionTable(static_cast<PRUNTIME_FUNCTION>(table));
}

constexpr hot::jit::UnwindTableHooks system_unwind_tables{add_function_table, delete_function_table, nullptr};

    #ifdef _MSC_VER
// The throw of the MSVC C++ ABI. The throw record (ThrowInfo) has offsets from the image base
// of its module. The C runtime gets the base with RtlPcToFileHeader, which knows no JIT memory.
// This function gives the base of the JIT object. The parameters are those of vcruntime (x64).
void __stdcall jit_cxx_throw_exception(void* object, void* throw_info)
{
    constexpr DWORD cxx_exception_code = 0xE06D7363;
    constexpr ULONG_PTR magic = 0x19930520;
    constexpr ULONG_PTR pure_magic = 0x01994000;
    constexpr unsigned pure_attribute = 0x8;
    uintptr_t base = jit_image_base(throw_info);
    if (!base && throw_info)
    {
        void* image = nullptr;
        base = reinterpret_cast<uintptr_t>(RtlPcToFileHeader(throw_info, &image));
    }
    const unsigned attributes = throw_info ? *static_cast<const unsigned*>(throw_info) : 0;
    const ULONG_PTR parameters[4] = {(attributes & pure_attribute) ? pure_magic : magic,
        reinterpret_cast<ULONG_PTR>(object), reinterpret_cast<ULONG_PTR>(throw_info), base};
    RaiseException(cxx_exception_code, EXCEPTION_NONCONTINUABLE, 4, parameters);
}
    #endif

#endif // HOT_JIT_COFF_UNWIND

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

// On the MSVC target clang guards each access to a thread variable with dynamic
// initialization (also an extern "__declspec(thread)" variable): it reads the TLS
// variable "__tls_guard" and calls "__dyn_tls_on_demand_init". Both belong to the native TLS
// directory of the host image, and the CRT of the host runs only the TLS initializers of
// that image. The JIT code uses emulated TLS and has no TLS directory. So the guard is a
// symbol that the JIT cannot resolve, and the init call cannot run JIT initializers. The
// JIT compile turns the guards off (-fno-ms-tls-guards), the same as before MSVC 2019 16.5.
void no_ms_tls_guards(clang::CompilerInvocation& ci)
{
    const llvm::Triple triple(ci.getTargetOpts().Triple);
    if (triple.isWindowsMSVCEnvironment())
        ci.getCodeGenOpts().TlsGuards = false;
}

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
    if (s.invocation)
        no_ms_tls_guards(*s.invocation);
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

#ifdef _MSC_VER
// The search of the process symbols for JIT code on Windows. The process search of LLVM takes
// the first loaded module with the name. That can be another C runtime: msvcrt.dll ("fflush") or
// msvcp_win.dll ("std::cout"). Their state is not the state of the host: a FILE of msvcrt.dll has
// another layout than a FILE of ucrtbase.dll. This generator searches the C runtime DLLs of the
// host first. Then it searches the other loaded modules in the load order, without another C
// runtime DLL (symbol_search_order). It gives no name of the allocator family: only the absolute
// symbols of the host (hot_jit_add) give these names.
class HostProcessGenerator : public llvm::orc::DefinitionGenerator
{
public:
    HostProcessGenerator(std::vector<HMODULE> host, char global_prefix)
        : host_(std::move(host))
        , prefix_(global_prefix)
    {
    }

    llvm::Error tryToGenerate(llvm::orc::LookupState&, llvm::orc::LookupKind, llvm::orc::JITDylib& jd,
        llvm::orc::JITDylibLookupFlags, const llvm::orc::SymbolLookupSet& symbols) override
    {
        const std::vector<HMODULE> order = search_order();
        llvm::orc::SymbolMap found;
        for (const auto& item : symbols)
        {
            llvm::StringRef name = *item.first;
            if (prefix_ != 0)
            {
                if (name.empty() || name.front() != prefix_)
                    continue;
                name = name.drop_front();
            }
            if (name.empty() || hot::jit::is_host_allocator_name(name.data(), name.size()))
                continue;
            const std::string text = name.str();
            HMODULE from = nullptr;
            for (HMODULE module : order)
                if (FARPROC address = GetProcAddress(module, text.c_str()))
                {
                    found[item.first] = llvm::orc::ExecutorSymbolDef(
                        llvm::orc::ExecutorAddr(reinterpret_cast<uint64_t>(address)), llvm::JITSymbolFlags::Exported);
                    from = module;
                    break;
                }
            if (trace_)
                trace(text, from);
        }
        if (found.empty())
            return llvm::Error::success();
        return jd.define(llvm::orc::absoluteSymbols(std::move(found)));
    }

private:
    // HOT_JIT_TRACE_SYMBOLS=1 writes each name and the module that gives it to stderr.
    static bool trace_on()
    {
        const char* v = std::getenv("HOT_JIT_TRACE_SYMBOLS");
        return v && *v && *v != '0';
    }

    static void trace(const std::string& name, HMODULE module)
    {
        char path[MAX_PATH] = "(not found)";
        if (module)
            GetModuleFileNameA(module, path, MAX_PATH);
        std::fprintf(stderr, "[jit-sym] %s -> %s\n", name.c_str(), path);
        std::fflush(stderr);
    }

    // The loaded modules now: a module can load after the start of the JIT.
    std::vector<HMODULE> search_order() const
    {
        std::vector<HMODULE> modules(256);
        DWORD needed = 0;
        for (;;)
        {
            const DWORD bytes = static_cast<DWORD>(modules.size() * sizeof(HMODULE));
            if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(), bytes, &needed))
            {
                // Without the list of the process, only the host runtime DLLs, in the same rank order.
                modules = host_;
                needed = static_cast<DWORD>(modules.size() * sizeof(HMODULE));
                break;
            }
            if (needed <= bytes)
                break;
            modules.resize(needed / sizeof(HMODULE) + 16);
        }
        modules.resize(needed / sizeof(HMODULE));
        std::vector<std::string> paths;
        for (HMODULE module : modules)
        {
            wchar_t wide[MAX_PATH] = {};
            const DWORD n = GetModuleFileNameW(module, wide, MAX_PATH);
            std::string path(n, '?');
            for (DWORD k = 0; k < n; ++k)
                if (wide[k] < 0x80)
                    path[k] = static_cast<char>(wide[k]);
            paths.push_back(std::move(path));
        }
        std::vector<size_t> host;
        for (HMODULE h : host_)
        {
            const auto it = std::find(modules.begin(), modules.end(), h);
            if (it != modules.end())
                host.push_back(static_cast<size_t>(it - modules.begin()));
        }
        std::vector<HMODULE> out;
        for (size_t i : hot::jit::symbol_search_order(paths, host))
            out.push_back(modules[i]);
        return out;
    }

    std::vector<HMODULE> host_;
    char prefix_;
    bool trace_ = trace_on();
};
#endif

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

char* hot_jit_coff_unwind_check(const hot_jit_ir* ir)
{
    init_llvm_once();
    std::unique_ptr<llvm::Module> copy = llvm::CloneModule(*ir->module);
    char* out = nullptr;
    put(&out, hot::jit::check_coff_unwind(*copy));
    return out;
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

hot_jit_engine* hot_jit_engine_create(int debugger_support, const void* const* host_runtime, size_t n_host_runtime,
    int* out_debugger_on, char** out_error, char** out_debugger_error)
{
    init_llvm_once();
    if (out_debugger_on)
        *out_debugger_on = 0;
    llvm::orc::LLJITBuilder builder;
#ifdef HOT_JIT_COFF_UNWIND
    // The same layer as the LLJIT default for COFF, with the memory manager of the unwind data.
    builder.setObjectLinkingLayerCreator(
        [](llvm::orc::ExecutionSession& es,
            llvm::jitlink::JITLinkMemoryManager&) -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>>
        {
            auto layer = std::make_unique<llvm::orc::RTDyldObjectLinkingLayer>(
                es, [](const llvm::MemoryBuffer&) { return std::make_unique<CoffMemoryManager>(system_unwind_tables); });
            layer->setOverrideObjectFlagsWithResponsibilityFlags(true);
            layer->setAutoClaimResponsibilityForObjectSymbols(true);
            return std::unique_ptr<llvm::orc::ObjectLayer>(std::move(layer));
        });
#endif
#ifdef _MSC_VER
    // The modules of the host addresses: the C runtime DLLs of the host, in the order of the list.
    std::vector<HMODULE> host_modules;
    for (size_t i = 0; i < n_host_runtime; ++i)
    {
        HMODULE module = nullptr;
        if (host_runtime[i]
            && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(host_runtime[i]), &module))
            host_modules.push_back(module);
    }
    // The JITDylib of each module links to "<Platform>" and to "<Process Symbols>" (LLJIT), not to
    // the main JITDylib. So the process symbols JITDylib must have the search of the host: it has
    // only one generator (HostProcessGenerator).
    builder.setProcessSymbolsJITDylibSetup(
        [host_modules](llvm::orc::LLJIT& j) -> llvm::Expected<llvm::orc::JITDylibSP>
        {
            auto& jd = j.getExecutionSession().createBareJITDylib("<Process Symbols>");
            jd.addGenerator(std::make_unique<HostProcessGenerator>(host_modules, j.getDataLayout().getGlobalPrefix()));
            return &jd;
        });
#else
    (void)host_runtime;
    (void)n_host_runtime;
#endif
    auto jit = builder.create();
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
#ifndef _MSC_VER
    // The executable links with -rdynamic: the engine, the runtime and libstdc++ resolve here.
    auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        engine->jit->getDataLayout().getGlobalPrefix());
    if (!gen)
    {
        put(out_error, "cannot search the process symbols: " + error_text(gen.takeError()) + "\n");
        return nullptr;
    }
    engine->jit->getMainJITDylib().addGenerator(std::move(*gen));
#endif
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
    llvm::orc::SymbolMap symbols;
    for (size_t i = 0; i < n_abs; ++i)
        symbols[jit.mangleAndIntern(abs_names[i])] =
            llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr(abs_values[i]),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Absolute);
#if defined(HOT_JIT_COFF_UNWIND) && defined(_MSC_VER)
    // A throw in the JIT code gives the image base of the JIT object (jit_cxx_throw_exception).
    symbols.try_emplace(jit.mangleAndIntern("_CxxThrowException"),
        llvm::orc::ExecutorAddr(reinterpret_cast<uint64_t>(&jit_cxx_throw_exception)),
        llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Absolute);
#endif
    if (!symbols.empty())
    {
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
