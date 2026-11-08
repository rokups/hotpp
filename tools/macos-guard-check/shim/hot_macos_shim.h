// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The guard check (check.py) includes this file first in each compile. The Linux headers stand
// in for the macOS SDK. These names map the Darwin spelling to the Linux spelling.
#pragma once

#define st_mtimespec st_mtim
#define st_ctimespec st_ctim
