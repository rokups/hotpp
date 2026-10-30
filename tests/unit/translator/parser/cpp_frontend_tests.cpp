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
    // The chunks keep the source order. The module block is at the first module variable.
    using K = ast::Script::Chunk::Kind;
    ASSERT_EQ(s.chunks.size(), 7u);
    EXPECT_EQ(s.chunks[0].text, "#include \"Actor.h\"");
    EXPECT_EQ(s.chunks[1].text, "#include <spp/script.h>");
    EXPECT_EQ(s.chunks[2].line, 3);
    EXPECT_EQ(s.chunks[3].text, "const int kMax = 5;");
    EXPECT_EQ(s.chunks[3].line, 5);
    EXPECT_EQ(s.chunks[4].kind, K::Module);
    EXPECT_EQ(s.chunks[4].line, 6);
    EXPECT_EQ(s.chunks[5].kind, K::Text);
    EXPECT_EQ(s.chunks[5].text, "static int helper(int a) { return a + 1; }");
    EXPECT_EQ(s.chunks[6].kind, K::Class);
    EXPECT_EQ(s.chunks[6].name, "Player");
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
    expect_error("class S : public Actor { virtual void F() {} };",
        "'virtual' is not supported in a .cpp script; use 'override' of a native virtual");
    expect_error("class S : public Actor { void F(); };", "define the method inside the class");
    expect_error("class S : public Actor { int hp = 0; S() : hp(1) {} };", "use a default member initializer");
    expect_error("class S : public Actor { int hp = 0; S() : Actor(), hp(1) {} };", "use a default member initializer");
    expect_error("int total = 0;\nstruct Late { int v = 0; };\nLate late;",
        "the type 'Late' must come before the first module variable 'total': the module block is there "
        "and needs the complete type");
    // C++ accepts a template argument of an incomplete type here, but the module block needs it.
    expect_error("#include <vector>\nstruct Late;\nint total = 0;\nstd::vector<Late> l;\nstruct Late {};",
        "the type 'Late' must come before the first module variable 'total': the module block is there "
        "and needs the complete type");
    expect_error("int total = 0;\nint seven() { return 7; }\nint other = seven();",
        "'seven' must come before the first module variable 'total': the module block is there and holds "
        "the initializers");
    expect_error("int total = 0;\nnamespace n { const int k = total; }",
        "the variable 'k' cannot use the module variable 'total' in a .cpp script: it gets its value before "
        "the module state exists");
    expect_error("class S : public Actor {};\nvoid g(S s) { (void)s; }",
        "the script class 'S' by value is not supported in a .cpp script; use a pointer or a reference");
}

// A static data member is a module variable "<Class>__<name>". The out-of-class definition gives the
// initializer and does not stay in the output. A static const member is a const before the class.
TEST(CppFrontend, StaticDataMembers)
{
    Parsed p("#include \"Actor.h\"\n"
             "int total = 0;\n"
             "class S : public Actor\n"
             "{\n"
             "public:\n"
             "    void F() { n++; S::n += 1; this->n += 1; hits = k + S::k; int* q = &S::n; (void)q; }\n"
             "    static int n;\n"
             "    static inline int hits = 5;\n"
             "    static constexpr int k = 3;\n"
             "private:\n"
             "    static const int f = 2;\n"
             "};\n"
             "int S::n = total + 1;\n"
             "int later() { return S::n + S::hits + S::k; }\n"
             "int after = S::n;\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const ast::Script& s = p.script;
    using K = ast::Script::Chunk::Kind;
    // include, module block, k, f, class, later. The definition "int S::n" has no chunk.
    ASSERT_EQ(s.chunks.size(), 6u);
    EXPECT_EQ(s.chunks[1].kind, K::Module);
    EXPECT_EQ(s.chunks[2].text, "constexpr int S__s_k = 3;");
    EXPECT_EQ(s.chunks[3].text, "const int S__s_f = 2;");
    EXPECT_EQ(s.chunks[4].kind, K::Class);
    EXPECT_EQ(s.chunks[5].text, "int later() { return spp_mod()->S__s_n + spp_mod()->S__s_hits + S__s_k; }");
    ASSERT_EQ(s.vars.size(), 4u);
    EXPECT_EQ(s.vars[0].name, "total");
    EXPECT_EQ(s.vars[1].name, "S__s_n");
    EXPECT_EQ(s.vars[1].type, "int");
    EXPECT_EQ(s.vars[1].raw_init, "spp_mod()->total + 1"); // from the definition outside the class
    EXPECT_EQ(s.vars[2].name, "S__s_hits");
    EXPECT_EQ(s.vars[2].raw_init, "5");
    EXPECT_EQ(s.vars[3].name, "after");
    EXPECT_EQ(s.vars[3].raw_init, "spp_mod()->S__s_n");
    EXPECT_EQ(p.method("F").raw_body,
        " spp_mod()->S__s_n++; spp_mod()->S__s_n += 1; spp_mod()->S__s_n += 1; spp_mod()->S__s_hits = S__s_k + S__s_k; "
        "int* q = &spp_mod()->S__s_n; (void)q; ");
}

// A static data member in a class makes the module block, when no module variable comes before.
TEST(CppFrontend, StaticDataMemberStartsModuleBlock)
{
    Parsed p("#include \"Actor.h\"\nint pre = 1;\nclass S : public Actor { static int n; };\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    using K = ast::Script::Chunk::Kind;
    ASSERT_EQ(p.script.chunks.size(), 3u);
    EXPECT_EQ(p.script.chunks[1].kind, K::Module); // at "pre"
    EXPECT_EQ(p.script.chunks[2].kind, K::Class);
    Parsed q("#include \"Actor.h\"\nclass S : public Actor { static int n; };\nint post = 1;\n");
    ASSERT_TRUE(q.diag.items.empty()) << q.errors();
    ASSERT_EQ(q.script.chunks.size(), 3u);
    EXPECT_EQ(q.script.chunks[1].kind, K::Module); // at the class
    EXPECT_EQ(q.script.chunks[2].kind, K::Class);
}

// A static method is a free function: a prototype before the class block and the definition after
// it. The prototype has the default arguments. The definition has none and keeps the lines.
TEST(CppFrontend, StaticMethods)
{
    Parsed p("#include \"Actor.h\"\n"
             "int total = 0;\n"
             "class S : public Actor\n"
             "{\n"
             "public:\n"
             "    void F() { total = make(1) + S::make(2, 3) + this->make(4) + (*this).make(5); }\n"
             "    static int make(int a, int b = total,\n"
             "                    int c = 2) { total += a; return helper(a) + b + c; }\n"
             "    static int helper(int a) { return a + total; }\n"
             "};\n"
             "int g() { return S::make(1) + S::helper(2); }\n"
             "int (*const fp)(int) = &S::helper;\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const ast::Script& s = p.script;
    using K = ast::Script::Chunk::Kind;
    // include, module, 2 prototypes, class, 2 definitions, g, fp
    ASSERT_EQ(s.chunks.size(), 9u);
    EXPECT_EQ(s.chunks[2].text,
        "int S__s_make(int a, int b = spp_mod()->total,\n                    int c = 2);");
    EXPECT_EQ(s.chunks[3].text, "int S__s_helper(int a);");
    EXPECT_EQ(s.chunks[4].kind, K::Class);
    EXPECT_EQ(s.chunks[5].text,
        "int S__s_make(int a, int b,\n                    int c)"
        " { spp_mod()->total += a; return S__s_helper(a) + b + c; }");
    EXPECT_EQ(s.chunks[5].line, 7);
    EXPECT_EQ(s.chunks[6].text, "int S__s_helper(int a) { return a + spp_mod()->total; }");
    EXPECT_EQ(s.chunks[7].text, "int g() { return S__s_make(1) + S__s_helper(2); }");
    EXPECT_EQ(s.chunks[8].text, "int (*const fp)(int) = &S__s_helper;");
    EXPECT_EQ(p.method("F").raw_body,
        " spp_mod()->total = S__s_make(1) + S__s_make(2, 3) + S__s_make(4) + S__s_make(5); ");
    EXPECT_TRUE(p.script.classes.at(0).methods.size() == 1u); // a static method is not a method of the class
}

// A static method "convert_<name>" is the hook of the static data member <name>.
TEST(CppFrontend, StaticConvertHook)
{
    Parsed p("#include \"Actor.h\"\n"
             "class S : public Actor\n"
             "{\n"
             "    static float n;\n"
             "    static float convert_n(int old) { return old * 2.0f; }\n"
             "    static int convert_other(int old) { return old; }\n"
             "};\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.script.var_hooks.size(), 1u);
    EXPECT_EQ(p.script.var_hooks[0].name, "convert_S__s_n");
    EXPECT_EQ(p.script.var_hooks[0].ret, "float");
    EXPECT_EQ(p.script.var_hooks[0].raw_body, " return old * 2.0f; ");
    // "convert_other" has no static data member "other": it is a plain static method.
    bool found = false;
    for (const auto& c : p.script.chunks)
        found = found || c.text == "int S__s_convert_other(int old);";
    EXPECT_TRUE(found);
}

// The uses of a static member: the object of "obj.name" is dropped only when it has no side effect.
TEST(CppFrontend, StaticMemberObject)
{
    Parsed p("#include \"Actor.h\"\n"
             "class S : public Actor\n"
             "{\n"
             "    void F(Actor* a) { (void)a; }\n"
             "    static int n;\n"
             "    static int make() { return 1; }\n"
             "};\n"
             "struct Plain { static int v; };\n"
             "int Plain::v = 1;\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    // A static member of a class that is not a script class stays as written.
    ASSERT_EQ(p.script.vars.size(), 1u);
    EXPECT_EQ(p.script.vars[0].name, "S__s_n");
    EXPECT_EQ(p.script.chunks.back().text, "int Plain::v = 1;");
    expect_error("class S : public Actor { int get() { return 0; } void F() { (get(), this)->n = 1; } static int n; };",
        "a static member must be used by its name");
}

TEST(CppFrontend, StaticMemberErrors)
{
    // clang rejects "this" and a base member in a static method. The message is clear.
    expect_error("class S : public Actor { static void F() { Move(Vec3()); } };",
        "call to non-static member function without an object argument");
    expect_error("class S : public Actor { static void F() { this->Move(Vec3()); } };",
        "invalid use of 'this' outside of a non-static member function");
    expect_error("class S : public Actor { static int n; };\nint g() { S s; return 0; }",
        "the script class 'S' by value is not supported in a .cpp script; use a pointer or a reference");
    expect_error("class S : public Actor { static void F(); };", "define the method inside the class");
    expect_error("class S : public Actor { static const int k; };", "a static const member needs an initializer");
    expect_error("int a = 1;\nclass S : public Actor { public: static int f() { return 1; } static int n; };\nint b = S::f();",
        "'f' must come before the first module variable 'a'");
}

// A static const member gets its value at load. It cannot use a module variable or a static method.
TEST(CppFrontend, StaticConstInitializerErrors)
{
    expect_error("int g = 1;\nclass C : public Actor { inline static const int s = g; };",
        "cannot use the module variable 'g'");
    expect_error("class C : public Actor { static int scale(int v) { return v; } inline static const int s = scale(2); };",
        "the static const member 's' cannot call the static method");
}

// The default argument of a static method follows "=". An array bound or a template argument in
// the type is not a default argument.
TEST(CppFrontend, StaticMethodDefaultArgumentDetection)
{
    Parsed p("#include <array>\n#include \"Actor.h\"\n"
             "constexpr int kN = 3;\n"
             "class C : public Actor {\n"
             "public:\n"
             "    static int sum(const std::array<int, kN>& a, int k = kN) { return a[0] + k; }\n"
             "    static int arr(int (&a)[kN], int k = 2) { return a[0] + k; }\n"
             "};\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    std::string protos, defs;
    for (const auto& c : p.script.chunks)
        (c.text.find("{") == std::string::npos ? protos : defs) += c.text + "\n";
    EXPECT_NE(protos.find("std::array<int, kN>& a, int k = kN)"), std::string::npos) << protos;
    EXPECT_NE(protos.find("int (&a)[kN], int k = 2)"), std::string::npos) << protos;
    EXPECT_NE(defs.find("std::array<int, kN>& a, int k)"), std::string::npos) << defs;
    EXPECT_NE(defs.find("int (&a)[kN], int k)"), std::string::npos) << defs;
}

// A static member has the name "<Class>__s_<name>". It never equals a generated name of the class,
// such as "C__plan". The generated code has the function "C__plan" next to the static method.
TEST(CppFrontend, StaticMemberNamesDoNotCollide)
{
    sppc::TranslateOptions o;
    o.script_name = "t.cpp";
    o.script_text = R"(#include "Actor.h"
class C : public Actor
{
public:
    C() : Actor("C") {}
    void Tick(float dt) override { last = plan() + super_args() + new_default() + construct_keys; }
    static int plan() { return 1; }
    static int super_args() { return 2; }
    static int new_default() { return 3; }
    inline static const int construct_keys = 4;
    int last = 0;
};
)";
    o.generation = 1;
    o.include_dirs = {std::string(SPP_SOURCE_DIR) + "/tests/engine", std::string(SPP_SOURCE_DIR) + "/include"};
    o.resource_dir = SPP_RESOURCE_DIR;
    const sppc::TranslateResult r = sppc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    for (const char* n : {"int C__s_plan() {", "int C__s_super_args() {", "int C__s_new_default() {",
             "const int C__s_construct_keys = 4;"})
        EXPECT_NE(r.cpp.find(n), std::string::npos) << n << "\n" << r.cpp;
    EXPECT_NE(r.cpp.find("C__s_plan() + C__s_super_args() + C__s_new_default() + C__s_construct_keys"),
        std::string::npos)
        << r.cpp;
    EXPECT_NE(r.cpp.find("SPP_HIDDEN static auto C__plan()"), std::string::npos) << r.cpp; // the generated one
}

// A pointer or a reference to a type needs only a declaration before the first module variable.
TEST(CppFrontend, ModuleVariablePointerToLaterType)
{
    Parsed p("struct Fwd;\nint total = 0;\nFwd* pf = nullptr;\nFwd& rf = *pf;\nstruct Fwd { int x; };\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.script.vars.size(), 3u);
}

// A variable in a namespace and a static local variable are not module variables. Their text
// stays as written. A use of a module variable next to them gets the edit.
TEST(CppFrontend, NamespaceAndStaticLocalVariables)
{
    Parsed p("namespace n { int c = 0; }\n"
             "int total = 0;\n"
             "int f() { static int calls = 0; return ++calls + n::c + total; }\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const ast::Script& s = p.script;
    ASSERT_EQ(s.vars.size(), 1u);
    EXPECT_EQ(s.vars[0].name, "total");
    ASSERT_EQ(s.chunks.size(), 3u);
    EXPECT_EQ(s.chunks[0].text, "namespace n { int c = 0; }");
    EXPECT_EQ(s.chunks[1].kind, ast::Script::Chunk::Kind::Module);
    EXPECT_EQ(s.chunks[2].text, "int f() { static int calls = 0; return ++calls + n::c + spp_mod()->total; }");
}

// A declaration that uses a module variable stays in its place, with the edits of a body. This
// is true for a free function, a function template, a constexpr function, a function in a
// namespace and a default argument. A declaration without a module variable stays as written.
TEST(CppFrontend, FreeFunctionWithModuleVariable)
{
    Parsed p("int total = 0;\n"
             "static inline int plain(int a) { return a + 1; }\n"
             "static inline int bump(int a,\n"
             "                       int b = 2) { total += a + b; return total; }\n"
             "template <class T> T g(T a) { return a + total; }\n"
             "constexpr int c(bool b) { return b ? 1 : total; }\n"
             "namespace n { int h() { return total; } }\n"
             "int d(int a = total);\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const ast::Script& s = p.script;
    ASSERT_EQ(s.chunks.size(), 7u);
    EXPECT_EQ(s.chunks[0].kind, ast::Script::Chunk::Kind::Module);
    EXPECT_EQ(s.chunks[1].text, "static inline int plain(int a) { return a + 1; }");
    EXPECT_EQ(s.chunks[2].line, 3);
    EXPECT_EQ(s.chunks[2].text,
        "static inline int bump(int a,\n                       int b = 2)"
        " { spp_mod()->total += a + b; return spp_mod()->total; }");
    EXPECT_EQ(s.chunks[3].text, "template <class T> T g(T a) { return a + spp_mod()->total; }");
    EXPECT_EQ(s.chunks[4].text, "constexpr int c(bool b) { return b ? 1 : spp_mod()->total; }");
    EXPECT_EQ(s.chunks[5].text, "namespace n { int h() { return spp_mod()->total; } }");
    EXPECT_EQ(s.chunks[6].text, "int d(int a = spp_mod()->total);");
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
    expect_error("class S : public Actor { void F() { S p; (void)p; } };",
        "the script class 'S' by value is not supported in a .cpp script; use a pointer or a reference");
}

// A file with two errors reports the first error in the order of the source. The body edits
// run after the read of the structure, but an earlier edit error must stay first.
TEST(CppFrontend, FirstErrorInSourceOrder)
{
    const std::string lambda_msg = "a lambda that uses a base member must capture with [&] or [=]";
    const std::string static_msg = "'virtual' is not supported in a .cpp script";
    const std::string type_msg = "the script class 'S' by value is not supported in a .cpp script; use a pointer or a reference";
    {
        Parsed p("#include \"Actor.h\"\nclass S : public Actor { void F() { auto f = [this] { Move(Vec3()); }; f(); } };\n"
                 "class T : public Actor { virtual void G() {} };");
        EXPECT_NE(p.errors().find(lambda_msg), std::string::npos) << p.errors();
        EXPECT_EQ(p.errors().find(static_msg), std::string::npos) << p.errors();
    }
    {
        Parsed p("#include \"Actor.h\"\nclass T : public Actor { virtual void G() {} void F() { auto f = [this] { "
                 "Move(Vec3()); }; f(); } };");
        EXPECT_NE(p.errors().find(static_msg), std::string::npos) << p.errors();
        EXPECT_EQ(p.errors().find(lambda_msg), std::string::npos) << p.errors();
    }
    {
        Parsed p("#include \"Actor.h\"\nclass S : public Actor { void F() { auto f = [this] { Move(Vec3()); }; f(); } };\n"
                 "class T : public Actor { void G() { S p; (void)p; } };");
        EXPECT_NE(p.errors().find(lambda_msg), std::string::npos) << p.errors();
        EXPECT_EQ(p.errors().find(type_msg), std::string::npos) << p.errors();
    }
}

const char* const kObjects = R"(#include "Actor.h"
class S : public Actor
{
public:
    S(int a = 1) : Actor("S") { v = a; }
    int get() const { return v; }
    void set(int x) { v = x; }
    S* peer() { return this; }
    void Tick(float dt) override { (void)dt; }
    int v = 0;
};

S* first = nullptr;

class T : public Actor
{
public:
    T() : Actor("T") {}
    void A(Actor* a) { S* p = new S(2); S* q = new S; S* r = new S{3}; const S& c = *p; p->v = c.v; q->set(p->get()); r->peer()->v = (int)sizeof(S); (void)static_cast<S*>(a); (void)(S*)a; }
    void B(Actor* a, S& s) { S* d = dynamic_cast<S*>(a); S& e = dynamic_cast<S&>(*a); s.set(d->v + e.v); d->Tick(1); delete d; }
    void C() { first->v = 1; first->set(first->get()); }
    int D(const Actor* a) { const S& r = dynamic_cast<const S&>(*a); return r.v; }
};
)";

const ast::Method& method_of(const Parsed& p, const std::string& cls, const std::string& name)
{
    for (const auto& c : p.script.classes)
        if (c.name == cls)
            for (const auto& m : c.methods)
                if (m.name == name)
                    return m;
    throw std::runtime_error("no method " + cls + "::" + name);
}

// Script code uses a script class as a type. A pointer, a reference, sizeof and a cast name the
// shell. "new" calls the generated constructor function. A field of another object is in its
// state. A script method of another object is a call of the generated function. An override
// stays a virtual call. "dynamic_cast" uses the class records of the runtime.
TEST(CppFrontend, ScriptClassTypeEdits)
{
    Parsed p(kObjects);
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    EXPECT_EQ(method_of(p, "T", "A").raw_body,
        " S_Shell* p = S__new(2); S_Shell* q = S__new(); S_Shell* r = S__new(3); const S_Shell& c = *p; "
        "state_of(p)->v = state_of(const_cast<S_Shell*>(&(c)))->v; S__set(q, S__get(p)); "
        "state_of(S__peer(r))->v = (int)sizeof(S_Shell); (void)static_cast<S_Shell*>(a); (void)(S_Shell*)a; ");
    EXPECT_EQ(method_of(p, "T", "B").raw_body,
        " S_Shell* d = spp::as_script<S_Shell>(a, \"S\"); "
        "S_Shell& e = (*spp::shell_of<S_Shell>(*a, \"S\")); "
        "S__set(&(s), state_of(d)->v + state_of(&(e))->v); d->Tick(1); delete d; ");
    // A module variable and a field access edit the same place: the field access is outside.
    EXPECT_EQ(method_of(p, "T", "C").raw_body,
        " state_of(spp_mod()->first)->v = 1; S__set(spp_mod()->first, S__get(spp_mod()->first)); ");
    // A const object: the field access keeps the C++ check of the source.
    EXPECT_EQ(method_of(p, "T", "D").raw_body,
        " const S_Shell& r = (*spp::shell_of<S_Shell>(*a, \"S\")); return state_of(const_cast<S_Shell*>(&(r)))->v; ");
}

TEST(CppFrontend, ScriptClassTypeErrors)
{
    const std::string cls = "class S : public Actor { public: void set(int x) { (void)x; } int v = 0; };\n";
    expect_error(cls + "void g() { S arr[2]; (void)arr; }",
        "an array of the script class 'S' is not supported in a .cpp script; use a pointer or a reference");
    expect_error("#include <vector>\n" + cls + "void g() { std::vector<S> l; (void)l; }",
        "the script class 'S' as a template argument is not supported in a .cpp script; use a pointer or a "
        "reference");
    expect_error("#include <typeinfo>\n" + cls + "void g() { (void)typeid(S); }",
        "'typeid' of the script class 'S' is not supported in a .cpp script: a script class has no C++ RTTI; "
        "use dynamic_cast");
    expect_error("#include <typeinfo>\n" + cls + "void g(S* p) { (void)typeid(*p); }",
        "'typeid' of the script class 'S' is not supported in a .cpp script");
    expect_error("#include <new>\n" + cls + "void g(void* b) { (void)new (b) S(); }",
        "a placement 'new' of the script class 'S' is not supported in a .cpp script");
    expect_error(cls + "void g() { (void)new S[2]; }",
        "an array 'new' of the script class 'S' is not supported in a .cpp script; use an array of pointers");
    expect_error(cls + "void g(S* p) { p->S::set(1); }",
        "the script class 'S' can qualify only a static member in a .cpp script");
    expect_error("class S;\nclass S : public Actor {};",
        "a forward declaration of the script class 'S' is not supported in a .cpp script; define the class "
        "before its first use");
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
TEST(AnalyzeCpp, VirtualMethodHasPlace)
{
    const std::string text =
        "#include \"Actor.h\"\nclass S : public Actor\n{\npublic:\n    virtual void F() {}\n};\n";
    const sppc::AnalyzeResult r = sppc::analyze_cpp(text, "/w/t.cpp", analyze_options());
    EXPECT_FALSE(r.ok);
    ASSERT_EQ(r.errors.size(), 1u) << r.diagnostics;
    EXPECT_EQ(r.errors[0].message, "'virtual' is not supported in a .cpp script; use 'override' of a native virtual");
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
