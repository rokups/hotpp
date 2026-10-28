// spp-ls: the S++ language server. It speaks LSP over stdin and stdout (see server.h).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "server.h"

int main(int argc, char** argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    for (const std::string& a : args)
        if (a == "--help" || a == "-h") {
            std::fputs(spp::ls::usage_text(), stdout);
            return 0;
        }
    spp::ls::ServerOptions options;
    std::string error;
    if (!spp::ls::parse_arguments(args, options, error)) {
        std::fprintf(stderr, "spp-ls: error: %s\n%s", error.c_str(), spp::ls::usage_text());
        return 2;
    }
    return spp::ls::run_server(0, 1, options);
}
