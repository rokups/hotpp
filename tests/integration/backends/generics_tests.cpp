// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Script generics (doc/generics.md) end to end: each backend of the build compiles the
// instantiations, loads the module and runs "main".
#include "hot/backend.h"
#include "hot/module.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace
{

std::vector<hot::BackendKind> backend_kinds()
{
    std::vector<hot::BackendKind> kinds;
    for (hot::BackendKind kind : {hot::BackendKind::ClangFiles, hot::BackendKind::Jit})
        if (hot::backend_available(kind))
            kinds.push_back(kind);
    return kinds;
}

// Loads the script and gives the result of its "int main()".
void expect_main(const std::string& name, const std::string& script, int expected)
{
    for (hot::BackendKind kind : backend_kinds())
    {
        hot::DefaultBackendOptions o;
        o.kind = kind;
        auto backend = hot::make_default_backend(o);
        SCOPED_TRACE(backend->name());
        const hot::ScriptSource src{name, script, {}};
        hot::LoadResult r = backend->load(src, 1);
        ASSERT_TRUE(r) << r.error;
        ASSERT_NE(r.module->entry_main, nullptr);
        EXPECT_EQ(r.module->entry_main(0, nullptr), expected);
    }
}

TEST(Generics, StructAndFreeFunction)
{
    expect_main("gen_struct.hot",
        "struct Box<T> {\n"
        "    T value;\n"
        "    T Get() => value;\n"
        "    Box<T> With(T v) { Box<T> b; b.value = v; return b; }\n"
        "}\n"
        "T Larger<T>(T a, T b) => a > b ? a : b;\n"
        "Box<T> Wrap<T>(T v) { Box<T> b; b.value = v; return b; }\n"
        "int main() {\n"
        "    Box<int> b;\n"
        "    b.value = 3;\n"
        "    Box<std.string> s = Wrap<std.string>(\"four\");\n"
        "    return Larger<int>(b.With(40).Get(), 2) + (int)s.value.size() + (int)Larger<float>(1.5, 0.5);\n"
        "}\n",
        45);
}

// The module has no loader here, so the test checks the compile, the load and a static method.
// The object state of a script class needs a loader (tests/integration/reload).
TEST(Generics, ClassWithHandleArgumentAndStatic)
{
    expect_main("gen_class.hot",
        "class Child { int hp = 3; }\n"
        "class Bag<T> {\n"
        "    std.vector<T> items = std.vector<T>();\n"
        "    void Add(T v) { items.push_back(v); }\n"
        "    int Count() => (int)items.size();\n"
        "    static int Kind() => 7;\n"
        "}\n"
        "class Children : Bag<$Child> {\n"
        "    int Hp() { int s = 0; foreach (var c in items) { s += c.hp; } return s; }\n"
        "}\n"
        "int main() {\n"
        "    var b = new $Bag<int>();\n"
        "    b.Add(1); b.Add(2);\n"
        "    var c = new $Children();\n"
        "    c.Add(new $Child()); c.Add(new $Child());\n"
        "    int n = b.Count() + c.Hp();\n"
        "    return Bag<float>.Kind();\n"
        "}\n",
        7);
}

TEST(Generics, NestedArgumentsAliasesAndRecursion)
{
    expect_main("gen_nested.hot",
        "struct Pair<A, B> { A a; B b; }\n"
        "using IntPair = Pair<int, int>;\n"
        "int Depth<T>(int n) => n == 0 ? 0 : 1 + Depth<T>(n - 1);\n"
        "int main() {\n"
        "    Pair<IntPair, Pair<float, std.string>> p;\n"
        "    p.a.a = 1; p.a.b = 2; p.b.b = \"abc\";\n"
        "    IntPair q = p.a;\n"
        "    return q.a + q.b + (int)p.b.b.size() * 10 + Depth<float>(5) * 100;\n"
        "}\n",
        533);
}

} // namespace
