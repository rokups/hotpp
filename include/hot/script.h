// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The optional header for a script file (design 4.7 and 7.8).
// The hotc tool injects this header, so a script does not need the include for hotc.
// A normal C++ build (without hotc) needs the include. It gives the macro HOT_ALIAS
// and the type hot::OldState, and the file compiles unchanged.
// The header is C++17 only, header-only, and has no link dependency.
#pragma once
#include "hot/runtime/old_state.h"

// HOT_ALIAS("old") on a field gives the old name of the field to hotc.
// The argument is a string literal. Only clang keeps the annotation.
// Other compilers ignore the macro.
#if defined(__clang__)
#define HOT_ALIAS(old) [[clang::annotate("hot.alias=" old)]]
#else
#define HOT_ALIAS(old)
#endif

// HOT_ATTRIBUTE("Name") on a script class gives hotc a script attribute tag, the same as "[Name]"
// in a .hot script. Write it after "class": "class HOT_ATTRIBUTE("Name") Player : public Actor".
// A prelude declares the attribute class: "[attribute] class Name;". Only clang keeps the
// annotation. Other compilers ignore the macro.
#if defined(__clang__)
#define HOT_ATTRIBUTE(name) [[clang::annotate("hot.attribute=" name)]]
#else
#define HOT_ATTRIBUTE(name)
#endif

// HOT_SHELL_MACRO("Name") after "class" names a macro that a call in the class body declares
// members for the shell. hotc copies the call "Name(...)" into the shell and hides the members
// that it declares from the script. HOT_NO_SLOT("A B") names virtuals that the shell keeps
// without a slot (the macro declares them). Both are for a header that defines such a macro for
// a base class, for example the type information of an engine. Only clang keeps the annotation.
// The file is still a normal C++ file: other compilers ignore the macros.
#if defined(__clang__)
#define HOT_SHELL_MACRO(name) [[clang::annotate("hot.shell_macro=" name)]]
#define HOT_NO_SLOT(names) [[clang::annotate("hot.no_slot=" names)]]
#else
#define HOT_SHELL_MACRO(name)
#define HOT_NO_SLOT(names)
#endif
