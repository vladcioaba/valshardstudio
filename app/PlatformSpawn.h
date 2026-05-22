// PlatformSpawn — cross-platform process spawn + own-executable discovery.
//
// All editor tear-off / detached-panel code goes through this helper so
// AppDelegate.cpp doesn't need any #ifdef. POSIX uses posix_spawn; Windows
// uses CreateProcess. Linux uses /proc/self/exe to resolve the executable
// path; macOS uses _NSGetExecutablePath; Windows uses GetModuleFileName.

#pragma once

#include <string>
#include <vector>

namespace ocs::spawn
{

/// Absolute path to the currently-running executable. Returns empty
/// on failure.
std::string selfExecutablePath();

/// Launch `exePath` with the given argv (argv[0] should still be the
/// executable name as is conventional). Detached — the parent does
/// not wait. Returns the child PID on success (>0), 0 on failure.
/// Inherits the parent's environment.
int launchDetached(const std::string& exePath,
                   const std::vector<std::string>& argv);

}  // namespace ocs::spawn
