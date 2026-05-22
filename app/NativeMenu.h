// Native-platform File menu + file dialogs. On macOS this installs a real
// NSMenu into NSApp's main menu bar so the File items show up in the system
// menu bar (not inside the ImGui window). Open/Save As trigger NSOpenPanel
// and NSSavePanel respectively.
//
// On non-Apple platforms this header exists so AppDelegate.cpp compiles, but
// installMacMenu is a no-op.

#pragma once

#include <functional>
#include <string>

namespace ocs
{

struct MacMenuCallbacks
{
    std::function<void(const std::string& path)> onOpen;
    std::function<void()>                        onSave;
    std::function<void(const std::string& path)> onSaveAs;
    std::function<void(const std::string& dir)>  onNewProject;
    std::function<void()>                        onExportCsb;

    // View menu — panel visibility toggles. panelId: 0=Scene, 1=Assets,
    // 2=Properties, 3=Layout, 4=Mode, 5=CanvasDebug. isPanelVisible is
    // queried right before the menu is shown so the checkmark reflects
    // current state even when a panel was closed via its titlebar X.
    std::function<void(int panelId)>             onTogglePanel;
    std::function<bool(int panelId)>             isPanelVisible;

    // View menu — detach a panel into its own OS window. Same panelId
    // mapping as onTogglePanel. Source window hides the docked copy;
    // peer processes do the same via the IPC `detached.<Name>` file.
    std::function<void(int panelId)>             onDetachPanel;

    // View menu — restore the default dock layout and panel visibility.
    // Also asks any detached panel processes to exit so every panel is
    // back in its home position in this window.
    std::function<void()>                        onResetLayout;

    // Detached-panel process only: "View → Reattach to parent window".
    // Unused by the main-app menu.
    std::function<void()>                        onReattachToParent;

    // Preferences window — Cmd+,.
    std::function<void()>                        onOpenPreferences;
};

void installMacMenu(const MacMenuCallbacks& cb);

/// Minimal menu for a detached-panel process — just View → Reattach
/// to parent window. The OS default app menu (Quit, Hide, Minimize,
/// Close Window) stays as-is.
void installMacMenuForDetachedPanel(const MacMenuCallbacks& cb);

/// Show an NSOpenPanel filtered to image file types. Blocks on the main
/// thread until the user picks a file or cancels. Returns the absolute
/// path, or empty string on cancel / non-Apple platforms.
std::string showOpenImageDialog();

/// Show an NSOpenPanel that picks a directory. Used by File → New Project.
/// Returns the absolute directory path, or empty string on cancel.
std::string showChooseDirectoryDialog();

/// Defer a glfwSetWindowSize call to the next main-runloop iteration,
/// when ImGui is between frames and a synchronous resize won't trigger
/// the "Forgot to call EndFrame" assertion. Caller passes the GLFWwindow*
/// (as void* to keep the header GLFW-free) and the new size in points.
void asyncSetWindowSize(void* glfwWindow, int widthPx, int heightPx);

}  // namespace ocs
