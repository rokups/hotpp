// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Script runtime (design sections 4.4, 4.6 and 4.7).
// The parts are in include/hot/runtime. This header includes all of them.
// The files in src/runtime have the globals and the generation loader.
#pragma once
#include "hot/module.h"
#include "hot/runtime/arith.h"
#include "hot/runtime/checked_ptr.h"
#include "hot/runtime/closure.h"
#include "hot/runtime/container_ops.h"
#include "hot/runtime/coalesce.h"
#include "hot/runtime/deref.h"
#include "hot/runtime/enumerate.h"
#include "hot/runtime/errors.h"
#include "hot/runtime/handles.h"
#include "hot/runtime/hashing.h"
#include "hot/runtime/iface.h"
#include "hot/runtime/in.h"
#include "hot/runtime/loader.h"
#include "hot/runtime/migration.h"
#include "hot/runtime/old_state.h"
#include "hot/runtime/properties.h"
#include "hot/runtime/records.h"
#include "hot/runtime/registry.h"
#include "hot/runtime/shell.h"
#include "hot/runtime/state.h"
#include "hot/runtime/task.h" // empty without C++20 coroutines
#include "hot/runtime/threads.h"
#include "hot/runtime/trace.h"
#include "hot/runtime/value.h"

#include <array> // the generated code uses std::array for the array types of a script
#include <cassert> // the script builtin "assert" (gap G08) is the C++ assert
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional> // the generated code uses std::optional for "T?"
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>
#if __has_include(<span>) && (__cplusplus >= 202002L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L))
#include <span> // std::span, the view "T[]" with the view type std
#endif
