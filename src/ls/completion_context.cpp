// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "completion_context.h"

#include <algorithm>
#include <string>
#include <vector>

namespace hot::ls
{
namespace
{

using Kind = CompletionContext::Kind;

enum class Fr
{
    Top,
    Namespace,
    Class,
    Enum,
    Block,
    Params,   // the parameter list of a declaration
    Call,     // the arguments of a call
    Group,    // a "(" that is not a call: a group or a cast
    ForParen, // "(" after "for", "foreach" or "using" in a block
    ExprParen, // "(" after "if", "while", "switch", "return" and similar words
    Catch,
    AttrArg,
    Attr,     // "[" at the start of a declaration
    Index,
    Angle,    // a template argument list
};

struct Tok
{
    enum T { Ident, Punct, Str, Num } t;
    std::string_view s;
    std::size_t end;
};

struct Frame
{
    Fr k;
    std::vector<Tok> toks; // the tokens of the current statement in this frame
    bool forDecl = true;
    bool capture = false; // an Index frame that is the capture list of a lambda
};

bool isIdentStart(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (static_cast<unsigned char>(c) >= 0x80);
}

bool isIdentChar(char c)
{
    return isIdentStart(c) || (c >= '0' && c <= '9');
}

bool isDecl(Fr k)
{
    return k == Fr::Top || k == Fr::Namespace || k == Fr::Class;
}

bool oneOf(std::string_view s, std::initializer_list<std::string_view> list)
{
    return std::find(list.begin(), list.end(), s) != list.end();
}

bool isModifier(std::string_view s)
{
    return oneOf(s, {"public", "private", "protected", "internal", "static", "const", "virtual", "abstract",
                     "sealed", "new", "async", "extern", "readonly", "ref", "out", "in", "unsafe", "partial",
                     "volatile", "override"});
}

// Words that never name a type before "<".
bool isStatementWord(std::string_view s)
{
    return oneOf(s, {"using", "return", "if", "while", "for", "foreach", "switch", "case", "else", "throw", "do",
                     "await", "in", "is", "as", "new", "override", "catch"});
}

bool is(const Tok& t, Tok::T kind, std::string_view s)
{
    return t.t == kind && t.s == s;
}

bool isWord(const Tok& t, std::string_view s)
{
    return is(t, Tok::Ident, s);
}

bool isPunct(const Tok& t, std::string_view s)
{
    return is(t, Tok::Punct, s);
}

// A token that can be a part of a type name.
bool isTypeish(const Tok& t)
{
    if (t.t == Tok::Ident)
        return true;
    return t.t == Tok::Punct && oneOf(t.s, {"::", ".", "<>", "?", "*", "&"});
}

bool allTypeish(const std::vector<Tok>& toks)
{
    return std::all_of(toks.begin(), toks.end(), isTypeish);
}

bool allModifiers(const std::vector<Tok>& toks)
{
    return std::all_of(toks.begin(), toks.end(), [](const Tok& t) { return t.t == Tok::Ident && isModifier(t.s); });
}

bool hasPunct(const std::vector<Tok>& toks, std::string_view s)
{
    return std::any_of(toks.begin(), toks.end(), [&](const Tok& t) { return isPunct(t, s); });
}

bool hasWord(const std::vector<Tok>& toks, std::string_view s)
{
    return std::any_of(toks.begin(), toks.end(), [&](const Tok& t) { return isWord(t, s); });
}

bool isClassWord(std::string_view s)
{
    return s == "class" || s == "struct" || s == "interface";
}

bool hasClassWord(const std::vector<Tok>& toks)
{
    return std::any_of(toks.begin(), toks.end(), [](const Tok& t) { return t.t == Tok::Ident && isClassWord(t.s); });
}

// A punct that starts or continues an expression.
bool isOperator(const Tok& t)
{
    return t.t == Tok::Punct && !isTypeish(t) && !oneOf(t.s, {",", ")", "]", "$"});
}

// True when the last token ends a position that takes an expression. The tokens "*", "&" and "?"
// are binary or ternary operators in an expression and a part of a type in a declaration. The
// tokens before them decide: an operator, a call or a group before them means an expression.
bool exprAfter(const std::vector<Tok>& toks, Fr k)
{
    if (toks.empty())
        return false;
    const Tok& last = toks.back();
    if (isOperator(last))
        return true;
    if (last.t != Tok::Punct || !oneOf(last.s, {"*", "&", "?"}))
        return false;
    if (toks.size() == 1)
        return k == Fr::Block;
    const Tok& prev = toks[toks.size() - 2];
    const bool nameLike = prev.t != Tok::Punct || oneOf(prev.s, {")", "]", "<>"});
    if (!nameLike)
        return true;
    if (k == Fr::Group || k == Fr::ExprParen || k == Fr::Call)
        return true;
    if (toks[0].t == Tok::Ident && isStatementWord(toks[0].s))
        return true;
    for (std::size_t i = 0; i + 1 < toks.size(); ++i)
    {
        const Tok& t = toks[i];
        if (isOperator(t) || t.t == Tok::Num || t.t == Tok::Str || isPunct(t, ")"))
            return true;
    }
    return false;
}

struct Scanner
{
    std::string_view text;
    std::vector<Frame> stack;
    std::string_view lastSig; // the last token of any kind, also ";" "{" and ")"
    std::string_view prevSig; // the token before lastSig

    void sig(std::string_view s)
    {
        prevSig = lastSig;
        lastSig = s;
    }

    Frame& cur() { return stack.back(); }

    void push(Fr k) { stack.push_back(Frame{k, {}, true}); }

    void popAngles()
    {
        while (stack.size() > 1 && cur().k == Fr::Angle)
            stack.pop_back();
    }

    void add(Tok::T t, std::string_view s, std::size_t end) { cur().toks.push_back(Tok{t, s, end}); }

    bool plausibleAngle()
    {
        const auto& toks = cur().toks;
        if (toks.empty() || toks.back().t != Tok::Ident || isStatementWord(toks.back().s))
            return false;
        Fr k = cur().k;
        if (k == Fr::ExprParen || k == Fr::Attr || k == Fr::Index || k == Fr::Call || k == Fr::AttrArg
            || k == Fr::Enum)
            return false;
        if (toks.size() >= 2)
        {
            const Tok& p = toks[toks.size() - 2];
            if (oneOf(p.s, {"new", "as", "is", "$"}) && p.t != Tok::Str)
                return true;
        }
        if (!toks.empty() && isWord(toks[0], "using"))
            return toks.size() >= 3 && isPunct(toks[toks.size() - 2], "=") ? true : hasPunct(toks, "=");
        if (hasClassWord(toks) || (isDecl(k) && hasWord(toks, "enum")))
            return true;
        if (!allTypeish(toks))
            return false;
        // In an expression, "a < b" is common. A type name starts with a capital letter.
        if (k == Fr::Block || k == Fr::Group || k == Fr::ForParen)
        {
            char c = toks.back().s.empty() ? 'a' : toks.back().s[0];
            return c >= 'A' && c <= 'Z';
        }
        return true;
    }

    Fr braceKind()
    {
        const auto& toks = cur().toks;
        if (isDecl(cur().k) || cur().k == Fr::Block)
        {
            if (hasClassWord(toks) && isDecl(cur().k))
                return Fr::Class;
            if (hasWord(toks, "enum") && isDecl(cur().k))
                return Fr::Enum;
            if (hasWord(toks, "namespace") && isDecl(cur().k))
                return Fr::Namespace;
        }
        return Fr::Block;
    }

    Fr parenKind()
    {
        const auto& toks = cur().toks;
        Fr k = cur().k;
        if (!toks.empty() && isWord(toks.back(), "catch"))
            return Fr::Catch;
        if (k == Fr::Attr)
            return Fr::AttrArg;
        if (!toks.empty() && isPunct(toks.back(), "cap]"))
            return Fr::Params; // the parameters of a lambda
        const bool prevName = !toks.empty() && toks.back().t == Tok::Ident;
        if (prevName && isStatementWord(toks.back().s) && k != Fr::Top)
        {
            if (oneOf(toks.back().s, {"for", "foreach", "using"}))
                return Fr::ForParen;
            return Fr::ExprParen;
        }
        if (prevName && oneOf(toks.back().s, {"if", "while", "switch", "lock", "return", "typeof", "sizeof"}))
            return Fr::ExprParen;
        if (prevName && isDecl(k) && !hasPunct(toks, "="))
            return Fr::Params;
        if (!toks.empty()
            && (prevName || isPunct(toks.back(), ")") || isPunct(toks.back(), "]") || isPunct(toks.back(), "<>")))
            return Fr::Call;
        return Fr::Group;
    }

    // Reads the text up to "end". It returns false when the end is in a comment or a string.
    // "usingString" is true when the end is in the string after "using".
    bool scan(std::size_t end, bool& usingString)
    {
        usingString = false;
        std::size_t i = 0;
        push(Fr::Top);
        while (i < end)
        {
            const char c = text[i];
            const char n = i + 1 < end ? text[i + 1] : '\0';
            if (c == '/' && n == '/')
            {
                while (i < end && text[i] != '\n')
                    ++i;
                if (i >= end)
                    return false;
                continue;
            }
            if (c == '/' && n == '*')
            {
                i += 2;
                while (i < end && !(text[i] == '*' && i + 1 < end && text[i + 1] == '/'))
                    ++i;
                if (i >= end)
                    return false;
                i += 2;
                if (i > end)
                    return false;
                continue;
            }
            if (c == '"' || c == '\'')
            {
                const std::size_t start = i++;
                while (i < end && text[i] != c)
                {
                    if (text[i] == '\\')
                        ++i;
                    else if (text[i] == '\n' && c == '\'')
                        break;
                    ++i;
                }
                if (i >= end)
                {
                    const auto& toks = cur().toks;
                    usingString = c == '"' && isDecl(cur().k) && toks.size() == 1 && isWord(toks[0], "using");
                    return false;
                }
                ++i;
                add(Tok::Str, text.substr(start, i - start), i);
                sig(text.substr(start, i - start));
                continue;
            }
            if (isIdentStart(c))
            {
                const std::size_t start = i;
                while (i < end && isIdentChar(text[i]))
                    ++i;
                add(Tok::Ident, text.substr(start, i - start), i);
                sig(text.substr(start, i - start));
                continue;
            }
            if (c >= '0' && c <= '9')
            {
                const std::size_t start = i;
                while (i < end && (isIdentChar(text[i]) || (text[i] == '.' && i + 1 < end && text[i + 1] >= '0' && text[i + 1] <= '9')))
                    ++i;
                add(Tok::Num, text.substr(start, i - start), i);
                sig(text.substr(start, i - start));
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
                ++i;
                continue;
            }
            const std::size_t punctStart = i;
            punct(i, end);
            sig(text.substr(punctStart, i - punctStart));
        }
        return true;
    }

    void punct(std::size_t& i, std::size_t end)
    {
        const char c = text[i];
        const char n = i + 1 < end ? text[i + 1] : '\0';
        // The characters that end a template list that is not one.
        if (cur().k == Fr::Angle && std::string_view(";{})]=&|!+-/%^").find(c) != std::string_view::npos
            && !(c == '&' && n != '&') && !(c == '-' && n == '>'))
            popAngles();
        switch (c)
        {
        case ';':
            cur().toks.clear();
            cur().forDecl = false;
            ++i;
            return;
        case ',':
            if (!isDecl(cur().k) && cur().k != Fr::Block)
                cur().toks.clear();
            else
                add(Tok::Punct, text.substr(i, 1), i + 1);
            ++i;
            return;
        case '{':
        {
            const Fr k = braceKind();
            push(k);
            ++i;
            return;
        }
        case '}':
            while (stack.size() > 1 && !isBrace(cur().k))
                stack.pop_back();
            if (stack.size() > 1)
                stack.pop_back();
            cur().toks.clear();
            ++i;
            return;
        case '(':
            push(parenKind());
            ++i;
            return;
        case ')':
            while (stack.size() > 1 && !isParen(cur().k) && !isBrace(cur().k))
                stack.pop_back();
            if (stack.size() > 1 && isParen(cur().k))
            {
                stack.pop_back();
                add(Tok::Punct, text.substr(i, 1), i + 1);
            }
            ++i;
            return;
        case '[':
        {
            const auto& toks = cur().toks;
            const bool declLike = isDecl(cur().k) || cur().k == Fr::Params;
            const bool capture = !declLike
                && (toks.empty() || isOperator(toks.back()) || isPunct(toks.back(), ",")
                    || (toks.back().t == Tok::Ident && isStatementWord(toks.back().s)));
            push(declLike && (toks.empty() || allModifiers(toks)) ? Fr::Attr : Fr::Index);
            cur().capture = capture;
            ++i;
            return;
        }
        case ']':
            while (stack.size() > 1 && cur().k != Fr::Attr && cur().k != Fr::Index && !isBrace(cur().k))
                stack.pop_back();
            if (stack.size() > 1 && (cur().k == Fr::Attr || cur().k == Fr::Index))
            {
                const bool attr = cur().k == Fr::Attr;
                const bool capture = cur().capture;
                stack.pop_back();
                if (!attr)
                    add(Tok::Punct, capture ? std::string_view("cap]") : text.substr(i, 1), i + 1);
            }
            ++i;
            return;
        case '<':
            if (n != '=' && n != '<' && plausibleAngle())
            {
                push(Fr::Angle);
                ++i;
                return;
            }
            break;
        case '>':
            if (cur().k == Fr::Angle && n != '=')
            {
                stack.pop_back();
                add(Tok::Punct, "<>", i + 1);
                ++i;
                return;
            }
            break;
        default:
            break;
        }
        // The two-character operators. "::" and "->" and "=>" matter for the rules.
        if ((c == ':' && n == ':') || (c == '-' && n == '>') || (c == '=' && (n == '=' || n == '>'))
            || (c == '!' && n == '=') || (c == '<' && (n == '=' || n == '<')) || (c == '>' && (n == '=' || n == '>'))
            || (c == '&' && n == '&') || (c == '|' && n == '|'))
        {
            add(Tok::Punct, text.substr(i, 2), i + 2);
            i += 2;
            return;
        }
        add(Tok::Punct, text.substr(i, 1), i + 1);
        ++i;
    }

    static bool isBrace(Fr k) { return k == Fr::Class || k == Fr::Enum || k == Fr::Namespace || k == Fr::Block; }
    static bool isParen(Fr k)
    {
        return k == Fr::Params || k == Fr::Call || k == Fr::Group || k == Fr::ForParen || k == Fr::ExprParen
            || k == Fr::Catch || k == Fr::AttrArg;
    }
};

// The kind for the statement in a declaration frame or a parameter list.
Kind classifyDecl(const std::vector<Tok>& toks)
{
    if (allModifiers(toks))
        return Kind::Type;
    const Tok& last = toks.back();
    if (hasWord(toks, "where") && isPunct(last, ":"))
        return Kind::Type;
    if (hasWord(toks, "enum") && isPunct(last, ":"))
        return Kind::Type;
    if (hasClassWord(toks))
    {
        if (hasPunct(toks, ":") && (isPunct(last, ":") || isPunct(last, ",")))
            return Kind::Base;
        return Kind::None;
    }
    if (exprAfter(toks, Fr::Class))
        return Kind::Expr;
    return Kind::None;
}

Kind classifyTop(const Frame& f, bool partial, bool& both)
{
    const auto& toks = f.toks;
    switch (f.k)
    {
    case Fr::Angle:
        return allModifiers(toks) ? Kind::Type : Kind::None;
    case Fr::Attr:
        return toks.empty() ? Kind::Attribute : Kind::None;
    case Fr::AttrArg:
        return Kind::AttributeArg;
    case Fr::Catch:
        return allModifiers(toks) ? Kind::Type : Kind::None;
    case Fr::Call:
    case Fr::Index:
    case Fr::ExprParen:
        return Kind::Expr;
    case Fr::ForParen:
        if (!f.forDecl)
            return Kind::Expr;
        if (!toks.empty() && isWord(toks.back(), "in") && toks.size() >= 2)
            return Kind::Expr;
        if (allModifiers(toks))
        {
            both = true;
            return Kind::Type;
        }
        return exprAfter(toks, Fr::ForParen) ? Kind::Expr : Kind::None;
    case Fr::Group:
        if (toks.empty())
        {
            both = true;
            return partial ? Kind::Type : Kind::Expr;
        }
        return exprAfter(toks, Fr::Group) ? Kind::Expr : Kind::None;
    case Fr::Enum:
        if (toks.empty())
            return Kind::EnumBody;
        return exprAfter(toks, Fr::Enum) ? Kind::Expr : Kind::None;
    case Fr::Block:
        if (toks.empty())
        {
            both = true;
            return Kind::Expr;
        }
        if (toks.back().t == Tok::Ident)
            return oneOf(toks.back().s, {"return", "else", "case", "throw", "await", "do", "in", "yield"})
                ? Kind::Expr
                : Kind::None;
        return exprAfter(toks, Fr::Block) || isPunct(toks.back(), ",") || isPunct(toks.back(), ")") ? Kind::Expr
                                                                                                    : Kind::None;
    case Fr::Top:
    case Fr::Namespace:
    case Fr::Class:
    case Fr::Params:
        break;
    }
    if (!toks.empty() && isWord(toks[0], "using") && f.k != Fr::Params)
    {
        if (!hasPunct(toks, "="))
            return Kind::Using;
        return isPunct(toks.back(), "=") ? Kind::Type : Kind::None;
    }
    if (toks.empty())
        return Kind::Type;
    return classifyDecl(toks);
}

} // namespace

const char* kindName(CompletionContext::Kind kind)
{
    switch (kind)
    {
    case Kind::None: return "None";
    case Kind::Type: return "Type";
    case Kind::Base: return "Base";
    case Kind::Attribute: return "Attribute";
    case Kind::AttributeArg: return "AttributeArg";
    case Kind::Using: return "Using";
    case Kind::Override: return "Override";
    case Kind::EnumBody: return "EnumBody";
    case Kind::Expr: return "Expr";
    case Kind::Member: return "Member";
    }
    return "?";
}

CompletionContext classify(std::string_view text, std::size_t offset)
{
    offset = std::min(offset, text.size());
    CompletionContext result;
    result.prefixStart = offset;

    Scanner sc{text, {}};
    bool usingString = false;
    if (!sc.scan(offset, usingString))
    {
        result.kind = usingString ? Kind::Using : Kind::None;
        return result;
    }

    // The identifier under the cursor is a prefix. It is not a token of the statement.
    bool partial = false;
    auto& toks = sc.cur().toks;
    if (!toks.empty() && toks.back().t == Tok::Ident && toks.back().end == offset)
    {
        partial = true;
        result.prefixStart = offset - toks.back().s.size();
        toks.pop_back();
    }
    const Frame& f = sc.cur();

    // "using" in a declaration takes a namespace or a path, so "." and "::" are part of it.
    const bool usingDecl = !toks.empty() && isWord(toks[0], "using") && isDecl(f.k) && !hasPunct(toks, "=");
    if (usingDecl)
    {
        result.kind = Kind::Using;
        return result;
    }

    // A member access: the token before the prefix is "." or "::" or "->". A comment between
    // them is not a token, so the scanner tells the real token.
    const std::string_view before = partial ? sc.prevSig : sc.lastSig;
    if (before == "." || before == "::" || before == "->")
    {
        result.kind = Kind::Member;
        return result;
    }

    if (!toks.empty())
    {
        const Tok& last = toks.back();
        if (isWord(last, "override"))
        {
            result.kind = Kind::Override;
            return result;
        }
        if (isWord(last, "as") || isWord(last, "is") || isWord(last, "new") || isPunct(last, "$"))
        {
            result.kind = Kind::Type;
            return result;
        }
    }
    result.kind = classifyTop(f, partial, result.exprAndType);
    return result;
}

} // namespace hot::ls
