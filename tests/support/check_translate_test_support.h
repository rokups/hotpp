// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Helpers for the tests of the translator checks: a header with native types of many shapes,
// and a function that translates a script with stubs, members and preludes.
#pragma once

#include "translator_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace check_translate_test
{

using namespace translator_test;

inline const char* kCheckHeader =
    "#include <memory>\n"
    "#include <string>\n"
    "#include <vector>\n"
    "namespace mystd = std;\n"
    "namespace my { namespace eastl = std; }\n"
    "struct SinkBase { virtual ~SinkBase() {} virtual void Take(mystd::vector<int> v) {} };\n"
    "struct Weapon { int dmg = 0; };\n"
    "struct Vec3 { float x, y, z; };\n"
    "struct Mesh { std::string name; virtual ~Mesh() {} };\n"
    "struct NoCopy { NoCopy() {} NoCopy(const NoCopy&) = delete; };\n"
    "struct ThrowMove { ThrowMove() {} ThrowMove(const ThrowMove&) {} ThrowMove(ThrowMove&&) noexcept(false) {} };\n"
    "struct Poly { virtual ~Poly() {} };\n"
    "struct PolyFinal final { virtual ~PolyFinal() {} };\n"
    "struct Plain { int a = 0; };\n"
    "template <class T> struct NoArrow { T* p = nullptr; };\n"
    "template <class T> struct Arrow { T* p = nullptr; T* operator->() const { return p; } };\n"
    "struct Shared : std::enable_shared_from_this<Shared> { int v = 0; virtual ~Shared() {} };\n"
    "struct SharedKid : Shared {};\n"
    "struct Counted { void AddRef(); void Release(); };\n"
    "struct Overloaded { void AddRef(); void AddRef(int); void Release(); };\n"
    "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n"
    "template <class T> struct Box { Box() {} Box(const Box&) = delete; };\n"
    "struct Owner { Weapon* weapon = nullptr; int N = 0; Weapon* GetWeapon() { return weapon; }\n"
    "    Weapon* Pick() { return weapon; } Weapon* Pick(int) { return weapon; } static Owner* Get() { return nullptr; "
    "} };\n"
    "inline Weapon* FindWeapon() { return nullptr; }\n"
    "inline Weapon* Echo(Weapon* w) { return w; }\n";

// Translates "using \"c.h\"; <stubs> class Foo : Widget { <members> }".
// "preludes" are (name, text) pairs. "strict" sets strict_borrowed.
inline hotc::TranslateResult check_translate(const std::string& stubs, const std::string& members,
    const std::vector<std::pair<std::string, std::string>>& preludes = {}, bool strict = true,
    hotc::TranslatorSession* session = nullptr)
{
    static TempDir* dir = nullptr;
    if (!dir)
    {
        dir = new TempDir();
        write_file(dir->path() / "c.h", kCheckHeader); // once: a new time would make the session parse again
    }
    hotc::TranslateOptions o =
        temp_options(*dir, "using \"c.h\";\n" + stubs + "class Foo : Widget {\n" + members + "}\n");
    o.preludes = preludes;
    o.strict_borrowed = strict;
    return session ? hotc::translate(o, *session) : hotc::translate(o);
}

inline bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

inline const char* kDefaultShared = "[native, default_handle(\"std::shared_ptr<$T>\")] class *;\n";

} // namespace check_translate_test
