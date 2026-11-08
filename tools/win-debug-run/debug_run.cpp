// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A small debugger for CI on Windows. It runs one program as a debuggee and prints:
//   - the debug output of the program (OutputDebugString and DbgPrint, so also the text of a
//     verifier stop of the page heap),
//   - each crash exception and each breakpoint after the first one, with the stack of the
//     thread: a StackWalk64 walk and a raw walk (the values on the stack that point into
//     executable memory, as in tests/support/seh_report.cpp).
// Without a debugger, a verifier stop ends the process with STATUS_VERIFIER_STOP (0xC0000421),
// and the handlers of the process do not run. Under this debugger the verifier breaks first.
//
// Usage: debug_run [--symbols <path>] <program> [arguments...]
// The exit code is the exit code of the program. Build: cl /nologo /EHsc /O1 debug_run.cpp dbghelp.lib

#include <windows.h>

#include <dbghelp.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace
{

constexpr DWORD kVerifierStop = 0xC0000421;   // STATUS_VERIFIER_STOP
constexpr DWORD kHeapCorruption = 0xC0000374; // STATUS_HEAP_CORRUPTION
constexpr DWORD kBadFunctionTable = 0xC00000FF; // STATUS_BAD_FUNCTION_TABLE
constexpr DWORD kPrintC = 0x40010006;           // DBG_PRINTEXCEPTION_C
constexpr DWORD kPrintW = 0x4001000A;           // DBG_PRINTEXCEPTION_WIDE_C
constexpr unsigned kMaxReports = 6;

HANDLE g_process = nullptr;
std::map<DWORD, HANDLE> g_threads;
bool g_symbols = false;

const char* code_text(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case kHeapCorruption: return "heap corruption";
    case kVerifierStop: return "verifier stop (page heap)";
    case kBadFunctionTable: return "bad function table";
    default: return "other";
    }
}

bool is_crash(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW || code == kHeapCorruption
        || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO
        || code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_IN_PAGE_ERROR || code == kVerifierStop
        || code == kBadFunctionTable;
}

// The debug text of the debuggee at "address": "length" characters, ANSI or UTF-16.
void print_debug_text(ULONG_PTR address, ULONG_PTR length, bool wide)
{
    if (length == 0 || length > 64 * 1024)
        return;
    std::string text;
    if (wide)
    {
        std::wstring w(static_cast<size_t>(length), L'\0');
        SIZE_T got = 0;
        if (!ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(address), &w[0], length * sizeof(wchar_t), &got))
            return;
        w.resize(got / sizeof(wchar_t));
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        text.resize(n > 0 ? static_cast<size_t>(n) : 0);
        if (n > 0)
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &text[0], n, nullptr, nullptr);
    }
    else
    {
        text.resize(static_cast<size_t>(length));
        SIZE_T got = 0;
        if (!ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(address), &text[0], length, &got))
            return;
        text.resize(got);
    }
    while (!text.empty() && (text.back() == '\0' || text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    std::printf("[dbg] output: %s\n", text.c_str());
}

std::string module_name(DWORD64 address)
{
    IMAGEHLP_MODULE64 m = {};
    m.SizeOfStruct = sizeof(m);
    if (g_symbols && SymGetModuleInfo64(g_process, address, &m))
        return m.ModuleName;
    return "(no module, JIT code?)";
}

// One line for an address of the debuggee: the module, the offset and the symbol.
void print_address(const char* label, DWORD64 address)
{
    const DWORD64 base = g_symbols ? SymGetModuleBase64(g_process, address) : 0;
    std::printf("[dbg] %s 0x%llx %s+0x%llx", label, static_cast<unsigned long long>(address), module_name(address).c_str(),
        static_cast<unsigned long long>(base ? address - base : 0));
    if (g_symbols && base)
    {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (SymFromAddr(g_process, address, &displacement, symbol))
            std::printf(" %s+0x%llx", symbol->Name, static_cast<unsigned long long>(displacement));
        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        DWORD line_displacement = 0;
        if (SymGetLineFromAddr64(g_process, address, &line_displacement, &line))
            std::printf(" (%s:%lu)", line.FileName, line.LineNumber);
    }
    std::printf("\n");
}

// True when "p" points into executable memory of the debuggee.
bool is_executable(ULONG_PTR p)
{
    MEMORY_BASIC_INFORMATION m = {};
    if (p < 0x10000 || !VirtualQueryEx(g_process, reinterpret_cast<LPCVOID>(p), &m, sizeof(m)) || m.State != MEM_COMMIT)
        return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (m.Protect & exec) != 0;
}

// The raw walk of seh_report.cpp, on the memory of the debuggee. It does not use the unwind
// data, so it also finds the frames of JIT code.
void print_raw_stack(const CONTEXT& context)
{
    ULONG_PTR stack[2048] = {};
    SIZE_T got = 0;
    // The read stops at the end of the stack: read less, page by page.
    SIZE_T want = sizeof(stack);
    while (want >= 4096 && !ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(context.Rsp), stack, want, &got))
        want /= 2;
    unsigned found = 0;
    for (SIZE_T i = 0; i < got / sizeof(ULONG_PTR) && found < 24; ++i)
    {
        if (!is_executable(stack[i]))
            continue;
        char label[32];
        std::snprintf(label, sizeof(label), "raw rsp+0x%llx", static_cast<unsigned long long>(i * 8));
        print_address(label, stack[i]);
        ++found;
    }
}

void print_stack(HANDLE thread)
{
    CONTEXT context = {};
    context.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &context))
    {
        std::printf("[dbg] GetThreadContext failed: %lu\n", GetLastError());
        return;
    }
    std::printf("[dbg] rip 0x%llx rsp 0x%llx\n", static_cast<unsigned long long>(context.Rip),
        static_cast<unsigned long long>(context.Rsp));
    print_raw_stack(context);
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    CONTEXT walk = context; // StackWalk64 changes the context
    for (unsigned i = 0; i < 64; ++i)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, g_process, thread, &frame, &walk, nullptr, SymFunctionTableAccess64,
                SymGetModuleBase64, nullptr)
            || frame.AddrPC.Offset == 0)
            break;
        char label[16];
        std::snprintf(label, sizeof(label), "#%u", i);
        print_address(label, frame.AddrPC.Offset);
    }
}

void load_module(HANDLE file, void* base)
{
    if (!g_symbols)
        return;
    char path[MAX_PATH * 2] = {};
    if (file && GetFinalPathNameByHandleA(file, path, sizeof(path), FILE_NAME_NORMALIZED) == 0)
        path[0] = '\0';
    const char* name = path[0] ? path : nullptr;
    if (name && std::strncmp(name, "\\\\?\\", 4) == 0)
        name += 4;
    SymLoadModuleEx(g_process, file, name, nullptr, reinterpret_cast<DWORD64>(base), 0, nullptr, 0);
}

std::string quote(const char* arg)
{
    std::string s = "\"";
    for (const char* p = arg; *p; ++p)
    {
        if (*p == '"')
            s += '\\';
        s += *p;
    }
    return s + "\"";
}

} // namespace

int main(int argc, char** argv)
{
    int first = 1;
    std::string symbols;
    if (argc > 2 && std::strcmp(argv[1], "--symbols") == 0)
    {
        symbols = argv[2];
        first = 3;
    }
    if (first >= argc)
    {
        std::fprintf(stderr, "usage: debug_run [--symbols <path>] <program> [arguments...]\n");
        return 2;
    }
    if (symbols.empty()) // the directory of the program: the PDB files are there
    {
        symbols = argv[first];
        const size_t slash = symbols.find_last_of("\\/");
        symbols = slash == std::string::npos ? "." : symbols.substr(0, slash);
    }
    std::string command;
    for (int i = first; i < argc; ++i)
        command += (i > first ? " " : "") + quote(argv[i]);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, &command[0], nullptr, nullptr, FALSE, DEBUG_ONLY_THIS_PROCESS, nullptr, nullptr, &si, &pi))
    {
        std::fprintf(stderr, "[dbg] CreateProcess failed: %lu: %s\n", GetLastError(), command.c_str());
        return 2;
    }
    std::printf("[dbg] run: %s\n", command.c_str());
    std::fflush(stdout);

    bool initial_break = false;
    unsigned reports = 0;
    DWORD exit_code = 1;
    // The first crash. A later fault (for example a stack overflow in a handler of the program) can
    // end the process, so the last line repeats the first crash.
    std::string first_crash;
    // The first-chance C++ exceptions (0xE06D7363) that get a line: the throw address and the
    // parameters (magic, object, throw info, image base).
    unsigned cxx_lines = 0;
    for (;;)
    {
        DEBUG_EVENT e = {};
        if (!WaitForDebugEvent(&e, INFINITE))
        {
            std::fprintf(stderr, "[dbg] WaitForDebugEvent failed: %lu\n", GetLastError());
            break;
        }
        DWORD status = DBG_CONTINUE;
        switch (e.dwDebugEventCode)
        {
        case CREATE_PROCESS_DEBUG_EVENT:
            g_process = e.u.CreateProcessInfo.hProcess;
            g_threads[e.dwThreadId] = e.u.CreateProcessInfo.hThread;
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
            g_symbols = SymInitialize(g_process, symbols.c_str(), FALSE) != FALSE;
            load_module(e.u.CreateProcessInfo.hFile, e.u.CreateProcessInfo.lpBaseOfImage);
            if (e.u.CreateProcessInfo.hFile)
                CloseHandle(e.u.CreateProcessInfo.hFile);
            break;
        case CREATE_THREAD_DEBUG_EVENT:
            g_threads[e.dwThreadId] = e.u.CreateThread.hThread;
            break;
        case EXIT_THREAD_DEBUG_EVENT:
            g_threads.erase(e.dwThreadId);
            break;
        case LOAD_DLL_DEBUG_EVENT:
            load_module(e.u.LoadDll.hFile, e.u.LoadDll.lpBaseOfDll);
            if (e.u.LoadDll.hFile)
                CloseHandle(e.u.LoadDll.hFile);
            break;
        case UNLOAD_DLL_DEBUG_EVENT:
            if (g_symbols)
                SymUnloadModule64(g_process, reinterpret_cast<DWORD64>(e.u.UnloadDll.lpBaseOfDll));
            break;
        case OUTPUT_DEBUG_STRING_EVENT:
        {
            const OUTPUT_DEBUG_STRING_INFO& s = e.u.DebugString;
            print_debug_text(reinterpret_cast<ULONG_PTR>(s.lpDebugStringData), s.nDebugStringLength, s.fUnicode != 0);
            break;
        }
        case EXCEPTION_DEBUG_EVENT:
        {
            const EXCEPTION_RECORD& r = e.u.Exception.ExceptionRecord;
            const DWORD code = r.ExceptionCode;
            const bool first_chance = e.u.Exception.dwFirstChance != 0;
            if (code == kPrintC || code == kPrintW)
            {
                if (r.NumberParameters >= 2)
                    print_debug_text(r.ExceptionInformation[1], r.ExceptionInformation[0], code == kPrintW);
                break; // DBG_CONTINUE: the text is printed
            }
            if (code == EXCEPTION_BREAKPOINT && !initial_break)
            {
                initial_break = true; // the loader breaks once at the start
                break;
            }
            if (code == 0xE06D7363 && first_chance && cxx_lines < 40)
            {
                ++cxx_lines;
                std::printf("[dbg] C++ exception in thread %lu, %lu parameters:", e.dwThreadId,
                    static_cast<unsigned long>(r.NumberParameters));
                for (DWORD i = 0; i < r.NumberParameters && i < 4; ++i)
                    std::printf(" 0x%llx", static_cast<unsigned long long>(r.ExceptionInformation[i]));
                std::printf("\n");
                print_address("C++ exception raised at", reinterpret_cast<DWORD64>(r.ExceptionAddress));
                if (r.NumberParameters >= 3)
                    std::printf("[dbg] C++ throw info in %s\n", module_name(r.ExceptionInformation[2]).c_str());
                std::fflush(stdout);
            }
            if (first_crash.empty() && is_crash(code))
            {
                char text[256];
                std::snprintf(text, sizeof(text), "0x%08lx (%s) at %p in thread %lu", static_cast<unsigned long>(code),
                    code_text(code), r.ExceptionAddress, e.dwThreadId);
                first_crash = text;
                if (code == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 2)
                {
                    const ULONG_PTR kind = r.ExceptionInformation[0];
                    std::snprintf(text, sizeof(text), ", %s of address 0x%llx",
                        kind == 0 ? "read" : kind == 1 ? "write" : "execute",
                        static_cast<unsigned long long>(r.ExceptionInformation[1]));
                    first_crash += text;
                }
            }
            const bool report = code == EXCEPTION_BREAKPOINT || is_crash(code) || !first_chance;
            if (report && reports < kMaxReports)
            {
                ++reports;
                std::printf("[dbg] %s exception 0x%08lx (%s) in thread %lu\n", first_chance ? "first-chance" : "second-chance",
                    static_cast<unsigned long>(code), code_text(code), e.dwThreadId);
                print_address("at", reinterpret_cast<DWORD64>(r.ExceptionAddress));
                if (code == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 2)
                {
                    const ULONG_PTR kind = r.ExceptionInformation[0];
                    std::printf("[dbg] %s of address 0x%llx\n", kind == 0 ? "read" : kind == 1 ? "write" : "execute",
                        static_cast<unsigned long long>(r.ExceptionInformation[1]));
                }
                auto t = g_threads.find(e.dwThreadId);
                if (t != g_threads.end())
                    print_stack(t->second);
                std::fflush(stdout);
            }
            // A breakpoint (a verifier stop breaks too) goes on. Each other exception goes to the
            // handlers of the program, as without a debugger.
            status = code == EXCEPTION_BREAKPOINT ? DBG_CONTINUE : DBG_EXCEPTION_NOT_HANDLED;
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            exit_code = e.u.ExitProcess.dwExitCode;
            std::printf("[dbg] exit code 0x%08lx (%s)\n", static_cast<unsigned long>(exit_code), code_text(exit_code));
            if (!first_crash.empty())
                std::printf("[dbg] first crash: %s\n", first_crash.c_str());
            std::fflush(stdout);
            ContinueDebugEvent(e.dwProcessId, e.dwThreadId, DBG_CONTINUE);
            if (g_symbols)
                SymCleanup(g_process);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return static_cast<int>(exit_code);
        default:
            break;
        }
        ContinueDebugEvent(e.dwProcessId, e.dwThreadId, status);
    }
    return static_cast<int>(exit_code);
}
