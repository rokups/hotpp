// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The program that the tests of run_process start. It has no dependency, so it runs on all
// platforms. The first argument is the mode:
//   exit <code>     exits with the code.
//   args <a>...     prints each argument on its own line between "[" and "]", then a line on stderr.
//   both            prints "out-line" on stdout and "err-line" on stderr.
//   big <bytes>     prints the number of bytes on stdout and on stderr, in turns.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
    #include <fcntl.h>
    #include <io.h>
#endif

int main(int argc, char** argv)
{
#ifdef _WIN32
    // Binary mode: the C runtime must not change "\n" to "\r\n".
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    const char* mode = argc > 1 ? argv[1] : "";
    if (std::strcmp(mode, "exit") == 0 && argc > 2)
        return std::atoi(argv[2]);
    if (std::strcmp(mode, "args") == 0)
    {
        for (int i = 2; i < argc; ++i)
            std::fprintf(stdout, "[%s]\n", argv[i]);
        std::fprintf(stderr, "args=%d\n", argc - 2);
        return 0;
    }
    if (std::strcmp(mode, "both") == 0)
    {
        std::fputs("out-line\n", stdout);
        std::fflush(stdout);
        std::fputs("err-line\n", stderr);
        std::fflush(stderr);
        return 0;
    }
    if (std::strcmp(mode, "big") == 0 && argc > 2)
    {
        const long total = std::atol(argv[2]);
        char line[64];
        std::memset(line, 'x', sizeof line - 1);
        line[sizeof line - 2] = '\n';
        line[sizeof line - 1] = '\0';
        for (long done = 0; done < total; done += static_cast<long>(sizeof line - 1))
            std::fputs(line, (done / 4096) % 2 == 0 ? stdout : stderr);
        return 0;
    }
    std::fputs("unknown mode\n", stderr);
    return 64;
}
