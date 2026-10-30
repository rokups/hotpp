// Tests for the ".cpp" frontend: the structure, the body edits and the error messages.
// The base class Actor comes from tests/engine/Actor.h.
#include "parser/cpp_frontend.h"
#include "spp/sppc_lib.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace sppc;

namespace
{

std::vector<std::string> clang_args(const std::string& extra_dir)
{
    std::vector<std::string> a = {"-x", "c++", "-std=c++17", "-I" + std::string(SPP_SOURCE_DIR) + "/tests/engine",
        "-I" + std::string(SPP_SOURCE_DIR) + "/include", "-resource-dir=" + std::string(SPP_RESOURCE_DIR)};
    if (!extra_dir.empty())
        a.push_back("-I" + extra_dir);
    return a;
}

struct Parsed
{
    std::string text;
    LineMap map;
    Diagnostics diag;
    ast::Script script;
    explicit Parsed(std::string t, const std::string& extra_dir = {})
        : text(std::move(t))
        , map(text)
    {
        script = parse_cpp_script(text, "script.cpp", clang_args(extra_dir), map, diag);
    }
    std::string errors() const { return diag.format("t"); }
    const ast::Method& method(const std::string& name) const
    {
        for (const auto& m : script.classes.at(0).methods)
            if (m.name == name)
                return m;
        throw std::runtime_error("no method " + name);
    }
};

const char* const kMain = R"(#include "Actor.h"
#include <spp/script.h>
#include <vector>

const int kMax = 5;
int total = 0;
std::vector<int> list{1, 2};
static int helper(int a) { return a + 1; }

class Player : public Actor
{
public:
    Player() : Actor("a)b") { hp = kMax; }
    ~Player() { total = 0; }
    void Tick(float dt) override { Move(Vec3(1, 2, 3)); this->Move(Vec3()); Use(this); hp -= 1; this->hp += 1; TakeDamage(dt); Jump(); total += 1; Position.x = 1; }
    void Again() { Tick(0.5f); this->Tick(1); auto f = [&] { Move(Vec3()); }; f(); }
    void Jump() {}
    void Use(Actor* a) { (void)a; }
    void Peek(Player& other) { other.Move(Vec3()); }
    void onReload(const spp::OldState& old) { (void)old; }
    int hp = 100;
    SPP_ALIAS("mana") int energy = 3;
    std::vector<int> items{1, 2, 3};
    int lives{3};
};

int convert_total(int old) { return old + 1; }
)";

TEST(CppFrontend, Structure)
{
    Parsed p(kMain);
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const ast::Script& s = p.script;
    EXPECT_TRUE(s.is_cpp);
    ASSERT_EQ(s.imports.size(), 3u);
    EXPECT_EQ(s.imports[0].path, "Actor.h");
    EXPECT_EQ(s.imports[1].path, "<spp/script.h>");
    EXPECT_EQ(s.imports[2].line, 3);
    ASSERT_EQ(s.prelude.size(), 2u);
    EXPECT_EQ(s.prelude[0].text, "const int kMax = 5;");
    EXPECT_EQ(s.prelude[0].line, 5);
    EXPECT_EQ(s.prelude[1].text, "static int helper(int a) { return a + 1; }");
    ASSERT_EQ(s.vars.size(), 2u);
    EXPECT_EQ(s.vars[0].name, "total");
    EXPECT_EQ(s.vars[0].raw_init, "0");
    EXPECT_TRUE(s.vars[0].raw);
    EXPECT_EQ(s.vars[1].raw_init, "{1, 2}");
    EXPECT_TRUE(s.vars[1].raw_brace_init);
    ASSERT_EQ(s.var_hooks.size(), 1u);
    EXPECT_EQ(s.var_hooks[0].raw_body, " return old + 1; ");
    ASSERT_EQ(s.classes.size(), 1u);
    const ast::ClassDef& c = s.classes[0];
    EXPECT_EQ(c.name, "Player");
    EXPECT_EQ(c.base, "Actor");
    EXPECT_EQ(c.line, 10);
    const ast::Method& tick = p.method("Tick");
    EXPECT_TRUE(tick.is_override);
    EXPECT_FALSE(tick.is_virtual);
    EXPECT_TRUE(tick.raw);
    EXPECT_EQ(tick.ret, "void");
    ASSERT_EQ(tick.params.size(), 1u);
    EXPECT_EQ(tick.params[0].type, "float");
    EXPECT_EQ(tick.line, 15);
    EXPECT_EQ(tick.end_line, 15);
    EXPECT_EQ(p.method("onReload").params.at(0).type, "OldState");
}

TEST(CppFrontend, MethodBodyEdits)
{
    Parsed p(kMain);
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    // A base call gets "__self", a script field and a script method stay, "this" as a value
    // becomes "__self", a module variable goes through spp_mod().
    EXPECT_EQ(p.method("Tick").raw_body,
        " __self->Move(Vec3(1, 2, 3)); __self->Move(Vec3()); Use(__self); hp -= 1; this->hp += 1; "
        "__self->TakeDamage(dt); Jump(); spp_mod()->total += 1; __self->Position.x = 1; ");
    // A call of an override goes through the shell. A [&] lambda can use a base member.
    EXPECT_EQ(p.method("Again").raw_body,
        " __self->Tick(0.5f); __self->Tick(1); auto f = [&] { __self->Move(Vec3()); }; f(); ");
    // An explicit other object stays.
    EXPECT_EQ(p.method("Peek").raw_body, " other.Move(Vec3()); ");
}

TEST(CppFrontend, CtorDtorFields)
{
    Parsed p(kMain);
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const ast::ClassDef& c = p.script.classes.at(0);
    ASSERT_EQ(c.ctors.size(), 1u);
    EXPECT_TRUE(c.ctors[0].raw);
    EXPECT_TRUE(c.ctors[0].has_base_call);
    EXPECT_EQ(c.ctors[0].raw_base_args, "\"a)b\"");
    EXPECT_EQ(c.ctors[0].raw_body, " hp = kMax; ");
    ASSERT_EQ(c.dtors.size(), 1u);
    EXPECT_EQ(c.dtors[0].name, "~Player");
    EXPECT_EQ(c.dtors[0].raw_body, " spp_mod()->total = 0; ");
    ASSERT_EQ(c.fields.size(), 4u);
    EXPECT_EQ(c.fields[0].name, "hp");
    EXPECT_EQ(c.fields[0].raw_init, "100");
    EXPECT_TRUE(c.fields[0].raw);
    EXPECT_EQ(c.fields[1].name, "energy");
    EXPECT_EQ(c.fields[1].alias, "mana");
    EXPECT_EQ(c.fields[1].raw_init, "3");
    EXPECT_EQ(c.fields[2].raw_init, "{1, 2, 3}");
    EXPECT_TRUE(c.fields[2].raw_brace_init);
    EXPECT_EQ(c.fields[3].raw_init, "{3}");
    EXPECT_TRUE(c.fields[3].raw_brace_init);
}

TEST(CppFrontend, BaseStaticAndNestedNames)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(testing::TempDir()) / "spp_cpp_frontend";
    fs::create_directories(dir);
    std::ofstream(dir / "NativeBase.h") << "#pragma once\nstruct NativeBase {\n  virtual ~NativeBase() = default;\n"
                                           "  static int Count;\n  enum Kind { KA, KB };\n  struct Info { int v = 0; };\n"
                                           "  static int Twice(int x) { return 2 * x; }\n};\n";
    Parsed p("#include \"NativeBase.h\"\nclass S : public NativeBase\n{\npublic:\n"
             "    int F() { Info i{}; return Count + Twice(KA) + NativeBase::Count + i.v; }\n};\n",
        dir.string());
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    EXPECT_EQ(p.method("F").raw_body,
        " NativeBase::Info i{}; return NativeBase::Count + NativeBase::Twice(NativeBase::KA) + NativeBase::Count + i.v; ");
}

void expect_error(const std::string& body, const std::string& msg)
{
    Parsed p("#include \"Actor.h\"\n" + body);
    EXPECT_NE(p.errors().find(msg), std::string::npos) << "script:\n" << body << "\nerrors:\n" << p.errors();
}

TEST(CppFrontend, StructureErrors)
{
    expect_error("struct B { virtual ~B() {} };\nclass S : public B {};",
        "a .cpp script class cannot derive from a class of the same file (first version)");
    expect_error("class S : public Actor { static int n; };", "a static data member is not supported in a .cpp script");
    expect_error("class S : public Actor { virtual void F() {} };",
        "'virtual' is not supported in a .cpp script; use 'override' of a native virtual");
    expect_error("class S : public Actor { static void F() {} };", "a static method is not supported in a .cpp script");
    expect_error("class S : public Actor { void F(); };", "define the method inside the class");
    expect_error("class S : public Actor { int hp = 0; S() : hp(1) {} };", "use a default member initializer");
    expect_error("class S : public Actor { int hp = 0; S() : Actor(), hp(1) {} };", "use a default member initializer");
    expect_error("int total = 0;\ntemplate <class T> T g(T a) { return a + total; }",
        "a function template cannot use the module variable 'total' in a .cpp script");
    expect_error("int total = 0;\nconstexpr int g(bool b) { return b ? 1 : total; }",
        "a constexpr function cannot use the module variable 'total' in a .cpp script");
    expect_error("int total = 0;\nnamespace n { int g() { return total; } }",
        "a function in a namespace cannot use the module variable 'total' in a .cpp script");
    expect_error("int total = 0;\nint g(int a = total) { return a; }",
        "a default argument cannot use the module variable 'total' in a .cpp script");
    expect_error("class S : public Actor {};\nvoid g(S* s) { (void)s; }",
        "the script class 'S' cannot be named outside its body (first version)");
}

// A free function that uses a module variable: the prelude has the prototype and "post_funcs"
// has the definition with the edited body. A function without a module variable stays as written.
TEST(CppFrontend, FreeFunctionWithModuleVariable)
{
    Parsed p("int total = 0;\n"
             "static inline int plain(int a) { return a + 1; }\n"
             "static inline int bump(int a,\n"
             "                       int b = 2) { total += a + b; return total; }\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const ast::Script& s = p.script;
    ASSERT_EQ(s.prelude.size(), 2u);
    EXPECT_EQ(s.prelude[0].text, "static inline int plain(int a) { return a + 1; }");
    EXPECT_EQ(s.prelude[1].text, "static inline int bump(int a,\n                       int b = 2);");
    EXPECT_EQ(s.prelude[1].line, 3);
    ASSERT_EQ(s.post_funcs.size(), 1u);
    EXPECT_EQ(s.post_funcs[0].line, 3);
    EXPECT_EQ(s.post_funcs[0].text,
        "static inline int bump(int a,\n                       int b)"
        " { spp_mod()->total += a + b; return spp_mod()->total; }");
}

// A default argument keeps its text. A module variable and a base member get the edits of a body.
TEST(CppFrontend, DefaultArguments)
{
    Parsed p(std::string(kMain) + R"(
int level = 3;
class D : public Actor
{
public:
    D(int a = 4, float b = 0.5f) : Actor("D") {}
    int F(int x, int y = kMax, int z = level, int w = {}) { return x + y + z + w; }
    int G(int x) { return F(x); }
};
)");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const ast::ClassDef* d = nullptr;
    for (const auto& c : p.script.classes)
        if (c.name == "D")
            d = &c;
    ASSERT_NE(d, nullptr);
    ASSERT_EQ(d->ctors.size(), 1u);
    ASSERT_EQ(d->ctors[0].params.size(), 2u);
    ASSERT_NE(d->ctors[0].params[0].def, nullptr);
    EXPECT_EQ(d->ctors[0].params[0].def->cpp, "4");
    EXPECT_EQ(d->ctors[0].params[1].def->cpp, "0.5f");
    const ast::Method* f = nullptr;
    for (const auto& m : d->methods)
        if (m.name == "F")
            f = &m;
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->params[0].def, nullptr);
    EXPECT_EQ(f->params[1].def->cpp, "kMax");
    EXPECT_EQ(f->params[2].def->cpp, "spp_mod()->level"); // a module variable, as in a body
    EXPECT_EQ(f->params[3].def->cpp, "{}");
}

// The prototype has the default. The definition and the thunk do not repeat it.
TEST(CppFrontend, DefaultArgumentsEmit)
{
    sppc::TranslateOptions o;
    o.script_name = "t.cpp";
    o.script_text = R"(#include "Actor.h"
int level = 3;
class S : public Actor
{
public:
    S(int a = 4) : Actor("S") {}
    int F(int x, int y = 2, int z = level) { return x + y + z; }
    void Tick(float dt) override { F(1); }
};
)";
    o.generation = 1;
    o.include_dirs = {std::string(SPP_SOURCE_DIR) + "/tests/engine", std::string(SPP_SOURCE_DIR) + "/include"};
    o.resource_dir = SPP_RESOURCE_DIR;
    const sppc::TranslateResult r = sppc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("S__F(S_Shell* __self, int x, int y = 2, int z = spp_mod()->level);"), std::string::npos)
        << r.cpp;
    EXPECT_NE(r.cpp.find("S__new(int a = 4)"), std::string::npos) << r.cpp;
    // The body calls the member directly, so the member declaration has the defaults too.
    EXPECT_NE(r.cpp.find("    int F(int x, int y = 2, int z = spp_mod()->level);"), std::string::npos) << r.cpp;
    // The definitions have no default.
    EXPECT_NE(r.cpp.find("SState_g1::F(int x, int y, int z)"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("S__F(S_Shell* __self, int x, int y, int z)\n"), std::string::npos) << r.cpp;
}

// A default argument on an override is an error: the call through the base sees the base declaration.
TEST(CppFrontend, DefaultArgumentOnOverrideFails)
{
    sppc::TranslateOptions o;
    o.script_name = "t.cpp";
    o.script_text = R"(#include "Actor.h"
class S : public Actor
{
public:
    void TakeDamage(float a = 1.0f) override {}
};
)";
    o.generation = 1;
    o.include_dirs = {std::string(SPP_SOURCE_DIR) + "/tests/engine", std::string(SPP_SOURCE_DIR) + "/include"};
    o.resource_dir = SPP_RESOURCE_DIR;
    const sppc::TranslateResult r = sppc::translate(o);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("default values are not allowed on virtual or override methods"), std::string::npos)
        << r.diagnostics;
}

TEST(CppFrontend, EditErrors)
{
    expect_error("#define MV Move(Vec3())\nclass S : public Actor { void F() { MV; } };",
        "a macro hides a member of the base class; write the member in the script text");
    expect_error("class S : public Actor { void F() { auto f = [this] { Move(Vec3()); }; f(); } };",
        "a lambda that uses a base member must capture with [&] or [=]");
    expect_error("class S : public Actor { void F() { S* p = nullptr; (void)p; } };",
        "the script class 'S' cannot be named outside its body (first version)");
}

// A brace initializer goes to the generated code once: "T{1, 2}", not "T{{1, 2}}". An aggregate
// and a container need this.
TEST(CppFrontend, BraceInitializerEmit)
{
    sppc::TranslateOptions o;
    o.script_name = "t.cpp";
    o.script_text = R"(#include "Actor.h"
#include <vector>
struct Point { float x = 0, y = 0; };
Point origin{3, 4};
std::vector<int> table{5, 6};
class S : public Actor
{
public:
    Point p{1, 2};
    std::vector<int> items{1, 2};
};
)";
    o.generation = 1;
    o.include_dirs = {std::string(SPP_SOURCE_DIR) + "/tests/engine", std::string(SPP_SOURCE_DIR) + "/include"};
    o.resource_dir = SPP_RESOURCE_DIR;
    const sppc::TranslateResult r = sppc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("using T = Point; new (f) T{1, 2};"), std::string::npos);
    EXPECT_NE(r.cpp.find("using T = std::vector<int>; new (f) T{1, 2};"), std::string::npos);
    EXPECT_NE(r.cpp.find("using T = Point; new (f) T{3, 4};"), std::string::npos);
    EXPECT_NE(r.cpp.find("using T = std::vector<int>; new (f) T{5, 6};"), std::string::npos);
    EXPECT_EQ(r.cpp.find("T{{"), std::string::npos);
}

// The raw body starts right after the "{", so the "#line" of the body is the line of the "{".
// An Allman brace and a signature of two lines move the "{" away from the name.
TEST(CppFrontend, RawBodyLine)
{
    const std::string text = R"(#include "Actor.h"
int total = 0;
int convert_total(int old)
{
    return old * 2;
}
class S : public Actor
{
public:
    S()
    {
        hp = 1;
    }
    void F(int a,
        int b)
    {
        Move(Vec3());
    }
    int hp = 0;
    ~S()
    {
    }
};
)";
    Parsed p(text);
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.script.var_hooks.size(), 1u);
    EXPECT_EQ(p.script.var_hooks[0].raw_line, 4);
    const ast::ClassDef& c = p.script.classes.at(0);
    EXPECT_EQ(c.ctors.at(0).raw_line, 11);
    EXPECT_EQ(p.method("F").line, 14);
    EXPECT_EQ(p.method("F").raw_line, 16);
    EXPECT_EQ(c.dtors.at(0).raw_line, 21);

    sppc::TranslateOptions o;
    o.script_name = "t.cpp";
    o.script_text = text;
    o.generation = 1;
    o.include_dirs = {std::string(SPP_SOURCE_DIR) + "/tests/engine", std::string(SPP_SOURCE_DIR) + "/include"};
    o.resource_dir = SPP_RESOURCE_DIR;
    const sppc::TranslateResult r = sppc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // The first line after the "#line" is the rest of the line of the "{". The statement is two lines later.
    EXPECT_NE(r.cpp.find("#line 16 \"t.cpp\"\n\n        __self->Move(Vec3());"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("#line 11 \"t.cpp\"\n\n        hp = 1;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("#line 4 \"t.cpp\"\n\n    return old * 2;"), std::string::npos) << r.cpp;
}

// A "(...)" initializer gives "T(3, 7)". A comma expression "T((3, 7))" builds a vector of 7 zeros.
TEST(CppFrontend, ParenInitializerEmit)
{
    sppc::TranslateOptions o;
    o.script_name = "t.cpp";
    o.script_text = R"(#include "Actor.h"
#include <vector>
std::vector<int> v(3, 7);
class S : public Actor
{
public:
    int hp = 0;
};
)";
    o.generation = 1;
    o.include_dirs = {std::string(SPP_SOURCE_DIR) + "/tests/engine", std::string(SPP_SOURCE_DIR) + "/include"};
    o.resource_dir = SPP_RESOURCE_DIR;
    const sppc::TranslateResult r = sppc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    size_t n = 0;
    for (size_t at = r.cpp.find("new (f) T(3, 7);"); at != std::string::npos; at = r.cpp.find("new (f) T(3, 7);", at + 1))
        ++n;
    EXPECT_GE(n, 1u) << r.cpp; // the module variable
    EXPECT_EQ(r.cpp.find("T(("), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("(3, 7)"), std::string::npos);
}

TEST(CppFrontend, ParenInitializerText)
{
    // C++ has no "T x(a);" in a class, so a module variable is the only case.
    Parsed p("#include \"Actor.h\"\n#include <vector>\nstd::vector<int> v(3, 7);\nstd::vector<int> w{3, 7};\n"
             "std::vector<int> z = std::vector<int>(3, 7);\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.script.vars.size(), 3u);
    EXPECT_EQ(p.script.vars[0].raw_init, "3, 7");
    EXPECT_FALSE(p.script.vars[0].raw_brace_init);
    EXPECT_EQ(p.script.vars[1].raw_init, "{3, 7}");
    EXPECT_TRUE(p.script.vars[1].raw_brace_init);
    EXPECT_EQ(p.script.vars[2].raw_init, "std::vector<int>(3, 7)");
}

// A convert hook cannot read a module variable: the new module state is not complete yet.
TEST(CppFrontend, ConvertHookModuleRef)
{
    expect_error("int level = 0;\nint total = 0;\nint convert_total(int old) { return old + level; }",
        "the module variable 'level' cannot be used in a convert hook: the new module state is not complete yet");
}

// A "this" in a local class of a body is the local object, not the script object.
TEST(CppFrontend, ThisInLocalClass)
{
    Parsed p("#include \"Actor.h\"\nclass S : public Actor\n{\npublic:\n"
             "    void* F() { struct L { void* f() { return this; } }; L l; (void)l; return this; }\n};\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    EXPECT_EQ(p.method("F").raw_body,
        " struct L { void* f() { return this; } }; L l; (void)l; return __self; ");
}

// A prefixed char literal is a literal, not a digit separator. A bracket in it does not count.
TEST(CppFrontend, PrefixedCharLiteral)
{
    for (const char* lit : {"L'('", "u'('", "U'('", "u8'('", "1'000", "0x1'F"})
    {
        const std::string arg = std::string(lit) + " == 0 ? \"a\" : \"b\"";
        Parsed p("#include \"Actor.h\"\nclass S : public Actor\n{\npublic:\n    S() : Actor(" + arg + ") {}\n};\n");
        ASSERT_TRUE(p.diag.items.empty()) << lit << "\n" << p.errors();
        EXPECT_EQ(p.script.classes.at(0).ctors.at(0).raw_base_args, arg) << lit;
    }
    Parsed f("#include \"Actor.h\"\nclass S : public Actor\n{\npublic:\n    int k = (L'(' == 0 ? 1 : 2);\n    int m = 0;\n};\n");
    ASSERT_TRUE(f.diag.items.empty()) << f.errors();
    EXPECT_EQ(f.script.classes.at(0).fields.at(0).raw_init, "(L'(' == 0 ? 1 : 2)");
}

// Only a real directive gives an import: not one in a block comment or in an "#if 0" region.
TEST(CppFrontend, IncludesOnlyReal)
{
    Parsed p("#include \"Actor.h\"\n/*\n#include <nothere1>\n*/\n#if 0\n#include <nothere2>\n#endif\n"
             "  #  include <vector>\nint x = 0;\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.script.imports.size(), 2u);
    EXPECT_EQ(p.script.imports[0].path, "Actor.h");
    EXPECT_EQ(p.script.imports[0].line, 1);
    EXPECT_EQ(p.script.imports[1].path, "<vector>");
    EXPECT_EQ(p.script.imports[1].line, 8);
}

// A base that is a template instantiation: a bare base member gets "__self->".
TEST(CppFrontend, TemplateBase)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(testing::TempDir()) / "spp_cpp_frontend_tpl";
    fs::create_directories(dir);
    std::ofstream(dir / "TplBase.h") << "#pragma once\ntemplate <typename T> struct Base {\n"
                                        "  virtual ~Base() = default;\n  T value{};\n  void Hit() {}\n};\n";
    Parsed p("#include \"TplBase.h\"\nclass S : public Base<int>\n{\npublic:\n"
             "    void F() { Hit(); value = 2; }\n};\n",
        dir.string());
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    EXPECT_EQ(p.method("F").raw_body, " __self->Hit(); __self->value = 2; ");
}

TEST(CppFrontend, ClangError)
{
    Parsed p("int x = ;\n");
    ASSERT_FALSE(p.diag.items.empty());
    EXPECT_EQ(p.diag.items[0].line, 1);
}

} // namespace

// ---- sppc::analyze_cpp: the errors of a ".cpp" script for an editor

namespace
{

sppc::TranslateOptions analyze_options()
{
    sppc::TranslateOptions o;
    o.generation = 1;
    o.include_dirs = {std::string(SPP_SOURCE_DIR) + "/tests/engine", std::string(SPP_SOURCE_DIR) + "/include"};
    o.resource_dir = SPP_RESOURCE_DIR;
    return o;
}

} // namespace

TEST(AnalyzeCpp, ValidScriptHasNoError)
{
    const sppc::AnalyzeResult r = sppc::analyze_cpp(
        "#include \"Actor.h\"\nclass S : public Actor\n{\npublic:\n    void Tick(float dt) override {}\n};\n",
        "/w/t.cpp", analyze_options());
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.errors.empty());
    EXPECT_TRUE(r.cpp.empty());
}

// A parse error comes with the line and the column of the script.
TEST(AnalyzeCpp, StaticMethodHasPlace)
{
    const std::string text =
        "#include \"Actor.h\"\nclass S : public Actor\n{\npublic:\n    static void F() {}\n};\n";
    const sppc::AnalyzeResult r = sppc::analyze_cpp(text, "/w/t.cpp", analyze_options());
    EXPECT_FALSE(r.ok);
    ASSERT_EQ(r.errors.size(), 1u) << r.diagnostics;
    EXPECT_EQ(r.errors[0].message, "a static method is not supported in a .cpp script");
    EXPECT_EQ(r.errors[0].pos.line, 5);
    EXPECT_GT(r.errors[0].pos.col, 0);
    // The offset is the place of the error in the text.
    EXPECT_EQ(r.errors[0].offset,
        text.find('\n', text.find("public:")) + 1 + static_cast<size_t>(r.errors[0].pos.col - 1));
}

// An error of a later check (not of the parser) is an error too.
TEST(AnalyzeCpp, TranslationErrorIsReported)
{
    const sppc::AnalyzeResult r = sppc::analyze_cpp(
        "#include \"Actor.h\"\nclass S : public Actor\n{\npublic:\n    void TakeDamage(float a = 1.0f) override {}\n};\n",
        "/w/t.cpp", analyze_options());
    EXPECT_FALSE(r.ok);
    ASSERT_FALSE(r.errors.empty()) << r.diagnostics;
    EXPECT_EQ(r.errors[0].pos.line, 5);
}

TEST(AnalyzeCpp, PathMustBeCpp)
{
    const sppc::AnalyzeResult r = sppc::analyze_cpp("class S {};", "/w/t.spp", analyze_options());
    EXPECT_FALSE(r.ok);
    ASSERT_EQ(r.errors.size(), 1u);
    EXPECT_EQ(r.errors[0].pos.line, 1);
}
