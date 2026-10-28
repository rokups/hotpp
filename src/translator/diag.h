// A diagnostics sink with byte offset, line and column.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sppc {

// Maps a byte offset to a 1-based line and a 1-based byte column.
struct LineMap {
    explicit LineMap(std::string_view text)
    {
        starts_.push_back(0);
        for (size_t i = 0; i < text.size(); ++i)
            if (text[i] == '\n') starts_.push_back(static_cast<uint32_t>(i + 1));
    }

    int line(uint32_t pos) const
    {
        auto it = std::upper_bound(starts_.begin(), starts_.end(), pos);
        return static_cast<int>(it - starts_.begin());
    }

    int col(uint32_t pos) const
    {
        return static_cast<int>(pos - starts_[static_cast<size_t>(line(pos) - 1)]) + 1;
    }

private:
    std::vector<uint32_t> starts_;  // the byte offset of the first char of each line
};

struct Diag {
    uint32_t pos = 0;
    int line = 0;  // 0: no script line
    int col = 0;   // 0: the column is not known
    std::string msg;
    std::string file;  // empty: the script name
    std::string severity = "error";
};

struct Diagnostics {
    std::vector<Diag> items;     // the errors
    std::vector<Diag> warnings;  // the warnings. They do not stop the translation.
    // The byte offsets of a "." with no member name after it ("player." and then nothing).
    // The parser also adds an error for each. A language server uses them for completion.
    std::vector<uint32_t> member_holes;

    void add(uint32_t pos, const LineMap& map, std::string msg)
    {
        items.push_back(Diag{pos, map.line(pos), map.col(pos), std::move(msg), {}, "error"});
    }

    void warn(uint32_t pos, const LineMap& map, std::string msg)
    {
        warnings.push_back(Diag{pos, map.line(pos), map.col(pos), std::move(msg), {}, "warning"});
    }

    void add_line(int line, std::string msg)
    {
        items.push_back(Diag{0, line, 0, std::move(msg), {}, "error"});
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
        for (const Diag& d : list) {
            if (d.line > 0) {
                out += (d.file.empty() ? name : d.file) + ":" + std::to_string(d.line);
                if (d.col > 0) out += ":" + std::to_string(d.col);
            } else {
                out += "sppc";  // no script line: the text starts with the tool name
            }
            out += ": " + d.severity + ": " + d.msg + "\n";
        }
        return out;
    }
};

}  // namespace sppc
