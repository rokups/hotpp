// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "hot/command_line.h"

#include <cctype>
#include <filesystem>
#include <system_error>

namespace hot
{

namespace fs = std::filesystem;

std::string windows_quote_arg(const std::string& arg)
{
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos)
        return arg;
    std::string r = "\"";
    for (size_t i = 0;; ++i)
    {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == '\\')
        {
            ++backslashes;
            ++i;
        }
        if (i == arg.size())
        {
            // The closing quote follows: each backslash becomes two.
            r.append(backslashes * 2, '\\');
            break;
        }
        if (arg[i] == '"')
        {
            // Each backslash becomes two, and one more backslash escapes the quote.
            r.append(backslashes * 2 + 1, '\\');
            r += '"';
        }
        else
        {
            r.append(backslashes, '\\');
            r += arg[i];
        }
    }
    return r + "\"";
}

std::string windows_command_line(const std::vector<std::string>& argv)
{
    std::string r;
    for (size_t i = 0; i < argv.size(); ++i)
    {
        if (i)
            r += ' ';
        r += windows_quote_arg(argv[i]);
    }
    return r;
}

bool is_mingw_triple(const std::string& target_triple)
{
    return target_triple.find("mingw") != std::string::npos || target_triple.find("windows-gnu") != std::string::npos;
}

bool is_darwin_triple(const std::string& target_triple)
{
    for (const char* key : {"apple", "darwin", "macos"})
        if (target_triple.find(key) != std::string::npos)
            return true;
    return false;
}

std::vector<std::string> darwin_target_flags(bool darwin)
{
    if (!darwin)
        return {};
    return {"-Wl,-undefined,dynamic_lookup"};
}

std::vector<std::string> darwin_sysroot_flags(const std::vector<std::string>& args, const std::string& sdk)
{
    if (sdk.empty())
        return {};
    for (const std::string& a : args)
        if (a.rfind("-isysroot", 0) == 0 || a.rfind("--sysroot", 0) == 0)
            return {};
    return {"-isysroot", sdk};
}

std::string macos_entitlement_hint(const std::string& error)
{
    auto has = [&](const char* text) { return error.find(text) != std::string::npos; };
    if (has("code signature") || has("library validation") || has("Team ID"))
        return "hot: macOS refused the signature of the module. A host with the hardened runtime needs the "
               "entitlement com.apple.security.cs.disable-library-validation (doc/macos.md).\n";
    if (has("Permission denied") || has("protect") || has("Operation not permitted"))
        return "hot: macOS refused executable memory. A host with the hardened runtime needs the entitlement "
               "com.apple.security.cs.allow-unsigned-executable-memory for the JIT (doc/macos.md).\n";
    return {};
}

namespace
{

// The numbers of a version text such as "16.2.0". A part that is not a number ends the list.
std::vector<unsigned long> version_parts(const std::string& text)
{
    std::vector<unsigned long> parts;
    size_t i = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
    {
        unsigned long v = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
            v = v * 10 + static_cast<unsigned long>(text[i++] - '0');
        parts.push_back(v);
        if (i < text.size() && text[i] == '.')
            ++i;
        else
            break;
    }
    return parts;
}

// The highest version directory under "dir". Empty when there is none.
std::string highest_version_dir(const fs::path& dir)
{
    std::error_code ec;
    std::string best;
    std::vector<unsigned long> best_parts;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_directory(ec))
            continue;
        const std::string name = it->path().filename().string();
        const std::vector<unsigned long> parts = version_parts(name);
        if (parts.empty())
            continue;
        if (best.empty() || best_parts < parts || (best_parts == parts && best < name))
        {
            best = name;
            best_parts = parts;
        }
    }
    return best;
}

} // namespace

std::string mingw_sysroot_error(const std::string& target_triple, const std::string& sysroot)
{
    if (!is_mingw_triple(target_triple) || sysroot.empty())
        return {};
    // The libstdc++ layout has a version directory. The libc++ layout (llvm-mingw) has "v1".
    std::error_code ec;
    if (!highest_version_dir(sysroot + "/include/c++").empty() || fs::is_directory(sysroot + "/include/c++/v1", ec))
        return {};
    return "the sysroot '" + sysroot + "' has no C++ standard library headers (" + sysroot
        + "/include/c++/<version> for libstdc++ or " + sysroot + "/include/c++/v1 for libc++). The MinGW target "
        + target_triple
        + " needs them: without them the module and the host do not agree on std::string. Give the sysroot of "
          "the MinGW toolchain of the host, for example /usr/x86_64-w64-mingw32";
}

std::vector<std::string> mingw_target_flags(const std::string& target_triple, const std::string& sysroot)
{
    std::vector<std::string> flags;
    if (!is_mingw_triple(target_triple))
        return flags;
    // The libstdc++ headers of the sysroot replace the C++ headers of the clang installation, so
    // the module and a GCC host agree on std::string and on the exceptions. Without these headers
    // the flag -nostdinc++ removes all C++ headers, so the flag comes only with them.
    if (!sysroot.empty())
    {
        const std::string cxx_dir = sysroot + "/include/c++";
        const std::string version = highest_version_dir(cxx_dir);
        if (!version.empty())
        {
            const std::string base = cxx_dir + "/" + version;
            flags.push_back("-nostdinc++");
            const std::string arch = target_triple.substr(0, target_triple.find('-'));
            flags.push_back("-isystem");
            flags.push_back(base);
            flags.push_back("-isystem");
            flags.push_back(base + "/" + arch + "-w64-mingw32");
            flags.push_back("-isystem");
            flags.push_back(base + "/backward");
        }
        // The static libgcc (libgcc.a, libgcc_eh.a) is in the GCC directory, not in the sysroot:
        // <sysroot>/../lib/gcc/<arch>-w64-mingw32/<version> (the layout of a Linux MinGW package).
        const std::string arch = target_triple.substr(0, target_triple.find('-'));
        const std::string gcc_dir =
            fs::path(sysroot).parent_path().generic_string() + "/lib/gcc/" + arch + "-w64-mingw32";
        const std::string gcc_version = highest_version_dir(gcc_dir);
        if (!gcc_version.empty())
            flags.push_back("-L" + gcc_dir + "/" + gcc_version);
    }
    // A clang for the MSVC target has no GNU linker for MinGW. lld links the DLL.
    flags.push_back("-fuse-ld=lld");
    // A MinGW host links libstdc++ and libgcc statically (cmake/toolchains/mingw-w64-x86_64.cmake).
    // The module gets its own static copy, so it needs no libstdc++-6.dll beside the host. Both
    // copies use the malloc of the C runtime, so a std::string can cross the boundary. The SEH
    // unwinder of Windows runs the exceptions across the modules, and libstdc++ on Windows
    // compares the type_info names, so the host catches a ScriptError from a module.
    flags.push_back("-static-libgcc");
    flags.push_back("-static-libstdc++");
    // The static libstdc++ uses the POSIX threads of MinGW (winpthread). The host links it
    // statically too (-static), so the module takes the static library.
    flags.push_back("-Wl,-Bstatic");
    flags.push_back("-lwinpthread");
    flags.push_back("-Wl,-Bdynamic");
    return flags;
}

std::string cxx_driver(const std::string& compiler)
{
    const size_t slash = compiler.find_last_of("/\\");
    const size_t start = slash == std::string::npos ? 0 : slash + 1;
    const std::string name = compiler.substr(start);
    if (name == "clang")
        return compiler + "++";
    if (name == "clang.exe")
        return compiler.substr(0, compiler.size() - 4) + "++.exe";
    return compiler;
}

} // namespace hot
