// Helpers for the tests of the debug info. They run llvm-dwarfdump and read the line table.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace test_support {

// True when llvm-dwarfdump runs. A test skips when it does not.
inline bool have_dwarfdump()
{
    return std::system("llvm-dwarfdump --version >/dev/null 2>&1") == 0;
}

// Runs a shell command. Gives stdout and stderr.
inline std::string run_capture(const std::string& cmd)
{
    std::string text;
    FILE* p = popen((cmd + " 2>&1").c_str(), "r");
    if (p == nullptr) return text;
    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof buf, p)) text.append(buf, n);
    pclose(p);
    return text;
}

// The text of "llvm-dwarfdump --debug-line file".
inline std::string dump_line_table(const std::string& file)
{
    return run_capture("llvm-dwarfdump --debug-line '" + file + "'");
}

// The lines of the rows of the line table that belong to the file with this base name. A name
// in the table can have a directory: "example/scripts/x.spp" matches "x.spp".
// The dump has a file table ("file_names[ 1]:" and "name: "x.spp"") and then the rows
// "0x... LINE COLUMN FILE ...". A dump with more than one table is read table by table.
inline std::set<int> lines_of_file(const std::string& dump, const std::string& base_name)
{
    std::set<int> lines;
    std::map<int, std::string> names;
    int pending = -1;
    std::istringstream in(dump);
    for (std::string l; std::getline(in, l);) {
        const size_t fn = l.find("file_names[");
        if (fn != std::string::npos) {
            const int index = std::atoi(l.c_str() + fn + 11);
            if (index == 0) names.clear();
            pending = index;
            continue;
        }
        const size_t nm = l.find("name: \"");
        if (nm != std::string::npos && pending >= 0) {
            const size_t end = l.find('"', nm + 7);
            names[pending] = l.substr(nm + 7, end - nm - 7);
            pending = -1;
            continue;
        }
        if (l.rfind("0x", 0) != 0) continue;
        std::istringstream row(l);
        std::string address;
        int line = 0, column = 0, file = 0;
        if (!(row >> address >> line >> column >> file)) continue;
        auto it = names.find(file);
        if (it == names.end()) continue;
        const std::string& n = it->second;
        if (n == base_name || (n.size() > base_name.size() && n.compare(n.size() - base_name.size(), base_name.size(), base_name) == 0 &&
                               n[n.size() - base_name.size() - 1] == '/'))
            lines.insert(line);
    }
    return lines;
}

}  // namespace test_support
