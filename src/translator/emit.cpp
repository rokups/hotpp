// The printer of the expression and statement AST.
//
// Spacing rules and quirks. The old token rewriter keeps the white space of the
// script. The printer writes a fixed layout, and it keeps these rules of the old
// body emitter:
//  1. A leaf starts a new output line when its script line differs from the
//     current one. The line starts with "#line N "file"", a new line, and the
//     indentation (4 spaces for each level). The first "#line" has no new line
//     before it, because the caller ends the previous line. finish() ends the
//     last line.
//  2. Only the first leaf of an output line gets the indentation. A "}" at the
//     start of a line gets one level less.
//  3. The level counts each leaf with the text "{" or "}", also the braces of a
//     braced list. This is a quirk of the old emitter. A braced list over more
//     than one line has the same indentation effect as a block.
//  4. A "case" or "default" label has the same indentation as the statements
//     of the switch. It has no outdent.
//  5. A line break drops the space before the leaf. A leaf with pos 0 never
//     starts a line. A node that has no pos (for example ";" or ")") stays on
//     the line of the leaf before it.
// Differences from the old emitter:
//  6. The old emitter breaks on any change of the line. The printer breaks only
//     when the line is greater than the current line.
//  7. An operator, a "." or a "->" has no pos. A line break before an operator
//     appears before the next leaf. The old emitter breaks before the operator.
//  8. This prints as "this", Base as "base" and Null as "nullptr". The
//     lowering pass must replace "this" and "base" with Raw nodes.
//  9. A For with more than one VarDecl in init prints invalid C++. Several
//     ExprStmt in init print as a comma expression.
#include "emit.h"

#include <algorithm>

namespace sppc::ast
{

namespace
{

std::string escape(const std::string& f)
{
    std::string r;
    for (char c : f)
    {
        if (c == '\\' || c == '"')
            r += '\\';
        r += c;
    }
    return r;
}

} // namespace

void Emitter::put(std::string_view t, uint32_t pos)
{
    bool broke = false;
    if (map_ && pos != 0)
    {
        int line = map_->line(pos);
        if (line > line_)
        {
            if (!out_.empty())
                out_ += '\n';
            // A position in an imported script names the file of that script (LineMap::add_file).
            const std::string& f = map_->file(pos);
            out_ += "#line " + std::to_string(line) + " \"" + escape(f.empty() ? file_ : f) + "\"\n";
            int d = depth - (t == "}" ? 1 : 0);
            out_.append(static_cast<size_t>(4 * std::max(d, 0)), ' ');
            line_ = line;
            broke = true;
        }
    }
    if (!broke && space_ && !out_.empty())
        out_ += ' ';
    space_ = false;
    if (map_ && pos != 0 && !t.empty())
        segments_.push_back({pos, static_cast<uint32_t>(out_.size()), static_cast<uint32_t>(t.size())});
    out_ += t;
    if (t == "{")
        ++depth;
    if (t == "}")
        --depth;
}

void Emitter::put_named(std::string_view t, uint32_t pos, size_t name_at, uint32_t name_pos)
{
    if (!map_ || name_pos == 0 || name_at >= t.size())
        return put(t, pos);
    // The prefix gets the line logic and the space. The name follows with no space.
    put(t.substr(0, name_at), pos);
    segments_.push_back({name_pos, static_cast<uint32_t>(out_.size()), static_cast<uint32_t>(t.size() - name_at)});
    out_ += t.substr(name_at);
}

void Emitter::put_mapped(std::string_view t, uint32_t pos)
{
    if (!t.empty() && pos != 0)
    {
        if (map_)
        {
            if (space_ && !out_.empty())
                out_ += ' ';
            space_ = false;
            segments_.push_back({pos, static_cast<uint32_t>(out_.size()), static_cast<uint32_t>(t.size())});
            out_ += t;
            return;
        }
    }
    put(t);
}

void Emitter::finish()
{
    if (map_ && line_ != 0)
        out_ += '\n';
}

void Emitter::rewind(uint32_t pos)
{
    if (!map_ || pos == 0)
        return;
    const int line = map_->line(pos);
    if (line_ > line)
        line_ = line - 1;
}

namespace
{

void block(const BlockStmt& b, Emitter& e);
void stmt_list(const std::vector<StmtP>& v, Emitter& e);

void expr_list(const std::vector<ExprP>& v, Emitter& e)
{
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (i)
        {
            e.put(",");
            e.space();
        }
        emit_expr(*v[i], e);
    }
}

struct ExprPrinter
{
    Emitter& e;

    void operator()(const NameExpr& n) const
    {
        std::string t;
        for (size_t i = 0; i < n.parts.size(); ++i)
            t += (i ? "::" : "") + n.parts[i];
        if (n.parts.size() >= 2 && n.pos != 0 && n.last_pos != 0)
        {
            // The last part gets its own segment, so the language server maps a position in it.
            // The generated text has "::" between the parts. A part holds the template
            // arguments of its own, in the form the lowering wrote.
            size_t name_at = 0;
            for (size_t i = 0; i + 1 < n.parts.size(); ++i)
                name_at += n.parts[i].size() + 2;
            e.put_named(t + n.template_args, n.pos, name_at, n.last_pos);
            return;
        }
        e.put(t + n.template_args, n.pos);
    }
    void operator()(const ThisExpr& n) const { e.put("this", n.pos); }
    void operator()(const BaseExpr& n) const { e.put("base", n.pos); }
    void operator()(const NullExpr& n) const { e.put("nullptr", n.pos); }
    void operator()(const NumberExpr& n) const { e.put(n.text, n.pos); }
    void operator()(const StringExpr& n) const { e.put(n.text, n.pos); }
    void operator()(const CharExpr& n) const { e.put(n.text, n.pos); }
    void operator()(const RawExpr& n) const { e.put_named(n.text, n.pos, n.name_at, n.name_pos); }
    // The script form. The lowering replaces the node, so only a parse test prints it.
    void operator()(const InterpExpr& n) const
    {
        std::string t = "\"";
        for (size_t i = 0; i < n.lits.size(); ++i)
        {
            t += n.lits[i];
            if (i < n.args.size())
            {
                t += "${" + print(*n.args[i]);
                if (!n.specs[i].empty())
                    t += ":" + n.specs[i];
                t += "}";
            }
        }
        e.put(t + "\"", n.pos);
    }
    // A braced list. A map entry is a braced pair.
    void operator()(const CollectionExpr& n) const
    {
        e.put("{", n.pos);
        for (size_t i = 0; i < n.elems.size(); ++i)
        {
            if (i)
            {
                e.put(",");
                e.space();
            }
            if (n.is_map)
                e.put("{");
            emit_expr(*n.elems[i], e);
            if (n.is_map)
            {
                e.put(",");
                e.space();
                emit_expr(*n.values[i], e);
                e.put("}");
            }
        }
        e.put("}", n.end_pos);
    }
    void operator()(const MemberExpr& n) const
    {
        emit_expr(*n.obj, e);
        e.put(".");
        e.put_mapped(n.name, n.name_pos);
    }
    void operator()(const CallExpr& n) const
    {
        emit_expr(*n.callee, e);
        e.put("(");
        expr_list(n.args, e);
        e.put(")");
    }
    void operator()(const IndexExpr& n) const
    {
        emit_expr(*n.obj, e);
        e.put("[");
        emit_expr(*n.index, e);
        e.put("]");
    }
    void operator()(const UnaryExpr& n) const
    {
        if (n.postfix)
        {
            emit_expr(*n.operand, e);
            e.put(n.op);
            return;
        }
        e.put(n.op, n.pos);
        // "- -x" must not become "--x".
        const auto* inner = std::get_if<UnaryExpr>(&n.operand->v);
        if (inner && !inner->postfix && !n.op.empty() && !inner->op.empty() && inner->op[0] == n.op.back()
            && (n.op.back() == '-' || n.op.back() == '+' || n.op.back() == '&'))
            e.space();
        emit_expr(*n.operand, e);
    }
    void operator()(const BinaryExpr& n) const
    {
        emit_expr(*n.lhs, e);
        e.space();
        e.put(n.op);
        e.space();
        emit_expr(*n.rhs, e);
    }
    void operator()(const TernaryExpr& n) const
    {
        emit_expr(*n.cond, e);
        e.space();
        e.put("?");
        e.space();
        emit_expr(*n.a, e);
        e.space();
        e.put(":");
        e.space();
        emit_expr(*n.b, e);
    }
    void operator()(const ParenExpr& n) const
    {
        e.put("(", n.pos);
        emit_expr(*n.inner, e);
        e.put(")");
    }
    void operator()(const NewExpr& n) const
    {
        e.put("new", n.pos);
        e.space();
        e.put(n.type);
        e.put("(");
        expr_list(n.args, e);
        e.put(")");
    }
    // The script form. The lowering replaces the node, so only a parse test prints it.
    void operator()(const TypeTestExpr& n) const
    {
        emit_expr(*n.obj, e);
        e.space();
        e.put(n.is_as ? "as" : "is");
        e.space();
        e.put(n.type);
        if (!n.bind.empty())
        {
            e.space();
            e.put(n.bind);
        }
    }
    void operator()(const LambdaExpr& n) const
    {
        e.put("[=" + n.captures + "]", n.pos);
        e.put("(");
        for (size_t i = 0; i < n.params.size(); ++i)
        {
            if (i)
            {
                e.put(",");
                e.space();
            }
            const Param& p = n.params[i];
            e.put(p.type.empty() ? "auto" : p.type);
            e.space();
            e.put(p.name);
        }
        e.put(")");
        block(n.body, e);
    }
};

// A declaration or an expression statement without the ";" (the header of a for).
void simple(const Stmt& s, Emitter& e);

// True when "T()" is valid C++ text for the type: no pointer, reference, array or multi-word type.
bool plain_type(const std::string& t)
{
    int depth = 0;
    for (char c : t)
    {
        if (c == '<')
            ++depth;
        if (c == '>')
            --depth;
        if (depth == 0 && (c == '*' || c == '&' || c == '[' || c == '(' || c == ' '))
            return false;
    }
    return true;
}

// A local without an initializer is value-initialized: zero, null or the default constructor.
// It is never indeterminate.
// The names of a structured binding: "[a, b]".
void binding_names(const std::vector<std::string>& names, const std::vector<uint32_t>& poss, Emitter& e)
{
    e.put("[");
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (i)
        {
            e.put(",");
            e.space();
        }
        e.put_mapped(names[i], i < poss.size() ? poss[i] : 0);
    }
    e.put("]");
}

void decl(const VarDeclStmt& d, Emitter& e)
{
    e.put(std::string(d.is_const ? "const " : "") + (d.type.empty() ? (d.is_ref ? "auto&" : "auto") : d.type), d.pos);
    e.space();
    if (!d.names.empty())
        binding_names(d.names, d.name_poss, e);
    else
        e.put_mapped(d.name, d.name_pos);
    if (d.init)
    {
        e.space();
        e.put("=");
        e.space();
        emit_expr(*d.init, e);
    }
    else if (!d.type.empty() && !d.is_const)
    {
        e.space();
        e.put("=");
        e.space();
        e.put(plain_type(d.type) ? d.type + "()" : "std::type_identity_t<" + d.type + ">()");
    }
}

// The "(" ... ")" part of a statement keyword, then a space.
void open_paren(Emitter& e)
{
    e.space();
    e.put("(");
}
void close_paren(Emitter& e)
{
    e.put(")");
    e.space();
}

struct StmtPrinter
{
    Emitter& e;

    void operator()(const BlockStmt& n) const { block(n, e); }
    void operator()(const ExprStmt& n) const
    {
        emit_expr(*n.expr, e);
        e.put(";");
    }
    void operator()(const VarDeclStmt& n) const
    {
        decl(n, e);
        e.put(";");
    }
    void operator()(const IfStmt& n) const
    {
        e.put("if", n.pos);
        open_paren(e);
        emit_expr(*n.cond, e);
        close_paren(e);
        emit_stmt(*n.then, e);
        if (n.otherwise)
        {
            e.space();
            e.put("else", n.else_pos);
            e.space();
            emit_stmt(*n.otherwise, e);
        }
    }
    void operator()(const WhileStmt& n) const
    {
        e.put("while", n.pos);
        open_paren(e);
        emit_expr(*n.cond, e);
        close_paren(e);
        emit_stmt(*n.body, e);
    }
    void operator()(const DoWhileStmt& n) const
    {
        e.put("do", n.pos);
        e.space();
        emit_stmt(*n.body, e);
        e.space();
        e.put("while", n.while_pos);
        open_paren(e);
        emit_expr(*n.cond, e);
        e.put(")");
        e.put(";");
    }
    void operator()(const ForStmt& n) const
    {
        e.put("for", n.pos);
        open_paren(e);
        for (size_t i = 0; i < n.init.size(); ++i)
        {
            if (i)
            {
                e.put(",");
                e.space();
            }
            simple(*n.init[i], e);
        }
        e.put(";");
        if (n.cond)
        {
            e.space();
            emit_expr(*n.cond, e);
        }
        e.put(";");
        if (!n.step.empty())
        {
            e.space();
            expr_list(n.step, e);
        }
        close_paren(e);
        emit_stmt(*n.body, e);
    }
    void operator()(const ForOfStmt& n) const
    {
        e.put("for", n.pos);
        open_paren(e);
        e.put(std::string(n.is_const ? "const " : "") + (n.type.empty() ? (n.is_ref ? "auto&" : "auto") : n.type));
        e.space();
        if (!n.names.empty())
            binding_names(n.names, n.name_poss, e);
        else
            e.put_mapped(n.name, n.name_pos);
        e.space();
        e.put(":");
        e.space();
        emit_expr(*n.range, e);
        close_paren(e);
        emit_stmt(*n.body, e);
    }
    void operator()(const ReturnStmt& n) const
    {
        e.put("return", n.pos);
        if (n.expr)
        {
            e.space();
            emit_expr(*n.expr, e);
        }
        e.put(";");
    }
    void operator()(const BreakStmt& n) const
    {
        e.put("break", n.pos);
        e.put(";");
    }
    void operator()(const ContinueStmt& n) const
    {
        e.put("continue", n.pos);
        e.put(";");
    }
    void operator()(const SwitchStmt& n) const
    {
        e.put("switch", n.pos);
        open_paren(e);
        emit_expr(*n.subject, e);
        close_paren(e);
        e.put("{");
        for (const SwitchCase& c : n.cases)
        {
            bool first = true;
            for (size_t i = 0; i < c.labels.size(); ++i)
            {
                e.space();
                e.put("case", i < c.label_pos.size() ? c.label_pos[i] : first ? c.pos : 0);
                first = false;
                e.space();
                emit_expr(*c.labels[i], e);
                e.put(":");
            }
            if (c.is_default)
            {
                e.space();
                e.put("default", c.default_pos != 0 ? c.default_pos : first ? c.pos : 0);
                e.put(":");
            }
            stmt_list(c.stmts, e);
        }
        e.space();
        e.put("}", n.end_pos);
    }
    void operator()(const ThrowStmt& n) const
    {
        e.put("throw", n.pos);
        if (n.expr)
        {
            e.space();
            emit_expr(*n.expr, e);
        }
        e.put(";");
    }
    // A finally block prints as an RAII guard before the try:
    //   { spp::Finally __fin_N([&] { body }); try { ... } catch ... }
    // The guard runs the body when the outer block ends, on each path out of it.
    // N is unique in the Emitter, so a nested try gets a different name.
    void operator()(const TryStmt& n) const
    {
        if (n.finally_block)
        {
            e.put("{", n.pos);
            e.space();
            e.put("spp::Finally __fin_" + std::to_string(e.next_finally()) + "([&" + n.finally_captures + "]");
            block(*n.finally_block, e);
            e.put(")");
            e.put(";");
            e.space();
            e.rewind(n.pos);
        }
        // A try without a catch clause has no use in C++. The body is a plain block.
        if (!n.catches.empty())
            e.put("try", n.pos);
        block(n.block, e);
        for (const CatchClause& c : n.catches)
        {
            e.space();
            e.put("catch", c.pos);
            open_paren(e);
            if (c.is_all)
            {
                e.put("...");
            }
            else
            {
                e.put(c.type);
                e.space();
                e.put(c.name);
            }
            e.put(")");
            block(c.block, e);
        }
        if (n.finally_block)
        {
            e.space();
            e.put("}");
        }
    }
    void operator()(const EmptyStmt& n) const { e.put(";", n.pos); }
    void operator()(const RawStmt& n) const { e.put(n.text, n.pos); }
};

void simple(const Stmt& s, Emitter& e)
{
    if (const auto* d = std::get_if<VarDeclStmt>(&s.v))
        decl(*d, e);
    else if (const auto* x = std::get_if<ExprStmt>(&s.v))
        emit_expr(*x->expr, e);
    else
        emit_stmt(s, e);
}

void stmt_list(const std::vector<StmtP>& v, Emitter& e)
{
    for (const StmtP& s : v)
    {
        e.space();
        emit_stmt(*s, e);
    }
}

// The braces of a block at the current depth.
void block(const BlockStmt& b, Emitter& e)
{
    e.space();
    e.put("{", b.pos);
    stmt_list(b.stmts, e);
    if (!b.stmts.empty())
        e.space();
    e.put("}", b.end_pos);
}

} // namespace

void emit_expr(const Expr& x, Emitter& out)
{
    std::visit(ExprPrinter{out}, x.v);
}
void emit_stmt(const Stmt& s, Emitter& out)
{
    std::visit(StmtPrinter{out}, s.v);
}

void emit_block(const BlockStmt& b, Emitter& out, int depth)
{
    out.depth = depth;
    block(b, out);
}

void emit_stmts(const std::vector<StmtP>& v, Emitter& out, int depth)
{
    out.depth = depth;
    stmt_list(v, out);
}

std::string print(const Expr& x)
{
    Emitter e;
    emit_expr(x, e);
    return e.text();
}

std::string print(const Stmt& s)
{
    Emitter e;
    emit_stmt(s, e);
    return e.text();
}

std::string print_lines(const Stmt& s, const LineMap& map, const std::string& file, int depth)
{
    Emitter e(map, file);
    e.depth = depth;
    emit_stmt(s, e);
    e.finish();
    return e.text();
}

} // namespace sppc::ast
