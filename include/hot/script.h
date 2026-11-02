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
