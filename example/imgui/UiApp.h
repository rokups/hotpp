// UiApp: the native base class of a script that draws a user interface.
// The host calls Frame once for each frame. A script class overrides it.
#pragma once

class UiApp {
public:
    UiApp();
    virtual ~UiApp();
    virtual void Frame();
};
