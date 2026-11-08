// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A crash report for the MSVC test executables (hot_gtest adds this file). GoogleTest
// catches an SEH exception, but it writes only the code. This vectored handler writes the code,
// the address, the module and offset of the address, and the stack, to stderr. Then the normal
// handling continues. The handler sees first-chance exceptions, so a report does not always
// mean a failure. A test that has no SEH exception writes nothing.
#if defined(_WIN32) && defined(_MSC_VER)

// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

#include <cstdio>

#pragma comment(lib, "dbghelp.lib")

namespace
{

volatile LONG g_reports = 0;
bool g_symbols = false;

bool is_crash(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW || code == 0xC0000374
        || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO
        || code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_IN_PAGE_ERROR;
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
    std::fprintf(stderr, "[seh] %s %p %s+0x%llx", label, address, module ? base : "(no module, JIT code?)", offset);
    if (g_symbols && module)
    {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(address), &displacement, symbol))
            std::fprintf(stderr, " %s+0x%llx", symbol->Name, static_cast<unsigned long long>(displacement));
        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        DWORD line_displacement = 0;
        if (SymGetLineFromAddr64(GetCurrentProcess(), reinterpret_cast<DWORD64>(address), &line_displacement, &line))
            std::fprintf(stderr, " (%s:%lu)", line.FileName, line.LineNumber);
    }
    std::fprintf(stderr, "\n");
}

LONG CALLBACK report(EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    if (!is_crash(record->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;
    // Some reports per process only: a crash can repeat in a loop.
    if (InterlockedIncrement(&g_reports) > 3)
        return EXCEPTION_CONTINUE_SEARCH;
    std::fprintf(stderr, "[seh] first-chance exception 0x%08lx in thread %lu\n",
        static_cast<unsigned long>(record->ExceptionCode), GetCurrentThreadId());
    // A fixture DLL can load after SymInitialize.
    if (g_symbols)
        SymRefreshModuleList(GetCurrentProcess());
    print_address("at", record->ExceptionAddress);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
    {
        const ULONG_PTR kind = record->ExceptionInformation[0];
        std::fprintf(stderr, "[seh] %s of address 0x%llx\n", kind == 0 ? "read" : kind == 1 ? "write" : "execute",
            static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }
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
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

struct Install
{
    Install()
    {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        g_symbols = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
        AddVectoredExceptionHandler(1, report);
    }
};

const Install g_install;

} // namespace

#endif
