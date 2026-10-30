// The optional header for a script file (design 4.7 and 7.8).
// The sppc tool injects this header, so a script does not need the include for sppc.
// A normal C++ build (without sppc) needs the include. It gives the macro SPP_ALIAS
// and the type spp::OldState, and the file compiles unchanged.
// The header is C++17 only, header-only, and has no link dependency.
#pragma once
#include "spp/runtime/old_state.h"

// SPP_ALIAS("old") on a field gives the old name of the field to sppc.
// The argument is a string literal. Only clang keeps the annotation.
// Other compilers ignore the macro.
#if defined(__clang__)
#define SPP_ALIAS(old) [[clang::annotate("spp.alias=" old)]]
#else
#define SPP_ALIAS(old)
#endif
