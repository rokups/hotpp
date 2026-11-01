// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A diagnostics sink with byte offset, line and column.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "std_ns.h"

namespace hotc
{

// Maps a byte offset to a 1-based line and a 1-based byte column.
struct LineMap
{
    explicit LineMap(std::string_view text)
    {
        starts_.push_back(0);
        for (size_t i = 0; i < text.size(); ++i)
            if (text[i] == '\n')
                starts_.push_back(static_cast<uint32_t>(i + 1));
    }

    // Adds the text of another file (an imported script) at the byte offset "base". The
    // parser of that file reads a text with "base" blank bytes before it, so each position is
    // unique in the translation. A position at or after "base" belongs to that file. Call it
    // with increasing "base" values, each after the end of the earlier texts.
    void add_file(uint32_t base, std::string_view text, std::string file)
    {
        Region r;
        r.base = base;
        r.file = std::move(file);
        r.starts.push_back(base);
        for (size_t i = 0; i < text.size(); ++i)
            if (text[i] == '\n')
                r.starts.push_back(static_cast<uint32_t>(base + i + 1));
        regions_.push_back(std::move(r));
    }

    int line(uint32_t pos) const
    {
        const std::vector<uint32_t>& s = starts_of(pos);
        auto it = std::upper_bound(s.begin(), s.end(), pos);
        return static_cast<int>(it - s.begin());
    }

    int col(uint32_t pos) const
    {
        return static_cast<int>(pos - starts_of(pos)[static_cast<size_t>(line(pos) - 1)]) + 1;
    }

    // The file of a position: empty for the main script, else the name of the imported script.
    const std::string& file(uint32_t pos) const
    {
        static const std::string none;
        const Region* r = region_of(pos);
        return r ? r->file : none;
    }

private:
    struct Region
    {
        uint32_t base = 0;
        std::string file;
        std::vector<uint32_t> starts;
    };
    const Region* region_of(uint32_t pos) const
    {
        for (size_t i = regions_.size(); i-- > 0;)
            if (pos >= regions_[i].base)
                return &regions_[i];
        return nullptr;
    }
    const std::vector<uint32_t>& starts_of(uint32_t pos) const
    {
        const Region* r = region_of(pos);
        return r ? r->starts : starts_;
    }
    std::vector<uint32_t> starts_; // the byte offset of the first char of each line
    std::vector<Region> regions_; // the imported scripts, by increasing base
};

struct Diag
{
    uint32_t pos = 0;
    int line = 0; // 0: no script line
    int col = 0; // 0: the column is not known
    std::string msg;
    std::string file; // empty: the script name
    std::string severity = "error";
};

struct Diagnostics
{
    std::vector<Diag> items; // the errors
    std::vector<Diag> warnings; // the warnings. They do not stop the translation.
    // The byte offsets of a "." with no member name after it ("player." and then nothing).
    // The parser also adds an error for each. A language server uses them for completion.
    std::vector<uint32_t> member_holes;

    // The type text of the translator holds the placeholder "\x01std::" of the standard namespace
    // (std_ns.h). A message shows the namespace of the host.
    static std::string script_form(std::string msg) { return expand_std(msg, current_std_ns()); }

    void add(uint32_t pos, const LineMap& map, std::string msg)
    {
        items.push_back(Diag{pos, map.line(pos), map.col(pos), script_form(std::move(msg)), map.file(pos), "error"});
    }

    void warn(uint32_t pos, const LineMap& map, std::string msg)
    {
        warnings.push_back(
            Diag{pos, map.line(pos), map.col(pos), script_form(std::move(msg)), map.file(pos), "warning"});
    }

    void add_line(int line, std::string msg)
    {
        items.push_back(Diag{0, line, 0, script_form(std::move(msg)), {}, "error"});
    }

    // A diagnostic from another file, for example a header that libclang reads.
    void add_foreign(std::string file, int line, int col, std::string severity, std::string msg)
    {
        items.push_back(Diag{0, line, col, std::move(msg), std::move(file), std::move(severity)});
    }

    void clear()
    {
        items.clear();
        warnings.clear();
        member_holes.clear();
    }

    // One diagnostic in each text line, in the order of insertion. The errors only.
    std::string format(const std::string& name) const { return format(name, items); }

    // The same for the warnings.
    std::string format_warnings(const std::string& name) const { return format(name, warnings); }

    static std::string format(const std::string& name, const std::vector<Diag>& list)
    {
        std::string out;
        for (const Diag& d : list)
        {
            if (d.line > 0)
            {
                out += (d.file.empty() ? name : d.file) + ":" + std::to_string(d.line);
                if (d.col > 0)
                    out += ":" + std::to_string(d.col);
            }
            else
            {
                out += "hotc"; // no script line: the text starts with the tool name
            }
            out += ": " + d.severity + ": " + d.msg + "\n";
        }
        return out;
    }
};

} // namespace hotc
