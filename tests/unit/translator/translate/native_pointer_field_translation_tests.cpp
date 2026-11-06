// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the native pointer fields. A field or module variable of type "T*" or "const T*"
// is stored as hot::CheckedPtr<T>. A reference field and a pointer to a value type are errors.
// Each row runs in the ".hot" frontend and in the ".cpp" frontend.
#include "hot/hotc_lib.h"
#include "support/check_translate_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace check_translate_test;

namespace
{

const char* kSharedStub = "[native, handle(\"std::shared_ptr<$T>\")] class Shared;\n";
const char* kWeaponStub = "[native, handle(\"std::shared_ptr<$T>\")] class Weapon;\n";
const char* kIntrusiveStub = "[native, handle(\"$T*\"), intrusive(\"AddRef\", \"Release\")] class Counted;\n";

// The ".hot" frontend: one field line in the class Foo.
hotc::TranslateResult hot_field(const std::string& stubs, const std::string& line)
{
    return check_translate(stubs, "    " + line + "\n");
}

// The ".cpp" frontend: one field line in the class A. "stubs" are the C++ stubs of the script.
hotc::TranslateResult cpp_script(const std::string& body, const std::string& stubs = "")
{
    static TempDir* dir = nullptr;
    if (!dir)
    {
        dir = new TempDir();
        write_file(dir->path() / "c.h", std::string(kCheckHeader) + "enum class NativeMode { A, B };\n");
    }
    hotc::TranslateOptions o = temp_options(*dir, "#include \"c.h\"\n" + stubs + body);
    o.script_name = "t.cpp";
    return hotc::translate(o);
}

hotc::TranslateResult cpp_field(const std::string& line, const std::string& stubs = "")
{
    return cpp_script("class A : public Widget { public:\n    " + line + "\n};\n", stubs);
}

const char* kRefMessage =
    "a field cannot be a reference: a reference cannot change its target and has no null. "
    "Use 'T*' (a checked pointer) or a value";

std::string value_pointer_message(const std::string& type)
{
    return "a field cannot hold a pointer to the value type '" + type
        + "': nothing owns it, so the pointer can dangle. Use a value, or a pointer to the object that owns it";
}

} // namespace

// ---- the valid forms

TEST(NativePointerField, HotPointerIsStoredAsCheckedPtr)
{
    const hotc::TranslateResult r = hot_field("", "Shared* m;");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::CheckedPtr<Shared> m")) << r.cpp;
}

TEST(NativePointerField, HotConstPointerFieldIsAnError)
{
    const hotc::TranslateResult r = hot_field("", "const Shared* m;");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a class member cannot be 'const'")) << r.diagnostics;
}

TEST(NativePointerField, HotPointerOfAHandleStubWithAWeakSourceIsStoredAsCheckedPtr)
{
    const hotc::TranslateResult r = hot_field(kSharedStub, "Shared* w;");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::CheckedPtr<Shared> w")) << r.cpp;
}

TEST(NativePointerField, HotNullInitializerStaysValid)
{
    const hotc::TranslateResult r = hot_field("", "Shared* m = null;");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::CheckedPtr<Shared> m")) << r.cpp;
}

TEST(NativePointerField, HotIntrusivePointerStaysTheHandle)
{
    const hotc::TranslateResult r = hot_field(kIntrusiveStub, "Counted* c;");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "CheckedPtr<Counted>")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Counted* c")) << r.cpp;
}

TEST(NativePointerField, CppPointerIsStoredAsCheckedPtr)
{
    const hotc::TranslateResult r = cpp_field("Shared* m = nullptr;");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::CheckedPtr<Shared> m")) << r.cpp;
}

TEST(NativePointerField, CppConstPointerIsStoredAsCheckedPtrOfConst)
{
    const hotc::TranslateResult r = cpp_field("const Shared* m = nullptr;");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::CheckedPtr<const Shared> m")) << r.cpp;
}

TEST(NativePointerField, CppModuleVariableIsStoredAsCheckedPtr)
{
    const hotc::TranslateResult r = cpp_script("Shared* g_mesh = nullptr;\nclass A : public Widget { public: int n = 0; };\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::CheckedPtr<Shared> g_mesh")) << r.cpp;
}

TEST(NativePointerField, HotModuleVariableIsStoredAsCheckedPtr)
{
    TempDir dir;
    write_file(dir.path() / "c.h", kCheckHeader);
    const hotc::TranslateResult m = hotc::translate(
        temp_options(dir, "using \"c.h\";\nShared* g_mesh;\nclass Foo : Widget { int n = 0; }\n"));
    ASSERT_TRUE(m.ok) << m.diagnostics;
    EXPECT_TRUE(has(m.cpp, "hot::CheckedPtr<Shared> g_mesh")) << m.cpp;
}

// ---- reference fields

TEST(NativePointerField, HotReferenceFieldIsAnError)
{
    for (const char* line : {"Mesh& r;", "const Mesh& r;", "int& gr;"})
    {
        const hotc::TranslateResult r = hot_field("", line);
        EXPECT_FALSE(r.ok) << line;
        EXPECT_TRUE(has(r.diagnostics, "\"&\" is not a type suffix; write ref T x")) << line << "\n" << r.diagnostics;
    }
}

TEST(NativePointerField, CppReferenceFieldIsAnError)
{
    for (const char* line : {"Mesh& r;", "const Mesh& r;", "int& gr;"})
    {
        const hotc::TranslateResult r = cpp_field(line);
        EXPECT_FALSE(r.ok) << line;
        EXPECT_TRUE(has(r.diagnostics, std::string("error: ") + kRefMessage)) << line << "\n" << r.diagnostics;
    }
}

// ---- pointer to a value type

TEST(NativePointerField, HotPointerToBuiltinIsAnError)
{
    const hotc::TranslateResult r = hot_field("", "int* p;");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "error: " + value_pointer_message("int"))) << r.diagnostics;
}

TEST(NativePointerField, CppPointerToBuiltinIsAnError)
{
    const hotc::TranslateResult r = cpp_field("int* p = nullptr;");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "error: " + value_pointer_message("int"))) << r.diagnostics;
}

TEST(NativePointerField, CppPointerToEnumIsAnError)
{
    const hotc::TranslateResult r = cpp_script("class A : public Widget { public:\n    NativeMode* p = nullptr;\n};\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a field cannot hold a pointer to the value type 'NativeMode'")) << r.diagnostics;
}

// ---- weak on a pointer field

TEST(NativePointerField, HotWeakPointerFieldIsAnError)
{
    const hotc::TranslateResult r = hot_field("", "[weak] Shared* w;");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "error: a pointer field is already a weak reference: remove '[weak]'"))
        << r.diagnostics;
}

// ---- the forms that keep the borrowed message

TEST(NativePointerField, HotPointerToPointerKeepsTheBorrowedMessage)
{
    const hotc::TranslateResult r = hot_field("", "Mesh** m;");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a field cannot hold a borrowed pointer or reference to 'Mesh'")) << r.diagnostics;
}

TEST(NativePointerField, CppPointerToPointerKeepsTheBorrowedMessage)
{
    const hotc::TranslateResult r = cpp_field("Mesh** m = nullptr;");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a field cannot hold a borrowed pointer or reference to 'Mesh'")) << r.diagnostics;
}

TEST(NativePointerField, HotReferenceToPointerIsATypeSuffixError)
{
    const hotc::TranslateResult r = hot_field("", "Mesh*& m;");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "\"&\" is not a type suffix; write ref T x")) << r.diagnostics;
}

// ---- the places that do not change

TEST(NativePointerField, HotReturnAndCollectionKeepTheBorrowedMessage)
{
    const hotc::TranslateResult ret = check_translate("", "    Mesh* Find() { return null; }\n");
    EXPECT_FALSE(ret.ok);
    EXPECT_TRUE(has(ret.diagnostics, "a return value cannot hold a borrowed pointer or reference to 'Mesh'"))
        << ret.diagnostics;
    const hotc::TranslateResult col = hot_field("", "std.vector<Mesh*> list;");
    EXPECT_FALSE(col.ok);
    EXPECT_TRUE(has(col.diagnostics, "a collection element cannot hold a borrowed pointer or reference to 'Mesh'"))
        << col.diagnostics;
}

// ---- a pointer without a weak source keeps the old borrowed finding

TEST(NativePointerField, HotPointerToAPlainStructKeepsTheBorrowedMessage)
{
    const hotc::TranslateResult r = hot_field("", "Weapon* w;");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "error: a field cannot hold a borrowed pointer or reference to 'Weapon'"))
        << r.diagnostics;
}

TEST(NativePointerField, HotPointerToAClassWithoutAWeakSourceKeepsTheBorrowedMessage)
{
    for (const char* stubs : {"", "[native] class Mesh;\n"})
    {
        const hotc::TranslateResult strict = hot_field(stubs, "Mesh* m;");
        EXPECT_FALSE(strict.ok) << stubs;
        EXPECT_TRUE(has(strict.diagnostics, "error: a field cannot hold a borrowed pointer or reference to 'Mesh'"))
            << strict.diagnostics;
        const hotc::TranslateResult relaxed = check_translate(stubs, "    Mesh* m;\n", {}, false);
        EXPECT_TRUE(relaxed.ok) << relaxed.diagnostics;
        EXPECT_TRUE(has(relaxed.warnings, "warning: a field cannot hold a borrowed pointer or reference to 'Mesh'"))
            << relaxed.warnings;
    }
}

TEST(NativePointerField, CppPointerToAClassWithoutAWeakSourceKeepsTheBorrowedMessage)
{
    const hotc::TranslateResult r = cpp_field("Mesh* m = nullptr;");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "error: a field cannot hold a borrowed pointer or reference to 'Mesh'"))
        << r.diagnostics;
}

TEST(NativePointerField, CppPointerToAPlainStructKeepsTheBorrowedMessage)
{
    const hotc::TranslateResult r = cpp_field("Weapon* w = nullptr;");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "error: a field cannot hold a borrowed pointer or reference to 'Weapon'"))
        << r.diagnostics;
}

TEST(NativePointerField, VoidPointerKeepsTheBorrowedMessage)
{
    const hotc::TranslateResult h = hot_field("", "void* p;");
    EXPECT_FALSE(h.ok);
    EXPECT_FALSE(has(h.diagnostics, "value type")) << h.diagnostics;
    EXPECT_TRUE(has(h.diagnostics, "a field cannot hold a borrowed pointer or reference to 'void'")) << h.diagnostics;
    const hotc::TranslateResult c = cpp_field("void* p = nullptr;");
    EXPECT_FALSE(c.ok);
    EXPECT_FALSE(has(c.diagnostics, "value type")) << c.diagnostics;
    EXPECT_TRUE(has(c.diagnostics, "a field cannot hold a borrowed pointer or reference to 'void'")) << c.diagnostics;
}

TEST(NativePointerField, ByteAndUnsignedCharPointersAreValuePointers)
{
    const hotc::TranslateResult h = hot_field("", "byte* p;");
    EXPECT_FALSE(h.ok);
    EXPECT_TRUE(has(h.diagnostics, "error: " + value_pointer_message("byte"))) << h.diagnostics;
    const hotc::TranslateResult c = cpp_field("unsigned char* p = nullptr;");
    EXPECT_FALSE(c.ok);
    EXPECT_TRUE(has(c.diagnostics, "a field cannot hold a pointer to the value type")) << c.diagnostics;
}

// ---- weak on a pointer without a checked pointer keeps the old behavior

TEST(NativePointerField, HotWeakOnAPointerWithoutAWeakSourceKeepsTheOldMessage)
{
    const hotc::TranslateResult r = hot_field("", "[weak] Weapon* w;");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(has(r.diagnostics, "already a weak reference")) << r.diagnostics;
}

// ---- a stub with a handle is a weak source

TEST(NativePointerField, HotPointerOfASharedPtrStubIsStoredAsCheckedPtr)
{
    const hotc::TranslateResult r = hot_field("[native, handle(\"std::shared_ptr<$T>\")] class Mesh;\n", "Mesh* m;");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::CheckedPtr<Mesh> m")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "struct weak_ref_traits<U, std::enable_if_t<std::is_same_v<U, ::Mesh> && "
                           "!detail::has_weak_from_this<U>::value>> : hot::shared_weak_ref<U> {};"))
        << r.cpp;
}

TEST(NativePointerField, HotPointerOfAWeakHandleStubIsStoredAsCheckedPtr)
{
    const hotc::TranslateResult r = hot_field(
        "[native, handle(\"Strong<$T>\"), weak_handle(\"Weak<$T>\")] class Mesh;\n", "Mesh* m;");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::CheckedPtr<Mesh> m")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, ": hot::custom_weak_ref<U, Strong<U>, Weak<U>> {};")) << r.cpp;
}

TEST(NativePointerField, HotPointerOfAStubWithWeakFromThisKeepsTheClassTraits)
{
    // Shared has enable_shared_from_this: the emitted traits are guarded and never apply to it.
    const hotc::TranslateResult r = hot_field(kSharedStub, "Shared* m;");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::CheckedPtr<Shared> m")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "!detail::has_weak_from_this<U>::value")) << r.cpp;
}

// ---- a valid checked pointer in non-strict mode, and a derived class

TEST(NativePointerField, HotCheckedPointerIsValidWithStrictOff)
{
    const hotc::TranslateResult r = check_translate("", "    Shared* m;\n", {}, false);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.warnings.empty()) << r.warnings;
    EXPECT_TRUE(has(r.cpp, "hot::CheckedPtr<Shared> m")) << r.cpp;
}

TEST(NativePointerField, HotPointerToADerivedClassOfAWeakSourceIsStoredAsCheckedPtr)
{
    const hotc::TranslateResult r = hot_field("", "SharedKid* m;");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::CheckedPtr<SharedKid> m")) << r.cpp;
}

// ---- the constructor initializer

TEST(NativePointerField, HotConstructorAssignsNullToTheCheckedPointer)
{
    const hotc::TranslateResult r =
        hot_field("", "Shared* m = null;\n    Foo() { this.m = null; }");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::CheckedPtr<Shared> m")) << r.cpp;
}

// ---- a .cpp script speaks C++

TEST(NativePointerField, CppMemberPointerFieldHasItsOwnMessage)
{
    const hotc::TranslateResult r = cpp_script(
        "struct P { int a; };\nclass A : public Widget { public:\n    int P::* mp = nullptr;\n};\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics,
        "t.cpp:4:14: error: a field cannot be a pointer to a data member ('int P::*'): it is not supported in a .cpp "
        "script. Use a pointer to a member function, or an index"))
        << r.diagnostics;
    EXPECT_FALSE(has(r.diagnostics, "borrowed")) << r.diagnostics;
}

TEST(NativePointerField, CppMemberFunctionPointerFieldIsStillValid)
{
    const hotc::TranslateResult r = cpp_script(
        "struct P { void f(); };\nclass A : public Widget { public:\n    void (P::* mp)() = nullptr;\n};\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(NativePointerField, CppBorrowedReturnMessageSpeaksCpp)
{
    for (const char* ret : {"Mesh*", "Mesh&"})
    {
        const hotc::TranslateResult r = cpp_script(std::string("Mesh m;\nclass A : public Widget { public:\n    ") + ret +
            " Find() { return " + (ret[4] == '&' ? "m" : "&m") + "; }\n};\n");
        EXPECT_FALSE(r.ok) << ret;
        EXPECT_TRUE(has(r.diagnostics,
            "a return value cannot hold a borrowed pointer or reference to 'Mesh': the object can be gone when the "
            "caller uses it. Use a value or a smart pointer such as 'std::shared_ptr<Mesh>'"))
            << ret << "\n" << r.diagnostics;
        EXPECT_FALSE(has(r.diagnostics, "stub")) << r.diagnostics;
        EXPECT_FALSE(has(r.diagnostics, "handle")) << r.diagnostics;
    }
}

// A script object has a fixed address for its life: a reload moves only the state block.
// So a return of "P&", "const P&" or "P*" of a script class is not a borrowed value.
namespace
{
const char* const kFluentPeer = "class P : public Widget { public:\n    int n = 0;\n    ";
}

TEST(NativePointerField, CppFluentReferenceReturnIsValid)
{
    const hotc::TranslateResult r = cpp_script(
        std::string(kFluentPeer) + "P& add(int v) { n += v; return *this; }\n};\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.diagnostics.empty()) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "return *__self;")) << r.cpp;
}

TEST(NativePointerField, CppConstReferenceReturnIsValid)
{
    const hotc::TranslateResult r = cpp_script(
        std::string(kFluentPeer) + "const P& get() const { return *this; }\n};\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(NativePointerField, CppPointerReturnOfAScriptClassIsValid)
{
    const hotc::TranslateResult r = cpp_script(std::string(kFluentPeer) + "P* self() { return this; }\n};\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "return __self;")) << r.cpp;
}

TEST(NativePointerField, CppDoublePointerReturnOfAScriptClassIsStillAnError)
{
    const hotc::TranslateResult r =
        cpp_script(std::string(kFluentPeer) + "P** bad() { return nullptr; }\n};\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a return value cannot hold a borrowed pointer or reference to 'P'"))
        << r.diagnostics;
}

TEST(NativePointerField, CppNativeReferenceReturnKeepsTheBorrowedMessage)
{
    const hotc::TranslateResult r = cpp_script(
        "Mesh m;\nclass A : public Widget { public:\n    Mesh& find() { return m; }\n};\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a return value cannot hold a borrowed pointer or reference to 'Mesh'"))
        << r.diagnostics;
}

TEST(NativePointerField, CppScriptClassReferenceFieldIsStillAnError)
{
    for (const char* line : {"P& peer;", "const P& peer;"})
    {
        const hotc::TranslateResult r = cpp_script(
            std::string("class P : public Widget { public: int n = 0; };\nclass A : public Widget { public:\n    ") +
            line + "\n};\n");
        EXPECT_FALSE(r.ok) << line;
        EXPECT_TRUE(has(r.diagnostics, std::string("error: ") + kRefMessage)) << line << "\n" << r.diagnostics;
    }
}

TEST(NativePointerField, CppStaleFluentReturnsCompileShapes)
{
    // A retired generation cannot return a reference: the stale call throws. A pointer gives null.
    const hotc::TranslateResult r = cpp_script(std::string(kFluentPeer) +
        "P& add(int v) { n += v; return *this; }\n    P* self() { return this; }\n};\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "throw_null_reference(\"call of a retired generation: P::add\")")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "return P &();")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "return P *();")) << r.cpp;
}

TEST(NativePointerField, CppBorrowedCollectionElementMessageSpeaksCpp)
{
    const hotc::TranslateResult r = cpp_field("std::vector<Mesh*> list;");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a collection element cannot hold a borrowed pointer or reference to 'Mesh': "))
        << r.diagnostics;
    EXPECT_FALSE(has(r.diagnostics, "stub")) << r.diagnostics;
}

// The warnings of the .cpp frontend reach the result of the translation. They do not stop it.
TEST(NativePointerField, CppFrontendWarningsReachTheResult)
{
    const hotc::TranslateResult r = cpp_script(
        "template <class T> class Crate : public Widget { public: T v; };\n"
        "class A : public Widget { public:\n    void F() { static int n = 0; n++; }\n};\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.warnings, "t.cpp:4:27: warning: the static local variable 'n' resets")) << r.warnings;
    EXPECT_TRUE(has(r.warnings, "t.cpp:2:")) << r.warnings;
    EXPECT_TRUE(has(r.warnings, "warning: the class template 'Crate' has a base class")) << r.warnings;
}
