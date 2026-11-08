// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A crash report for the MSVC test executables (hot_gtest adds this file). GoogleTest
// catches an SEH exception, but it writes only the code. This vectored handler writes the code,
// the address, the module and offset of the address, and the stack, to stderr. Then the normal
// handling continues. The handler sees first-chance exceptions, so a report does not always
// mean a failure. A test that has no SEH exception writes nothing. An unhandled exception filter
// writes the same report for the exception that ends the process.
#if defined(_WIN32) && defined(_MSC_VER)

// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

#include <cstdarg>
#include <cstdio>

#pragma comment(lib, "dbghelp.lib")

namespace
{

volatile LONG g_reports = 0;
// The thread that writes a report now, or 0. A fault in the report (for example in a broken C
// runtime) must not start a second report: that recursion ends in a stack overflow and hides the
// first fault.
volatile LONG g_writer = 0;

// Writes one formatted text to the standard error handle. It uses WriteFile and no FILE stream: the
// fault can be in the stream code of the C runtime, and a stream can be broken or locked.
void say(const char* format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (n <= 0)
        return;
    const DWORD size = static_cast<DWORD>(n < static_cast<int>(sizeof(text)) ? n : sizeof(text) - 1);
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, size, &written, nullptr);
}
bool g_symbols = false;

bool is_crash(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW || code == 0xC0000374
        || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO
        || code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_IN_PAGE_ERROR
        || code == 0xC0000421  // STATUS_VERIFIER_STOP: verifier stop (page heap)
        || code == 0xC00000FF; // STATUS_BAD_FUNCTION_TABLE: the unwind found a bad function table
}

// The text of an exception code, for the report line.
const char* code_text(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case 0xC0000374: return "heap corruption";
    // The page heap verifier ends the process with this code. It does that without an exception,
    // so this handler does not run for it: run the test under tools/win-debug-run for the text.
    case 0xC0000421: return "verifier stop (page heap)";
    case 0xC00000FF: return "bad function table";
    default: return "other";
    }
}

void print_address(const char* label, void* address);

// The registered function table entry that covers "address" (also an entry of JIT code).
void print_function_entry(DWORD64 address)
{
    DWORD64 base = 0;
    const PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(address, &base, nullptr);
    if (!f)
    {
        say("[seh] no function table entry covers %p\n", reinterpret_cast<void*>(address));
        return;
    }
    say("[seh] function entry: base 0x%llx begin +0x%lx end +0x%lx unwind +0x%lx\n",
        static_cast<unsigned long long>(base), static_cast<unsigned long>(f->BeginAddress),
        static_cast<unsigned long>(f->EndAddress), static_cast<unsigned long>(f->UnwindData));
}

// True when "p" points into executable memory.
bool is_executable(ULONG_PTR p)
{
    MEMORY_BASIC_INFORMATION m = {};
    if (p < 0x10000 || !VirtualQuery(reinterpret_cast<LPCVOID>(p), &m, sizeof(m)) || m.State != MEM_COMMIT)
        return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (m.Protect & exec) != 0;
}

// A raw stack walk: the values on the stack that point into executable memory. It does not use
// the unwind data, so it also works when a function table is bad.
void print_raw_stack(const CONTEXT* context)
{
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    auto sp = static_cast<ULONG_PTR>(context->Rsp);
    const auto top = reinterpret_cast<ULONG_PTR>(tib->StackBase);
    unsigned found = 0;
    for (unsigned i = 0; i < 2048 && sp + sizeof(ULONG_PTR) <= top && found < 16; ++i, sp += sizeof(ULONG_PTR))
    {
        const ULONG_PTR value = *reinterpret_cast<const ULONG_PTR*>(sp);
        if (!is_executable(value))
            continue;
        char label[32];
        std::snprintf(label, sizeof(label), "raw rsp+0x%x", i * 8);
        print_address(label, reinterpret_cast<void*>(value));
        ++found;
    }
}

// One line for an address: the module, the offset and, with symbols, the function and line.
void print_address(const char* label, void* address)
{
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (GetModuleHandleExA(flags, static_cast<LPCSTR>(address), &module) && module)
        GetModuleFileNameA(module, name, MAX_PATH);
    const char* base = name;
    for (const char* p = name; *p; ++p)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    const unsigned long long offset =
        module ? static_cast<unsigned long long>(reinterpret_cast<char*>(address) - reinterpret_cast<char*>(module))
               : 0ull;
    say("[seh] %s %p %s+0x%llx", label, address, module ? base : "(no module, JIT code?)", offset);
    if (g_symbols && module)
    {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(address), &displacement, symbol))
            say(" %s+0x%llx", symbol->Name, static_cast<unsigned long long>(displacement));
        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        DWORD line_displacement = 0;
        if (SymGetLineFromAddr64(GetCurrentProcess(), reinterpret_cast<DWORD64>(address), &line_displacement, &line))
            say(" (%s:%lu)", line.FileName, line.LineNumber);
    }
    say("\n");
}

void write_report(EXCEPTION_POINTERS* info, const char* kind)
{
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    say("[seh] %s exception 0x%08lx (%s) in thread %lu\n", kind,
        static_cast<unsigned long>(record->ExceptionCode), code_text(record->ExceptionCode), GetCurrentThreadId());
    // A fixture DLL can load after SymInitialize.
    if (g_symbols)
        SymRefreshModuleList(GetCurrentProcess());
    print_address("at", record->ExceptionAddress);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
    {
        const ULONG_PTR kind = record->ExceptionInformation[0];
        say("[seh] %s of address 0x%llx\n", kind == 0 ? "read" : kind == 1 ? "write" : "execute",
            static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }
    say("[seh] rip %p rsp %p\n", reinterpret_cast<void*>(info->ContextRecord->Rip),
        reinterpret_cast<void*>(info->ContextRecord->Rsp));
    print_function_entry(reinterpret_cast<DWORD64>(record->ExceptionAddress));
    if (record->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
        print_raw_stack(info->ContextRecord);
    // The handler runs on the stack of the fault, so the stack has the frames of the fault. The
    // stack overflow has no room for a stack walk.
    if (record->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
    {
        void* frames[48] = {};
        const USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        for (USHORT i = 0; i < n; ++i)
        {
            char label[16];
            std::snprintf(label, sizeof(label), "#%u", static_cast<unsigned>(i));
            print_address(label, frames[i]);
        }
    }
}

// Writes one report, when no report of this thread is in progress. Another thread waits for no
// report: it writes its report at the same time.
void guarded_report(EXCEPTION_POINTERS* info, const char* kind)
{
    const LONG self = static_cast<LONG>(GetCurrentThreadId());
    if (InterlockedCompareExchange(&g_writer, self, 0) == self)
        return; // a fault in the report of this thread
    write_report(info, kind);
    InterlockedCompareExchange(&g_writer, 0, self);
}

LONG CALLBACK report(EXCEPTION_POINTERS* info)
{
    if (!is_crash(info->ExceptionRecord->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;
    // Some reports per process only: a crash can repeat in a loop.
    if (InterlockedIncrement(&g_reports) > 3)
        return EXCEPTION_CONTINUE_SEARCH;
    guarded_report(info, "first-chance");
    return EXCEPTION_CONTINUE_SEARCH;
}

// The exception that nobody handles: each code, because the process ends now. Only the first
// call writes: a fault in the report also comes here.
LONG WINAPI report_unhandled(EXCEPTION_POINTERS* info)
{
    static volatile LONG calls = 0;
    if (InterlockedIncrement(&calls) == 1)
        guarded_report(info, "unhandled");
    return EXCEPTION_CONTINUE_SEARCH;
}

struct Install
{
    Install()
    {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        g_symbols = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
        AddVectoredExceptionHandler(1, report);
        SetUnhandledExceptionFilter(report_unhandled);
    }
};

const Install g_install;

} // namespace

#endif
