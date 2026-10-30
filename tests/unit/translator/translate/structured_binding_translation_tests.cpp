// Tests for the translation of the structured bindings and the foreach helpers.
#include <gtest/gtest.h>

#include <string>

#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

namespace {

using namespace translator_test;

// A header with the standard types of the scripts.
const char* kHeader = "#include <map>\n#include <utility>\n#include <vector>\n";

sppc::TranslateResult translate_with_header(const std::string& text)
{
    TempDir dir;
    write_file(dir.path() / "u.h", kHeader);
    return sppc::translate(temp_options(dir, text));
}

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// A class with the given body for the method Run.
std::string script(const std::string& body)
{
    return "using \"u.h\";\n"
           "class A {\n    std.pair<int, int> pr;\n    std.map<int, int> m;\n"
           "    std.pair<int, int> Mk() { return std.make_pair(1, 2); }\n"
           "    void Run() { " + body + " }\n}\n";
}

}  // namespace

TEST(StructuredBindingTranslation, LocalForms)
{
    const sppc::TranslateResult r = translate_with_header(
        script("var (a, b) = Mk(); var& (c, d) = this.pr; const var (e, f) = Mk(); int s = a + b + c + d + e + f;"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto [a, b] = A__Mk(__self);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "auto& [c, d] = __st->pr;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "const auto [e, f] = A__Mk(__self);")) << r.cpp;
}

TEST(StructuredBindingTranslation, ForeachForms)
{
    const sppc::TranslateResult r = translate_with_header(script(
        "int s = 0; foreach (var (k, v) in this.m) s += k + v; foreach (var& (k, v) in this.m) v += 1;"
        " foreach (var (i, x) in spp.enumerate(this.m)) s += x; foreach (int i in spp.range(3)) s += i;"
        " foreach (int i in spp.range(1, 3)) s += i;"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "for (auto [k, v] : __st->m)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "for (auto& [k, v] : __st->m)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "for (auto [i, x] : spp::enumerate(__st->m))")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "for (int32_t i : spp::range(3))")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "for (int32_t i : spp::range(1, 3))")) << r.cpp;
}

TEST(StructuredBindingTranslation, NamesAreLocals)
{
    // A binding name can shadow nothing and is not borrowed: a lambda may capture it.
    const sppc::TranslateResult r = translate_with_header(
        script("var (a, b) = Mk(); var f = () => a + b;"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
}

TEST(StructuredBindingTranslation, RefBindingIsBorrowedForALambda)
{
    const sppc::TranslateResult r = translate_with_header(
        script("var& (c, d) = this.pr; var f = () => c + d;"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'c', a borrowed pointer or reference")) << r.diagnostics;
}

TEST(StructuredBindingTranslation, ForeachRefBindingIsBorrowedForALambda)
{
    const sppc::TranslateResult r = translate_with_header(
        script("foreach (var& (k, v) in this.m) { var f = () => k + v; }"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'k', a borrowed pointer or reference")) << r.diagnostics;
}

TEST(StructuredBindingTranslation, MalformedBindingIsAnError)
{
    sppc::TranslateResult r = translate_with_header(script("var (a) = Mk();"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a structured binding needs two or more names")) << r.diagnostics;
    r = translate_with_header(script("var (a, b);"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a structured binding needs an initializer")) << r.diagnostics;
}

TEST(StructuredBindingTranslation, RefBindingCannotBindToATemporaryRange)
{
    sppc::TranslateResult r = translate_with_header(
        script("int s = 0; foreach (var& (i, x) in spp.enumerate(this.m)) s += x;"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics,
                    "'var&' cannot bind to the temporary value of 'spp.enumerate'; use 'var (i, x)'"))
        << r.diagnostics;
    r = translate_with_header(script("int s = 0; foreach (var& i in spp.range(3)) s += i;"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'var&' cannot bind to the temporary value of 'spp.range'; use 'var i'"))
        << r.diagnostics;
}
