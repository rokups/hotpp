// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A host that names the standard library "mystd" and "other_std". The module attribute "[std(\"mystd\")];" uses them.
#pragma once

#include <string>
#include <vector>

namespace mystd = std;
namespace other_std = std;
