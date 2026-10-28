// spc_lib: the translator from script text to the text of a generation .cpp file.
//
// The translator has three parts.
//   1. A lexer and a parser for the structure of the script: imports, classes,
//      fields, overrides and fns. The parser keeps the tokens of each body.
//   2. A query to libclang (C API). It reads the virtual methods of the native
//      base class. The translator resolves script names only. Clang resolves
//      all native names when it compiles the generated file.
//   3. An emitter. It writes the shell, the state struct, the reflection table,
//      the generation functions, the thunks and the record. Each body is a
//      token-level rewrite of the script tokens. A "#line" directive before
//      each line makes the Clang diagnostics point to the script.
//
// The function spc::translate takes strings and returns strings. It does not
// write a file. libclang reads the imported headers from the include dirs.
// The command line tool spc (translator/spc.cpp) is a thin file wrapper.
//
// The slot file keeps the slot order of the script fns stable between the
// generations. Native virtuals come first, in the order of the base class.
// The script fns follow. A fn keeps its slot in all later generations.
#include "spc_lib.h"

#include <clang-c/Index.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace {

// State of the call in progress. Each thread has its own copy.
thread_local std::string g_script;  // the script name, as given by the caller
thread_local std::string g_diag;    // the diagnostics text

struct TranslateAbort {};

[[noreturn]] void fail(int line, const std::string& msg)
{
    if (line > 0)
        g_diag += g_script + ":" + std::to_string(line) + ": error: " + msg + "\n";
    else
        g_diag += "spc: error: " + msg + "\n";
    throw TranslateAbort{};
}

// ------------------------------------------------------------------ lexer
enum class K { Ident, Number, String, Char, Punct };

struct Tok {
    K kind;
    std::string text;
    int line;
    bool space;  // white space before the token in the script
};

bool is_id0(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool is_id(char c) { return is_id0(c) || std::isdigit(static_cast<unsigned char>(c)); }

std::vector<Tok> lex(const std::string& s)
{
    static const char* const p3[] = {"<<=", ">>=", "...", nullptr};
    static const char* const p2[] = {"::", "->", "++", "--", "+=", "-=", "*=", "/=", "%=", "==",
                                     "!=", "<=", ">=", "&&", "||", "<<", ">>", "&=", "|=", "^=",
                                     "=>", nullptr};
    std::vector<Tok> out;
    size_t i = 0, n = s.size();
    int line = 1;
    bool space = false;
    while (i < n) {
        char c = s[i];
        if (c == '\n') {
            ++line, ++i, space = true;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i, space = true;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') ++i;
            space = true;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            int start = line;
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) {
                if (s[i] == '\n') ++line;
                ++i;
            }
            if (i + 1 >= n) fail(start, "unterminated comment");
            i += 2;
            space = true;
            continue;
        }
        Tok t{K::Punct, "", line, space};
        space = false;
        size_t b = i;
        bool prev_operand =
            !out.empty() && (out.back().kind == K::Ident || out.back().kind == K::Number ||
                             out.back().text == ")" || out.back().text == "]");
        if (is_id0(c)) {
            while (i < n && is_id(s[i])) ++i;
            t.kind = K::Ident;
        } else if (std::isdigit(static_cast<unsigned char>(c)) ||
                   (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(s[i + 1])) &&
                    !prev_operand)) {
            bool hex = c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X');
            while (i < n) {
                char d = s[i];
                if (is_id(d) || d == '.') {
                    ++i;
                } else if ((d == '+' || d == '-') && !hex && (s[i - 1] == 'e' || s[i - 1] == 'E')) {
                    ++i;
                } else {
                    break;
                }
            }
            t.kind = K::Number;
        } else if (c == '"' || c == '\'') {
            ++i;
            while (i < n && s[i] != c) {
                if (s[i] == '\n') fail(line, "unterminated string or character literal");
                if (s[i] == '\\') ++i;
                ++i;
            }
            if (i >= n) fail(line, "unterminated string or character literal");
            ++i;
            t.kind = c == '"' ? K::String : K::Char;
        } else if (c == '`') {
            fail(line, "string interpolation is not supported");
        } else {
            size_t len = 1;
            for (int k = 0; p3[k]; ++k)
                if (s.compare(i, 3, p3[k]) == 0) len = 3;
            if (len == 1)
                for (int k = 0; p2[k]; ++k)
                    if (s.compare(i, 2, p2[k]) == 0) len = 2;
            i += len;
        }
        t.text = s.substr(b, i - b);
        out.push_back(t);
    }
    return out;
}

// -------------------------------------------------------------- script AST
struct Param {
    std::string name, type;
};

struct Field {
    std::string name, was, type;
    std::vector<Tok> init;
    bool has_init = false;
    int line = 0;
};

struct Method {
    bool is_override = false;
    std::string name;
    std::vector<Param> params;
    std::string ret;  // empty: not written
    std::vector<Tok> body;
    int line = 0, end_line = 0;
};

struct ClassDef {
    std::string name, base;
    std::vector<Field> fields;
    std::vector<Method> methods;
    int line = 0;
};

struct Import {
    std::string path;  // the header name; a system header keeps its angle brackets
    int line;
    // The text after #include: <vector> for a system header, "Actor.h" for the others.
    std::string target() const
    {
        return !path.empty() && path.front() == '<' ? path : "\"" + path + "\"";
    }
};

struct Script {
    std::vector<Import> imports;
    std::vector<ClassDef> classes;
};

bool is_word(const Tok& t) { return t.kind == K::Ident || t.kind == K::Number; }

// Joins type tokens into C++ text.
std::string join_type(const std::vector<Tok>& v)
{
    std::string r;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i && ((is_word(v[i - 1]) && is_word(v[i])) || v[i - 1].text == ",")) r += ' ';
        r += v[i].text;
    }
    return r;
}

class Parser {
public:
    explicit Parser(std::vector<Tok> t) : t_(std::move(t)) {}

    Script parse()
    {
        Script sc;
        while (!eof()) {
            if (is("import")) {
                int line = cur().line;
                ++p_;
                if (eof()) fail(line, "import needs a header name");
                std::string path;
                if (cur().kind == K::String) {
                    path = unquote(cur().text);  // "Actor.h" or "<vector>"
                    ++p_;
                } else if (is("<")) {
                    // import <vector>;  The lexer splits the name into tokens.
                    path = "<";
                    ++p_;
                    while (!is(">")) {
                        if (eof() || cur().line != line) fail(line, "import <...> needs a closing '>'");
                        path += t_[p_++].text;
                    }
                    ++p_;
                    path += ">";
                } else {
                    fail(line, "import needs a string or <header>");
                }
                if (path.size() > 2 && path.front() == '<' && path.back() != '>')
                    fail(line, "import <...> needs a closing '>'");
                if (path == "<>" || path.empty()) fail(line, "import needs a header name");
                sc.imports.push_back({path, line});
                expect(";");
            } else if (is("class")) {
                sc.classes.push_back(parse_class());
            } else {
                fail(cur().line, "expected 'import' or 'class', found '" + cur().text + "'");
            }
        }
        return sc;
    }

private:
    std::vector<Tok> t_;
    size_t p_ = 0;

    bool eof() const { return p_ >= t_.size(); }
    const Tok& cur() const
    {
        if (eof()) fail(t_.empty() ? 0 : t_.back().line, "unexpected end of file");
        return t_[p_];
    }
    bool is(const char* s) const { return !eof() && t_[p_].text == s && t_[p_].kind != K::String; }
    void expect(const char* s)
    {
        if (!is(s)) fail(eof() ? t_.back().line : cur().line, std::string("expected '") + s + "'");
        ++p_;
    }
    std::string ident(const char* what)
    {
        if (cur().kind != K::Ident) fail(cur().line, std::string("expected ") + what);
        return t_[p_++].text;
    }
    static std::string unquote(const std::string& s) { return s.substr(1, s.size() - 2); }

    // Reads type tokens up to a terminator at depth zero.
    std::string type_until(std::initializer_list<const char*> terms)
    {
        std::vector<Tok> v;
        int angle = 0, paren = 0;
        while (true) {
            const Tok& t = cur();
            if (angle == 0 && paren == 0) {
                bool stop = false;
                for (const char* x : terms)
                    if (t.text == x && t.kind == K::Punct) stop = true;
                if (stop) break;
            }
            if (t.text == "<") ++angle;
            else if (t.text == ">") --angle;
            else if (t.text == ">>") angle -= 2;
            else if (t.text == "(" || t.text == "[") ++paren;
            else if (t.text == ")" || t.text == "]") --paren;
            v.push_back(t);
            ++p_;
        }
        if (v.empty()) fail(cur().line, "expected a type");
        return join_type(v);
    }

    std::vector<Param> parse_params()
    {
        std::vector<Param> ps;
        expect("(");
        while (!is(")")) {
            Param p;
            p.name = ident("a parameter name");
            expect(":");
            p.type = type_until({",", ")"});
            ps.push_back(p);
            if (is(",")) ++p_;
        }
        expect(")");
        return ps;
    }

    std::vector<Tok> parse_block(int& end_line)
    {
        expect("{");
        std::vector<Tok> body;
        int depth = 1;
        while (true) {
            const Tok& t = cur();
            if (t.kind == K::Punct && t.text == "{") ++depth;
            if (t.kind == K::Punct && t.text == "}" && --depth == 0) {
                end_line = t.line;
                ++p_;
                break;
            }
            body.push_back(t);
            ++p_;
        }
        return body;
    }

    ClassDef parse_class()
    {
        ClassDef c;
        c.line = cur().line;
        ++p_;
        c.name = ident("a class name");
        expect(":");
        c.base = ident("a base class name");
        while (is("::")) {
            ++p_;
            c.base += "::" + ident("a name");
        }
        expect("{");
        while (!is("}")) {
            int line = cur().line;
            std::string was;
            if (is("@")) {
                ++p_;
                if (ident("an annotation name") != "was") fail(line, "unknown annotation");
                expect("(");
                if (cur().kind != K::String) fail(line, "@was needs a string");
                was = unquote(cur().text);
                ++p_;
                expect(")");
            }
            if (is("override") || is("fn")) {
                if (!was.empty()) fail(line, "@was applies to fields only");
                Method m;
                m.is_override = is("override");
                m.line = cur().line;
                ++p_;
                m.name = ident("a method name");
                m.params = parse_params();
                if (is("->")) {
                    ++p_;
                    m.ret = type_until({"{"});
                }
                m.body = parse_block(m.end_line);
                c.methods.push_back(std::move(m));
            } else if (cur().kind == K::Ident && p_ + 1 < t_.size() && t_[p_ + 1].text == ":") {
                Field f;
                f.line = cur().line;
                f.was = was;
                f.name = ident("a field name");
                expect(":");
                f.type = type_until({"=", ";"});
                if (is("=")) {
                    ++p_;
                    f.has_init = true;
                    int depth = 0;
                    while (!(depth == 0 && is(";"))) {
                        const Tok& t = cur();
                        if (t.text == "(" || t.text == "{" || t.text == "[") ++depth;
                        if (t.text == ")" || t.text == "}" || t.text == "]") --depth;
                        f.init.push_back(t);
                        ++p_;
                    }
                    if (f.init.empty()) fail(f.line, "expected an initializer");
                }
                expect(";");
                c.fields.push_back(std::move(f));
            } else {
                fail(line, "expected a field, 'override' or 'fn', found '" + cur().text + "'");
            }
        }
        expect("}");
        return c;
    }
};

// ----------------------------------------------------------------- libclang
std::string cxs(CXString s)
{
    std::string r = clang_getCString(s) ? clang_getCString(s) : "";
    clang_disposeString(s);
    return r;
}

template <class F>
void visit(CXCursor c, F&& f)
{
    auto tramp = [](CXCursor cur, CXCursor, CXClientData d) -> CXChildVisitResult {
        return (*static_cast<std::remove_reference_t<F>*>(d))(cur);
    };
    clang_visitChildren(c, tramp, &f);
}

// Removes the space that libclang puts before '&' and '*'.
std::string tidy(std::string s)
{
    for (const char* q : {" &", " *"}) {
        for (size_t p; (p = s.find(q)) != std::string::npos;) s.erase(p, 1);
    }
    return s;
}

std::string norm(const std::string& s)
{
    std::string r;
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c))) r += c;
    return r;
}

struct NativeVirtual {
    std::string name, ret, owner;
    std::vector<std::string> ptypes;
    bool is_const = false, pure = false, final_ = false, no_except = false;
    std::string key() const
    {
        std::string k = name + "(";
        for (const auto& p : ptypes) k += norm(p) + ",";
        return k + ")" + (is_const ? "const" : "");
    }
    std::string text() const
    {
        std::string k = name + "(";
        for (size_t i = 0; i < ptypes.size(); ++i) k += (i ? ", " : "") + ptypes[i];
        return k + ")";
    }
};

struct BaseInfo {
    std::string name;
    std::vector<NativeVirtual> virtuals;
    bool virtual_dtor = false;
    int ctor_kind = 0;  // the constructor argument: 0 none, 1 std::string, 2 const char*
    bool default_ctor = false;
    bool any_ctor = false;  // the class declares a constructor
};

void collect_virtuals(CXCursor cls, BaseInfo& info)
{
    std::string owner = cxs(clang_getCursorSpelling(cls));
    visit(cls, [&](CXCursor c) {
        CXCursorKind k = clang_getCursorKind(c);
        if (k == CXCursor_CXXBaseSpecifier) {
            CXCursor def = clang_getCursorDefinition(clang_getCursorReferenced(c));
            if (!clang_Cursor_isNull(def) && clang_getCursorKind(def) != CXCursor_NoDeclFound)
                collect_virtuals(def, info);
        } else if (k == CXCursor_Destructor) {
            if (clang_CXXMethod_isVirtual(c)) info.virtual_dtor = true;
        } else if (k == CXCursor_CXXMethod && clang_CXXMethod_isVirtual(c) &&
                   clang_getCXXAccessSpecifier(c) != CX_CXXPrivate) {
            NativeVirtual v;
            v.name = cxs(clang_getCursorSpelling(c));
            v.owner = owner;
            v.ret = tidy(cxs(clang_getTypeSpelling(clang_getResultType(clang_getCursorType(c)))));
            int n = clang_Cursor_getNumArguments(c);
            for (int i = 0; i < n; ++i)
                v.ptypes.push_back(tidy(
                    cxs(clang_getTypeSpelling(clang_getCursorType(clang_Cursor_getArgument(c, i))))));
            v.is_const = clang_CXXMethod_isConst(c);
            v.pure = clang_CXXMethod_isPureVirtual(c);
            auto es = clang_getCursorExceptionSpecificationType(c);
            v.no_except = es == CXCursor_ExceptionSpecificationKind_BasicNoexcept ||
                          es == CXCursor_ExceptionSpecificationKind_DynamicNone ||
                          es == CXCursor_ExceptionSpecificationKind_NoThrow;
            visit(c, [&](CXCursor a) {
                if (clang_getCursorKind(a) == CXCursor_CXXFinalAttr) v.final_ = true;
                return CXChildVisit_Continue;
            });
            auto it = std::find_if(info.virtuals.begin(), info.virtuals.end(),
                                   [&](const NativeVirtual& o) { return o.key() == v.key(); });
            if (it != info.virtuals.end())
                *it = v;  // an override in a derived native class: same position
            else
                info.virtuals.push_back(v);
        }
        return CXChildVisit_Continue;
    });
}

void collect_ctors(CXCursor cls, BaseInfo& info)
{
    visit(cls, [&](CXCursor c) {
        if (clang_getCursorKind(c) == CXCursor_Constructor &&
            clang_getCXXAccessSpecifier(c) != CX_CXXPrivate) {
            int n = clang_Cursor_getNumArguments(c);
            info.any_ctor = true;
            if (clang_CXXConstructor_isDefaultConstructor(c)) info.default_ctor = true;
            if (n == 1 && info.ctor_kind == 0) {
                std::string t = norm(
                    cxs(clang_getTypeSpelling(clang_getCursorType(clang_Cursor_getArgument(c, 0)))));
                if (t.find("string") != std::string::npos && t.find("string_view") == std::string::npos)
                    info.ctor_kind = 1;
                else if (t == "constchar*")
                    info.ctor_kind = 2;
            }
        }
        return CXChildVisit_Continue;
    });
}

CXCursor find_class(CXCursor root, const std::string& qname)
{
    std::string name = qname.substr(qname.rfind(':') == std::string::npos ? 0 : qname.rfind(':') + 1);
    CXCursor found = clang_getNullCursor();
    visit(root, [&](CXCursor c) {
        CXCursorKind k = clang_getCursorKind(c);
        if ((k == CXCursor_ClassDecl || k == CXCursor_StructDecl) && clang_isCursorDefinition(c) &&
            cxs(clang_getCursorSpelling(c)) == name) {
            found = c;
            return CXChildVisit_Break;
        }
        if (k == CXCursor_Namespace) return CXChildVisit_Recurse;
        return CXChildVisit_Continue;
    });
    return found;
}

// ----------------------------------------------------------------- emitter
struct Out {
    std::string s;
    int lines = 0;
    void w(const std::string& t)
    {
        s += t;
        lines += static_cast<int>(std::count(t.begin(), t.end(), '\n'));
    }
};

std::string esc(const std::string& f)
{
    std::string r;
    for (char c : f) {
        if (c == '\\' || c == '"') r += '\\';
        r += c;
    }
    return r;
}

std::string str_join(const std::vector<std::string>& v, const char* sep)
{
    std::string r;
    for (size_t i = 0; i < v.size(); ++i) r += (i ? sep : "") + v[i];
    return r;
}

struct RTok {
    std::string text;
    int line;
    bool space;
};

// What the rewriter knows about the class that it works on.
struct RCtx {
    const ClassDef* cls = nullptr;
    std::map<std::string, std::string> field_type;
    std::set<std::string> fns;
    std::set<std::string> ptr_names;  // fields and parameters of pointer type
    std::set<std::string> locals;     // parameters and declared locals
    bool allow_this = true;
    std::string where;  // the name of the function, for the stale call log of a lambda
};

std::string rewrite_number(const std::string& n)
{
    if (n.size() > 1 && n[0] == '0' && (n[1] == 'x' || n[1] == 'X')) return n;
    bool fp = n.find_first_of(".eE") != std::string::npos;
    char last = n.back();
    if (fp && (last == 'd' || last == 'D')) return n.substr(0, n.size() - 1);
    if (fp && (last == 'f' || last == 'F')) return n;
    if (fp) return n + "f";
    if (last == 'L') return "int64_t{" + n.substr(0, n.size() - 1) + "}";
    return n;
}

bool starts_upper(const std::string& s) { return !s.empty() && std::isupper(static_cast<unsigned char>(s[0])); }

// Token-level rewrite of a body or an expression. It is not a parser.
std::vector<RTok> rewrite(const std::vector<Tok>& b, RCtx& cx)
{
    std::vector<RTok> out;
    const std::string& cn = cx.cls->name;
    auto at = [&](size_t i) -> const Tok* { return i < b.size() ? &b[i] : nullptr; };
    auto is_p = [&](size_t i, const char* s) {
        return i < b.size() && b[i].kind == K::Punct && b[i].text == s;
    };
    int for_state = 0;  // 0: none, else the paren depth inside a for header + 1
    for (size_t i = 0; i < b.size(); ++i) {
        const Tok& t = b[i];
        bool after_access = i > 0 && (is_p(i - 1, ".") || is_p(i - 1, "->") || is_p(i - 1, "::"));
        auto emit = [&](const std::string& s, bool space) { out.push_back({s, t.line, space}); };

        // Arrow lambda: ( name: type, ... ) => { body }
        if (t.kind == K::Punct && t.text == "(" && !for_state) {
            size_t close = i;
            for (int d = 0; close < b.size(); ++close) {
                if (is_p(close, "(")) ++d;
                if (is_p(close, ")") && --d == 0) break;
            }
            if (close < b.size() && is_p(close + 1, "=>")) {
                if (!cx.allow_this) fail(t.line, "a lambda is not allowed in a field initializer");
                if (!is_p(close + 2, "{")) fail(t.line, "a lambda body must be a block");
                size_t end = close + 2;
                for (int d = 0; end < b.size(); ++end) {
                    if (is_p(end, "{")) ++d;
                    if (is_p(end, "}") && --d == 0) break;
                }
                if (end >= b.size()) fail(t.line, "unterminated lambda body");
                // Parameters: split at the commas outside of <> () [].
                std::string params;
                std::vector<std::string> pnames;
                for (size_t j = i + 1; j < close;) {
                    size_t k = j;
                    int d = 0;
                    for (; k < close; ++k) {
                        const std::string& x = b[k].text;
                        if (x == "<" || x == "(" || x == "[") ++d;
                        if (x == ">" || x == ")" || x == "]") --d;
                        if (x == ">>") d -= 2;
                        if (x == "," && d == 0) break;
                    }
                    if (k - j < 3 || b[j].kind != K::Ident || !is_p(j + 1, ":"))
                        fail(b[j].line, "a lambda parameter needs the form 'name: type'");
                    std::vector<Tok> ty(b.begin() + static_cast<std::ptrdiff_t>(j + 2),
                                        b.begin() + static_cast<std::ptrdiff_t>(k));
                    const std::string ptype = join_type(ty);
                    params += (params.empty() ? "" : ", ") + ptype + " " + b[j].text;
                    cx.locals.insert(b[j].text);
                    if (ptype.back() == '*') cx.ptr_names.insert(b[j].text);
                    j = k + 1;
                }
                std::vector<Tok> inner(b.begin() + static_cast<std::ptrdiff_t>(close + 3),
                                       b.begin() + static_cast<std::ptrdiff_t>(end));
                std::vector<RTok> body = rewrite(inner, cx);
                // The lambda captures by value. "self" is a pointer to the shell; the state
                // pointer is read again inside, so a lambda never keeps the pointer to a block.
                emit("script::make_closure(GEN, \"" + cx.where + " lambda\", [=](" + params + ")",
                     t.space);
                out.push_back({"{", b[close + 2].line, true});
                out.push_back({"[[maybe_unused]] auto* st = state_of(self);", b[close + 2].line, true});
                for (RTok& r : body) out.push_back(std::move(r));
                out.push_back({"}", b[end].line, true});
                out.push_back({")", b[end].line, false});
                i = end;
                continue;
            }
        }
        if (t.kind == K::Punct) {
            if (for_state && t.text == "(") ++for_state;
            if (for_state && t.text == ")" && --for_state == 1) for_state = 0;
            emit(t.text, t.space);
            continue;
        }
        if (t.kind == K::Number) {
            emit(rewrite_number(t.text), t.space);
            continue;
        }
        if (t.kind != K::Ident) {
            emit(t.text, t.space);
            continue;
        }
        const std::string& id = t.text;
        if (after_access) {
            emit(id, t.space);
            continue;
        }
        if (id == "for" && is_p(i + 1, "(")) {
            for_state = 1;
            emit(id, t.space);
            continue;
        }
        if (id == "of" && for_state == 2) {
            emit(":", t.space);
            continue;
        }
        if (id == "null") {
            emit("nullptr", t.space);
            continue;
        }
        if (id == "this") {
            if (!cx.allow_this) fail(t.line, "'this' is not allowed in a field initializer");
            if (!(is_p(i + 1, ".") && at(i + 2) && at(i + 2)->kind == K::Ident)) {
                emit("self", t.space);
                continue;
            }
            const std::string& m = b[i + 2].text;
            if (cx.field_type.count(m)) {
                emit("st->" + m, t.space);
                i += 2;
                if (cx.ptr_names.count(m) && is_p(i + 1, ".") && at(i + 2) && at(i + 2)->kind == K::Ident) {
                    emit("->", false);
                    ++i;
                }
            } else if (cx.fns.count(m)) {
                if (!is_p(i + 3, "(")) fail(t.line, "script fn '" + m + "' must be called");
                bool empty = is_p(i + 4, ")");
                emit(cn + "__" + m + "(self" + (empty ? "" : ", "), t.space);
                i += 3;
            } else {
                emit("self->" + m, t.space);
                i += 2;
            }
            continue;
        }
        if (id == "super" && is_p(i + 1, ".") && at(i + 2) && at(i + 2)->kind == K::Ident) {
            emit("self->" + cx.cls->base + "::" + b[i + 2].text, t.space);
            i += 2;
            continue;
        }
        if ((id == "let" || id == "const") && at(i + 1) && at(i + 1)->kind == K::Ident &&
            (is_p(i + 2, "=") || is_p(i + 2, ":") || is_p(i + 2, ";") || at(i + 2)->text == "of")) {
            const std::string& name = b[i + 1].text;
            cx.locals.insert(name);
            std::string pre = id == "const" ? "const " : "";
            if (is_p(i + 2, ":")) {
                std::vector<Tok> ty;
                size_t j = i + 3;
                int angle = 0;
                for (; j < b.size(); ++j) {
                    if (b[j].text == "<") ++angle;
                    else if (b[j].text == ">") --angle;
                    else if (b[j].text == ">>") angle -= 2;
                    if (angle == 0 && b[j].kind == K::Punct &&
                        (b[j].text == "=" || b[j].text == ";" || b[j].text == "," || b[j].text == ")"))
                        break;
                    ty.push_back(b[j]);
                }
                if (ty.empty()) fail(t.line, "expected a type");
                emit(pre + join_type(ty) + " " + name, t.space);
                i = j - 1;
            } else {
                emit(pre + "auto " + name, t.space);
                i += 1;
            }
            continue;
        }
        if (cx.fns.count(id) && is_p(i + 1, "(") && !cx.locals.count(id)) {
            bool empty = is_p(i + 2, ")");
            emit(cn + "__" + id + "(self" + (empty ? "" : ", "), t.space);
            i += 1;
            continue;
        }
        if (starts_upper(id) && !cx.locals.count(id) && is_p(i + 1, ".") && at(i + 2) &&
            at(i + 2)->kind == K::Ident) {
            emit(id + "::", t.space);
            ++i;
            continue;
        }
        if (cx.ptr_names.count(id) && cx.locals.count(id) && is_p(i + 1, ".") && at(i + 2) &&
            at(i + 2)->kind == K::Ident) {
            emit(id + "->", t.space);
            ++i;
            continue;
        }
        emit(id, t.space);
    }
    return out;
}

// ------------------------------------------------------------------ driver
struct Slot {
    std::string id;  // "Tick", "TakeDamage_f", "heal"
    const NativeVirtual* nv = nullptr;
    const Method* impl = nullptr;  // override (native slot) or fn (script slot)
    int intro_gen = 0;             // script fns only
    bool native() const { return nv != nullptr; }
};

std::string mangle_type(const std::string& t)
{
    std::string n = norm(t);
    bool ptr = n.find('*') != std::string::npos;
    std::string core;
    for (size_t i = 0; i < n.size(); ++i) {
        if (n[i] == '&' || n[i] == '*') continue;
        core += n[i];
    }
    for (const char* q : {"const", "volatile"}) {
        size_t p = core.find(q);
        if (p == 0) core = core.substr(std::string(q).size());
    }
    static const std::map<std::string, std::string> builtin = {
        {"float", "f"},     {"double", "d"},       {"int", "i"},  {"bool", "b"},
        {"unsigned", "u"},  {"unsignedint", "u"},  {"char", "c"}, {"long", "l"},
        {"longlong", "ll"}, {"short", "s"},        {"void", "v"},
    };
    std::string r;
    auto it = builtin.find(core);
    if (it != builtin.end()) {
        r = it->second;
    } else {
        size_t p = core.rfind(':');
        std::string last = core.substr(p == std::string::npos ? 0 : p + 1);
        r = last.empty() ? "x" : std::string(1, static_cast<char>(std::toupper(last[0])));
    }
    return ptr ? r + "P" : r;
}

std::string arg_expr(const std::string& type, const std::string& name)
{
    std::string n = norm(type);
    if (n.size() > 2 && n.compare(n.size() - 2, 2, "&&") == 0) return "std::move(" + name + ")";
    return name;
}

struct Options {
    std::string input, output;
    int gen = 0;
    std::vector<std::string> includes;
    std::string resource_dir;  // the Clang resource directory (builtin headers)
};

struct ManifestEntry {
    std::string cls, fn;
    int gen;
};

std::vector<ManifestEntry> read_manifest(const std::string& text)
{
    std::vector<ManifestEntry> v;
    std::istringstream f(text);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string name;
        int gen = 0;
        if (!(ss >> name >> gen)) continue;
        size_t p = name.find("::");
        if (p == std::string::npos) continue;
        v.push_back({name.substr(0, p), name.substr(p + 2), gen});
    }
    return v;
}

// A guard for the libclang handles. It releases them on every exit path.
struct ClangHandles {
    CXIndex idx = nullptr;
    CXTranslationUnit tu = nullptr;
    ~ClangHandles()
    {
        if (tu) clang_disposeTranslationUnit(tu);
        if (idx) clang_disposeIndex(idx);
    }
};

// The identity of a header file: the path, the time of the last change and the size.
struct FileStamp {
    std::string path;
    long long mtime_ns = -1;
    long long size = -1;
    bool operator==(const FileStamp& o) const
    {
        return path == o.path && mtime_ns == o.mtime_ns && size == o.size;
    }
};

FileStamp stamp_of(const std::string& path)
{
    FileStamp f;
    f.path = path;
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) {
        f.mtime_ns = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
        f.size = static_cast<long long>(st.st_size);
    }
    return f;
}

// Parses the imports with libclang. Reads the base classes in "names" that it finds.
// Gives the stamps of all files that the parse included.
void parse_bases(const std::string& probe, const std::vector<std::string>& args,
                 const std::set<std::string>& names, std::map<std::string, BaseInfo>& out,
                 std::vector<FileStamp>& files)
{
    std::vector<const char*> cargs;
    for (const auto& a : args) cargs.push_back(a.c_str());
    ClangHandles ch;
    CXIndex idx = ch.idx = clang_createIndex(0, 0);
    CXUnsavedFile unsaved{"spc_probe.cpp", probe.c_str(), static_cast<unsigned long>(probe.size())};
    CXTranslationUnit tu = nullptr;
    CXErrorCode ec = clang_parseTranslationUnit2(idx, "spc_probe.cpp", cargs.data(),
                                                 static_cast<int>(cargs.size()), &unsaved, 1,
                                                 CXTranslationUnit_SkipFunctionBodies, &tu);
    ch.tu = tu;
    if (ec != CXError_Success || !tu) fail(0, "libclang cannot parse the imports");
    bool diag_error = false;
    for (unsigned i = 0, n = clang_getNumDiagnostics(tu); i < n; ++i) {
        CXDiagnostic d = clang_getDiagnostic(tu, i);
        if (clang_getDiagnosticSeverity(d) >= CXDiagnostic_Error) {
            // Use the presumed location, so "#line" gives the script name and line.
            CXString file;
            unsigned line = 0, col = 0;
            clang_getPresumedLocation(clang_getDiagnosticLocation(d), &file, &line, &col);
            const char* sev = clang_getDiagnosticSeverity(d) == CXDiagnostic_Fatal ? "fatal error" : "error";
            g_diag += cxs(file) + ":" + std::to_string(line) + ":" + std::to_string(col) + ": " + sev +
                      ": " + cxs(clang_getDiagnosticSpelling(d)) + "\n";
            diag_error = true;
        }
        clang_disposeDiagnostic(d);
    }
    if (diag_error) fail(0, "the imports have errors");

    std::set<std::string> seen;
    auto on_include = [](CXFile f, CXSourceLocation*, unsigned depth, CXClientData d) {
        if (depth == 0) return;  // the probe file
        auto* v = static_cast<std::pair<std::set<std::string>*, std::vector<FileStamp>*>*>(d);
        std::string path = cxs(clang_getFileName(f));
        if (v->first->insert(path).second) v->second->push_back(stamp_of(path));
    };
    std::pair<std::set<std::string>*, std::vector<FileStamp>*> ctx{&seen, &files};
    files.clear();
    clang_getInclusions(tu, on_include, &ctx);

    CXCursor root = clang_getTranslationUnitCursor(tu);
    for (const auto& name : names) {
        CXCursor base_cur = find_class(root, name);
        if (clang_Cursor_isNull(base_cur)) continue;
        BaseInfo bi;
        bi.name = name;
        collect_virtuals(base_cur, bi);
        collect_ctors(base_cur, bi);
        out[name] = std::move(bi);
    }
}

}  // namespace

namespace spc {

// The cache of one key (flags + imports): the base classes and the files that the parse read.
struct TranslatorSession::Impl {
    struct Entry {
        std::vector<FileStamp> files;
        std::map<std::string, BaseInfo> bases;
    };
    std::mutex mutex;
    std::map<std::string, Entry> entries;
    int parses = 0;
};

TranslatorSession::TranslatorSession() : impl_(new Impl) {}
TranslatorSession::~TranslatorSession() = default;

int TranslatorSession::parse_count() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->parses;
}

void TranslatorSession::clear()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->entries.clear();
}

}  // namespace spc

namespace {

// Gives the base classes for one key. A cache hit needs all names in the entry and
// the same stamp for each file. Otherwise it parses again and keeps the new result.
std::map<std::string, BaseInfo> session_bases(
    spc::TranslatorSession::Impl& S, const std::string& key, const std::set<std::string>& names,
    const std::function<void(std::map<std::string, BaseInfo>&, std::vector<FileStamp>&)>& parse)
{
    std::lock_guard<std::mutex> lock(S.mutex);
    auto& e = S.entries[key];
    bool fresh = !e.files.empty();
    for (size_t i = 0; fresh && i < e.files.size(); ++i) fresh = stamp_of(e.files[i].path) == e.files[i];
    if (!fresh) e = {};
    bool complete = fresh;
    for (const auto& n : names) complete = complete && e.bases.count(n);
    if (!complete) {
        std::map<std::string, BaseInfo> out = std::move(e.bases);
        std::vector<FileStamp> files;
        ++S.parses;
        try {
            parse(out, files);
        } catch (...) {
            S.entries.erase(key);
            throw;
        }
        e.bases = std::move(out);
        e.files = std::move(files);
    }
    std::map<std::string, BaseInfo> r;
    for (const auto& n : names) {
        auto it = e.bases.find(n);
        if (it != e.bases.end()) r.emplace(n, it->second);
    }
    return r;
}

void translate_impl(const spc::TranslateOptions& in, spc::TranslatorSession::Impl& session,
                    spc::TranslateResult& res)
{
    Options opt;
    opt.input = in.script_name;
    opt.output = in.output_name;
    opt.gen = in.generation;
    opt.includes = in.include_dirs;
    opt.resource_dir = in.resource_dir;
#ifdef SPC_RESOURCE_DIR
    if (opt.resource_dir.empty()) opt.resource_dir = SPC_RESOURCE_DIR;
#endif
    if (opt.gen < 1) fail(0, "the generation must be 1 or more");
    if (opt.output.empty()) opt.output = opt.input + ".cpp";
    g_script = opt.input;

    Script sc = Parser(lex(in.script_text)).parse();

    // ---- libclang: parse the imports and read the native base classes.
    std::string probe;
    for (const auto& im : sc.imports)
        probe += "#line " + std::to_string(im.line) + " \"" + esc(opt.input) + "\"\n#include " +
                 im.target() + "\n";
    std::vector<std::string> args = {"-x", "c++", "-std=c++20"};
    size_t slash = opt.input.rfind('/');
    args.push_back("-I" + (slash == std::string::npos ? std::string(".") : opt.input.substr(0, slash)));
    for (const auto& d : opt.includes) args.push_back("-I" + d);
    if (!opt.resource_dir.empty()) args.push_back("-resource-dir=" + opt.resource_dir);
    std::set<std::string> import_paths, base_names;
    for (const auto& im : sc.imports) import_paths.insert(im.path);
    for (const auto& cd : sc.classes) base_names.insert(cd.base);
    // The key: the flags, the current directory and the imports (the order does not matter).
    std::string key = std::filesystem::current_path().string() + "\n";
    for (const auto& a : args) key += a + "\n";
    for (const auto& p : import_paths) key += "#" + p + "\n";
    const std::map<std::string, BaseInfo> bases =
        session_bases(session, key, base_names,
                      [&](std::map<std::string, BaseInfo>& out, std::vector<FileStamp>& files) {
                          parse_bases(probe, args, base_names, out, files);
                      });

    std::vector<ManifestEntry> old_manifest;
    {
        // Keep the entries of the earlier generations. Keep also the entries of
        // the classes that this script does not define (an other script owns them).
        std::set<std::string> mine;
        for (const auto& cd : sc.classes) mine.insert(cd.name);
        for (auto& e : read_manifest(in.slots_in))
            if (e.gen < opt.gen || !mine.count(e.cls)) old_manifest.push_back(e);
    }
    std::vector<ManifestEntry> new_manifest = old_manifest;

    const bool multi = sc.classes.size() > 1;
    Out o;
    o.w("// Generated by spc from " + opt.input + " (generation " + std::to_string(opt.gen) +
        "). Do not edit.\n");
    o.w("#include <cstddef>\n#include <cstdint>\n#include <cstdio>\n"
        "#include <new>\n#include <string>\n#include <type_traits>\n#include <typeinfo>\n"
        "#include <utility>\n\n#include \"script_runtime.h\"\n\n");
    auto reset = [&] { o.w("#line " + std::to_string(o.lines + 2) + " \"" + esc(opt.output) + "\"\n"); };
    for (const auto& im : sc.imports) {
        o.w("#line " + std::to_string(im.line) + " \"" + esc(opt.input) + "\"\n");
        o.w("#include " + im.target() + "\n");
    }
    reset();
    o.w("\nconstexpr uint32_t GEN = " + std::to_string(opt.gen) + ";\n");

    std::vector<std::pair<std::string, bool>> registers;  // class name, for the register function
    std::vector<std::string> thunk_regs;

    for (const ClassDef& cd : sc.classes) {
        const std::string cn = cd.name;
        const std::string P = cn + "__";
        auto slot_id = [&](const std::string& n) { return (multi ? cn + "_" : std::string()) + "SLOT_" + n; };

        // ---- base class information from libclang
        auto base_it = bases.find(cd.base);
        if (base_it == bases.end())
            fail(cd.line, "base class '" + cd.base + "' is not found in the imported headers");
        BaseInfo bi = base_it->second;
        if (!bi.any_ctor) bi.default_ctor = true;  // the compiler declares one
        if (!bi.virtual_dtor)
            fail(cd.line, "base class '" + cd.base + "' has no virtual destructor");
        std::string ctor_args;
        if (bi.ctor_kind == 1) ctor_args = ", std::string(\"" + cn + "\")";
        else if (bi.ctor_kind == 2) ctor_args = ", \"" + cn + "\"";
        else if (!bi.default_ctor)
            fail(cd.line, "base class '" + cd.base + "' has no default constructor and no constructor that takes a name");

        // ---- check the script members
        std::set<std::string> field_names;
        for (const Field& f : cd.fields)
            if (!field_names.insert(f.name).second) fail(f.line, "field '" + f.name + "' is declared twice");
        std::set<std::string> fn_names;
        std::map<std::string, const Method*> overrides;  // by native key
        for (const Method& m : cd.methods) {
            for (const Param& p : m.params)
                if (p.name == "self" || p.name == "st" || p.name == "a")
                    fail(m.line, "parameter name '" + p.name + "' is reserved");
            if (!m.is_override) {
                if (!fn_names.insert(m.name).second)
                    fail(m.line, "fn '" + m.name + "' is declared twice (overloads are not supported)");
                if (field_names.count(m.name)) fail(m.line, "'" + m.name + "' is a field and a fn");
                for (const auto& nv : bi.virtuals)
                    if (nv.name == m.name)
                        fail(m.line, "fn '" + m.name + "' hides the virtual method of " + nv.owner);
                continue;
            }
            std::string key = m.name + "(";
            for (const Param& p : m.params) key += norm(p.type) + ",";
            const NativeVirtual* hit = nullptr;
            for (const auto& nv : bi.virtuals) {
                std::string k = nv.key();
                if (nv.is_const) k.resize(k.size() - 5);
                if (k == key + ")") hit = &nv;
            }
            if (!hit) {
                std::string msg = "no virtual method " + m.name + "(";
                for (size_t i = 0; i < m.params.size(); ++i) msg += (i ? ", " : "") + m.params[i].type;
                msg += ") in " + cd.base;
                std::string cands;
                for (const auto& nv : bi.virtuals)
                    if (nv.name == m.name) cands += (cands.empty() ? "" : "; ") + nv.text();
                if (!cands.empty()) msg += ". Candidates: " + cands;
                fail(m.line, msg);
            }
            if (hit->final_)
                fail(m.line, "'" + hit->text() + "' of " + hit->owner + " is final and cannot be overridden");
            if (!m.ret.empty() && norm(m.ret) != norm(hit->ret))
                fail(m.line, "return type '" + m.ret + "' differs from '" + hit->ret + "'");
            if (!overrides.emplace(hit->key(), &m).second)
                fail(m.line, "'" + hit->text() + "' is overridden twice");
        }
        for (const auto& nv : bi.virtuals)
            if (nv.pure && !overrides.count(nv.key()))
                fail(cd.line, "class " + cn + " does not override the pure virtual method '" +
                                  nv.text() + "' of " + nv.owner);

        // ---- slots: native virtuals first, then the script fns
        std::vector<Slot> slots;
        std::map<std::string, int> name_count;
        // A final method gets no slot: the shell cannot override it.
        for (const auto& nv : bi.virtuals)
            if (!nv.final_) ++name_count[nv.name];
        std::set<std::string> used;
        for (const auto& nv : bi.virtuals) {
            if (nv.final_) continue;
            Slot s;
            s.nv = &nv;
            s.id = nv.name;
            if (name_count[nv.name] > 1) {
                std::string suf;
                for (const auto& p : nv.ptypes) suf += mangle_type(p);
                s.id += "_" + (suf.empty() ? std::string("v") : suf);
            }
            for (int n = 2; used.count(s.id); ++n) s.id += std::to_string(n);
            used.insert(s.id);
            auto it = overrides.find(nv.key());
            if (it != overrides.end()) s.impl = it->second;
            slots.push_back(s);
        }
        const size_t native_count = slots.size();
        std::map<std::string, const Method*> fn_by_name;
        for (const Method& m : cd.methods)
            if (!m.is_override) fn_by_name[m.name] = &m;
        for (const auto& e : old_manifest) {
            if (e.cls != cn) continue;
            auto it = fn_by_name.find(e.fn);
            if (it == fn_by_name.end())
                fail(cd.line, "fn '" + e.fn + "' of an earlier generation is removed. The slot count must not shrink");
            Slot s;
            s.id = e.fn;
            s.impl = it->second;
            s.intro_gen = e.gen;
            slots.push_back(s);
        }
        for (const Method& m : cd.methods) {
            if (m.is_override) continue;
            bool known = false;
            for (const auto& s : slots) known |= !s.native() && s.id == m.name;
            if (known) continue;
            Slot s;
            s.id = m.name;
            s.impl = &m;
            s.intro_gen = opt.gen;
            slots.push_back(s);
            new_manifest.push_back({cn, m.name, opt.gen});
        }

        // ---- rewriter context
        RCtx base_cx;
        base_cx.cls = &cd;
        for (const Field& f : cd.fields) {
            base_cx.field_type[f.name] = f.type;
            if (!f.type.empty() && f.type.back() == '*') base_cx.ptr_names.insert(f.name);
        }
        for (const Method& m : cd.methods)
            if (!m.is_override) base_cx.fns.insert(m.name);

        const std::string S = cn + "State_g" + std::to_string(opt.gen);
        const std::string shell = cn + "_Shell";
        const std::string B = cd.base;

        o.w("\n// ---------------------------------------------------------------- class " + cn + "\n");
        o.w("// Slot numbers. Native virtuals come first, in the order of " + B + ".\n");
        o.w("enum : unsigned {\n");
        for (size_t i = 0; i < slots.size(); ++i)
            o.w("    " + slot_id(slots[i].id) + " = " + std::to_string(i) + ",\n");
        o.w("    " + slot_id("COUNT") + " = " + std::to_string(slots.size()) + ",\n};\n\n");
        o.w("extern script::ClassRecord " + cn + "_record;\n\n");

        // ---- shell
        o.w("// The shell overrides every virtual of " + B + ". Each stub reads one slot.\n");
        o.w("struct " + shell + " final : " + B + ", script::ShellBase {\n");
        o.w("    template <class... A>\n    explicit " + shell + "(script::Construct, A&&... a) : " + B +
            "(std::forward<A>(a)...)\n    {\n        script::init_state(this, &" + cn +
            "_record);\n    }\n    ~" + shell + "() override { script::destroy_state(this); }\n");
        for (size_t i = 0; i < native_count; ++i) {
            const NativeVirtual& nv = *slots[i].nv;
            std::vector<std::string> ps, sig = {B + "*"}, as = {nv.is_const ? "const_cast<" + shell + "*>(this)" : "this"};
            for (size_t k = 0; k < nv.ptypes.size(); ++k) {
                ps.push_back(nv.ptypes[k] + " a" + std::to_string(k));
                sig.push_back(nv.ptypes[k]);
                as.push_back(arg_expr(nv.ptypes[k], "a" + std::to_string(k)));
            }
            o.w("\n    " + nv.ret + " " + nv.name + "(" + str_join(ps, ", ") + ")" +
                (nv.is_const ? " const" : "") + (nv.no_except ? " noexcept" : "") + " override\n    {\n");
            o.w("        return script::call<" + nv.ret + "(" + str_join(sig, ", ") + ")>(cls, " +
                slot_id(slots[i].id) + ", " + str_join(as, ", ") + ");\n    }\n");
        }
        o.w("};\n\n");

        // ---- state struct and field table
        o.w("// ---------------------------------------------------------------- state\n");
        o.w("struct " + S + " {\n");
        for (const Field& f : cd.fields) o.w("    " + f.type + " " + f.name + ";\n");
        if (cd.fields.empty()) o.w("    char unused_;\n");
        o.w("};\n\n");
        for (const Field& f : cd.fields) {
            RCtx fc = base_cx;
            fc.allow_this = false;
            std::string init;
            if (f.has_init) {
                auto rt = rewrite(f.init, fc);
                for (size_t i = 0; i < rt.size(); ++i) init += (i && rt[i].space ? " " : "") + rt[i].text;
            }
            o.w("#line " + std::to_string(f.line) + " \"" + esc(opt.input) + "\"\n");
            o.w("static void " + P + "init_" + f.name + "(void*, void* f) { using T = " + f.type +
                "; new (f) T(" + init + "); }\n");
            reset();
        }
        o.w("\n#pragma clang diagnostic push\n#pragma clang diagnostic ignored \"-Winvalid-offsetof\"\n");
        if (cd.fields.empty()) {
            o.w("static const script::FieldInfo* const " + cn + "_fields = nullptr;\n");
        } else {
            o.w("static const script::FieldInfo " + cn + "_fields[] = {\n");
            for (const Field& f : cd.fields) {
                const std::string T = f.type;
                o.w("    {\"" + f.name + "\", " + (f.was.empty() ? "nullptr" : "\"" + f.was + "\"") +
                    ", typeid(" + T + ").name(), offsetof(" + S + ", " + f.name + "), sizeof(" + T +
                    "),\n     alignof(" + T + "), script::field_flags<" + T + ">(), " + P + "init_" +
                    f.name + ", script::move_field<" + T + ">, script::field_destroyer<" + T +
                    ">()},\n");
            }
            o.w("};\n");
        }
        o.w("#pragma clang diagnostic pop\n\n");
        o.w("static const script::StateLayout " + cn + "_layout = {\n    \"" + S + "\", sizeof(" + S +
            "), alignof(" + S + "), " + std::to_string(cd.fields.size()) + ", " + cn + "_fields,\n};\n\n");

        // ---- generation functions
        o.w("// ------------------------------------------------- generation functions\n");
        o.w("static inline " + S + "* state_of(" + shell + "* self)\n{\n    return static_cast<" + S +
            "*>(self->state);\n}\n\n");

        auto param_decl = [](const Method& m) {
            std::string r;
            for (const Param& p : m.params) r += ", " + p.type + " " + p.name;
            return r;
        };
        auto fn_ret = [](const Method& m) { return m.ret.empty() ? std::string("void") : m.ret; };
        auto type_list = [](const Method& m) {
            std::string r;
            for (const Param& p : m.params) r += ", " + p.type;
            return r;
        };
        auto arg_list = [](const Method& m) {
            std::string r;
            for (const Param& p : m.params) r += ", " + arg_expr(p.type, p.name);
            return r;
        };

        // forward declarations of the script fns
        for (const Method& m : cd.methods)
            if (!m.is_override)
                o.w("static " + fn_ret(m) + " " + P + m.name + "(" + shell + "* self" + param_decl(m) + ");\n");
        o.w("\n");

        auto emit_body = [&](const Method& m, bool is_fn, const std::string& sym, const std::string& ret,
                             const std::string& what) {
            RCtx cx = base_cx;
            cx.where = what;
            for (const Param& p : m.params) {
                cx.locals.insert(p.name);
                if (!p.type.empty() && p.type.back() == '*') cx.ptr_names.insert(p.name);
            }
            std::string head = "static " + ret + " " + sym + "(";
            if (is_fn) head += shell + "* self" + param_decl(m);
            else head += B + "* a" + param_decl(m);
            o.w(head + ")\n{\n");
            if (!is_fn) o.w("    [[maybe_unused]] auto* self = static_cast<" + shell + "*>(a);\n");
            o.w("    if (SCRIPT_UNLIKELY(script::active_generation != GEN)) {\n");
            o.w("        script::stale_call(\"" + what + "\");\n        return" +
                (ret == "void" ? std::string() : " " + ret + "()") + ";\n    }\n");
            o.w("    [[maybe_unused]] auto* st = state_of(self);\n");
            std::vector<RTok> rt = rewrite(m.body, cx);
            int cur = -1, depth = 1;
            for (size_t i = 0; i < rt.size(); ++i) {
                if (rt[i].line != cur) {
                    if (cur != -1) o.w("\n");
                    cur = rt[i].line;
                    int d = depth - (rt[i].text == "}" ? 1 : 0);
                    o.w("#line " + std::to_string(cur) + " \"" + esc(opt.input) + "\"\n");
                    o.w(std::string(4 * std::max(d, 0), ' ') + rt[i].text);
                } else {
                    o.w((rt[i].space ? " " : "") + rt[i].text);
                }
                if (rt[i].text == "{") ++depth;
                if (rt[i].text == "}") --depth;
            }
            if (cur != -1) o.w("\n");
            reset();
            o.w("}\n\n");
        };

        for (const Method& m : cd.methods) {
            if (m.is_override) {
                const Slot* s = nullptr;
                for (const Slot& x : slots)
                    if (x.impl == &m) s = &x;
                std::string what = cn + "::" + m.name + "(";
                for (size_t i = 0; i < m.params.size(); ++i) what += (i ? ", " : "") + m.params[i].type;
                what += ")";
                emit_body(m, false, P + s->id, s->nv->ret, what);
            } else {
                emit_body(m, true, P + m.name, fn_ret(m), cn + "::" + m.name);
            }
        }

        // forwarders for the native virtuals that the script does not override
        for (size_t i = 0; i < native_count; ++i) {
            const Slot& s = slots[i];
            if (s.impl) continue;
            const NativeVirtual& nv = *s.nv;
            std::string ps, as;
            for (size_t k = 0; k < nv.ptypes.size(); ++k) {
                ps += ", " + nv.ptypes[k] + " a" + std::to_string(k);
                as += (k ? ", " : "") + arg_expr(nv.ptypes[k], "a" + std::to_string(k));
            }
            o.w("// Not overridden: call the implementation of " + B + ".\n");
            o.w("static " + nv.ret + " " + P + "fwd_" + s.id + "(" + B + "* a" + ps + ")\n{\n");
            o.w("    return static_cast<" + shell + "*>(a)->" + B + "::" + nv.name + "(" + as + ");\n}\n\n");
        }

        // slot adapters and thunks for the script fns
        for (size_t i = native_count; i < slots.size(); ++i) {
            const Method& m = *slots[i].impl;
            o.w("static " + fn_ret(m) + " " + P + m.name + "_slot(" + B + "* a" + param_decl(m) + ")\n{\n");
            o.w("    return " + P + m.name + "(static_cast<" + shell + "*>(a)" + arg_list(m) + ");\n}\n\n");
        }
        for (size_t i = native_count; i < slots.size(); ++i) {
            if (slots[i].intro_gen != opt.gen) continue;
            const Method& m = *slots[i].impl;
            o.w("// The thunk has the same address in each generation (section 4.11). The\n"
                "// generation that adds the fn emits it. It reads the slot at call time.\n");
            o.w("static " + fn_ret(m) + " " + P + m.name + "__thunk(" + B + "* a" + param_decl(m) + ")\n{\n");
            o.w("    auto* self = static_cast<" + shell + "*>(a);\n");
            o.w("    return script::call<" + fn_ret(m) + "(" + B + "*" + type_list(m) + ")>(self->cls, " +
                slot_id(m.name) + ", a" + arg_list(m) + ");\n}\n\n");
            thunk_regs.push_back("    reg.add_thunk(\"" + cn + "::" + m.name +
                                 "\", reinterpret_cast<void*>(&" + P + m.name + "__thunk));\n");
        }

        // ---- record and entry
        o.w("// ------------------------------------------------------ record and entry\n");
        o.w("static void* " + cn + "_slots[" + slot_id("COUNT") + "] = {\n");
        for (size_t i = 0; i < slots.size(); ++i) {
            std::string fn;
            if (!slots[i].native()) fn = P + slots[i].id + "_slot";
            else fn = P + (slots[i].impl ? "" : "fwd_") + slots[i].id;
            o.w("    reinterpret_cast<void*>(&" + fn + "),\n");
        }
        o.w("};\n\n");
        o.w("script::ClassRecord " + cn + "_record = {\n    \"" + cn + "\", GEN, " + cn + "_slots, " +
            slot_id("COUNT") + ", &" + cn + "_layout, nullptr,\n    \"" + B + "\", script::shell_layout_hash(\"" +
            B + "\", sizeof(" + shell + "), alignof(" + shell + "), " + std::to_string(native_count) +
            "),\n};\n\n");
        o.w("static script::Instance " + cn + "_create()\n{\n    auto* p = new " + shell +
            "(script::Construct{}" + ctor_args + ");\n    return {static_cast<" + B + "*>(p), p};\n}\n\n");
        registers.push_back({cn, true});
    }

    o.w("extern \"C\" void script_register(script::Registry& reg)\n{\n");
    for (const auto& r : registers) o.w("    reg.add(&" + r.first + "_record, &" + r.first + "_create);\n");
    for (const auto& t : thunk_regs) o.w(t);
    o.w("}\n");

    res.cpp = o.s;
    std::ostringstream mf;
    for (const auto& e : new_manifest) mf << e.cls << "::" << e.fn << " " << e.gen << "\n";
    res.slots_out = mf.str();
}

}  // namespace

namespace spc {

TranslateResult translate(const TranslateOptions& in)
{
    TranslatorSession session;
    return translate(in, session);
}

TranslateResult translate(const TranslateOptions& in, TranslatorSession& session)
{
    TranslateResult res;
    g_diag.clear();
    g_script = in.script_name;
    try {
        translate_impl(in, *session.impl_, res);
        res.ok = true;
    } catch (const TranslateAbort&) {
        res.ok = false;
        res.cpp.clear();
        res.slots_out.clear();
    }
    res.diagnostics = g_diag;
    return res;
}

}  // namespace spc
