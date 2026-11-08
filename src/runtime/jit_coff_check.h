// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#pragma once

#include <string>

namespace llvm
{
class Module;
}

namespace hot::jit
{

// Compiles "m" (a COFF x86_64 target) to an object and loads it with CoffMemoryManager. Then it
// checks each RUNTIME_FUNCTION record, its UNWIND_INFO, the handler and the C++ FuncInfo. The
// first line of the text gives the counts. Each other line is one error.
std::string check_coff_unwind(llvm::Module& m);

} // namespace hot::jit
