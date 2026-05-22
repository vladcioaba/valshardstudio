// Walshard Studio app delegate — loads a .csd (passed via the global scene
// path below), renders it with cocos2d-legacy CSLoader via axmol's
// cocostudio extension, and overlays the UILayoutEditor ImGui panels via
// axmol's ImGuiPresenter.

#pragma once

#include "axmol.h"

#include <string>
#include <vector>

class AppDelegate : private ax::Application
{
public:
    AppDelegate();
    ~AppDelegate() override;

    void initGfxContextAttrs() override;
    bool applicationDidFinishLaunching() override;
    void applicationDidEnterBackground() override;
    void applicationWillEnterForeground() override;

    // Initial seed list from argv[1..N]. Populated by main() before
    // the Application starts. The first path becomes the active tab on
    // launch; the remaining ones open as background tabs.
    static std::vector<std::string> sCsdPaths;

    // Kept as the ACTIVE tab's path so existing callers reading
    // sCsdPath (menus, tests) still observe the current document.
    // Updated whenever the active tab changes or a file is opened.
    static std::string sCsdPath;

    // Detached-panel mode: when non-empty, this process was spawned via
    // `--panel=<name>` and should render only that one panel in its own
    // OS window (Properties / Scene / Assets / Layout / Mode).  The
    // main scene, tabs, dockspace, and other panels are suppressed.
    // Set by main.cpp before Application::run().
    static std::string sPanelMode;

    // Initial OS-window size for detached-panel mode, in ImGui points.
    // The parent window measures its ImGui panel rect at detach time
    // and forwards `--size=WxH` so the new OS window opens at the
    // exact same footprint the panel had while docked. Zero means
    // "fall back to kDesignResolution" (no override).
    static int sPanelWindowW;
    static int sPanelWindowH;
};
