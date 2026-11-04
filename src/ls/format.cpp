// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "format.h"

#include <algorithm>
#include <cstddef>
#include <set>

namespace hot::ls
{

namespace
{

using sv = std::string_view;

enum class Kind
{
    Word, // an identifier, a keyword or a number
    Str, // a string or a char literal, with the text of "${...}" inside
    LineComment,
    BlockComment,
    Punct
};

struct Tok
{
    Kind kind = Kind::Punct;
    size_t begin = 0, end = 0;
    size_t gap_begin = 0; // the white space before the token starts here
    size_t nl = 0; // the new lines in the white space before the token
};

bool is_word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$'
        || static_cast<unsigned char>(c) >= 0x80;
}

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

// The length of the longest punctuation token at the start of the text.
size_t punct_len(sv s)
{
    static const char* const three[] = {"<<=", "...", "?\?="};
    static const char* const two[] = {"::", "->", "++", "--",
        "+=", "-=", "*=", "/=", "%=", "==", "!=", "<=", ">=", "&&", "||", "<<", "&=", "|=", "^=", "=>", "??", "..",
        "?."};
    if (s.size() >= 3)
        for (const char* p : three)
            if (s.substr(0, 3) == p)
                return 3;
    if (s.size() >= 2)
        for (const char* p : two)
            if (s.substr(0, 2) == p)
            {
                // "?.5" is a "?" and a number.
                if (s[0] == '?' && s[1] == '.' && s.size() >= 3 && s[2] >= '0' && s[2] <= '9')
                    break;
                return 2;
            }
    return 1;
}

constexpr size_t npos = sv::npos;

// A scan that fails has read to the end of the text. To keep the lexer linear, the first failed
// scan of "${" or of a raw string turns off the same scan for the rest of the text. The text is
// then a lexical error anyway. The flags are reset at the start of lex().
thread_local bool g_interp_dead = false;
thread_local bool g_raw_dead = false;
constexpr int kMaxInterpDepth = 64;

size_t skip_interp(sv s, size_t p, int depth = 0);

// A string or a char inside "${...}". It can hold a new line. The position is at the quote.
// The result is after the closing quote, or npos.
size_t skip_nested(sv s, size_t p, int depth)
{
    const char q = s[p];
    size_t j = p + 1;
    while (j < s.size())
    {
        const char c = s[j];
        if (c == '\\')
            j += 2;
        else if (c == q)
            return j + 1;
        else if (q == '"' && c == '$' && j + 1 < s.size() && s[j + 1] == '{')
        {
            j = skip_interp(s, j + 2, depth + 1);
            if (j == npos)
                return npos;
        }
        else
            ++j;
    }
    return npos;
}

// The text of "${ ... }" after the "${". The result is after the matching "}", or npos.
size_t skip_interp(sv s, size_t p, int depth)
{
    if (depth > kMaxInterpDepth)
        return npos;
    while (p < s.size())
    {
        const char c = s[p];
        if (c == '}')
            return p + 1;
        if (c == '"' || c == '\'')
            p = skip_nested(s, p, depth);
        else if (c == '{')
            p = skip_interp(s, p + 1, depth + 1);
        else
            ++p;
        if (p == npos)
            return npos;
    }
    return npos;
}

// A string or a char literal. The position is at the quote. A literal has no new line, except
// inside "${...}". An unterminated literal ends before the new line.
size_t skip_quoted(sv s, size_t i)
{
    const char q = s[i];
    size_t j = i + 1;
    while (j < s.size())
    {
        const char c = s[j];
        if (c == '\n' || (c == '\r' && j + 1 < s.size() && s[j + 1] == '\n'))
            return j;
        if (c == '\\')
        {
            j += 2;
        }
        else if (c == q)
        {
            return j + 1;
        }
        else if (q == '"' && c == '$' && j + 1 < s.size() && s[j + 1] == '{')
        {
            const size_t k = g_interp_dead ? npos : skip_interp(s, j + 2);
            if (k == npos)
                g_interp_dead = true;
            j = k == npos ? j + 1 : k;
        }
        else
        {
            ++j;
        }
    }
    return s.size();
}

// A raw string R"d( ... )d" after the prefix. The position is at the quote. The result is after the
// closing quote, or npos when the text is not a raw string.
size_t skip_raw(sv s, size_t quote)
{
    if (g_raw_dead)
        return npos;
    size_t open = quote + 1;
    while (open < s.size() && open - quote <= 17 && s[open] != '(')
        ++open;
    if (open >= s.size() || s[open] != '(')
        return npos;
    const std::string close = ")" + std::string(s.substr(quote + 1, open - quote - 1)) + "\"";
    const size_t e = s.find(close, open + 1);
    if (e == npos)
    {
        // One failed search reads to the end of the text. A later raw string is not searched.
        g_raw_dead = true;
        return npos;
    }
    return e + close.size();
}

std::vector<Tok> lex(sv s)
{
    g_interp_dead = false;
    g_raw_dead = false;
    std::vector<Tok> out;
    size_t i = 0;
    while (i < s.size())
    {
        Tok t;
        t.gap_begin = i;
        while (i < s.size() && is_space(s[i]))
        {
            if (s[i] == '\n')
                ++t.nl;
            ++i;
        }
        if (i >= s.size())
            break;
        t.begin = i;
        const char c = s[i];
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/')
        {
            t.kind = Kind::LineComment;
            while (i < s.size() && s[i] != '\n')
                ++i;
            while (i > t.begin && (s[i - 1] == ' ' || s[i - 1] == '\t' || s[i - 1] == '\r'))
                --i;
        }
        else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*')
        {
            t.kind = Kind::BlockComment;
            const size_t e = s.find("*/", i + 2);
            i = e == npos ? s.size() : e + 2;
            // An unterminated comment ends at the end of the text. It loses the white space at
            // the end, so the new line that the formatter adds does not grow at each run.
            if (e == npos)
                while (i > t.begin + 2 && is_space(s[i - 1]))
                    --i;
        }
        else if (c == '"' || c == '\'')
        {
            t.kind = Kind::Str;
            i = skip_quoted(s, i);
        }
        else if (is_word_char(c))
        {
            t.kind = Kind::Word;
            const size_t b = i;
            const bool number = c >= '0' && c <= '9';
            const bool hex = number && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X');
            while (i < s.size() && (is_word_char(s[i]) || (number && s[i] == '.')))
            {
                const char d = s[i];
                ++i;
                if (number && !hex && (d == 'e' || d == 'E') && i + 1 < s.size() && (s[i] == '+' || s[i] == '-')
                    && s[i + 1] >= '0' && s[i + 1] <= '9')
                    ++i;
            }
            // A raw string: the prefix is R, LR, uR, UR or u8R.
            const sv w = s.substr(b, i - b);
            if (i < s.size() && s[i] == '"' && (w == "R" || w == "LR" || w == "uR" || w == "UR" || w == "u8R"))
            {
                const size_t e = skip_raw(s, i);
                if (e != npos)
                {
                    t.kind = Kind::Str;
                    i = e;
                }
            }
        }
        else
        {
            t.kind = Kind::Punct;
            i += punct_len(s.substr(i));
        }
        t.end = i;
        out.push_back(t);
    }
    return out;
}

bool is_keyword_before_paren(sv w)
{
    static const std::set<sv> k = {"if", "for", "foreach", "while", "switch", "catch", "using", "lock", "return",
        "throw", "case", "await", "yield", "else", "do", "in", "is", "as"};
    return k.count(w) != 0;
}

// A word that is not the end of an operand: "-" after it is a unary operator.
bool is_operator_word(sv w)
{
    static const std::set<sv> k = {"return", "case", "throw", "in", "new", "await", "yield", "else", "do", "is", "as"};
    return k.count(w) != 0;
}

bool is_body_keyword(sv w)
{
    static const std::set<sv> k = {"if", "for", "foreach", "while", "lock", "using"};
    return k.count(w) != 0;
}

enum class Sp
{
    None,
    One,
    Keep, // the author's choice: one space if the author put white space, else none
    Verbatim // the white space of the author, as it is
};

struct Bracket
{
    char ch = '{';
    int content = 0; // the level of the lines inside
    int base = 0; // the level of the logical line that opened the bracket
    bool ctrl = false; // "(" of if, for, while ...
    bool in_case = false; // "{" with an open "case" label
};

class Formatter
{
public:
    Formatter(sv text, const FormatOptions& o)
        : s_(text)
        , o_(o)
        , t_(lex(text))
    {
        ctrl_close_.assign(t_.size(), false);
    }

    FormatResult run();

private:
    sv txt(size_t i) const { return s_.substr(t_[i].begin, t_[i].end - t_[i].begin); }
    bool is_p(size_t i, sv p) const { return t_[i].kind == Kind::Punct && txt(i) == p; }
    bool is_w(size_t i, sv w) const { return t_[i].kind == Kind::Word && txt(i) == w; }
    bool is_comment(size_t i) const { return t_[i].kind == Kind::LineComment || t_[i].kind == Kind::BlockComment; }
    bool has_gap(size_t i) const { return t_[i].begin > t_[i].gap_begin; }
    bool is_closer(size_t i) const { return is_p(i, ")") || is_p(i, "]") || is_p(i, "}"); }

    bool operand_end(size_t i) const;
    int pm(size_t i) const; // for "+" and "-": 0 unary, 1 binary, 2 not sure
    bool is_prefix(size_t i) const;
    bool is_postfix(size_t i) const;
    bool is_binary(size_t i) const;
    Sp spacing(size_t i) const;
    bool safe_tight(size_t a, size_t b) const;
    bool op_continues(size_t i, size_t p) const; // p: the last token before i that is no comment
    std::string indent(int level) const;

    sv s_;
    FormatOptions o_;
    std::vector<Tok> t_;
    std::vector<Bracket> stack_;
    std::vector<bool> ctrl_close_;
};

bool Formatter::operand_end(size_t i) const
{
    for (int guard = 0; guard < 64; ++guard)
    {
        switch (t_[i].kind)
        {
        case Kind::Word: return !is_operator_word(txt(i));
        case Kind::Str: return true;
        case Kind::Punct:
            if (is_p(i, ")") || is_p(i, "]"))
                return true;
            if ((is_p(i, "++") || is_p(i, "--")) && i > 0)
            {
                --i;
                continue;
            }
            return false;
        default: return false;
        }
    }
    return false;
}

int Formatter::pm(size_t i) const
{
    if (i == 0)
        return 0;
    if (is_p(i - 1, ")"))
        return 2;
    return operand_end(i - 1) ? 1 : 0;
}

bool Formatter::is_prefix(size_t i) const
{
    if (t_[i].kind != Kind::Punct)
        return false;
    if (is_p(i, "~"))
        return true;
    if (is_p(i, "!"))
        return !(i > 0 && operand_end(i - 1) && !has_gap(i));
    if (is_p(i, "++") || is_p(i, "--"))
        return !(i > 0 && operand_end(i - 1));
    if (is_p(i, "+") || is_p(i, "-"))
        return pm(i) == 0;
    return false;
}

bool Formatter::is_postfix(size_t i) const
{
    if (t_[i].kind != Kind::Punct)
        return false;
    if (is_p(i, "!"))
        return i > 0 && operand_end(i - 1) && !has_gap(i);
    if (is_p(i, "++") || is_p(i, "--"))
        return i > 0 && operand_end(i - 1);
    return false;
}

bool Formatter::is_binary(size_t i) const
{
    if (t_[i].kind != Kind::Punct)
        return false;
    static const std::set<sv> ops = {"=", "==", "!=", "<=", ">=", "||", "|", "^", "/", "%", "<<",
        "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", "=>", "??", "?\?="};
    const sv x = txt(i);
    if (ops.count(x))
        return true;
    if (x == "*" || x == "&" || x == "&&")
    {
        // Binary, or else a pointer or a reference of a type ("Vec3* p"). The white space tells.
        if (i == 0 || i + 1 >= t_.size() || !operand_end(i - 1) || has_gap(i) != has_gap(i + 1))
            return false;
        const sv n = txt(i + 1);
        return t_[i + 1].kind == Kind::Word || t_[i + 1].kind == Kind::Str || n == "(" || n == "!" || n == "-"
            || n == "~";
    }
    if (x == "+" || x == "-")
    {
        const int m = pm(i);
        if (m == 1)
            return true;
        if (m == 2)
            return has_gap(i) || (i + 1 < t_.size() && has_gap(i + 1));
    }
    return false;
}

// True when the tokens a and b can touch each other without a change of the lexing.
bool Formatter::safe_tight(size_t a, size_t b) const
{
    if (t_[a].kind == Kind::Punct && t_[b].kind == Kind::Punct)
    {
        const std::string both = std::string(txt(a)) + std::string(txt(b));
        return punct_len(both) == txt(a).size();
    }
    if (t_[a].kind == Kind::Word && t_[b].kind == Kind::Punct && is_word_char(txt(a)[0]) && txt(a)[0] >= '0'
        && txt(a)[0] <= '9')
        return txt(b)[0] != '.';
    return !(t_[a].kind == Kind::Word && t_[b].kind == Kind::Word);
}

// The white space between the token i - 1 and the token i. Both are on one line.
Sp Formatter::spacing(size_t i) const
{
    const size_t a = i - 1, b = i;
    const bool ws = has_gap(b);
    const Kind ka = t_[a].kind, kb = t_[b].kind;
    // Comments keep the white space of the author. A line comment needs one space at least.
    if (ka == Kind::BlockComment || kb == Kind::BlockComment || ka == Kind::LineComment)
        return Sp::Verbatim;
    if (kb == Kind::LineComment)
        return ws ? Sp::Verbatim : Sp::One;

    Sp r = Sp::Keep;
    // The name of an operator method: "operator+(", "operator==(", "operator()(".
    if (is_w(a, "operator") && kb == Kind::Punct)
        r = Sp::None;
    else if (a > 0 && is_w(a - 1, "operator") && ka == Kind::Punct && is_p(b, "("))
        r = Sp::None;
    else if (is_p(b, ",") || is_p(b, ";"))
        r = Sp::None;
    else if (is_p(b, ")") || is_p(b, "]"))
        r = is_p(a, ";") ? Sp::Keep : Sp::None;
    else if (is_p(a, "(") || is_p(a, "["))
        r = Sp::None;
    else if (is_p(a, "{"))
        r = is_p(b, "}") ? Sp::Keep : Sp::One;
    else if (is_p(b, "}"))
        r = Sp::One;
    else if (is_p(b, "{"))
        r = Sp::One;
    else if (is_p(a, ","))
        r = is_p(b, ",") ? Sp::Keep : Sp::One;
    else if (is_p(a, ";"))
        r = Sp::One;
    else if (is_p(b, "(") && ka == Kind::Word)
        r = is_keyword_before_paren(txt(a)) ? Sp::One : Sp::None;
    else if (is_p(b, "(") && (is_p(a, ")") || is_p(a, "]") || is_p(a, ">")))
        r = Sp::Keep;
    else if (is_p(b, "[") && ka == Kind::Word)
        r = is_keyword_before_paren(txt(a)) ? Sp::One : Sp::None;
    else if (is_p(b, "[") && (is_p(a, ")") || is_p(a, "]")))
        r = Sp::Keep;
    else if (is_p(a, ".") || is_p(a, "?.") || is_p(a, "::") || is_p(a, "->"))
        r = txt(b)[0] == '.' ? Sp::Keep : Sp::None;
    else if (is_p(b, ".") || is_p(b, "?.") || is_p(b, "::") || is_p(b, "->"))
        r = Sp::None;
    else if (is_prefix(a))
        r = Sp::None;
    else if (is_postfix(b))
        r = Sp::None;
    else if (is_binary(b) || is_binary(a))
        r = Sp::One;
    else if (is_p(a, "}") && kb == Kind::Word)
        r = Sp::One;
    else if ((ka == Kind::Word || ka == Kind::Str) && (kb == Kind::Word || kb == Kind::Str))
        r = ws ? Sp::One : Sp::Keep; // glued tokens are a prefix or a suffix of a literal
    if (r == Sp::None && !safe_tight(a, b))
        r = Sp::Keep;
    return r;
}

// True when the line that starts at the token i continues an expression of the line before.
bool Formatter::op_continues(size_t i, size_t p) const
{
    if (p == npos || t_[p].kind != Kind::Punct)
        return false;
    static const std::set<sv> ends = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", "?\?=", "+", "-", "/",
        "%", "||", "&&", "|", "^", "==", "!=", "<=", ">=", "<<", "=>", "??", "?", ".", "?."};
    if (ends.count(txt(p)))
        return true;
    static const std::set<sv> opens = {";", "{", "}", ",", "(", "[", ":"};
    if (opens.count(txt(p)) || t_[i].kind != Kind::Punct)
        return false;
    static const std::set<sv> starts = {
        "&&", "||", "?", ".", "?.", "??", "=>", "==", "!=", "<=", ">=", "+", "-", "/", "%", "|", "^", ":"};
    return starts.count(txt(i)) != 0;
}

std::string Formatter::indent(int level) const
{
    if (level <= 0)
        return std::string();
    if (o_.use_tabs)
        return std::string(static_cast<size_t>(level), '\t');
    return std::string(static_cast<size_t>(level) * static_cast<size_t>(std::max(o_.indent_width, 0)), ' ');
}

FormatResult Formatter::run()
{
    FormatResult res;
    size_t in_lines = 1;
    for (char c : s_)
        if (c == '\n')
            ++in_lines;
    res.line_map.assign(in_lines, -1);
    if (t_.empty())
        return res;

    size_t crlf = 0, lf = 0;
    for (size_t k = 0; k < s_.size(); ++k)
        if (s_[k] == '\n')
            (k > 0 && s_[k - 1] == '\r' ? crlf : lf)++;
    const std::string eol = crlf > lf ? "\r\n" : "\n";

    std::string& out = res.text;
    int out_line = 0;
    size_t in_line = 0; // the input line where the next gap starts
    int last_base = 0;
    bool label_line = false; // the line has started with "case" or "default"
    auto count_nl = [](sv x)
    {
        size_t n = 0;
        for (char c : x)
            if (c == '\n')
                ++n;
        return n;
    };

    size_t last_sig = npos; // the last token before i that is no comment
    for (size_t i = 0; i < t_.size(); ++i)
    {
        const Tok& tk = t_[i];
        const bool first = i == 0 || tk.nl > 0;
        if (first)
        {
            // The new lines before the token.
            if (i > 0)
            {
                const size_t keep = std::min<size_t>(tk.nl, 2);
                for (size_t k = 0; k < keep; ++k)
                    out += eol;
                if (keep == 2)
                    res.line_map[in_line + 1] = out_line + 1;
                out_line += static_cast<int>(keep);
            }
            in_line += tk.nl;
            // A label of the previous line that ends with ":" opens the case body.
            if (label_line)
            {
                const size_t p = last_sig;
                if (p != npos && is_p(p, ":") && !stack_.empty() && stack_.back().ch == '{')
                    stack_.back().in_case = true;
            }
            const bool is_label = is_w(i, "case") || (is_w(i, "default") && i + 1 < t_.size() && is_p(i + 1, ":"));
            label_line = is_label;
            if (is_label && !stack_.empty() && stack_.back().ch == '{')
                stack_.back().in_case = false;
            // The brackets that the line closes at its start.
            size_t k = 0;
            while (i + k < t_.size() && (k == 0 || t_[i + k].nl == 0) && is_closer(i + k) && k < stack_.size())
                ++k;
            const Bracket* top = stack_.size() > k ? &stack_[stack_.size() - k - 1] : nullptr;
            int level, base;
            if (top && top->ch != '{')
            {
                level = top->content;
                base = top->base;
            }
            else
            {
                const bool in_case = top && top->in_case && !is_label;
                const size_t p = last_sig;
                bool pending = false;
                if (k == 0 && p != npos && !is_p(i, "{") && !is_comment(i))
                    pending = (is_p(p, ")") && ctrl_close_[p]) || is_w(p, "else") || is_w(p, "do");
                if (k == 0 && (pending || op_continues(i, p)))
                {
                    level = last_base + 1;
                    base = pending ? level : last_base;
                }
                else
                {
                    level = (top ? top->content : 0) + (in_case ? 1 : 0);
                    base = level;
                }
            }
            last_base = base;
            out += indent(level);
        }
        else
        {
            switch (spacing(i))
            {
            case Sp::None: break;
            case Sp::One: out += ' '; break;
            case Sp::Keep:
                if (has_gap(i))
                    out += ' ';
                break;
            case Sp::Verbatim:
            {
                const size_t before = out.size();
                for (char c : s_.substr(tk.gap_begin, tk.begin - tk.gap_begin))
                    if (c != '\r' && c != '\f' && c != '\v')
                        out += c;
                // A gap of a lone CR is white space: it becomes one space.
                if (out.size() == before && has_gap(i))
                    out += ' ';
                break;
            }
            }
        }
        // The token.
        const size_t lines_in = count_nl(txt(i));
        if (first)
            res.line_map[in_line] = out_line;
        out += txt(i);
        for (size_t k = 1; k <= lines_in; ++k)
            res.line_map[in_line + k] = out_line + static_cast<int>(k);
        in_line += lines_in;
        out_line += static_cast<int>(lines_in);
        // The brackets of the token.
        if (tk.kind == Kind::Punct)
        {
            const sv x = txt(i);
            // The level of the logical line that holds the token.
            if (x == "{" || x == "(" || x == "[")
            {
                Bracket b;
                b.ch = x[0];
                b.base = last_base;
                b.content = last_base + 1;
                if (x == "(")
                {
                    const size_t p = last_sig;
                    b.ctrl = p != npos && t_[p].kind == Kind::Word && is_body_keyword(txt(p));
                }
                stack_.push_back(b);
            }
            else if (x == ")" || x == "]" || x == "}")
            {
                if (!stack_.empty())
                {
                    if (x == ")")
                        ctrl_close_[i] = stack_.back().ctrl;
                    stack_.pop_back();
                }
            }
        }
        if (!is_comment(i))
            last_sig = i;
    }
    out += eol;
    return res;
}

} // namespace

FormatResult format_hot_map(std::string_view text, const FormatOptions& options)
{
    FormatResult r = Formatter(text, options).run();
    // A safety net. The white space of a broken text (an unterminated "${" for example) can change
    // how the lexer reads the text. If the tokens of the output differ, the text stays as it is.
    // This keeps the format idempotent and token preserving for all texts.
    const std::vector<Tok> a = lex(text), b = lex(r.text);
    bool same = a.size() == b.size();
    for (size_t i = 0; same && i < a.size(); ++i)
        same = text.substr(a[i].begin, a[i].end - a[i].begin)
            == std::string_view(r.text).substr(b[i].begin, b[i].end - b[i].begin);
    if (!same)
    {
        r.text = std::string(text);
        for (size_t i = 0; i < r.line_map.size(); ++i)
            r.line_map[i] = static_cast<int>(i);
    }
    return r;
}

std::vector<std::string> format_tokens(std::string_view text)
{
    std::vector<std::string> out;
    for (const Tok& t : lex(text))
        out.emplace_back(text.substr(t.begin, t.end - t.begin));
    return out;
}

std::string format_hot(std::string_view text, const FormatOptions& options)
{
    return format_hot_map(text, options).text;
}

} // namespace hot::ls
