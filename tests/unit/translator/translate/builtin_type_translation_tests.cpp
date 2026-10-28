// Tests for spc::translate with the builtin type names: fixed-width names, reload keys and errors.
#include <gtest/gtest.h>

#include <string>

#include "spp/spc_lib.h"
#include "support/stub_translate_test_support.h"
#include "support/translator_test_support.h"

using namespace translator_test;
using namespace stub_translate_test;

namespace {

// A native base with virtual methods that have fixed-width and C++ spelled parameters.
const char* kWidthHeader =
    "#include <cstdint>\n"
    "class Wide { public: virtual ~Wide() {}\n"
    "  virtual void A(uint64_t id, int n) {}\n"
    "  virtual void B(unsigned k, unsigned long long q, int8_t s) {}\n"
    "  virtual int64_t C(const uint16_t& r) { return 0; }\n"
    "};\n";

spc::TranslateResult wide_translate(const std::string& members)
{
    static TempDir* dir = nullptr;
    if (!dir) dir = new TempDir();
    write_file(dir->path() / "w.h", kWidthHeader);
    return spc::translate(temp_options(*dir, "using \"w.h\";\nclass Foo : Wide {\n" + members + "}\n"));
}

}  // namespace

TEST(BuiltinTypeTranslation, FieldsEmitTheFixedWidthName)
{
    const struct {
        const char* script;
        const char* alias;
        const char* cpp;
    } rows[] = {
        {"sbyte", "int8", "int8_t"},     {"byte", "uint8", "uint8_t"},     {"short", "int16", "int16_t"},
        {"ushort", "uint16", "uint16_t"}, {"int", "int32", "int32_t"},     {"uint", "uint32", "uint32_t"},
        {"long", "int64", "int64_t"},    {"ulong", "uint64", "uint64_t"},  {"nint", "intptr", "intptr_t"},
        {"nuint", "uintptr", "uintptr_t"}, {"wchar", "char16", "char16_t"},
    };
    for (const auto& r : rows) {
        for (const char* name : {r.script, r.alias}) {
            const spc::TranslateResult t = wide_translate(std::string("    ") + name + " f;\n");
            ASSERT_TRUE(t.ok) << name << ": " << t.diagnostics;
            EXPECT_TRUE(has(t.cpp, std::string("    ") + r.cpp + " f;")) << name << "\n" << t.cpp;
            EXPECT_TRUE(has(t.cpp, std::string("typeid(") + r.cpp + ").name()")) << name;
        }
    }
}

// The reload type key is typeid(T).name() of the field type. A rename of the type spelling
// must give the same layout text, so the reload does not reject it.
TEST(BuiltinTypeTranslation, RenamedAliasKeepsTheReloadTypeKey)
{
    const spc::TranslateResult a = wide_translate("    ulong x;\n");
    const spc::TranslateResult b = wide_translate("    uint64 x;\n");
    const spc::TranslateResult c = wide_translate("    uint64_t x;\n");
    ASSERT_TRUE(a.ok) << a.diagnostics;
    ASSERT_TRUE(b.ok) << b.diagnostics;
    ASSERT_TRUE(c.ok) << c.diagnostics;
    EXPECT_EQ(a.cpp, b.cpp);
    EXPECT_EQ(a.cpp, c.cpp);
    EXPECT_EQ(a.slots_out, b.slots_out);
    EXPECT_TRUE(has(a.cpp, "{\"x\", nullptr, typeid(uint64_t).name()")) << a.cpp;
}

TEST(BuiltinTypeTranslation, OverrideKeyMatchesTheNativeParameters)
{
    // The aliases and the fixed-width names match uint64_t and int.
    spc::TranslateResult r = wide_translate("    override void A(ulong id, int32 n) { }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    r = wide_translate("    override void A(uint64_t id, int n) { }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    // The native C++ spellings "unsigned" and "unsigned long long" match uint and ulong.
    r = wide_translate("    override void B(uint k, ulong q, sbyte s) { }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    // A wrong width is not an override.
    r = wide_translate("    override void A(uint id, int n) { }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "hides the virtual method")) << r.diagnostics;
}

TEST(BuiltinTypeTranslation, CppSpellingsAreErrorsWithAHint)
{
    const struct {
        const char* type;
        const char* hint;
    } cases[] = {
        {"unsigned", "use 'uint'"},           {"unsigned int", "use 'uint'"},
        {"long long", "use 'long'"},          {"unsigned long", "use 'ulong'"},
        {"unsigned long long", "use 'ulong'"}, {"unsigned char", "use 'byte'"},
        {"unsigned short", "use 'ushort'"},   {"signed char", "use 'sbyte'"},
    };
    for (const auto& c : cases) {
        const spc::TranslateResult r = wide_translate(std::string("    ") + c.type + " f;\n");
        EXPECT_FALSE(r.ok) << c.type;
        EXPECT_TRUE(has(r.diagnostics, c.hint)) << c.type << ": " << r.diagnostics;
    }
}
