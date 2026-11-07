// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Golden-output tests. The files in tests/golden/expected/ hold the generated C++ and the slot
// file of each example script. A later parser port must give the same bytes.
// Set HOT_UPDATE_GOLDEN to write the files and pass.
// CMake defines HOT_SOURCE_DIR.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace translator_test;

namespace
{

// The expected files are in tests/golden/expected/. HOT_UPDATE_GOLDEN writes them.
void check_golden(const std::string& name, const std::string& text)
{
    const fs::path path = fs::path(HOT_SOURCE_DIR) / "tests" / "golden" / "expected" / name;
    if (std::getenv("HOT_UPDATE_GOLDEN") != nullptr)
    {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
        return;
    }
    ASSERT_TRUE(fs::exists(path)) << "missing golden file: " << path;
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    EXPECT_EQ(ss.str(), text) << "golden file differs: " << name;
}

// Translates "<base>_v1" as generation 1 and "<base>_v2" as generation 2.
// The slot input of generation 2 is the slot output and the signature lines of generation 1.
// "ext" is the extension of the script files.
void golden_pair(const std::string& base, const std::string& ext = ".hot")
{
    std::string slots;
    for (int gen = 1; gen <= 2; ++gen)
    {
        const std::string file = base + "_v" + std::to_string(gen) + ext;
        hotc::TranslateOptions o = engine_options(file.c_str(), gen);
        o.script_name = file; // the file name only, so the output has no absolute path
        o.slots_in = slots;
        const hotc::TranslateResult r = hotc::translate(o);
        ASSERT_TRUE(r.ok) << r.diagnostics;
        const std::string stem = base + "_v" + std::to_string(gen) + ".g" + std::to_string(gen);
        check_golden(stem + ".cpp", r.cpp);
        check_golden(stem + ".slots", r.slots_out);
        slots = r.slots_out + r.sigs_out;
    }
}

// Translates one script as generation 1. The script has no second generation.
void golden_one(const std::string& base, const std::string& ext = ".hot", const std::string& std_version = "")
{
    const std::string file = base + ext;
    hotc::TranslateOptions o = engine_options(file.c_str(), 1);
    o.script_name = file;
    o.std_version = std_version;
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    check_golden(base + ".g1.cpp", r.cpp);
    check_golden(base + ".g1.slots", r.slots_out);
}

} // namespace

// An async method is a coroutine that returns a hot::Task. The script needs C++20.
TEST(GoldenTranslation, AsyncBasic)
{
    golden_one("async_basic", ".hot", "c++20");
}

// A virtual async method and an override go through the _virt wrapper.
TEST(GoldenTranslation, AsyncVirtual)
{
    golden_one("async_virtual", ".hot", "c++20");
}

TEST(GoldenTranslation, Player)
{
    golden_pair("player");
}

TEST(GoldenTranslation, NativeStd)
{
    golden_pair("native_std");
}

TEST(GoldenTranslation, Timer)
{
    golden_pair("timer");
}

// Fixed-size array types: fields, locals, parameters, a return type and a generic argument.
TEST(GoldenTranslation, Array)
{
    golden_pair("array");
}

// The runtime checks: "a[i]" is hot::index, "*p" is hot::star, and the catch types of the runtime.
TEST(GoldenTranslation, Checks)
{
    golden_one("checks_v1");
}

// A "ref T" return of a method, a static method and a virtual method: the C++ return type is "T&".
TEST(GoldenTranslation, RefReturn)
{
    golden_one("ref_return_v1");
}

// A native "T&" getter binds to a "ref" local: "auto& m = anim.GetMotion();".
TEST(GoldenTranslation, NativeRefGetter)
{
    golden_one("native_ref_getter_v1");
}

// A "ref" call on an object of another script class, a "ref" parameter and a static method.
TEST(GoldenTranslation, RefReturnOtherClass)
{
    golden_one("ref_return_xclass_v1");
}

// A "T*" return of a value struct: "null" or "&place". A caller "var p" is a borrowed pointer.
TEST(GoldenTranslation, PtrReturn)
{
    golden_one("ptr_return_v1");
}

// A lambda that writes a captured local or parameter: the variable is a shared cell (G06).
TEST(GoldenTranslation, SharedCell)
{
    golden_one("shared_cell_v1");
}

// The script exception types: a struct with the base ScriptError, and a struct with such a parent.
TEST(GoldenTranslation, Exceptions)
{
    golden_one("exceptions_v1");
}

// The scoped stub rules and cpp_prelude: "self", "class : Base" and the text in the shell.
TEST(GoldenTranslation, Prelude)
{
    golden_one("prelude_v1");
}

// An override with the plain handle type for native parameters of the forms T*, const T&,
// std::shared_ptr<T> and an intrusive T*: the slot keeps the native type and the body binds the handle.
TEST(GoldenTranslation, HandleOverride)
{
    golden_one("handle_override_v1");
}

// A handle passed to a native "T*" or "const T&" parameter converts (hot::as_ptr, hot::as_ref).
// Two overloads that disagree keep the explicit form.
TEST(GoldenTranslation, HandleCall)
{
    golden_one("handle_call_v1");
}

// Weak handles: "[weak] T" fields, parameters and locals. A read locks the handle.
TEST(GoldenTranslation, WeakHandle)
{
    golden_one("weak_handle_v1");
}

// Classes that use each other through handles: a weak back handle, a strong pair, a self
// reference and an intrusive pair. The forward block declares each class first.
TEST(GoldenTranslation, CycleHandles)
{
    golden_one("cycle_handles_v1");
}

// A borrowed value (T&, T*, ref, &x) is an argument of every call form: a method, a native call, a
// constructor, new, a lambda, a ternary, a cast and a nested call.
TEST(GoldenTranslation, BorrowedArgs)
{
    golden_one("borrowed_args_v1");
}

TEST(GoldenTranslation, Props)
{
    golden_one("props_v1");
}

TEST(GoldenTranslation, AbiChild)
{
    golden_pair("abi_child");
}

// C array fields: the state keeps "int data[4]"; the init and the migration row use the array type.
TEST(GoldenTranslation, CppArrayFields)
{
    golden_pair("cpp_array", ".cpp");
}

TEST(GoldenTranslation, CppPlayer)
{
    golden_pair("cpp_player", ".cpp");
}

// A .cpp script with static members: a static data member is a module variable, a static method
// is a free function, and a static constant is a const before the class block.
TEST(GoldenTranslation, CppStatic)
{
    golden_pair("cpp_static", ".cpp");
}

// A .cpp script with methods, a constructor and a destructor that are defined outside the class.
// The body goes to the member in the class block, and the definition text does not stay.
TEST(GoldenTranslation, CppOutOfClass)
{
    golden_pair("cpp_outofclass", ".cpp");
}

// A .cpp script with a constructor member initializer list. Each entry of a field becomes a
// statement at the start of the body, in the order of the field declarations.
TEST(GoldenTranslation, CppCtorInit)
{
    golden_pair("cpp_ctor_init", ".cpp");
}

// A .cpp script uses a script class as a type: "new", pointers, references, field access,
// method calls and casts on another script object.
TEST(GoldenTranslation, CppObjects)
{
    golden_pair("cpp_objects", ".cpp");
}

// A .cpp script class method returns "P&", "const P&" or "P*" of a script class (fluent style).
// The stale stub of a reference throws, and the stale stub of a pointer returns null.
TEST(GoldenTranslation, CppFluent)
{
    golden_pair("cpp_fluent", ".cpp");
}

// A .cpp script class has operator members. The frontend mangles the names (op_add_assign). The
// shell has the written operators only. Generation 2 changes the signature of "operator()".
TEST(GoldenTranslation, CppOperators)
{
    golden_pair("cpp_operators", ".cpp");
}

// A .cpp script class derives from a script class. The parent declares a virtual method.
// Generation 2 adds an override of a native virtual and a new virtual method in the child.
TEST(GoldenTranslation, CppInherit)
{
    golden_pair("cpp_inherit", ".cpp");
}

// A .cpp script with lambdas that use the object: the capture of "this" becomes the capture of
// the shell, and a field reads through the state of the shell.
TEST(GoldenTranslation, CppLambda)
{
    golden_pair("cpp_lambda", ".cpp");
}

// A .cpp script with default arguments: the prototype has them and the definition does not.
TEST(GoldenTranslation, CppDefaults)
{
    golden_one("cpp_defaults", ".cpp");
}

// A .cpp script with free functions that use a module variable: the prototype comes first and
// the definition comes after the module block.
TEST(GoldenTranslation, CppFreeFunction)
{
    golden_one("cpp_freefn", ".cpp");
}

// A .cpp script with a function template that uses a module variable. The template stays in its
// place, after the module block.
TEST(GoldenTranslation, CppTemplateFunction)
{
    golden_one("cpp_template_fn", ".cpp");
}

// A .cpp script with a function in a namespace that uses a module variable.
TEST(GoldenTranslation, CppNamespaceFunction)
{
    golden_one("cpp_namespace_fn", ".cpp");
}

// A .cpp script with classes and module variables in namespaces, one nested with the compact form.
// Two classes have the same name in two namespaces. Generation 2 changes a variable type.
TEST(GoldenTranslation, CppNamespaces)
{
    golden_pair("cpp_ns", ".cpp");
}

// A .cpp script with a constexpr function that reads a module variable only at runtime.
TEST(GoldenTranslation, CppConstexprFunction)
{
    golden_one("cpp_constexpr", ".cpp");
}

// The module attribute list "[std(\"mystd\")];" in a prelude. The text has "mystd::" for each standard name.
TEST(GoldenTranslation, ModuleAttrsStd)
{
    hotc::TranslateOptions o = engine_options("module_attrs_std_v1.hot", 1);
    o.script_name = "module_attrs_std_v1.hot";
    o.preludes.emplace_back("module.prelude", "[std(\"mystd\")];\n");
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    check_golden("module_attrs_std_v1.g1.cpp", r.cpp);
    check_golden("module_attrs_std_v1.g1.slots", r.slots_out);
}

// The module attribute std of the script overrides the default of the prelude for the script.
TEST(GoldenTranslation, ModuleAttrsOverride)
{
    hotc::TranslateOptions o = engine_options("module_attrs_override_v1.hot", 1);
    o.script_name = "module_attrs_override_v1.hot";
    o.preludes.emplace_back("module.prelude", "[std(\"mystd\")];\n");
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    check_golden("module_attrs_override_v1.g1.cpp", r.cpp);
    check_golden("module_attrs_override_v1.g1.slots", r.slots_out);
}

// The module attribute array of the script: the text has the template of the host for "T[N]".
TEST(GoldenTranslation, ModuleAttrsArray)
{
    hotc::TranslateOptions o = engine_options("module_attrs_array_v1.hot", 1);
    o.script_name = "module_attrs_array_v1.hot";
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    check_golden("module_attrs_array_v1.g1.cpp", r.cpp);
    check_golden("module_attrs_array_v1.g1.slots", r.slots_out);
}

// A stub with checked_index and the custom array template give a specialization of
// hot::checked_index. A name that the headers do not declare gets none.
TEST(GoldenTranslation, CheckedIndexStubs)
{
    hotc::TranslateOptions o = engine_options("checked_index_v1.hot", 1);
    o.script_name = "checked_index_v1.hot";
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    check_golden("checked_index_v1.g1.cpp", r.cpp);
    check_golden("checked_index_v1.g1.slots", r.slots_out);
}

// A script struct with operators: the members, the generated "==", "!=" and the compound operators.
TEST(GoldenTranslation, StructOperators)
{
    golden_one("operators_struct");
}

// A script class with operators: the shell members, the borrowed "in" parameter and the Value return.
// Generation 2 changes the body of operator+ only.
TEST(GoldenTranslation, ClassOperators)
{
    golden_pair("operators_class");
}

// Script code that uses the operators of a script class on boxes: "a + b", "a == b", "a += b", "-a", "a[i]".
TEST(GoldenTranslation, ClassOperatorUse)
{
    golden_one("operators_use_class");
}

// Free functions and static methods: the prototypes come before the bodies, and the manifest has no entry.
// Generation 2 changes two bodies.
// Method overloads (gap G17): the keys of the slots and the C++ forwarders of the written name.
TEST(GoldenTranslation, MethodOverload) { golden_pair("method_overload"); }

TEST(GoldenTranslation, StaticFunctions)
{
    golden_pair("functions");
}

// A native stub with the name of a header type is not a script type. It gives no name clash error.
TEST(GoldenTranslation, NativeStubWithAHeaderTypeNameIsNotAClash)
{
    hotc::TranslateOptions o = engine_options("player_v1.hot", 1);
    o.script_name = "stub_item.hot";
    o.script_text =
        "using \"Actor.h\";\nusing \"Object.h\";\n\n[native] class Item;\n\nclass Bag : Actor {\n    int count = 0;\n\n"
        "    override void Tick(float dt) { this.count = this.count + 1; }\n}\n";
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.diagnostics.find("has the same name"), std::string::npos) << r.diagnostics;
}

namespace
{
// Translates aot_demo.hot as AOT code (generation 1) and checks the golden file "name".
void golden_aot(const std::string& name, bool optimized)
{
    hotc::TranslateOptions o = engine_options("aot_demo.hot", 1);
    o.script_name = "aot_demo.hot";
    o.aot = true;
    o.aot_optimized = optimized;
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    check_golden(name, r.cpp);
}
} // namespace

// The plain AOT output of the demo. The shape of plain AOT code must not change.
TEST(GoldenTranslation, AotDemoAot)
{
    golden_aot("aot_demo_aot.g1.cpp", false);
}

// The optimized AOT output of the demo (hotc --aot-optimized): direct dispatch, no slot.
TEST(GoldenTranslation, AotDemoOptimized)
{
    golden_aot("aot_demo_opt.g1.cpp", true);
}

// Cross-module access (modules step 3). "xmod_use.hot" imports "xmod_lib.hot". The owner registers
// its free functions and static methods (Registry::add_fn). The importer calls them through
// hot::call_fn and reads and writes the module variable through hot::mod_var. A method and a
// property of the imported class are slot calls, a field is hot::field and "new" is hot::construct.
// The importer has no record, no state and no body of the imported class.
// The import text comes from TranslateOptions::modules, so the unit name has no absolute path.
TEST(GoldenTranslation, CrossModuleOwner)
{
    golden_one("xmod_lib");
}

// A "ref" call on an object of an imported class binds to a "ref" local. An assignment to the
// receiver ends the ref.
TEST(GoldenTranslation, CrossModuleRefCall)
{
    const std::string lib = read_file(std::string(HOT_SOURCE_DIR) + "/tests/scripts/xmod_ref_lib.hot");
    hotc::TranslateOptions o = engine_options("xmod_ref_use.hot", 1);
    o.script_name = "xmod_ref_use.hot";
    o.modules = {{"xmod_ref_lib.hot", lib}};
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("auto& s = "), std::string::npos);

    hotc::TranslateOptions bad = o;
    bad.script_text = "using \"xmod_ref_lib.hot\";\n"
                             "class Reader {\n"
                             "    void Touch() {\n"
                             "        $Shelf shelf = new $Shelf();\n"
                             "        ref s = shelf.At(0);\n"
                             "        shelf = new $Shelf();\n"
                             "        s.x = 1;\n"
                             "    }\n"
                             "}\n";
    const hotc::TranslateResult rb = hotc::translate(bad);
    ASSERT_FALSE(rb.ok);
    EXPECT_NE(rb.diagnostics.find("can move or remove the elements of 'shelf'"), std::string::npos) << rb.diagnostics;
}

TEST(GoldenTranslation, CrossModuleImporter)
{
    hotc::TranslateOptions o = engine_options("xmod_use.hot", 1);
    o.script_name = "xmod_use.hot";
    o.modules = {{"xmod_lib.hot", read_file(std::string(HOT_SOURCE_DIR) + "/tests/scripts/xmod_lib.hot")}};
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    check_golden("xmod_use.g1.cpp", r.cpp);
    check_golden("xmod_use.g1.slots", r.slots_out);
    EXPECT_NE(r.cpp.find("hot::call_fn<decltype(hot_fn_twice)>(hot::unit_ref(\"xmod_lib.hot\"), __site, \"twice\""),
        std::string::npos);
    EXPECT_NE(r.cpp.find("hot::mod_var<int32_t>(hot::unit_ref(\"xmod_lib.hot\"), __site, \"counter\")"), std::string::npos);
    EXPECT_NE(r.cpp.find("hot_mv_counter() = hot_mv_counter() + 1;"), std::string::npos);
    EXPECT_NE(r.cpp.find("return hot::call_script<void(hot::PlainObject*, int32_t)>(__self, Box_SLOT_grow, "),
        std::string::npos);
    EXPECT_NE(r.cpp.find("return hot::call_script<int32_t(hot::PlainObject*)>(__self, Box_SLOT_get_Doubled);"),
        std::string::npos);
    EXPECT_NE(r.cpp.find("return hot::field<int32_t>(__s, __site, \"size\");"), std::string::npos);
    EXPECT_NE(r.cpp.find("Box__fld_size(hot::shell_of<Box_Shell>(b, \"Box\")) = "), std::string::npos);
    EXPECT_NE(r.cpp.find("hot::construct<Box_Shell, int32_t>(hot::class_ref(\"Box\"), "), std::string::npos);
    EXPECT_NE(r.cpp.find("__site, \"Box::unit\");"), std::string::npos);
    EXPECT_EQ(r.cpp.find("reg.add(&Box_record"), std::string::npos);
    EXPECT_EQ(r.cpp.find("BoxState_g1"), std::string::npos);
    EXPECT_EQ(r.cpp.find("reg.add_fn(\"Box::unit\""), std::string::npos);
}
