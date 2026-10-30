// Helpers for the tests of the handle types: a native class with a stub, translated in a temporary directory.
#pragma once

#include "translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

namespace stub_translate_test
{

using namespace translator_test;

inline const char* kWeaponHeader =
    "#include <memory>\n"
    "struct Weapon { int dmg = 0; };\n"
    "struct Vec3 { float x, y, z; };\n"
    "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n";

inline const char* kWeaponStub = "[native, handle(\"std::shared_ptr<$T>\")] class Weapon;\n";

inline std::string weapon_script(const std::string& stub, const std::string& members)
{
    return "using \"w.h\";\n" + stub + "class Foo : Widget {\n" + members + "}\n";
}

inline sppc::TranslateResult weapon_translate(const std::string& members, bool stub_in_prelude = false)
{
    static TempDir* dir = nullptr;
    if (!dir)
        dir = new TempDir();
    write_file(dir->path() / "w.h", kWeaponHeader);
    sppc::TranslateOptions o = temp_options(*dir, weapon_script(stub_in_prelude ? "" : kWeaponStub, members));
    if (stub_in_prelude)
        o.preludes.push_back({"weapon.prelude", kWeaponStub});
    return sppc::translate(o);
}

inline bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

inline const char* kHierHeader =
    "#include <memory>\n"
    "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n"
    "struct Object { void AddRef(); void Release(); virtual ~Object(); };\n"
    "struct Item : Object { int v = 0; Item(int); };\n"
    "struct Gadget : Item { using Item::Item; };\n"
    "namespace eng { struct Base : Object {}; struct Thing : Base {}; }\n"
    "struct Flat { int a = 0; };\n"
    "struct FlatKid : Flat { int b = 0; };\n";

inline const char* kIntrusiveStub = "[native, handle(\"$T*\"), intrusive(\"AddRef\", \"Release\")] class Object;\n";

inline const char* kSharedStub = "[native, handle(\"std::shared_ptr<$T>\")] class Object;\n";

inline const std::string kIntr = "spp::Intrusive<Item, &Object::AddRef, &Object::Release>";

inline sppc::TranslateResult hier_translate(const std::string& stubs, const std::string& members,
    bool stub_in_prelude = false, sppc::TranslatorSession* session = nullptr)
{
    // Write the header once: a new mtime would make the session parse again.
    static TempDir* dir = nullptr;
    if (!dir)
    {
        dir = new TempDir();
        write_file(dir->path() / "w.h", kHierHeader);
    }
    sppc::TranslateOptions o = temp_options(*dir, weapon_script(stub_in_prelude ? "" : stubs, members));
    if (stub_in_prelude)
        o.preludes.push_back({"hier.prelude", stubs});
    return session ? sppc::translate(o, *session) : sppc::translate(o);
}

} // namespace stub_translate_test
