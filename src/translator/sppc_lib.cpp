// sppc_lib: the translator from script text to the text of a generation .cpp file.
//
// The translator has three parts.
//   1. A parser (src/translator/parser/). It reads the structure of the script:
//      imports, classes, fields, overrides and functions. It also parses each body and
//      each field initializer into an AST (ast.h). A lowering pass (lower.cpp)
//      changes the AST into C++ names.
//   2. A query to libclang (C API). It reads the virtual methods of the native
//      base class. The translator resolves script names only. Clang resolves
//      all native names when it compiles the generated file.
//   3. An emitter. It writes the shell, the state struct, the reflection table,
//      the generation functions, the thunks and the record. The printer
//      (emit.cpp) writes each body. A "#line" directive before each line makes
//      the Clang diagnostics point to the script.
//
// The function sppc::translate takes strings and returns strings. It does not
// write a file. libclang reads the imported headers from the include dirs.
// The command line tool sppc (src/translator/sppc.cpp) is a thin file wrapper.
//
// The slot file keeps the slot order of the script functions stable between the
// generations. Native virtuals come first, in the order of the base class.
// The script functions follow. A function keeps its slot in all later generations.
#include "spp/sppc_lib.h"

#include "ast.h"
#include "builtin_types.h"
#include "check.h"
#include "diag.h"
#include "emit.h"
#include "lower.h"
#include "parser/parser.h"
#include "types.h"

#include <clang-c/Index.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using namespace sppc::ast;

// State of the call in progress. Each thread has its own copy.
thread_local std::string g_script;  // the script name, as given by the caller
thread_local sppc::Diagnostics g_diag;  // the diagnostics of the call

struct TranslateAbort {};

// The name of the synthetic [value] class that holds the module variables (module state block).
const std::string kModuleName = "SppModule";

[[noreturn]] void fail(int line, const std::string& msg)
{
    g_diag.add_line(line, msg);
    throw TranslateAbort{};
}

// An error at a byte offset of the script. The diagnostic has a line and a column.
[[noreturn]] void fail_at(const sppc::LineMap& map, uint32_t pos, const std::string& msg)
{
    g_diag.add(pos, map, msg);
    throw TranslateAbort{};
}

// Parses the script: the structure, the method bodies and the field initializers.
// The parser reports all syntax errors in source order. Any error stops the translation.
Script parse_structure(const std::string& text, const sppc::LineMap& map)
{
    Script sc = sppc::parse_script(text, map, g_diag);
    if (!g_diag.items.empty()) throw TranslateAbort{};
    return sc;
}

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

// The key form of a type: the spelling is canonical (see builtin_types.h) and has no space.
// A native "int" and a script "int" or "int32" give the same key.
std::string key_norm(const std::string& s) { return norm(sppc::canonical_type(s, nullptr, true)); }

struct NativeVirtual {
    std::string name, ret, owner;
    std::string from;  // the direct native base of the script class that gives the virtual
    std::vector<std::string> ptypes;
    bool is_const = false, pure = false, final_ = false, no_except = false;
    std::string key() const
    {
        std::string k = name + "(";
        for (const auto& p : ptypes) k += key_norm(p) + ",";
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
    bool final_cls = false; // the class is "final": no class can derive from it
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

// What the headers say about a type. "known" is false when libclang cannot name the type.
// The facts come from C++ traits that the probe file evaluates (see facts_text).
struct TypeFacts {
    bool known = false;
    bool is_class = false, copy = false, nothrow_move = false, polymorphic = false, final_ = false;
    bool plain = false;  // trivially copyable and standard layout
    bool small = false;  // trivially copyable and 16 bytes or less (design 7.10.5)
    bool arrow = false;  // a pointer, or a type with operator->
    std::map<std::string, bool> members;  // for each asked name: "&T::name" is valid
};
using FactMap = std::map<std::string, TypeFacts>;

// The question for one type: the type text and the member names to look for.
struct FactReq {
    std::string type;
    std::vector<std::string> members;
};

// The direct bases of each native class, by qualified name, in declaration order.
using Hierarchy = std::map<std::string, std::vector<std::string>>;

// The qualified name of a declaration: the namespaces and the classes, joined with "::".
// An empty spelling (an anonymous namespace) is skipped.
std::string qualified_name(CXCursor c)
{
    std::vector<std::string> parts;
    std::string self = cxs(clang_getCursorSpelling(c));
    if (!self.empty()) parts.push_back(self);
    for (CXCursor p = clang_getCursorSemanticParent(c);
         !clang_Cursor_isNull(p) && !clang_isTranslationUnit(clang_getCursorKind(p));
         p = clang_getCursorSemanticParent(p)) {
        std::string n = cxs(clang_getCursorSpelling(p));
        if (!n.empty()) parts.push_back(n);
    }
    std::string r;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) r += (r.empty() ? "" : "::") + *it;
    return r;
}

// Collects the direct bases of the classes that are defined in the user headers.
void collect_hierarchy(CXCursor root, Hierarchy& out)
{
    visit(root, [&](CXCursor c) {
        CXCursorKind k = clang_getCursorKind(c);
        if (k == CXCursor_Namespace) return CXChildVisit_Recurse;
        if ((k == CXCursor_ClassDecl || k == CXCursor_StructDecl) && clang_isCursorDefinition(c) &&
            !clang_Location_isInSystemHeader(clang_getCursorLocation(c))) {
            std::vector<std::string> bases;
            visit(c, [&](CXCursor bs) {
                if (clang_getCursorKind(bs) == CXCursor_CXXBaseSpecifier) {
                    CXCursor def = clang_getCursorDefinition(clang_getCursorReferenced(bs));
                    if (!clang_Cursor_isNull(def) && clang_getCursorKind(def) != CXCursor_NoDeclFound)
                        bases.push_back(qualified_name(def));
                    else
                        bases.push_back(tidy(cxs(clang_getTypeSpelling(clang_getCursorType(bs)))));
                }
                return CXChildVisit_Continue;
            });
            out[qualified_name(c)] = std::move(bases);
        }
        return CXChildVisit_Continue;
    });
}

}  // namespace

namespace sppc {
uint32_t find_word(const std::string& text, uint32_t from, const std::string& name);
}  // namespace sppc

namespace {

// ----------------------------------------------------------------- emitter
struct Out {
    std::string s;
    int lines = 0;
    std::vector<sppc::SourceSegment> segs;  // in the order of writing
    void w(const std::string& t)
    {
        s += t;
        lines += static_cast<int>(std::count(t.begin(), t.end(), '\n'));
    }
    // Writes the text of an emitter and records its segments. "script" is the script text. A
    // leaf that is not the same text as the script at its position is a rewritten name (for
    // example "int32_t" for "int"): its script range is the name at the position.
    void w_emitted(const sppc::ast::Emitter& em, const std::string& script)
    {
        const uint32_t base = static_cast<uint32_t>(s.size());
        for (const auto& g : em.segments()) {
            const std::string_view gen(em.text().data() + g.gen_begin, g.gen_len);
            sppc::SourceSegment seg;
            seg.gen_begin = base + g.gen_begin;
            seg.gen_len = g.gen_len;
            seg.script_begin = g.pos;
            if (g.pos + g.gen_len <= script.size() && script.compare(g.pos, g.gen_len, gen) == 0) {
                seg.script_len = g.gen_len;
            } else {
                size_t e = g.pos;
                while (e < script.size() && (std::isalnum(static_cast<unsigned char>(script[e])) || script[e] == '_')) ++e;
                seg.script_len = static_cast<uint32_t>(std::max<size_t>(e - g.pos, 1));
                // Equal lengths mean the same text. A different text of the same length (for
                // example "st->" for "this") gets no segment.
                if (seg.script_len == seg.gen_len) continue;
            }
            segs.push_back(seg);
        }
        w(em.text());
    }
    // Records a segment for a declared name in "head", the text that w() writes next. The name
    // "gen_name" in the head comes from the word "script_name" at or after "script_from" in the
    // script. Returns the end of the name in the script and in the head, for the next name.
    // Nothing is recorded when a name is not found. The text of the output does not change.
    std::pair<size_t, size_t> name_segment(const std::string& head, size_t head_from, const std::string& gen_name,
                                           const std::string& script, size_t script_from,
                                           const std::string& script_name)
    {
        const size_t g = sppc::find_word(head, static_cast<uint32_t>(head_from), gen_name);
        const size_t p = sppc::find_word(script, static_cast<uint32_t>(script_from), script_name);
        if (gen_name.empty() || script_name.empty() || head.compare(g, gen_name.size(), gen_name) != 0 ||
            script.compare(p, script_name.size(), script_name) != 0)
            return {script_from, head_from};
        segs.push_back({static_cast<uint32_t>(p), static_cast<uint32_t>(script_name.size()),
                        static_cast<uint32_t>(s.size() + g), static_cast<uint32_t>(gen_name.size())});
        return {p + script_name.size(), g + gen_name.size()};
    }
    // Records the segments of a function head: the name and the parameter names (design 10.1).
    void head_segments(const std::string& head, const std::string& sym, const sppc::ast::Method& m,
                       const std::string& script)
    {
        if (m.pos == 0) return;
        auto [sp, gp] = name_segment(head, 0, sym, script, m.pos, m.name);
        if (gp == 0) return;
        for (const auto& prm : m.params) std::tie(sp, gp) = name_segment(head, gp, prm.name, script, sp, prm.name);
    }
    // Inserts text inside a line. The segments after the place move.
    void insert(size_t at, const std::string& t)
    {
        s.insert(at, t);
        for (auto& g : segs)
            if (g.gen_begin >= at) g.gen_begin += static_cast<uint32_t>(t.size());
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

using sppc::RCtx;

// The name of a function for the stale call log: "Cls::name" or "Cls::name(types)".
std::string body_what(const std::string& cn, const Method& m)
{
    std::string what = cn + "::" + m.name;
    if (!m.overrides_virtual) return what;
    what += "(";
    for (size_t i = 0; i < m.params.size(); ++i) what += (i ? ", " : "") + m.params[i].type;
    return what + ")";
}

// ------------------------------------------------------------------ script types
// Keeps the types as the script writes them. The translator rewrites "type" to the C++ storage
// type later. The type-directed lowering (lower.h) reads the script types. A type that is
// already set stays.
void snapshot_types(ClassDef& cd)
{
    auto keep = [](std::string& dst, const std::string& src) {
        if (dst.empty()) dst = src;
    };
    for (Field& f : cd.fields) keep(f.script_type, f.type);
    for (Property& p : cd.props) keep(p.script_type, p.type);
    auto params = [&](std::vector<Param>& ps) {
        for (Param& p : ps) keep(p.script_type, p.type);
    };
    for (Method& m : cd.methods) {
        keep(m.script_ret, m.ret);
        params(m.params);
    }
    for (sppc::ast::Ctor& c : cd.ctors) params(c.params);
}

// ------------------------------------------------------------------ properties
// Changes each property into script members:
//  - An auto property gets the hidden field "__p_Name". The field keeps the initializer.
//    The state migration treats it as a normal field.
//  - Each accessor becomes a script fn: "Type get_Name()" and "void set_Name(Type value)".
//    The body of an auto accessor reads or writes the hidden field.
// The property stays in cd.props. The lowering pass uses it to rewrite reads and writes.
void desugar_props(ClassDef& cd, const sppc::LineMap& map)
{
    std::set<std::string> prop_names;
    for (const Property& p : cd.props) {
        if (!prop_names.insert(p.name).second) fail_at(map, p.pos, "property '" + p.name + "' is declared twice");
        for (const Field& f : cd.fields)
            if (f.name == p.name) fail_at(map, p.pos, "'" + p.name + "' is a field and a property");
        for (const Method& m : cd.methods) {
            if (m.name == p.name) fail_at(map, p.pos, "'" + p.name + "' is a method and a property");
            for (const char* word : {"get", "set"})
                if (m.name == std::string(word) + "_" + p.name)
                    fail_at(map, m.pos, "method '" + m.name + "' collides with the '" + word +
                                            "' accessor of property '" + p.name + "'");
        }
    }
    for (Property& p : cd.props) {
        const std::string hidden = "__p_" + p.name;
        auto this_hidden = [&] { return make_expr(MemberExpr{make_expr(ThisExpr{p.pos}), hidden, p.pos}); };
        if (p.is_auto()) {
            Field f;
            f.name = hidden;
            f.type = p.type;
            f.init = std::move(p.init);
            f.init_span = p.init_span;
            f.has_init = p.has_init;
            f.access = Access::Private;
            f.line = p.line;
            f.pos = p.pos;
            cd.fields.push_back(std::move(f));
        }
        auto accessor = [&](bool is_get) {
            Method m;
            m.access = is_get ? p.get_access : p.set_access;
            m.name = (is_get ? "get_" : "set_") + p.name;
            m.ret = is_get ? p.type : std::string();
            if (!is_get) m.params.push_back({"value", p.type});
            m.pos = is_get ? p.get_pos : p.set_pos;
            m.line = m.end_line = map.line(m.pos);
            m.end_pos = m.pos;
            std::optional<BlockStmt>& body = is_get ? p.get_body : p.set_body;
            if (body) {
                m.body = std::move(*body);  // the optional keeps a value: the accessor is custom
            } else if (is_get) {
                m.body.stmts.push_back(make_stmt(ReturnStmt{this_hidden(), m.pos}));
            } else {
                BinaryExpr b;
                b.op = "=";
                b.lhs = this_hidden();
                b.rhs = make_expr(NameExpr{{"value"}, "", {}, false, m.pos});
                b.pos = m.pos;
                m.body.stmts.push_back(make_stmt(ExprStmt{make_expr(std::move(b)), m.pos}));
            }
            cd.methods.push_back(std::move(m));
        };
        if (p.has_get) accessor(true);
        if (p.has_set) accessor(false);
    }
    snapshot_types(cd);
}

// ------------------------------------------------------------------ driver
struct Slot {
    std::string id;  // "Tick", "TakeDamage_f", "heal"
    const NativeVirtual* nv = nullptr;
    const Method* impl = nullptr;  // override (native slot) or function (script slot)
    int intro_gen = 0;             // script functions only
    int index = -1;                // the slot number in a family of script classes, else -1
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
        {"int8_t", "i8"},   {"uint8_t", "u8"},     {"int16_t", "i16"},  {"uint16_t", "u16"},
        {"int32_t", "i"},   {"uint32_t", "u"},     {"int64_t", "i64"},  {"uint64_t", "u64"},
        {"intptr_t", "ip"}, {"uintptr_t", "up"},   {"char16_t", "w"},
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
// A file that changed lately also has a hash of its content. An edit can keep the time and
// the size, so the hash covers this case. A missing file has the time -1.
struct FileStamp {
    std::string path;
    long long mtime_ns = -1;
    long long size = -1;
    bool hashed = false;
    unsigned long long hash = 0;
};

// A file is recent when its time is less than this value before the check.
constexpr long long kRecentNs = 2000000000LL;

long long now_ns()
{
    struct timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// FNV-1a hash of the content of a file.
unsigned long long hash_file(const std::string& path)
{
    unsigned long long h = 14695981039346656037ULL;
    std::ifstream f(path, std::ios::binary);
    char buf[8192];
    while (f.read(buf, sizeof buf) || f.gcount() > 0) {
        for (std::streamsize i = 0; i < f.gcount(); ++i) {
            h ^= static_cast<unsigned char>(buf[i]);
            h *= 1099511628211ULL;
        }
    }
    return h;
}

// Reads the stamp. A file with a recent time also gets the hash of its content.
FileStamp stamp_of(const std::string& path)
{
    FileStamp f;
    f.path = path;
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) {
        f.mtime_ns = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
        f.size = static_cast<long long>(st.st_size);
        if (f.mtime_ns > now_ns() - kRecentNs) {
            f.hashed = true;
            f.hash = hash_file(path);
        }
    }
    return f;
}

// True when the file is the same as at the time of the record. The check reads the content
// only when the record has a hash. It drops the hash when the file is no longer recent.
bool stamp_fresh(FileStamp& rec)
{
    struct stat st;
    long long mtime = -1, size = -1;
    if (::stat(rec.path.c_str(), &st) == 0) {
        mtime = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
        size = static_cast<long long>(st.st_size);
    }
    if (mtime != rec.mtime_ns || size != rec.size) return false;
    if (rec.hashed) {
        if (hash_file(rec.path) != rec.hash) return false;
        if (mtime <= now_ns() - kRecentNs) rec.hashed = false;
    }
    return true;
}

// Adds the stamps of the paths that a search of the include dirs tried before it found
// "rel". A new file at one of these paths would hide the file that the parse used.
void add_shadow_stamps(const std::string& rel, const std::vector<std::string>& dirs,
                       std::set<std::string>& seen, std::vector<FileStamp>& files)
{
    for (const auto& d : dirs) {
        const std::string cand = d + "/" + rel;
        struct stat st;
        if (::stat(cand.c_str(), &st) == 0) return;  // the first file that exists
        if (seen.insert(cand).second) files.push_back(stamp_of(cand));
    }
}

// The handle type of a stub: the template with "$T" replaced by the class name.
std::string handle_type(std::string tpl, const std::string& name)
{
    for (size_t p = 0; (p = tpl.find("$T", p)) != std::string::npos; p += name.size()) tpl.replace(p, 2, name);
    return tpl;
}

// The name of the probe section with the facts. libclang reports an error in it for a type
// that it does not know. The translator does not treat such an error as an error of the imports.
const char* const kFactsFile = "sppc_facts";

// The text that asks libclang for the facts of the types. The section follows the imports.
// Each fact is one constexpr variable on its own line. The variable "sppc_f<N>" is on the
// line N of the file "sppc_facts". The traits are in functions with "if constexpr", so a
// type that is not complete gives false and no error inside a standard header.
std::string facts_text(const std::vector<FactReq>& reqs, std::vector<std::pair<size_t, std::string>>& lines)
{
    std::string head =
        "#include <type_traits>\n"
        "template <class T> constexpr bool sppc_complete() { return requires { sizeof(T); }; }\n"
        "template <class T> constexpr bool sppc_copy() { if constexpr (sppc_complete<T>()) return std::is_copy_constructible_v<T>; else return false; }\n"
        "template <class T> constexpr bool sppc_nothrow_move() { if constexpr (sppc_complete<T>()) return std::is_nothrow_move_constructible_v<T>; else return false; }\n"
        "template <class T> constexpr bool sppc_polymorphic() { if constexpr (sppc_complete<T>()) return std::is_polymorphic_v<T>; else return false; }\n"
        "template <class T> constexpr bool sppc_final() { if constexpr (sppc_complete<T>()) return std::is_final_v<T>; else return false; }\n"
        "template <class T> constexpr bool sppc_plain() { if constexpr (sppc_complete<T>()) return std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>; else return false; }\n"
        "template <class T> constexpr bool sppc_small() { if constexpr (sppc_complete<T>()) return std::is_trivially_copyable_v<T> && sizeof(T) <= 16; else return false; }\n"
        "template <class T> constexpr bool sppc_arrow() { if constexpr (std::is_pointer_v<T>) return true; else if constexpr (sppc_complete<T>()) return requires(T& h) { h.operator->(); }; else return false; }\n";
    std::string body;
    lines.clear();
    size_t member_id = 0;
    for (size_t i = 0; i < reqs.size(); ++i) {
        const std::string& t = reqs[i].type;
        const char* kinds[] = {"is_class", "copy", "nothrow_move", "polymorphic", "final", "plain", "small", "arrow"};
        for (const char* k : kinds) {
            lines.emplace_back(i, k);
            const std::string fn = std::string(k) == "is_class" ? "std::is_class_v<" + t + ">"
                                                                : std::string("sppc_") + k + "<" + t + ">()";
            body += "constexpr bool sppc_f" + std::to_string(lines.size()) + " = " + fn + ";\n";
        }
        for (const std::string& m : reqs[i].members) {
            const std::string fn = "sppc_member" + std::to_string(member_id++);
            head += "template <class T> constexpr bool " + fn + "() { return requires { &T::" + m + "; }; }\n";
            lines.emplace_back(i, "member:" + m);
            body += "constexpr bool sppc_f" + std::to_string(lines.size()) + " = " + fn + "<" + t + ">();\n";
        }
    }
    return head + "#line 1 \"" + kFactsFile + "\"\n" + body;
}

// Reads the value of "sppc_f<N>" from the translation unit. Gives -1 when libclang cannot
// evaluate it (the type is not known).
int eval_bool(CXCursor var)
{
    CXEvalResult r = clang_Cursor_Evaluate(var);
    int v = -1;
    if (r && clang_EvalResult_getKind(r) == CXEval_Int) v = clang_EvalResult_getAsInt(r) != 0 ? 1 : 0;
    if (r) clang_EvalResult_dispose(r);
    return v;
}

// Parses the imports with libclang. Reads the base classes in "names" that it finds.
// Gives the stamps of all files that the parse included.
void parse_bases(const std::string& probe, const std::vector<std::string>& args,
                 const std::set<std::string>& names, const std::set<std::string>& import_paths,
                 std::map<std::string, BaseInfo>& out, std::vector<FileStamp>& files,
                 Hierarchy& hierarchy, const std::vector<FactReq>& reqs, FactMap& facts)
{
    std::vector<std::pair<size_t, std::string>> fact_lines;
    const std::string full_probe = reqs.empty() ? probe : probe + facts_text(reqs, fact_lines);
    std::vector<const char*> cargs;
    for (const auto& a : args) cargs.push_back(a.c_str());
    ClangHandles ch;
    CXIndex idx = ch.idx = clang_createIndex(0, 0);
    CXUnsavedFile unsaved{"sppc_probe.cpp", full_probe.c_str(), static_cast<unsigned long>(full_probe.size())};
    CXTranslationUnit tu = nullptr;
    CXErrorCode ec = clang_parseTranslationUnit2(idx, "sppc_probe.cpp", cargs.data(),
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
            if (cxs(file) == kFactsFile) {  // an unknown type of a fact: not an error of the imports
                clang_disposeDiagnostic(d);
                continue;
            }
            const char* sev = clang_getDiagnosticSeverity(d) == CXDiagnostic_Fatal ? "fatal error" : "error";
            g_diag.add_foreign(cxs(file), static_cast<int>(line), static_cast<int>(col), sev,
                               cxs(clang_getDiagnosticSpelling(d)));
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

    // A new header earlier in the include path hides a header that the parse used.
    std::vector<std::string> dirs;
    for (const auto& a : args)
        if (a.size() > 2 && a.compare(0, 2, "-I") == 0) dirs.push_back(a.substr(2));
    for (const auto& ip : import_paths)
        if (!ip.empty() && ip.front() != '<') add_shadow_stamps(ip, dirs, seen, files);
        else if (ip.size() > 2) add_shadow_stamps(ip.substr(1, ip.size() - 2), dirs, seen, files);
    const std::vector<FileStamp> used = files;
    for (const FileStamp& f : used)
        for (const auto& d : dirs)
            if (f.mtime_ns >= 0 && f.path.size() > d.size() + 1 && f.path.compare(0, d.size() + 1, d + "/") == 0) {
                add_shadow_stamps(f.path.substr(d.size() + 1), dirs, seen, files);
                break;
            }

    CXCursor root = clang_getTranslationUnitCursor(tu);
    if (!reqs.empty()) {
        std::map<std::string, CXCursor> vars;
        visit(root, [&](CXCursor c) {
            if (clang_getCursorKind(c) == CXCursor_VarDecl) {
                const std::string n = cxs(clang_getCursorSpelling(c));
                if (n.compare(0, 6, "sppc_f") == 0) vars[n] = c;
            }
            return CXChildVisit_Continue;
        });
        for (size_t n = 0; n < fact_lines.size(); ++n) {
            auto it = vars.find("sppc_f" + std::to_string(n + 1));
            const int v = it == vars.end() ? -1 : eval_bool(it->second);
            TypeFacts& f = facts[reqs[fact_lines[n].first].type];
            const std::string& k = fact_lines[n].second;
            if (k == "is_class") {
                f.known = v >= 0;
                f.is_class = v == 1;
            } else if (k == "copy") f.copy = v == 1;
            else if (k == "nothrow_move") f.nothrow_move = v == 1;
            else if (k == "polymorphic") f.polymorphic = v == 1;
            else if (k == "final") f.final_ = v == 1;
            else if (k == "plain") f.plain = v == 1;
            else if (k == "small") f.small = v == 1;
            else if (k == "arrow") f.arrow = v == 1;
            else f.members[k.substr(7)] = v == 1;
        }
    }
    hierarchy.clear();
    collect_hierarchy(root, hierarchy);
    for (const auto& name : names) {
        CXCursor base_cur = find_class(root, name);
        if (clang_Cursor_isNull(base_cur)) continue;
        BaseInfo bi;
        bi.name = name;
        collect_virtuals(base_cur, bi);
        collect_ctors(base_cur, bi);
        visit(base_cur, [&](CXCursor a) {
            if (clang_getCursorKind(a) == CXCursor_CXXFinalAttr) bi.final_cls = true;
            return CXChildVisit_Continue;
        });
        out[name] = std::move(bi);
    }
}

}  // namespace

namespace sppc {

// The cache of one key (flags + imports): the base classes and the files that the parse read.
struct TranslatorSession::Impl {
    struct Entry {
        std::vector<FileStamp> files;
        std::map<std::string, BaseInfo> bases;
        Hierarchy hierarchy;
        FactMap facts;
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

}  // namespace sppc

namespace {

// Gives the base classes for one key. A cache hit needs all names in the entry and
// the same stamp for each file. Otherwise it parses again and keeps the new result.
std::map<std::string, BaseInfo> session_bases(
    sppc::TranslatorSession::Impl& S, const std::string& key, const std::set<std::string>& names,
    const std::vector<FactReq>& reqs,
    const std::function<void(std::map<std::string, BaseInfo>&, std::vector<FileStamp>&, Hierarchy&, FactMap&)>& parse,
    Hierarchy& hierarchy, FactMap& facts_out)
{
    std::lock_guard<std::mutex> lock(S.mutex);
    auto& e = S.entries[key];
    bool fresh = !e.files.empty();
    for (size_t i = 0; fresh && i < e.files.size(); ++i) fresh = stamp_fresh(e.files[i]);
    if (!fresh) e = {};
    bool complete = fresh;
    for (const auto& n : names) complete = complete && e.bases.count(n);
    for (const FactReq& r : reqs) {
        auto it = e.facts.find(r.type);
        complete = complete && it != e.facts.end();
        for (const std::string& m : r.members) complete = complete && it != e.facts.end() && it->second.members.count(m);
    }
    if (!complete) {
        std::map<std::string, BaseInfo> out = std::move(e.bases);
        FactMap facts = std::move(e.facts);
        std::vector<FileStamp> files;
        Hierarchy hier;
        ++S.parses;
        try {
            parse(out, files, hier, facts);
        } catch (...) {
            S.entries.erase(key);
            throw;
        }
        e.bases = std::move(out);
        e.files = std::move(files);
        e.hierarchy = std::move(hier);
        e.facts = std::move(facts);
    }
    hierarchy = e.hierarchy;
    for (const FactReq& r : reqs) facts_out[r.type] = e.facts[r.type];
    std::map<std::string, BaseInfo> r;
    for (const auto& n : names) {
        auto it = e.bases.find(n);
        if (it != e.bases.end()) r.emplace(n, it->second);
    }
    return r;
}

void translate_impl(const sppc::TranslateOptions& in, sppc::TranslatorSession::Impl& session,
                    sppc::TranslateResult& res)
{
    Options opt;
    opt.input = in.script_name;
    opt.output = in.output_name;
    opt.gen = in.generation;
    opt.includes = in.include_dirs;
    opt.resource_dir = in.resource_dir;
#ifdef SPP_RESOURCE_DIR
    if (opt.resource_dir.empty()) opt.resource_dir = SPP_RESOURCE_DIR;
#endif
    if (opt.gen < 1) fail(0, "the generation must be 1 or more");
    if (in.format_lib != "std" && in.format_lib != "fmt") fail(0, "the format library must be 'std' or 'fmt'");
    if (opt.output.empty()) opt.output = opt.input + ".cpp";
    g_script = opt.input;

    const sppc::LineMap map(in.script_text);
    Script sc = parse_structure(in.script_text, map);

    // ---- the type model: the preludes first, then the stubs of the script
    sppc::TypeModel types;
    // A stub with the place that holds it. The checks of the stubs (7.10.7) report there.
    struct StubSrc {
        const Stub* stub;
        const sppc::LineMap* map;
        std::string file;  // empty: the script
    };
    std::vector<StubSrc> all_stubs;
    struct PreludeData {
        std::string name;
        Script script;
        sppc::LineMap map;
    };
    std::deque<PreludeData> prelude_data;  // the addresses must stay
    for (const auto& [pname, ptext] : in.preludes) {
        const sppc::LineMap pmap(ptext);
        sppc::Diagnostics pd;
        Script ps = sppc::parse_script(ptext, pmap, pd);
        if (pd.items.empty() && !ps.classes.empty()) {
            const ClassDef& pc = ps.classes.front();
            pd.add(pc.pos, pmap, "a prelude can hold only stubs: '" + pname + "' has the class '" + pc.name + "'");
        }
        if (pd.items.empty() && !ps.enums.empty())
            pd.add(ps.enums.front().pos, pmap, "a prelude can hold only stubs: '" + pname + "' has an enum");
        if (pd.items.empty() && !ps.vars.empty())
            pd.add(ps.vars.front().pos, pmap, "a prelude can hold only stubs: '" + pname + "' has a module variable");
        if (pd.items.empty()) types.add_stubs(ps.stubs, pd, pmap, sppc::TypeModel::Source::Prelude);
        if (!pd.items.empty()) {
            for (auto& d : pd.items) {
                d.file = pname;
                g_diag.items.push_back(std::move(d));
            }
            throw TranslateAbort{};
        }
        prelude_data.push_back({pname, std::move(ps), pmap});
        for (const Stub& st : prelude_data.back().script.stubs)
            all_stubs.push_back({&st, &prelude_data.back().map, pname});
    }
    types.add_stubs(sc.stubs, g_diag, map, sppc::TypeModel::Source::Script);
    if (!g_diag.items.empty()) throw TranslateAbort{};
    for (const Stub& st : sc.stubs) all_stubs.push_back({&st, &map, std::string()});
    // ---- script enums (design 5.3, 7.2). An enum becomes a C++ "enum class". A field of an
    // enum type reloads as its underlying integer type.
    std::map<std::string, std::string> enum_types;  // the name and the underlying type
    {
        static const std::set<std::string> integer_types = {"int8_t",  "uint8_t",  "int16_t", "uint16_t",
                                                            "int32_t", "uint32_t", "int64_t", "uint64_t"};
        std::set<std::string> class_names;
        for (const ClassDef& cd : sc.classes) class_names.insert(cd.name);
        for (sppc::ast::EnumDef& e : sc.enums) {
            if (class_names.count(e.name)) fail_at(map, e.pos, "'" + e.name + "' is a class and an enum");
            if (e.underlying.empty()) e.underlying = "int32_t";
            if (!integer_types.count(e.underlying))
                fail_at(map, e.underlying_pos ? e.underlying_pos : e.pos,
                        "the type of an enum must be sbyte, byte, short, ushort, int, uint, long or ulong");
            if (!enum_types.emplace(e.name, e.underlying).second)
                fail_at(map, e.pos, "enum '" + e.name + "' is declared twice");
            std::set<std::string> names;
            for (const sppc::ast::Enumerator& m : e.items)
                if (!names.insert(m.name).second)
                    fail_at(map, m.pos, "enumerator '" + m.name + "' is declared twice in enum '" + e.name + "'");
        }
    }
    // ---- module variables (design 5.3, 7.3). They live in the module state block, which is
    // the state of a synthetic [value] class. The block migrates like the fields of a class.
    const bool has_module = !sc.vars.empty();
    std::map<std::string, std::string> module_vars;  // the name and the script type
    if (has_module) {
        ClassDef mod;
        mod.name = kModuleName;
        mod.is_value = true;
        mod.pos = sc.vars.front().pos;
        mod.line = sc.vars.front().line;
        std::set<std::string> names;
        for (const ClassDef& cd : sc.classes)
            if (cd.name == kModuleName) fail_at(map, cd.pos, "the class name '" + kModuleName + "' is reserved");
        for (Field& f : sc.vars) {
            if (!names.insert(f.name).second) fail_at(map, f.pos, "module variable '" + f.name + "' is declared twice");
            module_vars[f.name] = f.type;
        }
        mod.fields = std::move(sc.vars);
        sc.vars.clear();
        sc.classes.push_back(std::move(mod));
    }
    // Convert hooks of module variables (DECISIONS.md item 14). The module class emits them.
    std::vector<Method> mod_hooks = std::move(sc.var_hooks);
    sc.var_hooks.clear();
    for (const Method& m : mod_hooks)
        if (!module_vars.count(m.name.substr(8)))
            fail_at(map, m.pos, "'" + m.name + "' is not a convert hook: the script has no module variable '" +
                                    m.name.substr(8) + "'");
    types.add_script_classes(sc.classes);
    for (ClassDef& cd : sc.classes) snapshot_types(cd);
    for (const ClassDef& cd : sc.classes) {
        if (!cd.is_value) continue;
        // A [value] class is a plain struct (design 7.10.1): no base, no virtual method, no hook.
        if (!cd.base.empty())
            fail_at(map, cd.pos, "a [value] class '" + cd.name + "' has no base class");
        if (!cd.dtors.empty())
            fail_at(map, cd.dtors[0].pos, "a [value] class '" + cd.name + "' cannot have a destructor");
        for (const Method& m : cd.methods) {
            if (m.is_override || m.is_virtual)
                fail_at(map, m.pos, "a [value] class '" + cd.name + "' has no virtual methods");
            if (m.name == "onReload" || m.name.rfind("convert_", 0) == 0)
                fail_at(map, m.pos, "a [value] class '" + cd.name + "' cannot have the hook '" + m.name + "'");
        }
        for (const sppc::ast::Ctor& c : cd.ctors)
            if (c.has_super)
                fail_at(map, c.pos, "a [value] class '" + cd.name + "' has no base class: remove 'super(...)'");
    }
    for (const ClassDef& cd : sc.classes)
        if (cd.base.empty() && !cd.is_value)
            for (const sppc::ast::Ctor& c : cd.ctors)
                if (c.has_super)
                    fail_at(map, c.pos, "class '" + cd.name + "' has no base class: remove 'super(...)'");

    // ---- script inheritance (design 4.5). A base that is a script class of this script is a
    // script parent. The root of a chain is the first class without a script parent. Its base
    // is the native base of every class of the chain.
    std::map<std::string, ClassDef*> class_by_name;
    for (ClassDef& cd : sc.classes) class_by_name[cd.name] = &cd;
    auto script_parent = [&](const ClassDef& cd) -> ClassDef* {
        if (cd.base.empty()) return nullptr;
        auto it = class_by_name.find(cd.base);
        return it == class_by_name.end() ? nullptr : it->second;
    };
    for (const ClassDef& cd : sc.classes) {
        const ClassDef* par = script_parent(cd);
        if (!par) continue;
        if (par == &cd) fail_at(map, cd.pos, "class '" + cd.name + "' cannot derive from itself");
        if (par->is_value)
            fail_at(map, cd.pos, "class '" + cd.name + "' cannot derive from the [value] class '" + par->name + "'");
        if (cd.is_value)
            fail_at(map, cd.pos, "the [value] class '" + cd.name + "' cannot derive from the script class '" + par->name + "'");
    }
    // Native interface bases (design 4.4): "class Boss : Actor, IDamageable". The primary base
    // comes first. An interface base is a native class. Only the root of a family has them; the
    // children get them through the shell of the root. Design 4.5 forbids them on a child: the
    // shell of a child adds no overrides, and the native slots of a family come before its
    // script slots, so a child has no place for the slots of a new native base.
    for (const ClassDef& cd : sc.classes) {
        if (cd.interfaces.empty()) continue;
        if (const ClassDef* par = script_parent(cd)) {
            const ClassDef* root = par;
            for (int depth = 0; script_parent(*root) && depth < 64; ++depth) root = script_parent(*root);
            fail_at(map, cd.pos, "class '" + cd.name + "' has the script parent '" + par->name +
                                     "'. Only a class with a native base can have interface bases: the shell of a "
                                     "child adds no overrides (design 4.5). Add '" + cd.interfaces.front() +
                                     "' to the bases of the root class '" + root->name + "'");
        }
        std::set<std::string> seen = {cd.base};
        for (const std::string& i : cd.interfaces) {
            if (class_by_name.count(i))
                fail_at(map, cd.pos, "the interface base '" + i + "' of class '" + cd.name +
                                         "' is a script class. An interface base must be a native class");
            if (!seen.insert(i).second)
                fail_at(map, cd.pos, "class '" + cd.name + "' names the base '" + i + "' two times");
        }
    }
    auto script_root = [&](const ClassDef& cd) {
        const ClassDef* r = &cd;
        for (int depth = 0; script_parent(*r) && depth < 64; ++depth) r = script_parent(*r);
        return r;
    };
    std::set<std::string> has_children;
    for (const ClassDef& cd : sc.classes)
        if (const ClassDef* par = script_parent(cd)) has_children.insert(par->name);

    // ---- the questions for libclang about the types (design 7.10.7 and the stub "class *;")
    std::map<std::string, std::set<std::string>> asked;  // type text, member names
    auto is_exact = [](const std::string& n) {
        return n != "*" && n.find('*') == std::string::npos;
    };
    for (const StubSrc& ss : all_stubs) {
        const Stub& st = *ss.stub;
        if (!is_exact(st.name)) continue;
        asked[st.name];
        if (!st.intrusive_add.empty() || !st.intrusive_release.empty()) {
            asked[st.name].insert(st.intrusive_add);
            asked[st.name].insert(st.intrusive_release);
        } else if (!st.handle.empty()) {
            asked[handle_type(st.handle, st.name)];
        }
    }
    for (const std::string& t : sppc::used_type_names(sc))
        if (types.wants_facts(t)) asked[t];
    std::vector<FactReq> reqs;
    for (auto& [t, ms] : asked) reqs.push_back({t, std::vector<std::string>(ms.begin(), ms.end())});

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
    for (const auto& cd : sc.classes) {
        if (!cd.base.empty() && !class_by_name.count(cd.base)) base_names.insert(cd.base);
        for (const std::string& i : cd.interfaces) base_names.insert(i);
    }
    // The key: the flags, the current directory and the imports (the order does not matter).
    std::string key = std::filesystem::current_path().string() + "\n";
    for (const auto& a : args) key += a + "\n";
    for (const auto& p : import_paths) key += "#" + p + "\n";
    Hierarchy hier;  // must outlive every use of "types"
    FactMap facts;
    const std::map<std::string, BaseInfo> bases =
        session_bases(session, key, base_names, reqs,
                      [&](std::map<std::string, BaseInfo>& out, std::vector<FileStamp>& files, Hierarchy& h,
                          FactMap& fm) {
                          parse_bases(probe, args, base_names, import_paths, out, files, h, reqs, fm);
                      },
                      hier, facts);
    types.set_classes([&facts](const std::string& name) {
        auto it = facts.find(name);
        sppc::TypeModel::ClassFacts c;
        if (it != facts.end()) {
            c.known = it->second.known && it->second.is_class;
            c.plain = it->second.plain;
            c.small = it->second.small;
        }
        return c;
    });

    // ---- the checks of the stubs (design 7.10.7). A type that libclang does not know is skipped.
    for (const StubSrc& ss : all_stubs) {
        const Stub& st = *ss.stub;
        if (!is_exact(st.name)) continue;
        auto err = [&](const std::string& msg) {
            g_diag.add(st.pos, *ss.map, msg);
            g_diag.items.back().file = ss.file;
        };
        auto tf = facts.find(st.name);
        if (tf == facts.end() || !tf->second.known) continue;
        const TypeFacts& f = tf->second;
        if (st.is_value) {
            if (!f.copy) err("'value' needs a copy constructor: '" + st.name + "' has none");
            if (!f.nothrow_move)
                err("'value' needs a nothrow move constructor: '" + st.name +
                    "' has none. The reload moves the state block with it");
            if (f.polymorphic && !f.final_)
                err("'value' on the polymorphic type '" + st.name +
                    "' is not allowed, because a copy can slice. Mark the type 'final' or use a handle");
        }
        if (!st.intrusive_add.empty() || !st.intrusive_release.empty()) {
            for (const std::string& m : {st.intrusive_add, st.intrusive_release}) {
                auto mit = f.members.find(m);
                if (mit != f.members.end() && !mit->second)
                    err("'intrusive' method '" + m + "' is not a usable member of '" + st.name +
                        "'. It must exist and must not be overloaded");
            }
        } else if (!st.handle.empty()) {
            const std::string ht = handle_type(st.handle, st.name);
            auto hit = facts.find(ht);
            if (hit != facts.end() && hit->second.known && !hit->second.arrow)
                err("'handle' needs 'operator->': the handle type '" + ht + "' has none");
        }
    }
    if (!g_diag.items.empty()) throw TranslateAbort{};


    types.set_bases([&hier](const std::string& name) {
        auto it = hier.find(name);
        return it == hier.end() ? std::vector<std::string>{} : it->second;
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

    const bool multi = sc.classes.size() - (has_module ? 1 : 0) > 1;
    Out o;
    bool uses_format = false;  // the lowering sets it when a string uses "${...}"
    o.w("// Generated by sppc from " + opt.input + " (generation " + std::to_string(opt.gen) +
        "). Do not edit.\n");
    o.w("#include <cstddef>\n#include <cstdint>\n#include <cstdio>\n"
        "#include <new>\n#include <string>\n#include <type_traits>\n#include <typeinfo>\n"
        "#include <utility>\n");
    // The blank line below is a place for the include of the format library. A line stays a line,
    // so the line numbers do not change.
    const size_t format_include_pos = o.s.size();
    o.w("\n#include \"spp/runtime.h\"\n\n");
    auto reset = [&] { o.w("#line " + std::to_string(o.lines + 2) + " \"" + esc(opt.output) + "\"\n"); };
    for (const auto& im : sc.imports) {
        o.w("#line " + std::to_string(im.line) + " \"" + esc(opt.input) + "\"\n");
        o.w("#include " + im.target() + "\n");
    }
    reset();
    o.w("\nconstexpr uint32_t GEN = " + std::to_string(opt.gen) + ";\n");

    // ---- script enums. The enum is a C++ "enum class" with the underlying type of the script.
    {
        const ClassDef no_class;
        for (sppc::ast::EnumDef& e : sc.enums) {
            RCtx ec = sppc::make_class_ctx(no_class);
            ec.map = &map;
            ec.types = &types;
            ec.allow_this = false;
            ec.format_lib = in.format_lib;
            ec.uses_format = &uses_format;
            o.w("\n#line " + std::to_string(e.line) + " \"" + esc(opt.input) + "\"\n");
            o.w("enum class " + e.name + " : " + e.underlying + " {\n");
            for (sppc::ast::Enumerator& m : e.items) {
                o.w("#line " + std::to_string(m.line) + " \"" + esc(opt.input) + "\"\n");
                o.w("    " + m.name);
                if (m.has_value && m.value) {
                    sppc::lower_expr(m.value, ec, g_diag);
                    if (!g_diag.items.empty()) throw TranslateAbort{};
                    o.w(" = " + sppc::ast::print(*m.value));
                }
                o.w(",\n");
            }
            o.w("};\n");
            reset();
        }
    }

    std::vector<std::pair<std::string, bool>> registers;  // class name, for the register function
    std::vector<std::string> thunk_regs;
    // The script classes that a "new" expression names (design 7.3). They get a "new" function.
    const std::set<std::string> new_set = sppc::new_targets(sc);
    std::set<std::string> new_ready;  // the classes with a "new" function that are emitted so far

    // ---- [value] script classes (design 7.10.1). A value class is a C++ struct with real
    // copy semantics. Its member functions are inline in the generation module, so a reload
    // replaces them with the module. Its field table lets the migration recurse (design 5.3).
    const std::set<std::string> used_types = sppc::used_type_names(sc);
    std::set<std::string> script_names;
    for (const ClassDef& cd : sc.classes) script_names.insert(cd.name);
    auto param_text = [](const std::vector<sppc::ast::Param>& ps) {
        std::string r;
        for (const sppc::ast::Param& p : ps) r += ", " + p.type + " " + p.name;
        return r;
    };
    // The reload type key of a field type. An enum is its underlying integer type (design 5.3).
    auto key_type = [&](const std::string& t) {
        return enum_types.count(t) ? "std::underlying_type_t<" + t + ">" : t;
    };
    auto emit_value = [&](ClassDef& cd) {
        const std::string cn = cd.name, P = cn + "__";
        desugar_props(cd, map);
        {
            const size_t before = g_diag.items.size();
            sppc::check_ctors(cd, map, g_diag);
            if (g_diag.items.size() != before) throw TranslateAbort{};
        }
        auto reserved = [&](uint32_t pos, const std::vector<sppc::ast::Param>& ps) {
            for (const Param& p : ps)
                if (p.name == "self" || p.name == "st" || p.name == "a")
                    fail_at(map, pos, "parameter name '" + p.name + "' is reserved");
        };
        std::set<std::string> field_names, fn_names;
        for (const Field& f : cd.fields)
            if (!field_names.insert(f.name).second) fail_at(map, f.pos, "field '" + f.name + "' is declared twice");
        for (Method& m : cd.methods) {
            reserved(m.pos, m.params);
            if (!fn_names.insert(m.name).second)
                fail_at(map, m.pos, "function '" + m.name + "' is declared twice (overloads are not supported)");
            if (field_names.count(m.name)) fail_at(map, m.pos, "'" + m.name + "' is a field and a function");
        }
        for (const sppc::ast::Ctor& c : cd.ctors) reserved(c.pos, c.params);
        {
            const size_t before = g_diag.items.size();
            sppc::check_class(cd, types, map, g_diag, in.strict_borrowed);
            if (g_diag.items.size() != before) throw TranslateAbort{};
        }
        for (Field& f : cd.fields) f.type = types.storage(f.type);
        for (Method& m : cd.methods) {
            for (Param& p : m.params) p.type = p.mode == PassMode::Value ? types.param(p.type) : types.ref_param(p.type);
            if (!m.ret.empty()) m.ret = types.storage(m.ret);
        }
        for (sppc::ast::Ctor& c : cd.ctors)
            for (Param& p : c.params) p.type = types.param(p.type);
        // The convert hooks of the module variables: the same rules as a convert hook of a field.
        const bool is_module = cn == kModuleName;
        std::map<std::string, Method*> hooks;  // by variable name
        if (is_module)
            for (Method& m : mod_hooks) {
                if (m.is_override || m.is_virtual || m.is_new)
                    fail_at(map, m.pos, "'" + m.name + "' is a convert hook. It takes no modifier");
                if (m.params.size() != 1 || m.params[0].mode != PassMode::Value)
                    fail_at(map, m.pos, "'" + m.name + "' is a convert hook. It takes one parameter: the old value");
                if (m.ret.empty() || m.ret == "void")
                    fail_at(map, m.pos, "'" + m.name + "' is a convert hook. It must return the new variable type");
                if (!hooks.emplace(m.name.substr(8), &m).second)
                    fail_at(map, m.pos, "the convert hook '" + m.name + "' is declared twice");
                m.params[0].type = types.storage(m.params[0].type);
                m.ret = types.storage(m.ret);
                for (const Field& f : cd.fields)
                    if (f.name == m.name.substr(8) && f.type != m.ret)
                        fail_at(map, m.pos, "'" + m.name + "' must return the type of the module variable: '" + f.type + "'");
            }

        new_ready.insert(cn);
        RCtx base_cx = sppc::make_class_ctx(cd);
        base_cx.map = &map;
        base_cx.types = &types;
        base_cx.script_classes = script_names;
        for (const auto& sc_cls : sc.classes) base_cx.classes[sc_cls.name] = &sc_cls;
        base_cx.new_classes = new_ready;
        base_cx.format_lib = in.format_lib;
        base_cx.uses_format = &uses_format;
        base_cx.module_vars = module_vars;
        base_cx.in_module_init = cn == kModuleName;

        o.w("\n// ---------------------------------------------------------------- [value] class " + cn + "\n");
        // The field initializers: the text is the same in the constructor and in the field table.
        std::vector<std::string> inits;
        std::set<std::string> earlier;  // the module variables before this one
        for (Field& f : cd.fields) {
            RCtx fc = base_cx;
            fc.allow_this = false;
            fc.module_earlier = earlier;
            std::string init;
            if (f.has_init && f.init) {
                sppc::lower_expr(f.init, fc, g_diag);
                if (!g_diag.items.empty()) throw TranslateAbort{};
                init = sppc::ast::print(*f.init);
            }
            inits.push_back(init);
            earlier.insert(f.name);
        }
        const bool has_ctor = !cd.ctors.empty();
        const sppc::ast::Ctor* ctor = has_ctor ? &cd.ctors[0] : nullptr;
        auto arg_names = [](const std::vector<sppc::ast::Param>& ps) {
            std::string r;
            for (size_t i = 0; i < ps.size(); ++i) r += (i ? ", " : "") + arg_expr(ps[i].type, ps[i].name);
            return r;
        };
        auto fn_ret = [](const Method& m) { return m.ret.empty() ? std::string("void") : m.ret; };
        o.w("struct " + cn + ";\n");
        if (is_module) {
            // The module state block. The loader sets the slot of each generation (design 4.7).
            // An initializer and a function of the module read the block through spp_mod().
            o.w("// The module state block of this generation. The loader sets the slot.\n");
            o.w("static void* " + cn + "_slot = nullptr;\n");
            o.w("static inline " + cn + "* spp_mod() { return static_cast<" + cn + "*>(" + cn + "_slot); }\n");
        }
        for (const Method& m : cd.methods)
            o.w("static " + fn_ret(m) + " " + P + m.name + "(" + cn + "* self" + param_text(m.params) + ");\n");
        if (has_ctor) o.w("static void " + P + "ctor(" + cn + "* self" + param_text(ctor->params) + ");\n");
        o.w("struct " + cn + " {\n");
        for (const Field& f : cd.fields) o.w("    " + f.type + " " + f.name + ";\n");
        if (is_module) {
            // The loader builds the module block with the init functions of the field table, in
            // declaration order. An initializer can read an earlier variable through spp_mod().
            o.w("};\n\n");
        } else {
        o.w("    " + cn + "()");
        for (size_t i = 0; i < cd.fields.size(); ++i) {
            o.w("\n#line " + std::to_string(cd.fields[i].line) + " \"" + esc(opt.input) + "\"\n");
            o.w(std::string(i ? "        , " : "        : ") + cd.fields[i].name + "(" + inits[i] + ")");
        }
        if (!cd.fields.empty()) {
            o.w("\n");
            reset();
        }
        if (has_ctor && ctor->params.empty()) o.w(" { " + P + "ctor(this); }\n");
        else o.w(" {}\n");
        if (has_ctor && !ctor->params.empty())
            o.w("    " + cn + "(" + param_text(ctor->params).substr(2) + ") : " + cn + "() { " + P + "ctor(this, " +
                arg_names(ctor->params) + "); }\n");
        for (const Method& m : cd.methods) {
            std::string ps = param_text(m.params);
            if (!ps.empty()) ps = ps.substr(2);
            o.w("    " + fn_ret(m) + " " + m.name + "(" + ps + ") { " + (fn_ret(m) == "void" ? "" : "return ") + P +
                m.name + "(this" + (m.params.empty() ? "" : ", " + arg_names(m.params)) + "); }\n");
        }
        o.w("};\n\n");
        }
        // A convert hook of a module variable (design 4.7). The wrapper builds the new value.
        for (const auto& [vname, m] : hooks) {
            o.w("static " + m->ret + " " + P + m->name + "(" + m->params[0].type + " " + m->params[0].name + ");\n");
            o.w("static void " + P + m->name + "_hook(void* dst, const void* src)\n{\n    new (dst) " + m->ret + "(" + P +
                m->name + "(*static_cast<const " + m->params[0].type + "*>(src)));\n}\n");
        }

        // The field table of the struct: the migration recurses into it.
        for (size_t i = 0; i < cd.fields.size(); ++i) {
            const Field& f = cd.fields[i];
            o.w("#line " + std::to_string(f.line) + " \"" + esc(opt.input) + "\"\n");
            o.w("static void " + P + "init_" + f.name + "(void*, void* f) { using T = " + f.type + "; new (f) T(" + inits[i] +
                "); }\n");
            reset();
        }
        o.w("#pragma clang diagnostic push\n#pragma clang diagnostic ignored \"-Winvalid-offsetof\"\n");
        if (cd.fields.empty()) {
            o.w("static const spp::FieldInfo* const " + cn + "_fields = nullptr;\n");
        } else {
            o.w("static const spp::FieldInfo " + cn + "_fields[] = {\n");
            for (const Field& f : cd.fields) {
                const std::string T = f.type;
                const bool sub = types.is_script_value(T);
                o.w("    {\"" + f.name + "\", " + (f.alias.empty() ? "nullptr" : "\"" + f.alias + "\"") +
                    ", typeid(" + key_type(T) + ").name(), offsetof(" + cn + ", " + f.name + "), sizeof(" + T + "),\n     alignof(" +
                    T + "), spp::" + (sub ? "value_field_flags" : "field_flags") + "<" + T + ">(), " + P + "init_" +
                    f.name + ", spp::move_field<" + T + ">, spp::field_destroyer<" + T + ">()");
                auto hk = hooks.find(f.name);
                if (hk != hooks.end())
                    o.w(", typeid(" + hk->second->params[0].type + ").name(), " + P + hk->second->name + "_hook");
                else if (sub)
                    o.w(", nullptr, nullptr");
                o.w((sub ? ", &" + T + "_layout" : std::string()) + "},\n");
            }
            o.w("};\n");
        }
        o.w("#pragma clang diagnostic pop\n");
        o.w("static const spp::StateLayout " + cn + "_layout = {\n    \"" + cn + "\", sizeof(" + cn + "), alignof(" + cn +
            "), " + std::to_string(cd.fields.size()) + ", " + cn + "_fields,\n};\n\n");

        // The function bodies. "self" is the struct itself, so "st" is "self".
        auto emit_vbody = [&](Method& m, const std::string& sym, const std::string& ret, const std::string& what) {
            RCtx cx = base_cx;
            cx.where = what;
            sppc::add_params(cx, m.params);
            o.w("static " + ret + " " + sym + "(" + cn + "* self" + param_text(m.params) + ")\n{\n");
            o.w("    [[maybe_unused]] auto* st = self;\n");
            sppc::lower_block(m.body, cx, g_diag);
            if (!g_diag.items.empty()) throw TranslateAbort{};
            sppc::ast::Emitter em(map, opt.input);
            sppc::ast::emit_stmts(m.body.stmts, em, 1);
            em.finish();
            o.w_emitted(em, in.script_text);
            reset();
            o.w("}\n\n");
        };
        if (has_ctor) {
            Method cm;
            cm.name = cn;
            cm.params = cd.ctors[0].params;
            cm.pos = cd.ctors[0].pos;
            cm.body = std::move(cd.ctors[0].body);
            emit_vbody(cm, P + "ctor", "void", cn + "::" + cn);
        }
        for (Method& m : cd.methods) emit_vbody(m, P + m.name, fn_ret(m), body_what(cn, m));
        // A convert hook has no self and reads no module variable: the new block is not complete.
        for (auto& [vname, m] : hooks) {
            RCtx cx = base_cx;
            cx.where = m->name;
            cx.allow_this = false;
            cx.in_module_init = false;
            cx.in_module_hook = true;
            sppc::add_params(cx, m->params);
            o.w("static " + m->ret + " " + P + m->name + "(" + m->params[0].type + " " + m->params[0].name + ")\n{\n");
            sppc::lower_block(m->body, cx, g_diag);
            if (!g_diag.items.empty()) throw TranslateAbort{};
            sppc::ast::Emitter em(map, opt.input);
            sppc::ast::emit_stmts(m->body.stmts, em, 1);
            em.finish();
            o.w_emitted(em, in.script_text);
            reset();
            o.w("}\n\n");
        }
    };

    // The emit order (design 7.10.1). A class that names another script class in a field, a
    // parameter, a local or a "new" comes after that class. So the script can declare the
    // classes in any order. A [value] class comes first: other classes hold it as a field.
    // The other classes keep the script order when no use forces a change. A cycle is an error.
    std::vector<ClassDef*> order;
    {
        std::map<std::string, ClassDef*> by_name;
        for (ClassDef& cd : sc.classes) by_name[cd.name] = &cd;
        std::map<std::string, std::set<std::string>> deps;
        for (ClassDef& cd : sc.classes) {
            for (const std::string& u : sppc::used_type_names(cd)) {
                if (!by_name.count(u) || u == cd.name) continue;
                if (cd.name == kModuleName && !types.is_script_value(u))
                    fail_at(map, cd.pos, "a module variable can use only native types, enums and [value] classes: '" + u + "' is not");
                if (cd.is_value && !types.is_script_value(u))
                    fail_at(map, cd.pos, "a [value] class can use only native types and [value] classes: '" + u + "' is not");
                deps[cd.name].insert(u);
            }
            if (script_parent(cd)) deps[cd.name].insert(cd.base);  // the parent shell comes first
        }
        std::map<std::string, int> state;  // 1: on the path, 2: done
        std::vector<std::string> path;
        std::function<void(ClassDef&)> visit = [&](ClassDef& cd) {
            int& st = state[cd.name];
            if (st == 2) return;
            if (st == 1) {
                std::string cycle;
                for (const std::string& n : path) cycle += n + " -> ";
                fail_at(map, cd.pos, "the classes use each other in a cycle: " + cycle + cd.name +
                                         ". A handle type must not depend on itself through other classes");
            }
            st = 1;
            path.push_back(cd.name);
            for (const std::string& u : deps[cd.name]) visit(*by_name[u]);
            path.pop_back();
            state[cd.name] = 2;
            order.push_back(&cd);
        };
        for (ClassDef& cd : sc.classes)
            if (cd.is_value) visit(cd);
        for (ClassDef& cd : sc.classes)
            if (!cd.is_value) visit(cd);
    }
    std::set<std::string> emitted;
    // What a class with a script parent takes from the parent (design 4.5). The parent comes
    // first in the emit order, so its entry is complete then.
    struct ClassInfo {
        std::string S;                         // the name of the state struct
        std::vector<std::string> table;        // the function of each slot; empty is a hole
        std::vector<std::string> rows;         // the field table rows; "$S$" is the state struct
        std::set<std::string> fields;          // all field names, the inherited ones too
        std::string ctor_entry;                // the function that runs the constructors, or empty
        size_t ctor_params = 0;                // the parameter count of the constructor
        bool dtor_chain = false;               // the class or a parent has a destructor
        std::string on_reload;                 // the onReload hook of the class or a parent
        bool esft = false;                     // the shell has std::enable_shared_from_this
    };
    std::map<std::string, ClassInfo> infos;

    for (ClassDef* cdp : order) {
        ClassDef& cd = *cdp;
        const std::string cn = cd.name;
        emitted.insert(cn);
        if (cd.is_value) {
            emit_value(cd);
            continue;
        }
        const bool has_ctor = !cd.ctors.empty(), has_dtor = !cd.dtors.empty();
        // The classes of a chain (design 4.5): "par" is the script parent, "root" the first class.
        ClassDef* par = script_parent(cd);
        const ClassDef& root = *script_root(cd);
        const ClassInfo* pinfo = par ? &infos.at(par->name) : nullptr;
        const bool family = par || has_children.count(cn);
        const bool gen_new = has_ctor || new_set.count(cn) > 0 || family;
        const std::string P = cn + "__";
        const std::string PP = par ? par->name + "__" : std::string();
        auto slot_id = [&](const std::string& n) { return (multi ? cn + "_" : std::string()) + "SLOT_" + n; };

        // ---- base class information from libclang
        // A class without a base derives from spp::PlainObject (design 7.10.1).
        // A class with a script parent uses the native base of the root of the chain.
        BaseInfo bi;
        const std::string B = root.base.empty() ? std::string("spp::PlainObject") : root.base;
        if (root.base.empty()) {
            bi.virtual_dtor = true;
        } else {
            auto base_it = bases.find(root.base);
            if (base_it == bases.end())
                fail_at(map, root.pos, "base class '" + root.base + "' is not found in the imported headers");
            bi = base_it->second;
        }
        if (!bi.any_ctor) bi.default_ctor = true;  // the compiler declares one
        if (!bi.virtual_dtor)
            fail_at(map, cd.pos, "base class '" + root.base + "' has no virtual destructor");
        if (bi.final_cls) fail_at(map, root.pos, "base class '" + root.base + "' is final");
        for (NativeVirtual& nv : bi.virtuals) nv.from = B;
        // The virtuals of the interface bases come after the virtuals of the primary base, in the
        // order of the bases (design 4.4). The shell overrides one function for both bases when a
        // signature is in two bases. That function has one slot and no clear forwarding target.
        // So the same signature in two bases is an error.
        std::string BL = B;  // the list of the native bases: the record and the shell hash use it
        for (const std::string& iname : root.interfaces) {
            auto it = bases.find(iname);
            if (it == bases.end())
                fail_at(map, root.pos, "interface base '" + iname + "' is not found in the imported headers");
            const BaseInfo& ib = it->second;
            if (ib.final_cls) fail_at(map, root.pos, "interface base '" + iname + "' is final");
            if (ib.any_ctor && !ib.default_ctor)
                fail_at(map, root.pos, "interface base '" + iname + "' has no default constructor");
            for (NativeVirtual v : ib.virtuals) {
                v.from = iname;
                for (const NativeVirtual& o : bi.virtuals)
                    if (o.key() == v.key())
                        fail_at(map, root.pos, "'" + v.text() + "' is a virtual method of " + o.owner + " (base '" +
                                                   o.from + "') and of " + v.owner + " (base '" + iname +
                                                   "'). One method cannot override a virtual of two bases");
                bi.virtuals.push_back(std::move(v));
            }
            BL += ", " + iname;
        }
        const bool root_super = !root.ctors.empty() && root.ctors[0].has_super;
        std::string ctor_args;
        if (bi.ctor_kind == 1) ctor_args = ", std::string(\"" + cn + "\")";
        else if (bi.ctor_kind == 2) ctor_args = ", \"" + cn + "\"";
        else if (!bi.default_ctor && !root_super)
            fail_at(map, cd.pos, "base class '" + root.base + "' has no default constructor and no constructor that takes a name");
        if (par) {
            // The constructor of the parent runs first. Its arguments come from "super(...)".
            const bool sup = has_ctor && cd.ctors[0].has_super;
            if (pinfo->ctor_params > 0 && !sup)
                fail_at(map, has_ctor ? cd.ctors[0].pos : cd.pos,
                        "class '" + cn + "' must call 'super(...)': the constructor of '" + par->name + "' has parameters");
            if (sup && cd.ctors[0].super_args.size() != pinfo->ctor_params)
                fail_at(map, cd.ctors[0].super_pos,
                        "'super(...)' has " + std::to_string(cd.ctors[0].super_args.size()) + " arguments; the constructor of '" +
                            par->name + "' takes " + std::to_string(pinfo->ctor_params));
        }

        // ---- check the script members
        desugar_props(cd, map);
        {
            const size_t before = g_diag.items.size();
            sppc::check_ctors(cd, map, g_diag);
            if (g_diag.items.size() != before) throw TranslateAbort{};
        }
        for (const sppc::ast::Ctor& c : cd.ctors)
            for (const Param& p : c.params)
                if (p.name == "self" || p.name == "st" || p.name == "a")
                    fail_at(map, c.pos, "parameter name '" + p.name + "' is reserved");
        std::set<std::string> field_names;
        for (const Field& f : cd.fields)
            if (!field_names.insert(f.name).second) fail_at(map, f.pos, "field '" + f.name + "' is declared twice");
        if (par)
            for (const Field& f : cd.fields)
                if (pinfo->fields.count(f.name))
                    fail_at(map, f.pos, "field '" + f.name + "' hides a field of the script parent '" + par->name + "'");
        // The hooks of design 4.7: "void onReload(OldState old)" and "convert_<field>(Old old)".
        // They are not script functions: they get no slot and no thunk.
        enum class Hook { None, OnReload, Convert };
        auto hook_of = [&](const Method& m) {
            if (m.overrides_virtual) return Hook::None;
            if (m.name == "onReload") return Hook::OnReload;
            if (m.name.rfind("convert_", 0) == 0 && field_names.count(m.name.substr(8)))
                return Hook::Convert;
            return Hook::None;
        };
        std::set<std::string> fn_names;
        std::map<std::string, const Method*> overrides;  // by native key
        for (Method& m : cd.methods) {
            for (const Param& p : m.params)
                if (p.name == "self" || p.name == "st" || p.name == "a")
                    fail_at(map, m.pos, "parameter name '" + p.name + "' is reserved");
            // A method overrides a base virtual method when the name and the parameter types match.
            // The method must have the modifier "override" (C# rules).
            std::string key = m.name + "(";
            for (const Param& p : m.params)
                key += key_norm(p.type) + (p.mode == PassMode::Value ? "" : "&") + ",";
            const NativeVirtual* hit = nullptr;
            for (const auto& nv : bi.virtuals) {
                std::string k = nv.key();
                if (nv.is_const) k.resize(k.size() - 5);
                if (k != key + ")") continue;
                // The script has no "const" on a method, so "F()" and "F() const" both match.
                if (hit)
                    fail_at(map, m.pos, "the override '" + m.name + "' is ambiguous: it matches '" + hit->text() +
                                            (hit->is_const ? " const" : "") + "' of " + hit->owner + " and '" +
                                            nv.text() + (nv.is_const ? " const" : "") + "' of " + nv.owner);
                hit = &nv;
            }
            if (!hit) {
                // A virtual method with the same name and other parameters: the function hides it.
                const NativeVirtual* same_name = nullptr;
                std::string cands;
                for (const auto& nv : bi.virtuals)
                    if (nv.name == m.name) {
                        if (!same_name) same_name = &nv;
                        cands += (cands.empty() ? "" : "; ") + nv.text();
                    }
                if (same_name) {
                    std::string msg = "function '" + m.name + "' hides the virtual method of " + same_name->owner;
                    if (!cands.empty()) msg += ". Candidates: " + cands;
                    fail_at(map, m.pos, msg);
                }
                // A script virtual method (design 7.6). "override" replaces the slot of the nearest
                // parent function with the name. "new" hides it. "virtual" adds a slot.
                if (hook_of(m) == Hook::None) {
                    const Method* am = nullptr;
                    const ClassDef* aowner = nullptr;
                    for (const ClassDef* a = par; a && !am; a = script_parent(*a))
                        for (const Method& pm : a->methods)
                            if (pm.name == m.name && !pm.overrides_virtual && pm.name != "onReload" &&
                                pm.name.rfind("convert_", 0) != 0) {
                                am = &pm;
                                aowner = a;
                                break;
                            }
                    if (m.is_override) {
                        if (!am || am->vslot_owner.empty())
                            fail_at(map, m.pos, "'override' has no virtual method to override");
                        auto ptext = [](const Param& p) {
                            return key_norm(p.script_type.empty() ? p.type : p.script_type) +
                                   (p.mode == PassMode::Value ? "" : "&");
                        };
                        bool same = am->params.size() == m.params.size();
                        for (size_t k = 0; same && k < m.params.size(); ++k)
                            same = ptext(am->params[k]) == ptext(m.params[k]);
                        if (!same)
                            fail_at(map, m.pos, "'" + m.name + "' has other parameter types than the virtual method of " +
                                                    aowner->name + "; write 'new' to hide it");
                        const std::string r0 = am->script_ret.empty() ? "void" : am->script_ret;
                        const std::string r1 = m.script_ret.empty() ? "void" : m.script_ret;
                        if (key_norm(r0) != key_norm(r1))
                            fail_at(map, m.pos, "return type '" + r1 + "' differs from '" + r0 + "'");
                        m.vslot_owner = am->vslot_owner;
                    } else {
                        if (am && !m.is_new) {
                            const bool v = !am->vslot_owner.empty();
                            fail_at(map, m.pos, "function '" + m.name + "' hides the " + (v ? "virtual " : "") +
                                                    "function of " + aowner->name + "; write " +
                                                    (v ? "'override' or 'new'" : "'new'"));
                        }
                        if (m.is_virtual) m.vslot_owner = cn;
                    }
                    if (par && pinfo->fields.count(m.name))
                        fail_at(map, m.pos, "'" + m.name + "' is a field of '" + par->name + "' and a function");
                } else if (m.is_override) {
                    fail_at(map, m.pos, "'override' has no virtual method to override");
                }
                if (!fn_names.insert(m.name).second)
                    fail_at(map, m.pos, "function '" + m.name + "' is declared twice (overloads are not supported)");
                if (field_names.count(m.name)) fail_at(map, m.pos, "'" + m.name + "' is a field and a function");
                continue;
            }
            if (m.is_new) fail_at(map, m.pos, "'new' cannot hide a virtual method; write 'override'");
            if (!m.is_override)
                fail_at(map, m.pos, "method '" + m.name + "' overrides a virtual method of " + hit->owner +
                                  "; write 'override'");
            m.overrides_virtual = true;
            if (hit->final_)
                fail_at(map, m.pos, "'" + hit->text() + "' of " + hit->owner + " is final and cannot be overridden");
            if (!m.ret.empty() && key_norm(m.ret) != key_norm(hit->ret))
                fail_at(map, m.pos, "return type '" + m.ret + "' differs from '" + hit->ret + "'");
            if (!overrides.emplace(hit->key(), &m).second)
                fail_at(map, m.pos, "'" + hit->text() + "' is overridden twice");
        }
        for (const auto& nv : bi.virtuals)
            if (nv.pure && !overrides.count(nv.key()))
                fail_at(map, cd.pos, "class " + cn + " does not override the pure virtual method '" +
                                  nv.text() + "' of " + nv.owner);

        // ---- the hooks (design 4.7)
        for (const Method& m : cd.methods) {
            const Hook h = hook_of(m);
            if (h == Hook::None) continue;
            if (m.is_override || m.is_virtual)
                fail_at(map, m.pos, "the hook '" + m.name + "' cannot be 'override' or 'virtual'");
            if (h == Hook::OnReload) {
                if (!m.ret.empty() && m.ret != "void")
                    fail_at(map, m.pos, "'onReload' must return void");
                if (m.params.size() != 1 || m.params[0].type != "OldState" ||
                    m.params[0].mode != PassMode::Value)
                    fail_at(map, m.pos, "'onReload' takes one parameter: 'OldState old'");
            } else {
                if (m.params.size() != 1 || m.params[0].mode != PassMode::Value)
                    fail_at(map, m.pos, "'" + m.name + "' is a convert hook. It takes one parameter: the old value");
                if (m.ret.empty() || m.ret == "void")
                    fail_at(map, m.pos, "'" + m.name + "' is a convert hook. It must return the new field type");
            }
        }

        // ---- the borrowed-storage rule and the rows of design 5.3 that the script text shows
        {
            const size_t before = g_diag.items.size();
            sppc::check_class(cd, types, map, g_diag, in.strict_borrowed);
            if (g_diag.items.size() != before) throw TranslateAbort{};
        }

        // ---- the C++ storage types. An override keeps the native types.
        for (Field& f : cd.fields) f.type = types.storage(f.type);
        for (Method& m : cd.methods) {
            if (m.overrides_virtual) {
                // An override keeps the native type. A "ref" or "out" parameter is a reference.
                for (Param& p : m.params)
                    if (p.mode != PassMode::Value) p.type += "&";
                continue;
            }
            const Hook h = hook_of(m);
            if (h == Hook::OnReload) {
                m.params[0].type = "const spp::OldState&";
                m.ret = "void";
                continue;
            }
            if (h == Hook::Convert) {  // the old value is a copy of the old field type
                m.params[0].type = types.storage(m.params[0].type);
                m.ret = types.storage(m.ret);
                const std::string fname = m.name.substr(8);
                for (const Field& f : cd.fields)
                    if (f.name == fname && f.type != m.ret)
                        fail_at(map, m.pos, "'" + m.name + "' must return the type of the field: '" + f.type + "'");
                continue;
            }
            // The parameter analysis (design 7.10.5) picks the form in the generation function.
            // The slot adapter and the thunk keep the form of param().
            for (Param& p : m.params) {
                if (p.mode != PassMode::Value) {
                    p.type = types.ref_param(p.type);
                    continue;
                }
                const sppc::ParamUse use = sppc::analyze_param_use(m, p.name);
                const std::string body = types.body_param(p.type, use.mutated, use.rebound, use.stored);
                p.type = types.param(p.type);
                if (body != p.type) p.body_type = body;
            }
            if (!m.ret.empty()) m.ret = types.storage(m.ret);
        }

        for (sppc::ast::Ctor& c : cd.ctors)
            for (Param& p : c.params) p.type = types.param(p.type);

        // ---- slots: native virtuals first, then the script functions
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
            if (!m.overrides_virtual && hook_of(m) == Hook::None) fn_by_name[m.name] = &m;
        // The classes of a family (one root) share one slot numbering (design 7.6). The slots of
        // the manifest entries of the family come in the manifest order, so a number is stable
        // over generations. The table of a class has holes for the functions of other branches.
        std::map<std::string, size_t> fam_index;  // "Cls::fn" to the slot number
        size_t table_size = 0;
        if (!family) {
            for (const auto& e : old_manifest) {
                if (e.cls != cn) continue;
                auto it = fn_by_name.find(e.fn);
                if (it == fn_by_name.end())
                    fail_at(map, cd.pos, "function '" + e.fn + "' of an earlier generation is removed. The slot count must not shrink");
                Slot s;
                s.id = e.fn;
                s.impl = it->second;
                s.intro_gen = e.gen;
                slots.push_back(s);
            }
            for (const Method& m : cd.methods) {
                if (m.overrides_virtual || hook_of(m) != Hook::None) continue;
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
            table_size = slots.size();
        } else {
            for (const auto& e : old_manifest)
                if (e.cls == cn && !fn_by_name.count(e.fn))
                    fail_at(map, cd.pos, "function '" + e.fn + "' of an earlier generation is removed. The slot count must not shrink");
            // An "override" of a script virtual uses the slot of the parent. It adds no entry.
            for (const Method& m : cd.methods) {
                if (m.overrides_virtual || m.is_override || hook_of(m) != Hook::None) continue;
                bool known = false;
                for (const auto& e : new_manifest) known |= e.cls == cn && e.fn == m.name;
                if (!known) new_manifest.push_back({cn, m.name, opt.gen});
            }
            std::set<std::string> fam;
            for (const ClassDef& c : sc.classes)
                if (script_root(c) == &root) fam.insert(c.name);
            size_t k = native_count;
            for (const auto& e : new_manifest) {
                if (!fam.count(e.cls)) continue;
                const size_t idx = k++;
                fam_index[e.cls + "::" + e.fn] = idx;
                if (e.cls != cn) continue;
                Slot s;
                s.id = e.fn;
                s.impl = fn_by_name.at(e.fn);
                s.intro_gen = e.gen;
                s.index = static_cast<int>(idx);
                slots.push_back(s);
            }
            table_size = k;
        }
        // The function of each slot. A class with a script parent starts from the table of the
        // parent: the inherited functions and the overrides of the parent stay.
        std::vector<std::string> table(table_size);
        for (size_t i = 0; i < native_count; ++i)
            table[i] = !slots[i].impl && pinfo ? pinfo->table[i] : P + (slots[i].impl ? "" : "fwd_") + slots[i].id;
        if (pinfo)
            for (size_t i = native_count; i < pinfo->table.size() && i < table_size; ++i) table[i] = pinfo->table[i];
        for (size_t i = native_count; i < slots.size(); ++i)
            table[slots[i].index >= 0 ? size_t(slots[i].index) : i] = P + slots[i].id + "_slot";
        std::vector<const Method*> script_overrides;  // "override" of a script virtual, without an own slot
        for (const Method& m : cd.methods) {
            if (!m.is_override || m.overrides_virtual) continue;
            bool own = false;
            for (size_t i = native_count; i < slots.size(); ++i) own |= slots[i].id == m.name;
            table[fam_index.at(m.vslot_owner + "::" + m.name)] = P + m.name + "_slot";
            if (!own) script_overrides.push_back(&m);
        }
        ClassInfo info;
        info.table = table;

        // ---- lowering context
        RCtx base_cx = sppc::make_class_ctx(cd);
        base_cx.map = &map;
        base_cx.types = &types;
        for (const auto& sc_cls : sc.classes) {
            base_cx.script_classes.insert(sc_cls.name);
            base_cx.classes[sc_cls.name] = &sc_cls;
        }
        if (gen_new) new_ready.insert(cn);
        base_cx.new_classes = new_ready;
        if (par) {
            // A child sees the fields and the functions of its script parents.
            for (const ClassDef* a = par; a; a = script_parent(*a)) {
                RCtx pc = sppc::make_class_ctx(*a);
                for (const auto& [n, t] : pc.field_type) base_cx.field_type.emplace(n, t);
                base_cx.fns.insert(pc.fns.begin(), pc.fns.end());
            }
            base_cx.native_base = root.base;
            for (const auto& nv : bi.virtuals) base_cx.native_virtuals.insert(nv.name);
        }
        // "super.F()" names a base by the name F only. A name of the primary base keeps the
        // primary base. Else the first interface base with a virtual F gives the implementation.
        {
            std::set<std::string> primary_names;
            for (const auto& nv : bi.virtuals)
                if (nv.from == B) primary_names.insert(nv.name);
            for (const auto& nv : bi.virtuals)
                if (nv.from != B && !primary_names.count(nv.name)) base_cx.interface_virtuals.emplace(nv.name, nv.from);
        }

        const std::string S = cn + "State_g" + std::to_string(opt.gen);
        const std::string shell = cn + "_Shell";

        o.w("\n// ---------------------------------------------------------------- class " + cn + "\n");
        o.w("// Slot numbers. Native virtuals come first, in the order of " + BL + ".\n");
        o.w("enum : unsigned {\n");
        for (size_t i = 0; i < slots.size(); ++i)
            o.w("    " + slot_id(slots[i].id) + " = " + std::to_string(slots[i].index >= 0 ? size_t(slots[i].index) : i) +
                ",\n");
        o.w("    " + slot_id("COUNT") + " = " + std::to_string(table_size) + ",\n};\n\n");
        o.w("extern spp::ClassRecord " + cn + "_record;\n\n");

        // ---- shell
        o.w("// The shell overrides every virtual of " + BL + ". Each stub reads one slot.\n");
        // A bare "this" used as a handle needs "shared_from_this" (design 7.10.1). A plain class
        // gets it from spp::PlainObject. A native base needs it in the shell.
        // The lowering of the bodies sets "handle_this". The base is added to this line then.
        bool handle_this = false;
        base_cx.handle_this = &handle_this;
        base_cx.format_lib = in.format_lib;
        base_cx.uses_format = &uses_format;
        base_cx.module_vars = module_vars;
        base_cx.in_module_init = cn == kModuleName;
        // A shell with script children is not final. The tag spp::ConstructChild builds the base
        // without the state: the shell of the child makes the state of the child (design 4.5).
        const bool parent_shell = has_children.count(cn) > 0;
        const std::string shell_head = "struct " + shell + (parent_shell ? "" : " final") + " : " +
                                       (par ? par->name + "_Shell" : BL + ", spp::ShellBase");
        const size_t shell_head_end = o.s.size() + shell_head.size();
        o.w(shell_head + " {\n");
        if (par) {
            o.w("    template <class... A>\n    explicit " + shell + "(spp::Construct, A&&... a)\n        : " + par->name +
                "_Shell(spp::ConstructChild{}, std::forward<A>(a)...)\n    {\n        spp::init_state(this, &" + cn +
                "_record);\n    }\n");
            if (parent_shell)
                o.w("    template <class... A>\n    explicit " + shell + "(spp::ConstructChild c, A&&... a) : " + par->name +
                    "_Shell(c, std::forward<A>(a)...)\n    {\n    }\n");
        } else {
            o.w("    template <class... A>\n    explicit " + shell + "(spp::Construct, A&&... a) : " + B +
                "(std::forward<A>(a)...)\n    {\n        spp::init_state(this, &" + cn +
                "_record);\n    }\n    ~" + shell + "() override { spp::destroy_state(this); }\n");
            if (parent_shell)
                o.w("    template <class... A>\n    explicit " + shell + "(spp::ConstructChild, A&&... a) : " + B +
                    "(std::forward<A>(a)...)\n    {\n    }\n");
        }
        for (size_t i = 0; i < native_count && !par; ++i) {
            const NativeVirtual& nv = *slots[i].nv;
            std::vector<std::string> ps, sig = {B + "*"}, as = {nv.is_const ? "const_cast<" + shell + "*>(this)" : "this"};
            for (size_t k = 0; k < nv.ptypes.size(); ++k) {
                ps.push_back(nv.ptypes[k] + " a" + std::to_string(k));
                sig.push_back(nv.ptypes[k]);
                as.push_back(arg_expr(nv.ptypes[k], "a" + std::to_string(k)));
            }
            o.w("\n    " + nv.ret + " " + nv.name + "(" + str_join(ps, ", ") + ")" +
                (nv.is_const ? " const" : "") + (nv.no_except ? " noexcept" : "") + " override\n    {\n");
            o.w("        return spp::call<" + nv.ret + "(" + str_join(sig, ", ") + ")>(cls, " +
                slot_id(slots[i].id) + ", " + str_join(as, ", ") + ");\n    }\n");
        }
        o.w("};\n\n");
        // The type name of the class for the script: a handle field names it (design 7.10.1).
        if (gen_new || used_types.count(cn)) o.w("using " + cn + " = " + shell + ";\n\n");

        // ---- state struct and field table
        o.w("// ---------------------------------------------------------------- state\n");
        info.S = S;
        o.w("struct " + S + (pinfo ? " : " + pinfo->S : std::string()) + " {\n");
        for (const Field& f : cd.fields) {
            const std::string decl = "    " + f.type + " " + f.name + ";\n";
            if (f.pos != 0) o.name_segment(decl, 4 + f.type.size(), f.name, in.script_text, f.pos, f.name);
            o.w(decl);
        }
        if (cd.fields.empty() && !pinfo) o.w("    char unused_;\n");
        o.w("};\n\n");
        for (Field& f : cd.fields) {
            RCtx fc = base_cx;
            fc.allow_this = false;
            std::string init;
            if (f.has_init && f.init) {
                sppc::lower_expr(f.init, fc, g_diag);
                if (!g_diag.items.empty()) throw TranslateAbort{};
                init = sppc::ast::print(*f.init);
            }
            o.w("#line " + std::to_string(f.line) + " \"" + esc(opt.input) + "\"\n");
            o.w("static void " + P + "init_" + f.name + "(void*, void* f) { using T = " + f.type +
                "; new (f) T(" + init + "); }\n");
            reset();
        }
        // A convert hook of a field (design 4.7). The wrapper builds the new field from the old one.
        std::map<std::string, const Method*> converts;  // by field name
        for (const Method& m : cd.methods)
            if (hook_of(m) == Hook::Convert) converts[m.name.substr(8)] = &m;
        for (const auto& [fname, m] : converts) {
            o.w("static " + m->ret + " " + P + m->name + "(" + m->params[0].type + " " + m->params[0].name + ");\n");
            o.w("static void " + P + m->name + "_hook(void* dst, const void* src)\n{\n    new (dst) " + m->ret + "(" +
                P + m->name + "(*static_cast<const " + m->params[0].type + "*>(src)));\n}\n");
        }
        o.w("\n#pragma clang diagnostic push\n#pragma clang diagnostic ignored \"-Winvalid-offsetof\"\n");
        // The field table of a child has the rows of the parent first. "$S$" is the state struct.
        if (pinfo) info.rows = pinfo->rows;
        if (pinfo) info.fields = pinfo->fields;
        for (const Field& f : cd.fields) {
            info.fields.insert(f.name);
            const std::string T = f.type;
            const bool sub = types.is_script_value(T);
            std::string row = "    {\"" + f.name + "\", " + (f.alias.empty() ? "nullptr" : "\"" + f.alias + "\"") +
                              ", typeid(" + key_type(T) + ").name(), offsetof($S$, " + f.name + "), sizeof(" + T +
                              "),\n     alignof(" + T + "), spp::" + (sub ? "value_field_flags" : "field_flags") + "<" + T +
                              ">(), " + P + "init_" + f.name + ", spp::move_field<" + T + ">, spp::field_destroyer<" + T +
                              ">()";
            auto cv = converts.find(f.name);
            if (cv != converts.end())
                row += ", typeid(" + cv->second->params[0].type + ").name(), " + P + cv->second->name + "_hook";
            else if (sub)
                row += ", nullptr, nullptr";
            if (sub) row += ", &" + T + "_layout";
            info.rows.push_back(row + "},\n");
        }
        if (info.rows.empty()) {
            o.w("static const spp::FieldInfo* const " + cn + "_fields = nullptr;\n");
        } else {
            o.w("static const spp::FieldInfo " + cn + "_fields[] = {\n");
            for (std::string row : info.rows) {
                const size_t at = row.find("$S$");
                if (at != std::string::npos) row.replace(at, 3, S);
                o.w(row);
            }
            o.w("};\n");
        }
        o.w("#pragma clang diagnostic pop\n\n");
        o.w("static const spp::StateLayout " + cn + "_layout = {\n    \"" + S + "\", sizeof(" + S +
            "), alignof(" + S + "), " + std::to_string(info.rows.size()) + ", " + cn + "_fields,\n};\n\n");

        // ---- generation functions
        o.w("// ------------------------------------------------- generation functions\n");
        o.w("static inline " + S + "* state_of(" + shell + "* self)\n{\n    return static_cast<" + S +
            "*>(self->state);\n}\n\n");

        auto param_decl = [](const Method& m) {
            std::string r;
            for (const Param& p : m.params) r += ", " + p.type + " " + p.name;
            return r;
        };
        // The parameters of the generation function of a script function (design 7.10.5).
        auto body_decl = [](const Method& m) {
            std::string r;
            for (const Param& p : m.params) r += ", " + (p.body_type.empty() ? p.type : p.body_type) + " " + p.name;
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

        // forward declarations of the script functions
        for (const Method& m : cd.methods)
            if (!m.overrides_virtual && hook_of(m) != Hook::Convert) {
                const std::string decl = "static " + fn_ret(m) + " " + P + m.name + "(" + shell + "* self" + body_decl(m) + ");\n";
                o.head_segments(decl, P + m.name, m, in.script_text);
                o.w(decl);
            }
        o.w("\n");
        // A script virtual function (design 7.6). A call reads the slot of the class of the object.
        for (const Method& m : cd.methods) {
            if (m.vslot_owner != cn || m.is_override) continue;
            o.w("[[maybe_unused]] static " + fn_ret(m) + " " + P + m.name + "_virt(" + shell + "* self" + param_decl(m) +
                ")\n{\n    return spp::call<" + fn_ret(m) + "(" + B + "*" + type_list(m) + ")>(self->cls, " +
                slot_id(m.name) + ", static_cast<" + B + "*>(self)" + arg_list(m) + ");\n}\n\n");
        }

        // The constructor (design 7.3). The constructor body is not a slot. A "new" function
        // builds the shell in the module that made the class, then runs the body here.
        Method ctor_m;
        if (has_ctor) {
            ctor_m.name = cn;
            ctor_m.params = cd.ctors[0].params;
            ctor_m.pos = cd.ctors[0].pos;
            o.w("static void " + P + "ctor(" + shell + "* self" + param_decl(ctor_m) + ");\n");
        }
        if (has_dtor) o.w("static void " + P + "dtor(" + shell + "* self);\n");
        // The function that runs the constructors: the constructor of the parent first.
        const bool has_entry = has_ctor || (pinfo && !pinfo->ctor_entry.empty());
        const std::string entry = has_entry ? P + "ctor_chain" : std::string();
        info.ctor_entry = entry;
        info.ctor_params = ctor_m.params.size();
        if (gen_new) {
            std::string pd = param_decl(ctor_m), names, declvals, defaults;
            if (!pd.empty()) pd = pd.substr(2);
            for (size_t i = 0; i < ctor_m.params.size(); ++i) {
                const Param& p = ctor_m.params[i];
                names += (i ? ", " : "") + p.name;
                declvals += (i ? ", " : "") + ("std::declval<" + p.type + ">()");
                defaults += (i ? ", " : "") + ("std::decay_t<" + p.type + ">()");
            }
            std::string ctor_fwd = arg_list(ctor_m);
            std::string sup_text;
            const bool has_super = has_ctor && cd.ctors[0].has_super;
            int sup_line = cd.line;
            if (has_ctor) sup_line = cd.ctors[0].line;
            if (has_super) {
                RCtx fc = base_cx;
                fc.allow_this = false;
                fc.where = cn + "::" + cn;
                sppc::add_params(fc, ctor_m.params);
                std::vector<std::string> parts;
                for (ExprP& a : cd.ctors[0].super_args) {
                    sppc::lower_expr(a, fc, g_diag);
                    if (!g_diag.items.empty()) throw TranslateAbort{};
                    parts.push_back(sppc::ast::print(*a));
                }
                sup_text = str_join(parts, ", ");
            } else if (!ctor_args.empty()) {
                sup_text = ctor_args.substr(2);
            }
            // The plan of a construction evaluates each "super(...)" argument of the chain one
            // time. "first" is the tuple of the native base arguments. "second" is the chain: a
            // pair of the constructor arguments of the parent and the chain of the parent. The
            // chain of a root is an empty tuple.
            o.w("#line " + std::to_string(sup_line) + " \"" + esc(opt.input) + "\"\n");
            if (!par) {
                o.w("[[maybe_unused]] static auto " + P + "super_args(" + pd + ")\n{\n    return std::make_tuple(" + sup_text +
                    ");\n}\n");
                o.w("static auto " + P + "plan(" + pd + ")\n{\n    return std::make_pair(" + P + "super_args(" + names +
                    "), std::tuple<>());\n}\n");
            } else {
                // The base arguments come from the plan of the parent, or from the name of this
                // class when no class of the chain calls "super(...)" with arguments.
                const bool own_base = pinfo->ctor_params == 0 && !root_super;
                o.w("static auto " + P + "plan(" + pd + ")\n{\n    auto up = std::make_tuple(" +
                    (has_super ? sup_text : std::string()) + ");\n    auto pp = std::apply([](const auto&... x) { return " +
                    PP + "plan(x...); }, up);\n    return std::make_pair(" +
                    (own_base ? "std::make_tuple(" + (ctor_args.empty() ? std::string() : ctor_args.substr(2)) + ")"
                              : std::string("std::move(pp.first)")) +
                    ", std::make_pair(std::move(up), std::move(pp.second)));\n}\n");
            }
            if (has_entry) {
                // Runs the constructor of each parent first, with the arguments of the plan.
                o.w("static void " + P + "ctor_chain(" + shell + "* self, [[maybe_unused]] const auto& chain" +
                    param_decl(ctor_m) + ")\n{\n");
                if (pinfo && !pinfo->ctor_entry.empty())
                    o.w("    std::apply([&](const auto&... x) { " + pinfo->ctor_entry +
                        "(self, chain.second, x...); }, chain.first);\n");
                if (has_ctor) o.w("    " + P + "ctor(self" + ctor_fwd + ");\n");
                o.w("}\n");
            }
            reset();
            o.w("using " + cn + "_SuperArgs = decltype(" + P + "plan(" + declvals + ").first);\n");
            o.w("// Makes the shell with the base arguments. Only the first generation of the class runs it.\n");
            o.w("static spp::ShellBase* " + P + "construct(const void* args)\n{\n    const auto& t = *static_cast<const " +
                cn + "_SuperArgs*>(args);\n    return std::apply([](const auto&... x) -> spp::ShellBase* { return new " +
                shell + "(spp::Construct{}, x...); }, t);\n}\n\n");
            // "new" and the factory both use this function. "note" is false for the factory: the
            // engine owns that object, so its handle has no code of this generation.
            o.w("static " + shell + "* " + P + "build([[maybe_unused]] bool note" + param_decl(ctor_m) + ")\n{\n");
            o.w("    auto plan = " + P + "plan(" + names + ");\n");
            o.w("    auto* self = static_cast<" + shell + "*>(spp::construct_shell(&" + cn + "_record, typeid(" + cn +
                "_SuperArgs).name(), &plan.first, note));\n");
            if (has_entry) {
                o.w("    try {\n        " + entry + "(self, plan.second" + ctor_fwd + ");\n    } catch (...) {\n        delete static_cast<" +
                    B + "*>(self);\n        throw;\n    }\n");
            }
            o.w("    return self;\n}\n\n");
            o.w("[[maybe_unused]] static " + shell + "* " + P + "new(" + pd + ")\n{\n    return " + P + "build(true" +
                ctor_fwd + ");\n}\n\n");
            if (has_entry) {
                o.w("// The factory of the engine calls it through the canonical record: the current generation\n"
                    "// gives the base arguments and the constructors.\n");
                o.w("static spp::ShellBase* " + P + "new_default()\n{\n    return " + P + "build(false" +
                    (defaults.empty() ? "" : ", " + defaults) + ");\n}\n\n");
            }
        }
        // The destructor slot runs the destructor of the class, then the one of the parent.
        const bool dtor_chain = has_dtor || (pinfo && pinfo->dtor_chain);
        info.dtor_chain = dtor_chain;
        if (dtor_chain) {
            o.w("static void " + P + "dtor_slot(spp::ShellBase* s)\n{\n");
            if (has_dtor) o.w("    " + P + "dtor(static_cast<" + shell + "*>(s));\n");
            if (pinfo && pinfo->dtor_chain) o.w("    " + PP + "dtor_slot(s);\n");
            o.w("}\n\n");
        }

        auto emit_body = [&](Method& m, bool is_fn, const std::string& sym, const std::string& ret,
                             const std::string& what) {
            RCtx cx = base_cx;
            cx.where = what;
            cx.ret_type = m.script_ret;
            sppc::add_params(cx, m.params);
            std::string head = "static " + ret + " " + sym + "(";
            if (is_fn) head += shell + "* self" + body_decl(m);
            else head += B + "* a" + param_decl(m);
            o.head_segments(head, sym, m, in.script_text);
            o.w(head + ")\n{\n");
            if (!is_fn) o.w("    [[maybe_unused]] auto* self = static_cast<" + shell + "*>(a);\n");
            o.w("    if (SPP_UNLIKELY(spp::active_generation != GEN)) {\n");
            o.w("        spp::stale_call(\"" + what + "\");\n        return" +
                (ret == "void" ? std::string() : " " + ret + "()") + ";\n    }\n");
            o.w("    [[maybe_unused]] auto* st = state_of(self);\n");
            sppc::lower_block(m.body, cx, g_diag);
            if (!g_diag.items.empty()) throw TranslateAbort{};
            sppc::ast::Emitter em(map, opt.input);
            sppc::ast::emit_stmts(m.body.stmts, em, 1);
            em.finish();
            o.w_emitted(em, in.script_text);
            reset();
            o.w("}\n\n");
        };

        // A convert hook has no self and no state: the new block is not complete yet.
        auto emit_convert = [&](Method& m) {
            RCtx cx = base_cx;
            cx.where = body_what(cn, m);
            cx.allow_this = false;
            cx.field_type.clear();
            cx.fns.clear();
            sppc::add_params(cx, m.params);
            o.w("static " + m.ret + " " + P + m.name + "(" + m.params[0].type + " " + m.params[0].name + ")\n{\n");
            sppc::lower_block(m.body, cx, g_diag);
            if (!g_diag.items.empty()) throw TranslateAbort{};
            sppc::ast::Emitter em(map, opt.input);
            sppc::ast::emit_stmts(m.body.stmts, em, 1);
            em.finish();
            o.w_emitted(em, in.script_text);
            reset();
            o.w("}\n\n");
        };

        if (has_ctor) {
            ctor_m.body = std::move(cd.ctors[0].body);
            emit_body(ctor_m, true, P + "ctor", "void", cn + "::" + cn);
        }
        if (has_dtor) emit_body(cd.dtors[0], true, P + "dtor", "void", cn + "::~" + cn);
        for (Method& m : cd.methods) {
            if (hook_of(m) == Hook::Convert) {
                emit_convert(m);
            } else if (m.overrides_virtual) {
                const Slot* s = nullptr;
                for (const Slot& x : slots)
                    if (x.impl == &m) s = &x;
                emit_body(m, false, P + s->id, s->nv->ret, body_what(cn, m));
            } else {
                emit_body(m, true, P + m.name, fn_ret(m), body_what(cn, m));
            }
        }

        // forwarders for the native virtuals that the script does not override
        for (size_t i = 0; i < native_count; ++i) {
            const Slot& s = slots[i];
            if (s.impl || par) continue;
            const NativeVirtual& nv = *s.nv;
            std::string ps, as;
            for (size_t k = 0; k < nv.ptypes.size(); ++k) {
                ps += ", " + nv.ptypes[k] + " a" + std::to_string(k);
                as += (k ? ", " : "") + arg_expr(nv.ptypes[k], "a" + std::to_string(k));
            }
            o.w("// Not overridden: call the implementation of " + nv.from + ".\n");
            o.w("static " + nv.ret + " " + P + "fwd_" + s.id + "(" + B + "* a" + ps + ")\n{\n");
            o.w("    return static_cast<" + shell + "*>(a)->" + nv.from + "::" + nv.name + "(" + as + ");\n}\n\n");
        }

        // The native implementation that a child sees: "super.M()" in a child calls it.
        if (parent_shell)
            for (size_t i = 0; i < native_count; ++i) {
                const NativeVirtual& nv = *slots[i].nv;
                std::string ps, as;
                for (size_t k = 0; k < nv.ptypes.size(); ++k) {
                    ps += ", " + nv.ptypes[k] + " a" + std::to_string(k);
                    as += ", " + arg_expr(nv.ptypes[k], "a" + std::to_string(k));
                }
                o.w("[[maybe_unused]] static " + nv.ret + " " + P + "base_" + nv.name + "(" + B + "* a" + ps +
                    ")\n{\n    return " + table[i] + "(a" + as + ");\n}\n\n");
            }

        // slot adapters and thunks for the script functions
        for (size_t i = native_count; i < slots.size(); ++i) {
            const Method& m = *slots[i].impl;
            o.w("static " + fn_ret(m) + " " + P + m.name + "_slot(" + B + "* a" + param_decl(m) + ")\n{\n");
            o.w("    return " + P + m.name + "(static_cast<" + shell + "*>(a)" + arg_list(m) + ");\n}\n\n");
        }
        for (const Method* mp : script_overrides) {
            const Method& m = *mp;
            o.w("static " + fn_ret(m) + " " + P + m.name + "_slot(" + B + "* a" + param_decl(m) + ")\n{\n");
            o.w("    return " + P + m.name + "(static_cast<" + shell + "*>(a)" + arg_list(m) + ");\n}\n\n");
        }
        for (size_t i = native_count; i < slots.size(); ++i) {
            if (slots[i].intro_gen != opt.gen) continue;
            const Method& m = *slots[i].impl;
            o.w("// The thunk has the same address in each generation (section 4.11). The\n"
                "// generation that adds the function emits it. It reads the slot at call time.\n");
            o.w("static " + fn_ret(m) + " " + P + m.name + "__thunk(" + B + "* a" + param_decl(m) + ")\n{\n");
            o.w("    auto* self = static_cast<" + shell + "*>(a);\n");
            o.w("    return spp::call<" + fn_ret(m) + "(" + B + "*" + type_list(m) + ")>(self->cls, " +
                slot_id(m.name) + ", a" + arg_list(m) + ");\n}\n\n");
            thunk_regs.push_back("    reg.add_thunk(\"" + cn + "::" + m.name +
                                 "\", reinterpret_cast<void*>(&" + P + m.name + "__thunk));\n");
        }

        // A bare "this" used as a handle in a native-base class needs shared_from_this in the
        // shell. The insert stays in one line, so the line numbers do not change.
        // A shell with script children always gets it: a child cannot add it to the parent.
        if ((handle_this || parent_shell) && !par && !cd.base.empty() &&
            types.storage(cn).rfind("std::shared_ptr<", 0) == 0)
            o.insert(shell_head_end, ", std::enable_shared_from_this<" + shell + ">");

        // ---- record and entry
        o.w("// ------------------------------------------------------ record and entry\n");
        o.w("static void* " + cn + "_slots[" + slot_id("COUNT") + "] = {\n");
        for (const std::string& fn : table)
            o.w(fn.empty() ? std::string("    nullptr,\n") : "    reinterpret_cast<void*>(&" + fn + "),\n");
        o.w("};\n\n");
        std::string on_reload_init;
        for (const Method& m : cd.methods)
            if (hook_of(m) == Hook::OnReload) {
                o.w("static void " + P + "onReload_hook(spp::ShellBase* s, const spp::OldState& old)\n{\n    " + P +
                    "onReload(static_cast<" + shell + "*>(s), old);\n}\n\n");
                on_reload_init = " " + P + "onReload_hook,\n";
                info.on_reload = P + "onReload_hook";
            }
        // A child without an onReload hook uses the hook of the parent.
        if (on_reload_init.empty() && pinfo && !pinfo->on_reload.empty()) {
            on_reload_init = " " + pinfo->on_reload + ",\n";
            info.on_reload = pinfo->on_reload;
        }
        // After on_reload: construct, construct_key, new_default, canonical and dtor.
        std::string ctor_init;
        if (gen_new || dtor_chain)
            ctor_init = std::string(on_reload_init.empty() ? "    nullptr,\n" : "") + "    " +
                        (gen_new ? "&" + P + "construct, typeid(" + cn + "_SuperArgs).name()" : "nullptr, nullptr") +
                        ", " + (!entry.empty() ? "&" + P + "new_default" : std::string("nullptr")) + ", nullptr, " +
                        (dtor_chain ? "&" + P + "dtor_slot" : std::string("nullptr")) + ",\n";
        o.w("spp::ClassRecord " + cn + "_record = {\n    \"" + cn + "\", GEN, " + cn + "_slots, " +
            slot_id("COUNT") + ", &" + cn + "_layout, " + (par ? "&" + par->name + "_record" : std::string("nullptr")) +
            ",\n    \"" + BL + "\", spp::shell_layout_hash(\"" +
            BL + "\", sizeof(" + shell + "), alignof(" + shell + "), " + std::to_string(native_count) +
            "),\n" + (on_reload_init.empty() ? "" : "   " + on_reload_init) + ctor_init + "};\n\n");
        if (!entry.empty()) {
            // The factory of the engine has no arguments. The current generation builds the
            // object with default values (new_default). A removed class has no new_default: then
            // the factory builds the base with the plan of this generation and runs no constructor.
            std::string defaults;
            for (size_t i = 0; i < ctor_m.params.size(); ++i)
                defaults += (i ? ", " : "") + ("std::decay_t<" + ctor_m.params[i].type + ">()");
            o.w("static spp::Instance " + cn + "_create()\n{\n    spp::ShellBase* s = nullptr;\n    if (" + cn +
                "_record.new_default) {\n        s = " + cn + "_record.new_default();\n    } else {\n        auto plan = " + P +
                "plan(" + defaults + ");\n        s = " + P + "construct(&plan.first);\n    }\n    auto* p = static_cast<" +
                shell + "*>(s);\n    return {static_cast<" + B + "*>(p), p};\n}\n\n");
        } else {
            o.w("static spp::Instance " + cn + "_create()\n{\n    auto* p = new " + shell +
                "(spp::Construct{}" + ctor_args + ");\n    return {static_cast<" + B + "*>(p), p};\n}\n\n");
        }
        registers.push_back({cn, true});
        infos[cn] = std::move(info);
    }

    o.w("extern \"C\" void script_register(spp::Registry& reg)\n{\n");
    if (has_module) o.w("    reg.set_module_state(&" + kModuleName + "_layout, &" + kModuleName + "_slot);\n");
    for (const auto& r : registers) o.w("    reg.add(&" + r.first + "_record, &" + r.first + "_create);\n");
    for (const auto& t : thunk_regs) o.w(t);
    o.w("}\n");

    if (uses_format) o.insert(format_include_pos, in.format_lib == "fmt" ? "#include <fmt/format.h>" : "#include <format>");
    res.cpp = o.s;
    for (const auto& g : o.segs) res.source_map.add(g);
    res.source_map.build(in.script_text, res.cpp);
    std::ostringstream mf;
    for (const auto& e : new_manifest) mf << e.cls << "::" << e.fn << " " << e.gen << "\n";
    res.slots_out = mf.str();
}

}  // namespace

namespace sppc {

namespace {

std::vector<uint32_t> line_starts(std::string_view text)
{
    std::vector<uint32_t> v{0};
    for (size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n') v.push_back(static_cast<uint32_t>(i + 1));
    return v;
}

// The byte offset of a 1-based line and column, or no value when the line does not exist.
std::optional<uint32_t> offset_of(const std::vector<uint32_t>& starts, int line, int col)
{
    if (line < 1 || col < 1 || static_cast<size_t>(line) > starts.size()) return std::nullopt;
    return starts[static_cast<size_t>(line) - 1] + static_cast<uint32_t>(col - 1);
}

SourcePos pos_of(const std::vector<uint32_t>& starts, uint32_t off)
{
    const auto it = std::upper_bound(starts.begin(), starts.end(), off);
    const int line = static_cast<int>(it - starts.begin());
    return {line, static_cast<int>(off - starts[static_cast<size_t>(line) - 1]) + 1};
}

}  // namespace

void SourceMap::build(std::string_view script, std::string_view generated)
{
    std::stable_sort(segments_.begin(), segments_.end(),
                     [](const SourceSegment& a, const SourceSegment& b) { return a.gen_begin < b.gen_begin; });
    by_script_.resize(segments_.size());
    for (size_t i = 0; i < by_script_.size(); ++i) by_script_[i] = static_cast<uint32_t>(i);
    std::stable_sort(by_script_.begin(), by_script_.end(), [&](uint32_t a, uint32_t b) {
        return segments_[a].script_begin < segments_[b].script_begin;
    });
    script_lines_ = line_starts(script);
    gen_lines_ = line_starts(generated);
}

std::optional<SourcePos> SourceMap::map_to_script(int gen_line, int gen_col) const
{
    const auto off = offset_of(gen_lines_, gen_line, gen_col);
    if (!off || segments_.empty()) return std::nullopt;
    // The last segment that starts at or before the offset.
    auto it = std::upper_bound(segments_.begin(), segments_.end(), *off,
                               [](uint32_t o, const SourceSegment& g) { return o < g.gen_begin; });
    if (it == segments_.begin()) return std::nullopt;
    --it;
    if (*off >= it->gen_begin + it->gen_len) return std::nullopt;
    const uint32_t d = *off - it->gen_begin;
    return pos_of(script_lines_, it->script_begin + (it->script_len == it->gen_len ? d : 0));
}

std::optional<SourcePos> SourceMap::map_to_generated(int script_line, int script_col) const
{
    const auto off = offset_of(script_lines_, script_line, script_col);
    if (!off || by_script_.empty()) return std::nullopt;
    // The segments that start at or before the offset: the nearest one that holds the offset.
    auto it = std::upper_bound(by_script_.begin(), by_script_.end(), *off,
                               [&](uint32_t o, uint32_t i) { return o < segments_[i].script_begin; });
    for (int tries = 0; it != by_script_.begin() && tries < 16; ++tries) {
        --it;
        const SourceSegment& g = segments_[*it];
        if (*off >= g.script_begin + g.script_len) continue;
        // Several segments can start at the same offset: take the first one in the generated text.
        auto first = it;
        while (first != by_script_.begin() && segments_[*(first - 1)].script_begin == g.script_begin) --first;
        const SourceSegment& f = segments_[*first];
        const uint32_t d = *off - f.script_begin;
        return pos_of(gen_lines_, f.gen_begin + (f.script_len == f.gen_len ? d : 0));
    }
    return std::nullopt;
}

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
    res.diagnostics = g_diag.format(g_script);
    res.warnings = g_diag.format_warnings(g_script);
    return res;
}

}  // namespace sppc

namespace sppc {

namespace {

SourcePos src_pos(const LineMap& map, uint32_t off) { return {map.line(off), map.col(off)}; }

// Replaces the bytes of [begin, end) with spaces. The line breaks stay, so no position moves.
void blank(std::string& text, size_t begin, size_t end)
{
    end = std::min(end, text.size());
    for (size_t i = begin; i < end; ++i)
        if (text[i] != '\n') text[i] = ' ';
}

size_t line_begin(const std::string& text, size_t off)
{
    const size_t nl = off == 0 ? std::string::npos : text.rfind('\n', off - 1);
    return nl == std::string::npos ? 0 : nl + 1;
}

size_t line_end(const std::string& text, size_t off)
{
    const size_t nl = text.find('\n', off);
    return nl == std::string::npos ? text.size() : nl;
}

}  // namespace

// The offset of the first whole word "name" at or after "from", or "from".
uint32_t find_word(const std::string& text, uint32_t from, const std::string& name)
{
    auto is_id = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    for (size_t at = text.find(name, from); at != std::string::npos; at = text.find(name, at + 1)) {
        const bool left = at == 0 || !is_id(text[at - 1]);
        const bool right = at + name.size() >= text.size() || !is_id(text[at + name.size()]);
        if (left && right) return static_cast<uint32_t>(at);
    }
    return from;
}

AnalyzeResult analyze(const std::string& text, const TranslateOptions& options)
{
    TranslatorSession session;
    return analyze(text, options, session);
}

AnalyzeResult analyze(const std::string& text, const TranslateOptions& options, TranslatorSession& session)
{
    AnalyzeResult res;
    const LineMap map(text);
    Diagnostics diag;
    const ast::Script sc = parse_script(text, map, diag);

    for (const Diag& d : diag.items) res.errors.push_back({{d.line, d.col}, d.pos, d.msg});
    for (uint32_t h : diag.member_holes) res.completion_points.push_back(src_pos(map, h));

    // The ranges of the members, as (begin, end) byte offsets. The class facts and the map use them.
    std::vector<std::pair<size_t, size_t>> ranges;
    for (const ast::ClassDef& c : sc.classes) {
        AnalyzedClass ac;
        ac.name = c.name;
        ac.base = c.base;
        ac.begin = src_pos(map, c.pos);
        auto add = [&](const char* kind, const std::string& name, uint32_t begin, uint32_t end, bool body) {
            const uint32_t e = std::max(end, begin + static_cast<uint32_t>(name.size()));
            ac.members.push_back({kind, name, src_pos(map, begin), src_pos(map, e)});
            if (body) ranges.emplace_back(begin, e);
        };
        for (const ast::Field& f : c.fields)
            add("field", f.name, f.pos, f.has_init ? f.init_span.end + 1 : 0, false);
        // The pos of a method is the start of its type. The name comes after it.
        for (const ast::Method& m : c.methods)
            add("method", m.name, find_word(text, m.pos, m.name), m.end_pos + 1, true);
        for (const ast::Ctor& k : c.ctors) add("constructor", c.name, k.pos, k.end_pos + 1, true);
        for (const ast::Method& d : c.dtors) add("destructor", d.name, d.pos, d.end_pos + 1, true);
        for (const ast::Property& p : c.props)
            add("property", p.name, p.pos,
                std::max({p.has_init ? p.init_span.end + 1 : 0, p.get_body ? p.get_span.end + 1 : 0,
                          p.set_body ? p.set_span.end + 1 : 0}),
                p.get_body || p.set_body);
        std::stable_sort(ac.members.begin(), ac.members.end(), [](const AnalyzedMember& a, const AnalyzedMember& b) {
            return std::tie(a.begin.line, a.begin.col) < std::tie(b.begin.line, b.begin.col);
        });
        res.classes.push_back(std::move(ac));
    }

    // The text for the translation. A member that holds an error, or a line that holds an
    // error outside a member, is replaced by white space. The offsets stay the same.
    std::string valid = text;
    for (const Diag& d : diag.items) {
        bool in_member = false;
        for (const auto& [b, e] : ranges)
            if (d.pos >= b && d.pos < e) {
                blank(valid, line_begin(valid, b), e);
                in_member = true;
            }
        if (!in_member) blank(valid, line_begin(valid, d.pos), line_end(valid, d.pos));
    }
    TranslateOptions o = options;
    o.script_text = valid;
    TranslateResult tr = translate(o, session);
    if (diag.items.empty()) {
        res.ok = tr.ok;
        res.diagnostics = tr.diagnostics;
    }
    // The blank text has the same offsets and the same lines as "text". So the map is valid for it.
    if (tr.ok) {
        res.source_map = std::move(tr.source_map);
        res.cpp = std::move(tr.cpp);
    }
    return res;
}

}  // namespace sppc
