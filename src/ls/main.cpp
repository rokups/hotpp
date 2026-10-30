// hot-ls: the S++ language server. It speaks LSP over stdin and stdout (see server.h).
#include "server.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
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
