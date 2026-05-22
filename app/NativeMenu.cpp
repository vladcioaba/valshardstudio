// NativeMenu.cpp — non-Apple stubs.
//
// The real Cocoa implementation lives in NativeMenu.mm, which CMake only
// compiles inside its `if(APPLE)` block. On Linux/Windows this file
// provides empty bodies so AppDelegate links cleanly. ImGui's in-window
// menu bar carries the file actions on platforms without a native menu.

#if !defined(__APPLE__)

#    include "NativeMenu.h"

namespace ocs
{

void installMacMenu(const MacMenuCallbacks&) {}

void installMacMenuForDetachedPanel(const MacMenuCallbacks&) {}

std::string showOpenImageDialog()
{
    return {};
}

std::string showChooseDirectoryDialog()
{
    return {};
}

void asyncSetWindowSize(void*, int, int) {}

}  // namespace ocs

#endif  // !__APPLE__
