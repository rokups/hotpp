// Script runtime (design sections 4.4, 4.6 and 4.7).
// The parts are in include/hot/runtime. This header includes all of them.
// The files in src/runtime have the globals and the generation loader.
#pragma once
#include "hot/module.h"
#include "hot/runtime/closure.h"
#include "hot/runtime/coalesce.h"
#include "hot/runtime/deref.h"
#include "hot/runtime/enumerate.h"
#include "hot/runtime/handles.h"
#include "hot/runtime/hashing.h"
#include "hot/runtime/loader.h"
#include "hot/runtime/migration.h"
#include "hot/runtime/old_state.h"
#include "hot/runtime/properties.h"
#include "hot/runtime/records.h"
#include "hot/runtime/registry.h"
#include "hot/runtime/shell.h"
#include "hot/runtime/state.h"
#include "hot/runtime/threads.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>
