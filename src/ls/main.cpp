// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// hot-ls: the Hot++ language server. It speaks LSP over stdin and stdout (see server.h).
#include "server.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
    #include <windows.h>
    #include <shellapi.h>
#endif

int main(int argc, char** argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
#ifdef _WIN32
    // On Windows argv uses the ANSI code page. The server needs UTF-8 paths.
    int wargc = 0;
    if (LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc))
    {
        args.clear();
        for (int i = 1; i < wargc; ++i)
        {
            const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string a(n > 0 ? n - 1 : 0, '\0');
            if (n > 1)
                WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, a.data(), n, nullptr, nullptr);
            args.push_back(std::move(a));
        }
        LocalFree(wargv);
    }
#endif
    for (const std::string& a : args)
        if (a == "--help" || a == "-h")
        {
            std::fputs(hot::ls::usage_text(), stdout);
            return 0;
        }
    hot::ls::ServerOptions options;
    std::string error;
    if (!hot::ls::parse_arguments(args, options, error))
    {
        std::fprintf(stderr, "hot-ls: error: %s\n%s", error.c_str(), hot::ls::usage_text());
        return 2;
    }
    return hot::ls::run_server(0, 1, options);
}
