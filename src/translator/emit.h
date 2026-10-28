// The printer of the expression and statement AST. It writes C++ text.
//
// The Emitter has two modes:
//  - plain mode: no LineMap. The text has no "#line" and no line break.
//  - line mode: a LineMap and a file name. A leaf on a later script line starts
//    a new output line with a "#line" directive and the indentation.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ast.h"
#include "diag.h"

namespace sppc::ast {

class Emitter {
public:
    Emitter() = default;  // plain mode
    // Line mode. The map must live as long as the Emitter.
    Emitter(const LineMap& map, std::string file) : map_(&map), file_(std::move(file)) {}

    // Writes one leaf. A pos of 0 means: stay on the current line.
    void put(std::string_view text, uint32_t pos = 0);
    // Asks for one space before the next leaf. A line break drops it.
    void space() { space_ = true; }
    // Line mode: ends the last line. Call it once at the end.
    void finish();
    // Line mode: the next leaf at "pos" starts a new line, also when its script line is
    // not greater than the current line. A finally block uses it: the guard prints the
    // finally body before the try body, so the script lines go back.
    void rewind(uint32_t pos);
    // Gives a new number for the name of a finally guard. The numbers start at 0.
    int next_finally() { return finally_count_++; }

    const std::string& text() const { return out_; }
    int source_line() const { return line_; }

    int depth = 0;  // the indentation level; each "{" adds 1 and each "}" removes 1

private:
    const LineMap* map_ = nullptr;
    std::string file_;
    std::string out_;
    int line_ = 0;  // the script line of the last "#line"; 0: none yet
    bool space_ = false;
    int finally_count_ = 0;
};

void emit_expr(const Expr& e, Emitter& out);
void emit_stmt(const Stmt& s, Emitter& out);
// Sets out.depth to "depth", then writes the block with its braces. The braces are
// at the indentation of "depth" and the statements are at "depth + 1".
void emit_block(const BlockStmt& b, Emitter& out, int depth);
// Sets out.depth to "depth", then writes the statements without braces (a function body).
void emit_stmts(const std::vector<StmtP>& v, Emitter& out, int depth);

// Plain mode text of one node.
std::string print(const Expr& e);
std::string print(const Stmt& s);
// Line mode text of one statement, with the last line end.
std::string print_lines(const Stmt& s, const LineMap& map, const std::string& file, int depth);

}  // namespace sppc::ast
