// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Helpers for the tests of the debug info. They run llvm-dwarfdump and read the line table and
// the functions of the debug info.
#pragma once

#include "llvm_tool.h"
#include "process.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace test_support
{

// True when llvm-dwarfdump runs. A test skips when it does not.
// MSVC host: the modules have the target x86_64-pc-windows-msvc. Their debug info is CodeView
// (a PDB), not DWARF. The tests read DWARF, so they skip. MinGW has DWARF.
inline bool have_dwarfdump()
{
#if defined(_MSC_VER)
    return false;
#else
    return run_process({llvm_tool("llvm-dwarfdump"), "--version"}).exit_code == 0;
#endif
}

// Runs a program from an argument list. Gives stdout and stderr in one text. The line ends are "\n".
inline std::string run_capture(const std::vector<std::string>& argv)
{
    return lf_only(run_process(argv).output);
}

// The text of "llvm-dwarfdump --debug-line file".
// The file that has the DWARF of a library. On macOS the linker does not copy the DWARF into
// the image. The DWARF is in the bundle "<file>.dSYM". The clang driver makes it with dsymutil
// for "-g" when one command compiles and links. Else the helper runs dsymutil.
inline std::string dwarf_file(const std::string& file)
{
#ifdef __APPLE__
    const std::string dsym = file + ".dSYM";
    std::error_code ec;
    if (!std::filesystem::exists(dsym, ec))
        run_capture({llvm_tool("dsymutil"), file, "-o", dsym});
    if (std::filesystem::exists(dsym, ec))
        return dsym;
#endif
    return file;
}

inline std::string dump_line_table(const std::string& file)
{
    return run_capture({llvm_tool("llvm-dwarfdump"), "--debug-line", dwarf_file(file)});
}

// One row of the line table. The file is the name from the file table of the row.
struct LineRow
{
    unsigned long long address = 0;
    int line = 0;
    std::string file;
};

// True when the name in the file table is the base name, or a path that ends with "/base_name".
// A Windows path can have a backslash as the separator, also mixed with "/".
inline bool file_has_base_name(const std::string& n, const std::string& base_name)
{
    if (n == base_name)
        return true;
    if (n.size() <= base_name.size() || n.compare(n.size() - base_name.size(), base_name.size(), base_name) != 0)
        return false;
    const char separator = n[n.size() - base_name.size() - 1];
    return separator == '/' || separator == '\\';
}

// All rows of the line table, in the order of the dump.
// The dump has a file table ("file_names[ 1]:" and "name: "x.hot"") and then the rows
// "0x... LINE COLUMN FILE ...". A dump with more than one table is read table by table.
inline std::vector<LineRow> line_rows(const std::string& dump)
{
    std::vector<LineRow> rows;
    std::map<int, std::string> names;
    int pending = -1;
    std::istringstream in(dump);
    for (std::string l; std::getline(in, l);)
    {
        const size_t fn = l.find("file_names[");
        if (fn != std::string::npos)
        {
            const int index = std::atoi(l.c_str() + fn + 11);
            if (index == 0)
                names.clear();
            pending = index;
            continue;
        }
        const size_t nm = l.find("name: \"");
        if (nm != std::string::npos && pending >= 0)
        {
            const size_t end = l.find('"', nm + 7);
            names[pending] = l.substr(nm + 7, end - nm - 7);
            pending = -1;
            continue;
        }
        if (l.rfind("0x", 0) != 0)
            continue;
        std::istringstream row(l);
        std::string address;
        int line = 0, column = 0, file = 0;
        if (!(row >> address >> line >> column >> file))
            continue;
        auto it = names.find(file);
        if (it == names.end())
            continue;
        rows.push_back({std::strtoull(address.c_str(), nullptr, 16), line, it->second});
    }
    return rows;
}

// The lines of the rows of the line table that belong to the file with this base name. A name
// in the table can have a directory: "tests/scripts/x.hot" matches "x.hot".
inline std::set<int> lines_of_file(const std::string& dump, const std::string& base_name)
{
    std::set<int> lines;
    for (const LineRow& r : line_rows(dump))
        if (file_has_base_name(r.file, base_name))
            lines.insert(r.line);
    return lines;
}

// The text of "llvm-dwarfdump --debug-info file".
inline std::string dump_debug_info(const std::string& file)
{
    return run_capture({llvm_tool("llvm-dwarfdump"), "--debug-info", dwarf_file(file)});
}

// A variable or a parameter of a function.
struct DwarfVariable
{
    std::string tag; // "DW_TAG_formal_parameter" or "DW_TAG_variable"
    std::string name; // the name, also when it comes from DW_AT_abstract_origin
};

// A DW_TAG_subprogram of the debug info.
struct DwarfFunction
{
    std::string name; // DW_AT_name. Empty for a definition outside a class.
    std::string linkage; // DW_AT_linkage_name, or the linkage name of DW_AT_specification
    std::string decl_file;
    bool declaration = false; // a declaration in a class: it has no code
    bool has_code = false; // the function has DW_AT_low_pc and DW_AT_high_pc
    unsigned long long low_pc = 0;
    unsigned long long high_pc = 0; // the first address after the function
    // The parameters and variables of the function and of its lexical blocks. The variables of an
    // inlined function are not here.
    std::vector<DwarfVariable> vars;

    // The names of the variables with this tag, in the order of the dump.
    std::vector<std::string> names(const std::string& tag) const
    {
        std::vector<std::string> out;
        for (const DwarfVariable& v : vars)
            if (v.tag == tag)
                out.push_back(v.name);
        return out;
    }
    // The names of all parameters and variables, in the order of the dump.
    std::vector<std::string> all_names() const
    {
        std::vector<std::string> out;
        for (const DwarfVariable& v : vars)
            out.push_back(v.name);
        return out;
    }
};

namespace detail
{

// The text in the first pair of quotes of an attribute line. Empty when there is none.
inline std::string quoted(const std::string& l)
{
    const size_t a = l.find('"');
    if (a == std::string::npos)
        return {};
    const size_t b = l.find('"', a + 1);
    return b == std::string::npos ? std::string() : l.substr(a + 1, b - a - 1);
}

inline unsigned long long hex_in_parens(const std::string& l)
{
    const size_t a = l.find("(0x");
    return a == std::string::npos ? 0 : std::strtoull(l.c_str() + a + 1, nullptr, 16);
}

} // namespace detail

// All subprograms of the dump of "llvm-dwarfdump --debug-info". The dump is a tree. The column of
// the tag shows the depth of a DIE. Only a lexical block keeps the variables of its function.
inline std::vector<DwarfFunction> functions_of(const std::string& dump)
{
    struct Level
    {
        size_t column;
        int owner; // index of the function that owns the variables below, or -1
    };
    std::vector<DwarfFunction> funcs;
    std::vector<Level> stack;
    enum class Kind
    {
        None,
        Function,
        Variable
    } kind = Kind::None;
    int owner_of_variable = -1;
    std::istringstream in(dump);
    for (std::string l; std::getline(in, l);)
    {
        const size_t tag = l.find("DW_TAG_");
        if (l.rfind("0x", 0) == 0 && tag != std::string::npos)
        {
            const std::string name = l.substr(tag, l.find_first_of(" \t", tag) - tag);
            while (!stack.empty() && stack.back().column >= tag)
                stack.pop_back();
            const int parent = stack.empty() ? -1 : stack.back().owner;
            int owner = -1;
            kind = Kind::None;
            if (name == "DW_TAG_subprogram")
            {
                funcs.emplace_back();
                owner = static_cast<int>(funcs.size()) - 1;
                kind = Kind::Function;
            }
            else if (name == "DW_TAG_lexical_block")
            {
                owner = parent;
            }
            else if ((name == "DW_TAG_formal_parameter" || name == "DW_TAG_variable") && parent >= 0)
            {
                funcs[parent].vars.push_back({name, {}});
                owner_of_variable = parent;
                kind = Kind::Variable;
            }
            stack.push_back({tag, owner});
            continue;
        }
        const size_t at = l.find("DW_AT_");
        if (at == std::string::npos || kind == Kind::None || l.find_first_not_of(" \t") != at)
            continue;
        const std::string attr = l.substr(at, l.find_first_of(" \t", at) - at);
        if (kind == Kind::Function)
        {
            DwarfFunction& f = funcs.back();
            if (attr == "DW_AT_name")
                f.name = detail::quoted(l);
            else if (attr == "DW_AT_linkage_name")
                f.linkage = detail::quoted(l);
            else if (attr == "DW_AT_specification" && f.linkage.empty())
                f.linkage = detail::quoted(l);
            else if (attr == "DW_AT_decl_file")
                f.decl_file = detail::quoted(l);
            else if (attr == "DW_AT_declaration")
                f.declaration = true;
            else if (attr == "DW_AT_low_pc")
                f.low_pc = detail::hex_in_parens(l), f.has_code = true;
            else if (attr == "DW_AT_high_pc")
                f.high_pc = detail::hex_in_parens(l);
        }
        else if (attr == "DW_AT_name" || attr == "DW_AT_abstract_origin")
        {
            DwarfVariable& v = funcs[owner_of_variable].vars.back();
            if (attr == "DW_AT_name" || v.name.empty())
                v.name = detail::quoted(l);
        }
    }
    return funcs;
}

} // namespace test_support
