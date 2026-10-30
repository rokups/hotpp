// Script runtime (design sections 4.4, 4.6 and 4.7).
// The parts are in include/spp/runtime. This header includes all of them.
// The files in src/runtime have the globals and the generation loader.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <cstring>
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

#include "spp/module.h"
#include "spp/runtime/records.h"
#include "spp/runtime/hashing.h"
#include "spp/runtime/shell.h"
#include "spp/runtime/threads.h"
#include "spp/runtime/closure.h"
#include "spp/runtime/handles.h"
#include "spp/runtime/deref.h"
#include "spp/runtime/coalesce.h"
#include "spp/runtime/enumerate.h"
#include "spp/runtime/properties.h"
#include "spp/runtime/old_state.h"
#include "spp/runtime/state.h"
#include "spp/runtime/migration.h"
#include "spp/runtime/registry.h"
#include "spp/runtime/loader.h"
