// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// UiApp: the native base class of a script that draws a user interface.
// The host calls Frame once for each frame. A script class overrides it.
#pragma once

// HOT_EXPORT: the script module (a DLL on Windows) derives from the class. The executable
// exports the class, so the module uses its constructor, destructor and vtable.
#include "hot/runtime/export.h"

class HOT_EXPORT UiApp
{
public:
    UiApp();
    virtual ~UiApp();
    virtual void Frame();
};
