// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the ".cpp" frontend: the structure, the body edits and the error messages.
// The base class Actor comes from tests/engine/Actor.h.
#include "parser/cpp_frontend.h"
#include "hot/hotc_lib.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace hotc;

namespace
{

std::vector<std::string> clang_args(const std::string& extra_dir)
{
    // "-fno-delayed-template-parsing": the args of hotc (hotc_lib.cpp). The msvc target of clang
    // parses a template body late, and the frontend then misses the body.
    std::vector<std::string> a = {"-x", "c++", "-std=c++17", "-fno-delayed-template-parsing",
        "-I" + std::string(HOT_SOURCE_DIR) + "/tests/engine",
        "-I" + std::string(HOT_SOURCE_DIR) + "/include", "-resource-dir=" + std::string(HOT_RESOURCE_DIR)};
    if (!extra_dir.empty())
        a.push_back("-I" + extra_dir);
    for (const std::string& t : hotc::target_parse_args("", ""))
        a.push_back(t);
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
#include <hot/script.h>
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
    void onReload(const hot::OldState& old) { (void)old; }
    int hp = 100;
    HOT_ALIAS("mana") int energy = 3;
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
    EXPECT_EQ(s.imports[1].path, "<hot/script.h>");
    EXPECT_EQ(s.imports[2].line, 3);
    // The chunks keep the source order. The module block is at the first module variable.
    using K = ast::Script::Chunk::Kind;
    ASSERT_EQ(s.chunks.size(), 7u);
    EXPECT_EQ(s.chunks[0].text, "#include \"Actor.h\"");
    EXPECT_EQ(s.chunks[1].text, "#include <hot/script.h>");
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
    // becomes "__self", a module variable goes through hot_mod().
    EXPECT_EQ(p.method("Tick").raw_body,
        " __self->Move(Vec3(1, 2, 3)); __self->Move(Vec3()); Use(__self); hp -= 1; this->hp += 1; "
        "__self->TakeDamage(dt); Jump(); hot_mod()->total += 1; __self->Position.x = 1; ");
    // A call of an override goes through the shell. A [&] lambda can use a base member.
    EXPECT_EQ(p.method("Again").raw_body,
        " __self->Tick(0.5f); __self->Tick(1); auto f = [&, __self] { __self->Move(Vec3()); }; f(); ");
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
    EXPECT_EQ(c.dtors[0].raw_body, " hot_mod()->total = 0; ");
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

// A method, a constructor or a destructor can have its definition outside the class. The body is
// the body of the member, and the text does not stay in the output.
TEST(CppFrontend, OutOfClassMethod)
{
    Parsed p(R"(#include "Actor.h"
int total = 0;
class Player : public Actor
{
public:
    int hit(int d);
    int get() const;
    void Tick(float dt) override;
    void Use(Actor* a) { (void)a; }
    int hp = 10;
};
class Other : public Actor
{
public:
    void Poke() {}
};
int Player::hit(int dmg) { hp -= dmg; total += 1; Move(Vec3()); return this->hp; }
int Player::get() const { return hp; }
void Player::Tick(float dt) { this->Move(Vec3(dt, 0, 0)); Use(this); }
)");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.script.classes.size(), 2u);
    const ast::ClassDef& c = p.script.classes[0];
    ASSERT_EQ(c.methods.size(), 4u);
    EXPECT_EQ(p.method("hit").raw_body,
        " hp -= dmg; hot_mod()->total += 1; __self->Move(Vec3()); return this->hp; ");
    EXPECT_EQ(p.method("hit").params.at(0).name, "dmg"); // the name of the definition
    EXPECT_EQ(p.method("hit").ret, "int");
    EXPECT_EQ(p.method("hit").line, 6); // the head maps to the declaration
    EXPECT_EQ(p.method("hit").raw_line, 17);
    EXPECT_EQ(p.method("hit").end_line, 17);
    EXPECT_EQ(p.method("get").raw_body, " return hp; ");
    EXPECT_TRUE(p.method("Tick").is_override);
    EXPECT_EQ(p.method("Tick").raw_body, " __self->Move(Vec3(dt, 0, 0)); Use(__self); ");
    EXPECT_EQ(p.script.classes[1].methods.size(), 1u);
    // The definitions do not stay as chunks.
    for (const auto& ch : p.script.chunks)
        EXPECT_EQ(ch.text.find("Player::"), std::string::npos) << ch.text;
}

TEST(CppFrontend, OutOfClassDefaultArgumentAndOrder)
{
    // The default argument is only in the class. The definition comes after another class.
    Parsed p(R"(#include "Actor.h"
int limit = 3;
class A : public Actor
{
public:
    int add(int a, int b = limit);
};
class B : public Actor
{
public:
    void Go() {}
};
int A::add(int x, int y) { return x + y + limit; }
)");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const ast::Method& m = p.method("add");
    ASSERT_EQ(m.params.size(), 2u);
    EXPECT_EQ(m.params[0].name, "x");
    EXPECT_EQ(m.params[1].name, "y");
    ASSERT_TRUE(m.params[1].def);
    EXPECT_EQ(m.params[1].def->cpp, "hot_mod()->limit");
    EXPECT_EQ(m.raw_body, " return x + y + hot_mod()->limit; ");
}

TEST(CppFrontend, OutOfClassCtorDtor)
{
    Parsed p(R"(#include "Actor.h"
int total = 0;
class Player : public Actor
{
public:
    Player(int a);
    ~Player();
    int hp = 1;
};
Player::Player(int a) : Actor("p") { hp = a; total++; }
Player::~Player() { total--; }
)");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const ast::ClassDef& c = p.script.classes.at(0);
    ASSERT_EQ(c.ctors.size(), 1u);
    EXPECT_TRUE(c.ctors[0].has_base_call);
    EXPECT_EQ(c.ctors[0].raw_base_args, "\"p\"");
    EXPECT_EQ(c.ctors[0].raw_body, " hp = a; hot_mod()->total++; ");
    EXPECT_EQ(c.ctors[0].raw_line, 10);
    ASSERT_EQ(c.dtors.size(), 1u);
    EXPECT_EQ(c.dtors[0].raw_body, " hot_mod()->total--; ");
}

TEST(CppFrontend, OutOfClassEarlierDeclarationIsValid)
{
    // A prototype before the class, a definition after the class and a type before the class.
    Parsed p(R"(#include "Actor.h"
int later();
struct Early { int v = 2; };
class S : public Actor
{
public:
    int F();
};
int later() { return 1; }
int S::F() { Early e; return later() + e.v; }
)");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    EXPECT_EQ(p.method("F").raw_body, " Early e; return later() + e.v; ");
}

TEST(CppFrontend, OutOfClassOverloadsAndNames)
{
    // The overloads link by USR, not by name or by order. The names come from the definition.
    // An unnamed parameter keeps the name of the other declaration.
    Parsed p(R"(#include "Actor.h"
class S : public Actor
{
public:
    int F(int a);
    int F(float b);
    int G(int, int second);
    int H(int keep);
    int hp = 0;
};
int S::F(float x) { return 2 + (int)x; }
int S::F(int y) { return 1 + y; }
int S::G(int first, int) { return first; }
int S::H(int) { return 7; }
)");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const ast::ClassDef& c = p.script.classes.at(0);
    ASSERT_EQ(c.methods.size(), 4u);
    EXPECT_EQ(c.methods[0].params[0].name, "y");
    EXPECT_EQ(c.methods[0].raw_body, " return 1 + y; ");
    EXPECT_EQ(c.methods[1].params[0].name, "x");
    EXPECT_EQ(c.methods[1].raw_body, " return 2 + (int)x; ");
    EXPECT_EQ(c.methods[2].params[0].name, "first");
    EXPECT_EQ(c.methods[2].params[1].name, "second");
    EXPECT_EQ(c.methods[3].params[0].name, "keep");
}

TEST(CppFrontend, OutOfClassQualifiers)
{
    Parsed p(R"(#include "Actor.h"
class S : public Actor
{
public:
    S() noexcept;
    int F() noexcept;
    int G() &;
    int H() const &&;
    int hp = 1;
};
S::S() noexcept : Actor("s") { hp = 2; }
int S::F() noexcept { return hp; }
int S::G() & { return hp + 1; }
int S::H() const && { return hp + 2; }
)");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const ast::ClassDef& c = p.script.classes.at(0);
    ASSERT_EQ(c.ctors.size(), 1u);
    EXPECT_TRUE(c.ctors[0].has_base_call);
    EXPECT_EQ(c.ctors[0].raw_body, " hp = 2; ");
    EXPECT_EQ(p.method("F").raw_body, " return hp; ");
    EXPECT_EQ(p.method("G").raw_body, " return hp + 1; ");
    EXPECT_EQ(p.method("H").raw_body, " return hp + 2; ");
}

TEST(CppFrontend, BaseStaticAndNestedNames)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(testing::TempDir()) / "hot_cpp_frontend";
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

// A member initializer list. The statements come in the order of the field declarations.
TEST(CppFrontend, MemberInitializersKeepFieldOrder)
{
    Parsed p("#include \"Actor.h\"\n#include <string>\n#include <utility>\n"
             "class S : public Actor\n{\npublic:\n"
             "    S(int a, std::string n) : Actor(\"x\"), c{a + 1}, name(std::move(n)), b(a * 2), Actor_unused_marker_(0) {}\n"
             "    int b; int c; std::string name; int d = 5; int Actor_unused_marker_;\n};\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const ast::Ctor& c = p.script.classes.at(0).ctors.at(0);
    EXPECT_TRUE(c.has_base_call);
    EXPECT_EQ(c.raw_base_args, "\"x\"");
    ASSERT_EQ(c.raw_pre.size(), 4u); // the field "d" has a default member initializer and no entry
    EXPECT_EQ(c.raw_pre[0].text, "{ decltype(__st->b) __hot_mi(a * 2); __st->b = \x01std::move(__hot_mi); }");
    EXPECT_EQ(c.raw_pre[1].text, "{ decltype(__st->c) __hot_mi{a + 1}; __st->c = \x01std::move(__hot_mi); }");
    EXPECT_EQ(c.raw_pre[2].text, "{ decltype(__st->name) __hot_mi(std::move(n)); __st->name = \x01std::move(__hot_mi); }");
    EXPECT_EQ(c.raw_pre[3].text, "{ decltype(__st->Actor_unused_marker_) __hot_mi(0); __st->Actor_unused_marker_ = \x01std::move(__hot_mi); }");
    EXPECT_EQ(c.raw_pre[0].line, 7);
    EXPECT_EQ(c.raw_body, "");
}

// The text of an entry gets the edits of a body, and an entry can be empty. A lambda works.
TEST(CppFrontend, MemberInitializersEditsAndEmptyForms)
{
    Parsed p("#include \"Actor.h\"\nint total = 0;\n"
             "class S : public Actor\n{\npublic:\n"
             "    S() : a(), b{}, c(total + Name.size()), d([=] { return 3; }()) {}\n"
             "    int a; int b; int c; int d;\n};\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const ast::Ctor& c = p.script.classes.at(0).ctors.at(0);
    EXPECT_FALSE(c.has_base_call);
    ASSERT_EQ(c.raw_pre.size(), 4u);
    EXPECT_EQ(c.raw_pre[0].text, "{ decltype(__st->a) __hot_mi{}; __st->a = \x01std::move(__hot_mi); }");
    EXPECT_EQ(c.raw_pre[1].text, "{ decltype(__st->b) __hot_mi{}; __st->b = \x01std::move(__hot_mi); }");
    EXPECT_EQ(c.raw_pre[2].text,
        "{ decltype(__st->c) __hot_mi(hot_mod()->total + __self->Name.size()); __st->c = \x01std::move(__hot_mi); }");
    EXPECT_EQ(c.raw_pre[3].text, "{ decltype(__st->d) __hot_mi([=] { return 3; }()); __st->d = \x01std::move(__hot_mi); }");
}

// An entry may name a parameter that has the name of the field. A constructor with no list has none.
TEST(CppFrontend, MemberInitializersParameterWithFieldName)
{
    Parsed p("#include \"Actor.h\"\nclass S : public Actor\n{\npublic:\n"
             "    S(int hp) : Actor(\"s\"), hp(hp) {}\n    S() : Actor(\"s\") {}\n    int hp = 1;\n};\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const auto& cs = p.script.classes.at(0).ctors;
    ASSERT_EQ(cs.size(), 2u);
    ASSERT_EQ(cs[0].raw_pre.size(), 1u);
    EXPECT_EQ(cs[0].raw_pre[0].text, "{ decltype(__st->hp) __hot_mi(hp); __st->hp = \x01std::move(__hot_mi); }");
    EXPECT_TRUE(cs[1].raw_pre.empty());
}

// A constructor defined after the class has the same list. The parameter names of the definition win.
TEST(CppFrontend, MemberInitializersOutOfClass)
{
    Parsed p("#include \"Actor.h\"\nclass S : public Actor\n{\npublic:\n"
             "    S(int a);\n    int x; int y = 2;\n};\n"
             "S::S(int v) : Actor(\"s\"), y(v + 1), x(v) { y++; }\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const ast::Ctor& c = p.script.classes.at(0).ctors.at(0);
    EXPECT_EQ(c.raw_base_args, "\"s\"");
    ASSERT_EQ(c.raw_pre.size(), 2u);
    EXPECT_EQ(c.raw_pre[0].text, "{ decltype(__st->x) __hot_mi(v); __st->x = \x01std::move(__hot_mi); }");
    EXPECT_EQ(c.raw_pre[1].text, "{ decltype(__st->y) __hot_mi(v + 1); __st->y = \x01std::move(__hot_mi); }");
    EXPECT_EQ(c.raw_body, " y++; ");
}

// A line comment at the end of an entry does not hide the closing mark.
TEST(CppFrontend, MemberInitializerEndsWithLineComment)
{
    Parsed p("#include \"Actor.h\"\nclass S : public Actor\n{\npublic:\n"
             "    S() : a(1 // one\n) {}\n    int a;\n};\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    EXPECT_EQ(p.script.classes.at(0).ctors.at(0).raw_pre.at(0).text,
        "{ decltype(__st->a) __hot_mi(1 // one\n); __st->a = \x01std::move(__hot_mi); }");
}

TEST(CppFrontend, MemberInitializerErrors)
{
    expect_error("class S : public Actor { const int k = 1; S() : k(2) {} };",
        "the const field 'k' cannot be in the initializer list");
    expect_error("int g;\nclass S : public Actor { int& r = g; S() : r(g) {} };",
        "the reference field 'r' cannot be in the initializer list");
    expect_error("class S : public Actor { S(); S(int a) : S() {} int v = 0; };\nS::S() {}",
        "a delegating constructor is not supported in a .cpp script class");
    expect_error("class S : public Actor { S() : Name(\"x\") {} int v = 0; };",
        "member initializer 'Name' does not name a non-static data member or base class");
    expect_error("class S : public Actor { S() : nothing(1) {} int v = 0; };",
        "member initializer 'nothing' does not name a non-static data member or base class");
    expect_error("class S : public Actor { S() : a{1, 2} {} int a[2]; };",
        "the array field 'a' cannot be in the initializer list");
    expect_error("class S : public Actor { S() : v(1), v(2) {} int v = 0; };",
        "multiple initializations given for non-static member 'v'");
    // A pack needs a constructor template, and a script class has none.
    expect_error("class S : public Actor { template <class... A> S(A... a) : v(a...) {} int v = 0; };",
        "this declaration is not supported in a .cpp script class");
}

// Comments between the entries and before the body do not change the result.
TEST(CppFrontend, MemberInitializersSkipComments)
{
    Parsed p("#include \"Actor.h\"\nclass S : public Actor\n{\npublic:\n"
             "    S() : Actor(\"x\") // base\n        , /* one */ a(1) /* after */\n        , b{2} // two\n    /* before body: x */ {}\n"
             "    S(int v) /* no list: a, b */ {}\n"
             "    int a; int b;\n};\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const auto& cs = p.script.classes.at(0).ctors;
    ASSERT_EQ(cs.size(), 2u);
    EXPECT_EQ(cs[0].raw_base_args, "\"x\"");
    ASSERT_EQ(cs[0].raw_pre.size(), 2u);
    EXPECT_EQ(cs[0].raw_pre[0].text, "{ decltype(__st->a) __hot_mi(1); __st->a = \x01std::move(__hot_mi); }");
    EXPECT_EQ(cs[0].raw_pre[1].text, "{ decltype(__st->b) __hot_mi{2}; __st->b = \x01std::move(__hot_mi); }");
    EXPECT_TRUE(cs[1].raw_pre.empty());
}

// A default member initializer runs before the list. So it cannot read a field that has an entry.
TEST(CppFrontend, MemberInitializerDefaultReadsListedField)
{
    expect_error("class S : public Actor { S() : a(5) {} int a; int b = a + 1; };",
        "the default member initializer of the field 'b' reads the field 'a', which this constructor sets in its "
        "initializer list");
    // No error: the default reads a field that has no entry, or the field has an entry itself.
    Parsed ok("#include \"Actor.h\"\nclass S : public Actor { S() : a(5), b(2) {} int a; int b = a + 1; int c = 3; int d = c + 1; };\n");
    EXPECT_TRUE(ok.diag.items.empty()) << ok.errors();
}

TEST(CppFrontend, StructureErrors)
{
    expect_error("struct B { virtual ~B() {} };\nclass S : public B {};",
        "a .cpp script class cannot derive from 'B': it is a plain class of the script");
    expect_error("class S : public Actor { void F(); };",
        "the method 'S::F' is declared but never defined; define it in the class or after the class");
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

TEST(CppFrontend, OutOfClassErrors)
{
    expect_error("class S : public Actor { void F(); S(); ~S(); };\nS::S() {}\nS::~S() {}",
        "the method 'S::F' is declared but never defined");
    expect_error("class S : public Actor { S(); };\nvoid S::G() {}",
        "out-of-line definition of 'G' does not match any declaration");
    expect_error("class S : public Actor { S(); ~S(); };\nS::S() {}",
        "the destructor 'S::~S' is declared but never defined");
    expect_error("class S : public Actor { S(int a); };",
        "the constructor 'S::S' is declared but never defined");
    expect_error("class S : public Actor { S() = default; };", "a defaulted or deleted constructor is not supported");
    expect_error("class S : public Actor { static void F(); };\nvoid S::F() {}", "define the static method inside the class");
    expect_error("class S : public Actor { void F() = delete; };", "a defaulted or deleted method is not supported");
    expect_error("class S : public Actor { S& operator=(const S&) = default; };",
        "a defaulted or deleted method is not supported");
    expect_error("class S : public Actor { ~S() = default; };", "a defaulted or deleted method is not supported");
    expect_error("class S : public Actor { void Tick(float dt) override = 0; };",
        "a pure virtual method cannot be 'override' in a .cpp script class");
    expect_error("class S : public Actor { virtual void F() = 0; };\nvoid S::F() {}",
        "a pure virtual method cannot have a definition in a .cpp script class");
    // A method that is defined twice is an error of the C++ parser.
    expect_error("class S : public Actor { void F(); };\nvoid S::F() {}\nvoid S::F() {}", "redefinition of 'F'");
    // A definition before the class cannot name the class.
    expect_error("void S::F() {}\nclass S : public Actor { void F(); };", "S");
    // The body moves into the class: a name that the file declares after the class is not valid.
    expect_error("class S : public Actor { int F(); };\nconst int kLate = 3;\nint S::F() { return kLate; }",
        "the definition of 'S::F' uses 'kLate', which the file declares after the class");
    expect_error("class S : public Actor { int F(); };\nstruct Late { int v; };\nint S::F() { Late l{1}; return l.v; }",
        "the definition of 'S::F' uses 'Late', which the file declares after the class");
    expect_error("class S : public Actor { int F(); };\nint late() { return 1; }\nint S::F() { return late(); }",
        "the definition of 'S::F' uses 'late', which the file declares after the class");
    // An error in a definition keeps the rules of a body.
    expect_error("class S : public Actor { void F(); };\nvoid S::F() { S s; }",
        "the script class 'S' by value is not supported in a .cpp script");
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
    EXPECT_EQ(s.chunks[5].text, "int later() { return hot_mod()->S__s_n + hot_mod()->S__s_hits + S__s_k; }");
    ASSERT_EQ(s.vars.size(), 4u);
    EXPECT_EQ(s.vars[0].name, "total");
    EXPECT_EQ(s.vars[1].name, "S__s_n");
    EXPECT_EQ(s.vars[1].type, "int");
    EXPECT_EQ(s.vars[1].raw_init, "hot_mod()->total + 1"); // from the definition outside the class
    EXPECT_EQ(s.vars[2].name, "S__s_hits");
    EXPECT_EQ(s.vars[2].raw_init, "5");
    EXPECT_EQ(s.vars[3].name, "after");
    EXPECT_EQ(s.vars[3].raw_init, "hot_mod()->S__s_n");
    EXPECT_EQ(p.method("F").raw_body,
        " hot_mod()->S__s_n++; hot_mod()->S__s_n += 1; hot_mod()->S__s_n += 1; hot_mod()->S__s_hits = S__s_k + S__s_k; "
        "int* q = &hot_mod()->S__s_n; (void)q; ");
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
        "int S__s_make(int a, int b = hot_mod()->total,\n                    int c = 2);");
    EXPECT_EQ(s.chunks[3].text, "int S__s_helper(int a);");
    EXPECT_EQ(s.chunks[4].kind, K::Class);
    EXPECT_EQ(s.chunks[5].text,
        "int S__s_make(int a, int b,\n                    int c)"
        " { hot_mod()->total += a; return S__s_helper(a) + b + c; }");
    EXPECT_EQ(s.chunks[5].line, 7);
    EXPECT_EQ(s.chunks[6].text, "int S__s_helper(int a) { return a + hot_mod()->total; }");
    EXPECT_EQ(s.chunks[7].text, "int g() { return S__s_make(1) + S__s_helper(2); }");
    EXPECT_EQ(s.chunks[8].text, "int (*const fp)(int) = &S__s_helper;");
    EXPECT_EQ(p.method("F").raw_body,
        " hot_mod()->total = S__s_make(1) + S__s_make(2, 3) + S__s_make(4) + S__s_make(5); ");
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
    expect_error("class S : public Actor { static void F(); };", "define the static method inside the class");
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

// ">=" after a template close is not a default argument in C++: clang rejects "std::vector<int>={}".
// The form with a space is a default argument. hotc keeps it.
TEST(CppFrontend, StaticMethodDefaultArgumentAfterTemplateClose)
{
    expect_error("#include <vector>\nclass C : public Actor { public: static void f(std::vector<int>={}) {} };",
        "a space is required between a right angle bracket and an equals sign");
    Parsed p("#include <vector>\n#include \"Actor.h\"\n"
             "class C : public Actor {\n"
             "public:\n"
             "    static void f(std::vector<int> = {}) {}\n"
             "    static void g(std::vector<int> v = {}) {}\n"
             "};\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    std::string protos, defs;
    for (const auto& c : p.script.chunks)
        (!c.text.empty() && c.text.back() == ';' ? protos : defs) += c.text + "\n";
    EXPECT_NE(protos.find("f(std::vector<int> = {})"), std::string::npos) << protos;
    EXPECT_NE(protos.find("g(std::vector<int> v = {})"), std::string::npos) << protos;
    EXPECT_NE(defs.find("f(std::vector<int>)"), std::string::npos) << defs;
    EXPECT_NE(defs.find("g(std::vector<int> v)"), std::string::npos) << defs;
}

// An instance method "s_x" and a static member "x" have the same generated name.
TEST(CppFrontend, InstanceMethodWithStaticMemberNameIsAnError)
{
    expect_error("class C : public Actor { public: static const int x = 1; int s_x() { return 2; } };",
        "have the same generated name 'C__s_x'");
    expect_error("class C : public Actor { public: static int x() { return 1; } int s_x() { return 2; } };",
        "have the same generated name 'C__s_x'");
    expect_error("class C : public Actor { public: static int x; int s_x() { return 2; } };",
        "have the same generated name 'C__s_x'");
}

// A static member has the name "<Class>__s_<name>". It never equals a generated name of the class,
// such as "C__plan". The generated code has the function "C__plan" next to the static method.
TEST(CppFrontend, StaticMemberNamesDoNotCollide)
{
    hotc::TranslateOptions o;
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
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    o.resource_dir = HOT_RESOURCE_DIR;
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    for (const char* n : {"int C__s_plan() {", "int C__s_super_args() {", "int C__s_new_default() {",
             "const int C__s_construct_keys = 4;"})
        EXPECT_NE(r.cpp.find(n), std::string::npos) << n << "\n" << r.cpp;
    EXPECT_NE(r.cpp.find("C__s_plan() + C__s_super_args() + C__s_new_default() + C__s_construct_keys"),
        std::string::npos)
        << r.cpp;
    EXPECT_NE(r.cpp.find("HOT_HIDDEN static auto C__plan()"), std::string::npos) << r.cpp; // the generated one
}

// A pointer or a reference to a type needs only a declaration before the first module variable.
TEST(CppFrontend, ModuleVariablePointerToLaterType)
{
    Parsed p("struct Fwd;\nint total = 0;\nFwd* pf = nullptr;\nFwd& rf = *pf;\nstruct Fwd { int x; };\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.script.vars.size(), 3u);
}

// A variable in a namespace is a module variable with the namespace in its name. A static local
// variable is not a module variable. A use of the variable gets the edit.
TEST(CppFrontend, NamespaceAndStaticLocalVariables)
{
    Parsed p("namespace n { int c = 0; }\n"
             "int total = 0;\n"
             "int f() { static int calls = 0; return ++calls + n::c + total; }\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const ast::Script& s = p.script;
    ASSERT_EQ(s.vars.size(), 2u);
    EXPECT_EQ(s.vars[0].name, "n__c");
    EXPECT_EQ(s.vars[0].ns, "n");
    EXPECT_EQ(s.vars[1].name, "total");
    EXPECT_EQ(s.vars[1].ns, "");
    EXPECT_EQ(s.chunks.back().text, "int f() { static int calls = 0; return ++calls + hot_mod()->n__c + hot_mod()->total; }");
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
        " { hot_mod()->total += a + b; return hot_mod()->total; }");
    EXPECT_EQ(s.chunks[3].text, "template <class T> T g(T a) { return a + hot_mod()->total; }");
    EXPECT_EQ(s.chunks[4].text, "constexpr int c(bool b) { return b ? 1 : hot_mod()->total; }");
    EXPECT_EQ(s.chunks[5].text, "namespace n { int h() { return hot_mod()->total; } }");
    EXPECT_EQ(s.chunks[6].text, "int d(int a = hot_mod()->total);");
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
    EXPECT_EQ(f->params[2].def->cpp, "hot_mod()->level"); // a module variable, as in a body
    EXPECT_EQ(f->params[3].def->cpp, "{}");
}

// The prototype has the default. The definition and the thunk do not repeat it.
TEST(CppFrontend, DefaultArgumentsEmit)
{
    hotc::TranslateOptions o;
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
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    o.resource_dir = HOT_RESOURCE_DIR;
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("S__F(S_Shell* __self, int x, int y = 2, int z = hot_mod()->level);"), std::string::npos)
        << r.cpp;
    EXPECT_NE(r.cpp.find("S__new(int a = 4)"), std::string::npos) << r.cpp;
    // The body calls the member directly, so the member declaration has the defaults too.
    EXPECT_NE(r.cpp.find("    int F(int x, int y = 2, int z = hot_mod()->level);"), std::string::npos) << r.cpp;
    // The definitions have no default.
    EXPECT_NE(r.cpp.find("SState_g1::F(int x, int y, int z)"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("S__F(S_Shell* __self, int x, int y, int z)\n"), std::string::npos) << r.cpp;
}

// A default argument on an override is an error: the call through the base sees the base declaration.
TEST(CppFrontend, DefaultArgumentOnOverrideFails)
{
    hotc::TranslateOptions o;
    o.script_name = "t.cpp";
    o.script_text = R"(#include "Actor.h"
class S : public Actor
{
public:
    void TakeDamage(float a = 1.0f) override {}
};
)";
    o.generation = 1;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    o.resource_dir = HOT_RESOURCE_DIR;
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("default values are not allowed on virtual or override methods"), std::string::npos)
        << r.diagnostics;
}

TEST(CppFrontend, EditErrors)
{
    expect_error("#define MV Move(Vec3())\nclass S : public Actor { void F() { MV; } };",
        "a macro hides a member of the base class; write the member in the script text");
    expect_error("class S : public Actor { void F() { auto f = [*this] { return 1; }; f(); } };",
        "a lambda cannot capture '*this' in a .cpp script: it would copy the object; capture a copy of the fields");
    expect_error("class S : public Actor { void F() { S p; (void)p; } };",
        "the script class 'S' by value is not supported in a .cpp script; use a pointer or a reference");
}

// A lambda must not keep the pointer to the state, because a reload moves the state. A capture of
// "this" becomes a capture of "__self". A field or a script method reads through the shell.
TEST(CppFrontend, LambdaCapturesShell)
{
    Parsed p(R"(#include "Actor.h"
int total = 0;
#include <functional>
class S : public Actor
{
public:
    S() { auto f = [=] { return n; }; n = f(); }
    void A() { auto f = [this] { bump(1); Move(Vec3()); return n; }; f(); }
    void B() { auto f = [=] { return n + k; }; f(); }
    void C() { auto f = [&] { n += 1; bump(2); Move(Vec3()); }; f(); }
    void D() { auto f = [this](auto v) { return n + v; }; f(1); }
    void E() { auto f = [this] { auto g = [this] { return this->n; }; return g(); }; f(); }
    void F() { auto f = [=, this] { Use(this); total += 1; return this->n; }; f(); }
    void G() { auto f = [&, this] { return n; }; f(); }
    void H(int x) { auto f = [this, x, y = n] { return x + y; }; f(); }
    void I() { auto f = [] { return 1; }; f(); n = 2; }
    void J() { auto f = [&] { return n; }; f(); }
    void K(int x) { auto f = [&, x] { return x + n; }; f(); }
    void L() { auto f = [&] { return 1; }; f(); }
    void M() { auto f = [x = this] { return x->n; }; f(); }
    void N() { auto f = [&] { auto g = [this] { return n; }; return g(); }; f(); }
    void bump(int d) { n += d; }
    void Use(Actor*) {}
    int n = 0;
    int k = 0;
    std::function<int()> stored = [this] { return n; };
    std::function<int()> by_ref = [&] { return k; };
};
)");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    EXPECT_EQ(p.script.classes.at(0).ctors.at(0).raw_body,
        " auto f = [=] { return state_of(__self)->n; }; n = f(); ");
    EXPECT_EQ(p.method("A").raw_body,
        " auto f = [__self] { state_of(__self)->bump(1); __self->Move(Vec3()); return state_of(__self)->n; }; f(); ");
    EXPECT_EQ(p.method("B").raw_body, " auto f = [=] { return state_of(__self)->n + state_of(__self)->k; }; f(); ");
    EXPECT_EQ(p.method("C").raw_body,
        " auto f = [&, __self] { state_of(__self)->n += 1; state_of(__self)->bump(2); __self->Move(Vec3()); }; f(); ");
    EXPECT_EQ(p.method("D").raw_body, " auto f = [__self](auto v) { return state_of(__self)->n + v; }; f(1); ");
    EXPECT_EQ(p.method("E").raw_body,
        " auto f = [__self] { auto g = [__self] { return state_of(__self)->n; }; return g(); }; f(); ");
    // The default "=" makes the item "this" useless, so it goes away. "[&, this]" keeps the shell by copy.
    EXPECT_EQ(p.method("F").raw_body,
        " auto f = [=] { state_of(__self)->Use(__self); hot_mod()->total += 1; return state_of(__self)->n; }; f(); ");
    EXPECT_EQ(p.method("G").raw_body, " auto f = [&, __self] { return state_of(__self)->n; }; f(); ");
    EXPECT_EQ(p.method("H").raw_body, " auto f = [__self, x, y = state_of(__self)->n] { return x + y; }; f(); ");
    // A lambda with no capture and a field outside a lambda stay as they are.
    EXPECT_EQ(p.method("I").raw_body, " auto f = [] { return 1; }; f(); n = 2; ");
    // A lambda with "[&]" keeps the shell by copy, so it can live longer than the method.
    EXPECT_EQ(p.method("J").raw_body, " auto f = [&, __self] { return state_of(__self)->n; }; f(); ");
    EXPECT_EQ(p.method("K").raw_body, " auto f = [&, x, __self] { return x + state_of(__self)->n; }; f(); ");
    EXPECT_EQ(p.method("L").raw_body, " auto f = [&] { return 1; }; f(); ");
    // An init-capture of "this" is the shell.
    EXPECT_EQ(p.method("M").raw_body, " auto f = [x = __self] { return state_of(x)->n; }; f(); ");
    EXPECT_EQ(p.method("N").raw_body,
        " auto f = [&, __self] { auto g = [__self] { return state_of(__self)->n; }; return g(); }; f(); ");
    // A lambda in a field initializer has the same rules.
    const ast::ClassDef& c = p.script.classes.at(0);
    for (const auto& f : c.fields)
    {
        if (f.name == "stored")
            EXPECT_EQ(f.raw_init, "[__self] { return state_of(__self)->n; }");
        if (f.name == "by_ref")
            EXPECT_EQ(f.raw_init, "[&, __self] { return state_of(__self)->k; }");
    }
}

// A file with two errors reports the first error in the order of the source. The body edits
// run after the read of the structure, but an earlier edit error must stay first.
TEST(CppFrontend, FirstErrorInSourceOrder)
{
    const std::string lambda_msg =
        "a lambda cannot capture '*this' in a .cpp script: it would copy the object; capture a copy of the fields";
    const std::string static_msg = "a defaulted or deleted method is not supported in a .cpp script class";
    const std::string type_msg = "the script class 'S' by value is not supported in a .cpp script; use a pointer or a reference";
    {
        Parsed p("#include \"Actor.h\"\nclass S : public Actor { void F() { auto f = [*this] { return 1; }; f(); } };\n"
                 "class T : public Actor { void G() = delete; };");
        EXPECT_NE(p.errors().find(lambda_msg), std::string::npos) << p.errors();
        EXPECT_EQ(p.errors().find(static_msg), std::string::npos) << p.errors();
    }
    {
        Parsed p("#include \"Actor.h\"\n#define MV Move(Vec3())\n"
                 "class T : public Actor { void G() = delete; void F() { MV; } };");
        EXPECT_NE(p.errors().find(static_msg), std::string::npos) << p.errors();
        EXPECT_EQ(p.errors().find("a macro hides"), std::string::npos) << p.errors();
    }
    {
        Parsed p("#include \"Actor.h\"\nclass S : public Actor { void F() { auto f = [*this] { return 1; }; f(); } };\n"
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
        " S_Shell* d = hot::as_script<S_Shell>(a, \"S\"); "
        "S_Shell& e = (*hot::shell_of<S_Shell>(*a, \"S\")); "
        "S__set(&(s), state_of(d)->v + state_of(&(e))->v); d->Tick(1); delete d; ");
    // A module variable and a field access edit the same place: the field access is outside.
    EXPECT_EQ(method_of(p, "T", "C").raw_body,
        " state_of(hot_mod()->first)->v = 1; S__set(hot_mod()->first, S__get(hot_mod()->first)); ");
    // A const object: the field access keeps the C++ check of the source.
    EXPECT_EQ(method_of(p, "T", "D").raw_body,
        " const S_Shell& r = (*hot::shell_of<S_Shell>(*a, \"S\")); return state_of(const_cast<S_Shell*>(&(r)))->v; ");
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
        "the script class 'S' can qualify only a static member");
    expect_error("class S;\nclass S : public Actor {};",
        "a forward declaration of the script class 'S' is not supported in a .cpp script; define the class "
        "before its first use");
}

const char* const kChild = R"(#include "Actor.h"
int f(int a, int b) { return a + b; }
class Base : public Actor
{
public:
    Base(int a, int b = 0) : Actor("Base"), hits(a + b) {}
    virtual int Power() { return 1; }
    int Twice() { return Power() * 2; }
    void Bump() { hits += 1; }
    int hits = 0;
};
class Kid : public Base
{
public:
    Kid(int s = 3) : Base(f(s, 1), s) { kid = s; }
    int Power() override { return 5; }
    void Bump() { Base::Bump(); }
    void Tick(float dt) override { Base::Tick(dt); Bump(); hits = Base::Power() + Power() + kid; Kid* k = dynamic_cast<Kid*>(this); (void)k; }
    int kid = 0;
};
)";

// A script class derives from a script class. The model has the script parent as the base. A
// "virtual" method of a script class has a slot. A call of a virtual goes through the slot, and a
// qualified call of the parent goes to the version of the parent.
TEST(CppFrontend, ScriptChildClass)
{
    Parsed p(kChild);
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.script.classes.size(), 2u);
    EXPECT_EQ(p.script.classes[0].base, "Actor");
    EXPECT_EQ(p.script.classes[1].base, "Base");
    EXPECT_TRUE(method_of(p, "Base", "Power").is_virtual);
    EXPECT_FALSE(method_of(p, "Base", "Power").is_override);
    EXPECT_TRUE(method_of(p, "Kid", "Power").is_override);
    EXPECT_TRUE(method_of(p, "Kid", "Bump").is_new); // C++ hides Base::Bump
    const ast::Ctor& c = p.script.classes[1].ctors.at(0);
    EXPECT_EQ(c.raw_base_args, "f(s, 1), s");
    EXPECT_EQ(c.raw_base_argc, 2u);
    EXPECT_EQ(method_of(p, "Base", "Twice").raw_body, " return Base__Power_virt(__self) * 2; ");
    EXPECT_EQ(method_of(p, "Kid", "Bump").raw_body, " Base__Bump(__self); ");
    const std::string tick = method_of(p, "Kid", "Tick").raw_body;
    EXPECT_NE(tick.find("Base__base_Tick(__self, dt);"), std::string::npos) << tick;
    EXPECT_NE(tick.find("hits = Base__Power(__self) + Base__Power_virt(__self) + kid;"), std::string::npos) << tick;
    EXPECT_NE(tick.find("hot::as_script<Kid_Shell>("), std::string::npos) << tick;
}

TEST(CppFrontend, ScriptChildClassErrors)
{
    const std::string base = "class B : public Actor { public: virtual int P() { return 1; } int v = 0; };\n";
    expect_error(base + "struct I { virtual ~I() {} };\nclass K : public B, public I {};",
        "a .cpp script class with the base the script class 'B' cannot have another base class");
    expect_error(base + "class K : public virtual B {};",
        "a .cpp script class cannot derive from the script class 'B' with 'virtual'");
    expect_error(base + "class K : protected B {};",
        "a .cpp script class must derive from the script class 'B' with 'public'");
    expect_error(base + "class K : public B { virtual int P() { return 2; } };",
        "the method 'P' overrides a virtual method; write 'override'");
    expect_error(base + "class K : public B { int Q(B* o) { return o->B::P(); } };",
        "the script class 'B' can qualify only a static member, or a member that a child class uses on itself");
    // Only a class at file scope is a script class.
    expect_error(base + "void g() { struct L : B {}; }",
        "the class 'L' cannot derive from the script class 'B': only a class at file scope can derive from a "
        "script class in a .cpp script");
    expect_error(base + "class K : public Actor { void F() { struct L : public B {}; } };",
        "the class 'L' cannot derive from the script class 'B'");
    expect_error(base + "struct O { struct L : B {}; };", "the class 'L' cannot derive from the script class 'B'");
}

// A script class as a template argument after the member name is not a qualifier.
TEST(CppFrontend, TemplateArgumentIsNotQualifier)
{
    Parsed p("#include \"Actor.h\"\nstruct Box { template <class T> T Get() { return nullptr; } };\n"
             "class S : public Actor { public: void F(Box* b) { S* s = b->Get<S*>(); (void)s; } };\n");
    EXPECT_TRUE(p.diag.items.empty()) << p.errors();
}

// The argument count of the parent initializer comes from the AST: the comma of a template
// argument list does not separate two arguments.
TEST(CppFrontend, ParentInitArgumentCount)
{
    Parsed p("#include \"Actor.h\"\ntemplate <int A, int B> struct T { static const int v = A + B; };\n"
             "class B : public Actor { public: B(int a, int b = 0) : Actor(\"B\") { (void)a; (void)b; } };\n"
             "class K : public B { public: K() : B(T<1, 2>::v) {} };\n"
             "class M : public B { public: M() : B(T<1, 2>::v, (3, 4)) {} };\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    EXPECT_EQ(p.script.classes.at(1).ctors.at(0).raw_base_argc, 1u);
    EXPECT_EQ(p.script.classes.at(2).ctors.at(0).raw_base_argc, 2u);
}

// A brace initializer goes to the generated code once: "T{1, 2}", not "T{{1, 2}}". An aggregate
// and a container need this.
TEST(CppFrontend, BraceInitializerEmit)
{
    hotc::TranslateOptions o;
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
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    o.resource_dir = HOT_RESOURCE_DIR;
    const hotc::TranslateResult r = hotc::translate(o);
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

    hotc::TranslateOptions o;
    o.script_name = "t.cpp";
    o.script_text = text;
    o.generation = 1;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    o.resource_dir = HOT_RESOURCE_DIR;
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // The first line after the "#line" is the rest of the line of the "{". The statement is two lines later.
    EXPECT_NE(r.cpp.find("#line 16 \"t.cpp\"\n\n        __self->Move(Vec3());"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("#line 11 \"t.cpp\"\n\n        hp = 1;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("#line 4 \"t.cpp\"\n\n    return old * 2;"), std::string::npos) << r.cpp;
}

// A "(...)" initializer gives "T(3, 7)". A comma expression "T((3, 7))" builds a vector of 7 zeros.
TEST(CppFrontend, ParenInitializerEmit)
{
    hotc::TranslateOptions o;
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
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    o.resource_dir = HOT_RESOURCE_DIR;
    const hotc::TranslateResult r = hotc::translate(o);
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
    const fs::path dir = fs::path(testing::TempDir()) / "hot_cpp_frontend_tpl";
    fs::create_directories(dir);
    std::ofstream(dir / "TplBase.h") << "#pragma once\ntemplate <typename T> struct Base {\n"
                                        "  virtual ~Base() = default;\n  T value{};\n  void Hit() {}\n};\n";
    Parsed p("#include \"TplBase.h\"\nclass S : public Base<int>\n{\npublic:\n"
             "    void F() { Hit(); value = 2; }\n};\n",
        dir.string());
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    EXPECT_EQ(p.method("F").raw_body, " __self->Hit(); __self->value = 2; ");
}

// A class in a namespace has a file scope name and a key with the namespace. Two classes with
// the same name in two namespaces do not collide. The compact form of a nested namespace works.
TEST(CppFrontend, NamespaceClassNames)
{
    Parsed p("#include \"Actor.h\"\n"
             "namespace red { class Unit : public Actor { public: int hits = 0; }; }\n"
             "namespace blue { class Unit : public Actor { public: int hits = 0; }; }\n"
             "namespace game::ai { class Brain : public Actor { public: int x = 0; }; }\n"
             "class Plain : public Actor { public: int y = 0; };\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const auto& c = p.script.classes;
    ASSERT_EQ(c.size(), 4u);
    EXPECT_EQ(c[0].name, "red__Unit");
    EXPECT_EQ(c[0].key, "red::Unit");
    EXPECT_EQ(c[0].ns, "red");
    EXPECT_EQ(c[1].name, "blue__Unit");
    EXPECT_EQ(c[1].key, "blue::Unit");
    EXPECT_EQ(c[2].name, "game__ai__Brain");
    EXPECT_EQ(c[2].key, "game::ai::Brain");
    EXPECT_EQ(c[2].ns, "game::ai");
    EXPECT_EQ(c[3].name, "Plain");
    EXPECT_EQ(c[3].key_name(), "Plain");
    EXPECT_EQ(c[3].ns, "");
}

// A class of an inner namespace derives from a class of an outer namespace. A type in a field of
// a module variable and in a parameter uses the file scope name.
TEST(CppFrontend, NamespaceBaseAndTypes)
{
    Parsed p("#include \"Actor.h\"\n"
             "namespace game { class Player : public Actor { public: int points = 0; }; }\n"
             "namespace game::ai { class Brain : public game::Player { public: int mood = 0;\n"
             "    void Look(Player* p, game::Player& q) { mood = p->points + q.points; } }; }\n"
             "game::ai::Brain* brain = nullptr;\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.script.classes.size(), 2u);
    const ast::ClassDef& brain = p.script.classes[1];
    EXPECT_EQ(brain.name, "game__ai__Brain");
    EXPECT_EQ(brain.base, "game__Player");
    ASSERT_EQ(p.script.vars.size(), 1u);
    EXPECT_EQ(p.script.vars[0].type, "game__ai__Brain *");
    for (const auto& m : brain.methods)
        if (m.name == "Look")
        {
            ASSERT_EQ(m.params.size(), 2u);
            EXPECT_EQ(m.params[0].type, "game__Player *");
            EXPECT_EQ(m.params[1].type, "game__Player &");
        }
}

// A module variable in a namespace is a member of the module with the namespace in its name.
// A function of the namespace uses it through "hot_mod()".
TEST(CppFrontend, NamespaceModuleVariable)
{
    Parsed p("namespace game { int score = 0; int bonus(int x) { return x + score; } }\n"
             "namespace game::ai { int mood = 3; }\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const ast::Script& s = p.script;
    ASSERT_EQ(s.vars.size(), 2u);
    EXPECT_EQ(s.vars[0].name, "game__score");
    EXPECT_EQ(s.vars[0].ns, "game");
    EXPECT_EQ(s.vars[1].name, "game__ai__mood");
    EXPECT_EQ(s.vars[1].ns, "game::ai");
    bool found = false;
    for (const auto& ch : s.chunks)
        if (ch.text.find("int bonus(int x)") != std::string::npos)
        {
            found = true;
            EXPECT_EQ(ch.ns, "game");
            EXPECT_NE(ch.text.find("return x + hot_mod()->game__score;"), std::string::npos) << ch.text;
        }
    EXPECT_TRUE(found);
}

// A namespace without script code stays as one text chunk.
TEST(CppFrontend, NamespaceWithoutScriptCodeStaysText)
{
    Parsed p("namespace util { inline int twice(int x) { return x * 2; } }\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.script.chunks.size(), 1u);
    EXPECT_EQ(p.script.chunks[0].text, "namespace util { inline int twice(int x) { return x * 2; } }");
    EXPECT_EQ(p.script.chunks[0].ns, "");
}

// The convert hook of a variable in a namespace is at file scope. Its name has the namespace.
TEST(CppFrontend, NamespaceConvertHook)
{
    Parsed p("namespace game { int score = 0; }\n"
             "long long convert_game__score(int old) { return old * 2LL; }\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.script.vars.size(), 1u);
    EXPECT_FALSE(p.script.var_hooks.empty());
}

TEST(CppFrontend, NamespaceErrors)
{
    expect_error("namespace { int hidden = 0; }", "an anonymous namespace cannot hold a script class");
    expect_error("namespace { class S : public Actor {}; }", "an anonymous namespace cannot hold a script class");
    expect_error("inline namespace v1 { int level = 0; }", "an inline namespace cannot hold a script class");
    expect_error("namespace game { int score = 0; long long convert_score(int old) { return old; } }",
        "must be at file scope and must be named 'convert_game__score'");
    expect_error("namespace game { int y = 0; int convert_game__y(int old) { return old; } }",
        "must be at file scope and must be named 'convert_game__y'");
    expect_error("namespace game { class Player : public Actor {}; }\nclass game__Player : public Actor {};",
        "have the same generated name 'game__Player'");
    expect_error("namespace game { int score = 0; }\nint game__score = 1;",
        "have the same generated name 'game__score'");
}

// A plain class in an anonymous namespace with out-of-class members is not script code. It stays
// as written, like before.
TEST(CppFrontend, AnonymousNamespaceWithPlainClassStaysText)
{
    Parsed p("namespace { struct Helper { int f(); }; int Helper::f() { return 1; } }\n"
             "namespace { struct H { static int n; }; int H::n = 0; }\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.script.chunks.size(), 2u);
    EXPECT_TRUE(p.script.vars.empty());
}

// An extern "C" block passes through as written. A variable in it is not a module variable.
TEST(CppFrontend, ExternCBlockStaysText)
{
    Parsed p("extern \"C\" { int counter = 0; }\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.script.chunks.size(), 1u);
    EXPECT_EQ(p.script.chunks[0].text, "extern \"C\" { int counter = 0; }");
    EXPECT_TRUE(p.script.vars.empty());
}

TEST(CppFrontend, ClangError)
{
    Parsed p("int x = ;\n");
    ASSERT_FALSE(p.diag.items.empty());
    EXPECT_EQ(p.diag.items[0].line, 1);
}

} // namespace

// ---- hotc::analyze_cpp: the errors of a ".cpp" script for an editor

namespace
{

hotc::TranslateOptions analyze_options()
{
    hotc::TranslateOptions o;
    o.generation = 1;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    o.resource_dir = HOT_RESOURCE_DIR;
    return o;
}

} // namespace

TEST(AnalyzeCpp, ValidScriptHasNoError)
{
    const hotc::AnalyzeResult r = hotc::analyze_cpp(
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
        "#include \"Actor.h\"\nclass S : public Actor\n{\npublic:\n    void F() = delete;\n};\n";
    const hotc::AnalyzeResult r = hotc::analyze_cpp(text, "/w/t.cpp", analyze_options());
    EXPECT_FALSE(r.ok);
    ASSERT_EQ(r.errors.size(), 1u) << r.diagnostics;
    EXPECT_EQ(r.errors[0].message, "a defaulted or deleted method is not supported in a .cpp script class");
    EXPECT_EQ(r.errors[0].pos.line, 5);
    EXPECT_GT(r.errors[0].pos.col, 0);
    // The offset is the place of the error in the text.
    EXPECT_EQ(r.errors[0].offset,
        text.find('\n', text.find("public:")) + 1 + static_cast<size_t>(r.errors[0].pos.col - 1));
}

// A pure virtual method ("= 0") is an abstract method. The class that has it is abstract.
TEST(CppFrontend, PureVirtualMethodIsAbstract)
{
    Parsed p("#include \"Actor.h\"\nclass Shape : public Actor\n{\npublic:\n"
             "    virtual int Area(int k) = 0;\n    int Twice() { return 2; }\n};\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    const ast::Method& m = method_of(p, "Shape", "Area");
    EXPECT_TRUE(m.is_abstract);
    EXPECT_TRUE(m.is_virtual);
    EXPECT_FALSE(m.is_override);
    EXPECT_TRUE(m.raw_body.empty());
    EXPECT_TRUE(p.script.classes.at(0).is_abstract);
    EXPECT_FALSE(method_of(p, "Shape", "Twice").is_abstract);
}

// A class with no pure method is not abstract.
TEST(CppFrontend, ClassWithoutPureMethodIsConcrete)
{
    Parsed p("#include \"Actor.h\"\nclass S : public Actor\n{\npublic:\n    virtual int F() { return 1; }\n};\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    EXPECT_FALSE(p.script.classes.at(0).is_abstract);
}

// A child that overrides the pure method is concrete. A child that does not stays abstract.
TEST(CppFrontend, ChildOfAbstractClass)
{
    const std::string base =
        "#include \"Actor.h\"\nclass Shape : public Actor\n{\npublic:\n    virtual int Area() = 0;\n};\n";
    {
        Parsed p(base + "class Sq : public Shape\n{\npublic:\n    int Area() override { return 9; }\n};\n"
                        "class Sq2 : public Sq\n{\npublic:\n    int Area() override { return 4; }\n};\n");
        ASSERT_TRUE(p.diag.items.empty()) << p.errors();
        ASSERT_EQ(p.script.classes.size(), 3u);
        EXPECT_TRUE(p.script.classes[0].is_abstract);
        EXPECT_FALSE(p.script.classes[1].is_abstract);
        EXPECT_FALSE(p.script.classes[2].is_abstract);
        EXPECT_TRUE(method_of(p, "Sq", "Area").is_override);
        EXPECT_FALSE(method_of(p, "Sq", "Area").is_abstract);
    }
    {
        Parsed p(base + "class Mid : public Shape\n{\npublic:\n    int Other() { return 1; }\n};\n"
                        "class Leaf : public Mid\n{\npublic:\n    int Area() override { return 3; }\n};\n");
        ASSERT_TRUE(p.diag.items.empty()) << p.errors();
        ASSERT_EQ(p.script.classes.size(), 3u);
        EXPECT_TRUE(p.script.classes[1].is_abstract); // Mid does not override Area
        EXPECT_FALSE(p.script.classes[2].is_abstract); // Leaf does
    }
}

// A pure virtual method of a native base that no class of the chain overrides makes the class
// abstract. A child that overrides it is concrete.
TEST(CppFrontend, OpenNativePureVirtualMakesTheClassAbstract)
{
    Parsed p("#include \"Actor.h\"\n#include \"Guarded.h\"\n"
             "class Warden : public Actor, public IStrict\n{\npublic:\n    int n = 0;\n};\n"
             "class Jailer : public Warden\n{\npublic:\n    int Seal(int x) noexcept override { return x; }\n};\n"
             "class Idle : public Warden\n{\npublic:\n    int m = 0;\n};\n");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.script.classes.size(), 3u);
    EXPECT_TRUE(p.script.classes[0].is_abstract); // Warden leaves Seal open
    EXPECT_FALSE(p.script.classes[1].is_abstract); // Jailer implements it
    EXPECT_TRUE(p.script.classes[2].is_abstract); // Idle inherits the open slot
}

TEST(CppFrontend, PureVirtualMethodErrors)
{
    // The definition outside the class is an error.
    expect_error("class S : public Actor { virtual int F() = 0; };\nint S::F() { return 1; }",
        "a pure virtual method cannot have a definition in a .cpp script class");
    // A pure method is never "declared but never defined".
    Parsed p("#include \"Actor.h\"\nclass S : public Actor { public: virtual void F() = 0; };\n");
    EXPECT_TRUE(p.diag.items.empty()) << p.errors();
}

TEST(CppFrontend, NewOfAbstractClassIsAnError)
{
    const std::string base = "class Shape : public Actor { public: virtual int Area() = 0; };\n";
    expect_error(base + "static void make() { Shape* s = new Shape(); (void)s; }",
        "cannot create an instance of the abstract class 'Shape'");
    expect_error(base + "class Mid : public Shape { public: int Other() { return 1; } };\n"
                        "static void make() { Mid* s = new Mid(); (void)s; }",
        "cannot create an instance of the abstract class 'Mid'");
    // A child that overrides the method is concrete.
    Parsed p("#include \"Actor.h\"\n" + base +
             "class Sq : public Shape { public: int Area() override { return 1; } };\n"
             "static void make() { Sq* s = new Sq(); (void)s; }");
    EXPECT_TRUE(p.diag.items.empty()) << p.errors();
}

// An error of a later check (not of the parser) is an error too.
TEST(AnalyzeCpp, TranslationErrorIsReported)
{
    const hotc::AnalyzeResult r = hotc::analyze_cpp(
        "#include \"Actor.h\"\nclass S : public Actor\n{\npublic:\n    void TakeDamage(float a = 1.0f) override {}\n};\n",
        "/w/t.cpp", analyze_options());
    EXPECT_FALSE(r.ok);
    ASSERT_FALSE(r.errors.empty()) << r.diagnostics;
    EXPECT_EQ(r.errors[0].pos.line, 5);
}

TEST(AnalyzeCpp, PathMustBeCpp)
{
    const hotc::AnalyzeResult r = hotc::analyze_cpp("class S {};", "/w/t.hot", analyze_options());
    EXPECT_FALSE(r.ok);
    ASSERT_EQ(r.errors.size(), 1u);
    EXPECT_EQ(r.errors[0].pos.line, 1);
}

// A ".cpp" script cannot write a cpp_prelude, but its shells get the texts of the prelude files.
TEST(CppFrontend, PreludeFileTextReachesShell)
{
    hotc::TranslateOptions o;
    o.script_name = "t.cpp";
    o.script_text = "#include \"Actor.h\"\nclass C : public Actor\n{\npublic:\n    C() : Actor(\"C\") {}\n"
                    "    void Tick(float dt) override { last = 1; }\n    int last = 0;\n};\n";
    o.generation = 1;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    o.resource_dir = HOT_RESOURCE_DIR;
    o.preludes.push_back({"p.prelude", "[cpp_prelude(\"PRE_TEXT($N, $T, $B);\")] class *;\n"});
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("PRE_TEXT(C, C_Shell, Actor);"), std::string::npos) << r.cpp;
}
