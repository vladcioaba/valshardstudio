# Porting Walshard Studio to Windows

**Read this file end-to-end before changing any code.** It is the
handoff brief for a Claude Code session running inside a Windows VM
(or native Windows). The macOS and Linux builds are already working;
your job is to add Windows as the third supported target without
breaking either of them.

---

## 1. Goal and acceptance criteria

**Goal:** produce a Windows x64 build of Walshard Studio that:

- Compiles cleanly with Visual Studio 2022 + CMake + Ninja (or the
  Visual Studio generator).
- Launches as a `Walshard.exe`, opens the editor window, can open a
  `.csd` from the command line, and renders the scene.
- Survives a tab tear-off (the feature that spawns a new editor
  process via `posix_spawn` on POSIX) — on Windows this must use
  `CreateProcessW` instead.
- Persists `recents.txt` somewhere sensible (`%APPDATA%\Walshard
  Studio\recents.txt`).
- Co-operates with peer instances over the IpcBridge file protocol,
  using `%TEMP%\walshard\` instead of `/tmp/walshard/`.
- Does **not** regress macOS or Linux. Every Windows-specific change
  lives behind `#if defined(_WIN32)` and never touches the POSIX
  path.

**Definition of done:**

1. `cmake -S . -B build -G "Visual Studio 17 2022" -A x64
   -DAX_ROOT=C:\axmol -DCMAKE_BUILD_TYPE=Release` configures with no
   errors.
2. `cmake --build build --config Release -j` produces
   `build\bin\Walshard\Release\Walshard.exe` (or whatever path axmol's
   CMake macros put it at).
3. `.\Walshard.exe path\to\scene.csd` launches; the canvas renders
   the scene's root node.
4. macOS + Linux CI on GitHub Actions stay green (`.github/
   workflows/build.yml`).
5. A new matrix entry for `windows-2022` is added to the same
   workflow and goes green.

---

## 2. Project context (one-screen orientation)

- **What it is:** Walshard Studio is an open-source visual editor
  for `.csd` / `.csb` scene files used by cocos2d-x and axmol game
  engines. Built on axmol + ImGui. It replaces the discontinued
  Cocos Studio editor (Chukong, 2016).
- **Where you are:** branch `main` of
  `https://github.com/vladcioaba/walshardstudio`. axmol is pinned to
  `v2.9.1` (set the env var `AXMOL_REF=v2.9.1`).
- **License:** PolyForm Noncommercial 1.0.0 (editor) + MIT (runtime
  library `extensions/OCSExtension/`) + Asset Output Grant (files
  produced by the editor). Read top-level `LICENSE` if you need
  details. **Do not relicense any file.**
- **Standing user rule:** never auto-commit. The user reviews every
  change before it lands in git. Edit files; do not run `git
  commit`. If you finish a logical chunk, summarise what's staged
  and let the user commit it.
- **Brand rules:** trademark notice in `LICENSE` ("Trademark
  Notice"). The product name "Walshard Studio" must remain free of
  third-party marks. Tagline / README / About may mention
  cocos2d-x, axmol, Cocos Studio in lowercase prose under
  nominative fair use.

---

## 3. Environment setup (do this once)

Inside the Windows machine / VM:

```powershell
# 1. Install (or verify) developer tools
#    Visual Studio 2022 Community with "Desktop development with C++"
#    Git for Windows
#    Python 3 (axmol's setup needs it for some helper scripts)
#    CMake 3.22+ (bundled with VS, but make sure `cmake --version`
#                 works in a normal Developer PowerShell)

# 2. Clone axmol — pinned to v2.9.1, same as macOS + Linux CI
git clone --depth 1 --branch v2.9.1 `
  https://github.com/axmolengine/axmol C:\axmol

# 3. Clone Walshard
git clone https://github.com/vladcioaba/walshardstudio.git C:\walshard
cd C:\walshard

# 4. First configure attempt (will likely surface errors; that is
#    expected — proceed to Section 4)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DAX_ROOT=C:\axmol -DCMAKE_BUILD_TYPE=Release
```

---

## 4. Known port-blockers (the meat of the work)

The macOS build is the source of truth. Linux already works because
its differences from macOS are small (NativeMenu stub +
XDG paths). Windows diverges more deeply. Each item below is a
known failure site; fix them in order, each behind `#if
defined(_WIN32)`.

### 4.1 IPC base directory — `app/IpcBridge.cpp:24`

```cpp
// Current:
const fs::path kBaseDir = "/tmp/walshard";
```

POSIX-only path. Replace with platform-conditional construction:

```cpp
static fs::path makeBaseDir()
{
#if defined(_WIN32)
    const char* tmp = std::getenv("TEMP");
    if (!tmp || !*tmp) tmp = std::getenv("TMP");
    if (!tmp || !*tmp) tmp = "C:\\Temp";
    return fs::path(tmp) / "walshard";
#else
    return fs::path("/tmp/walshard");
#endif
}

const fs::path kBaseDir = makeBaseDir();
```

The rest of `IpcBridge.cpp` is `std::filesystem`-based and should
work as-is on Windows.

### 4.2 Recents file — `app/UILayoutEditor/src/UILayoutEditor/Editor.cpp` `Editor::recentsFile()`

This already has a 3-way platform split (macOS / Linux / other).
The `#else` branch currently returns `~/.walshard/recents.txt`.
Add a proper Windows branch:

```cpp
#elif defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (appdata && *appdata) dir = fs::path(appdata) / "Walshard Studio";
    else dir = fs::path(home) / "Walshard Studio";  // fallback
```

Also consider that `std::getenv("HOME")` is empty on Windows. Either:
- Use `USERPROFILE` env var instead when `HOME` is empty, or
- Skip the home-based fallback on Windows entirely (APPDATA is
  always set on a sane Windows install).

### 4.3 Log paths — `app/UILayoutEditor/src/UILayoutEditor/Log.cpp`

Three dotfile paths in this file:

- `~/.walshard.log`
- `~/.walshard.loglevel`
- `~/.walshard.uiprefs`

On Windows, dotfiles in `%USERPROFILE%` work but are non-idiomatic.
Switch to `%LOCALAPPDATA%\Walshard Studio\<file>` on Windows:

```cpp
fs::path platformConfigDir()
{
#if defined(_WIN32)
    const char* base = std::getenv("LOCALAPPDATA");
    if (!base || !*base) base = std::getenv("APPDATA");
    if (!base || !*base) return fs::path("walshard");
    return fs::path(base) / "Walshard Studio";
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    return home ? fs::path(home) / "Library" / "Application Support" / "Walshard Studio"
                : fs::path("walshard");
#else  // Linux / BSD — XDG
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return fs::path(xdg) / "walshard";
    const char* home = std::getenv("HOME");
    return home ? fs::path(home) / ".config" / "walshard" : fs::path("walshard");
#endif
}
```

Then replace the three dotfile path constructions with
`platformConfigDir() / "walshard.log"` etc. Make sure
`fs::create_directories(...)` is called before the first write,
otherwise the open will fail on Windows where the parent dir does
not auto-create.

### 4.4 Process spawning — `app/AppDelegate.cpp` around line 642

POSIX `posix_spawn` and `posix_spawn_file_actions_*` are used to
fork off a new editor process for tab tear-off and detached panels.
Windows has no `posix_spawn`. Wrap in a small abstraction:

Add a new header `app/PlatformSpawn.h`:

```cpp
#pragma once
#include <string>
#include <vector>

namespace ocs::platform
{

/// Spawn a new process with the given executable path + argv.
/// Returns the child's PID on success, or 0 on failure. The
/// caller is responsible for tracking the PID; this helper does
/// not wait.
int spawnDetached(const std::string& exePath,
                  const std::vector<std::string>& argv);

}
```

Implementations:

- `app/PlatformSpawn_Posix.cpp` (compile when NOT `_WIN32`) — moves
  the existing `posix_spawn` logic here.
- `app/PlatformSpawn_Win32.cpp` (compile when `_WIN32`) — uses
  `CreateProcessW` from `<windows.h>`. Convert the argv vector
  to a single Win32 command line (quote each arg, escape
  embedded quotes per the standard `CommandLineToArgvW` rules).

The existing call sites in `AppDelegate.cpp` (search for
`posix_spawn`, lines around 642 and 672) become
`ocs::platform::spawnDetached(exePath, argv)`.

Use the existing CMake glob (`app/*.cpp`) — both new files will be
picked up automatically. To stop them from being compiled on the
wrong platform, either:

- Put `#if defined(_WIN32)` at the top of `_Win32.cpp` (and
  `!defined(_WIN32)` for `_Posix.cpp`), making them empty TUs on
  the wrong platform. **Preferred** — keeps the CMake unchanged.

### 4.5 SVG rasterisation — `rsvg-convert` is POSIX-only by default

`rsvg-convert` is invoked by:

- The editor at runtime (SvgImageNode rasterisation in
  `extensions/OCSExtension/src/OCSExtension/SvgImageNode.cpp`)
- The editor at bake time (`svg-bake` RPC command)

Two options:

1. **Bundle a Windows build of `rsvg-convert.exe`** — gnome/librsvg
   ships Windows binaries via MSYS2 (`pacman -S librsvg`). Put the
   `.exe` and the DLLs it needs into `packaging/win/rsvg/` and
   adjust the shell-out in SvgImageNode.cpp to look there first
   on Windows.
2. **Swap to header-only `lunasvg`** — drop-in replacement, no
   external binary. Bigger change because `lunasvg::Document` API
   differs from invoking a CLI, but cleaner long-term.

Recommend option 1 for v0.1 Windows port (smaller diff). Option 2
is a follow-up.

### 4.6 GLFW backend + axmol Windows

axmol officially supports Windows. Its CMake should detect the
target automatically. If `cmake -S . -B build` fails citing
missing axmol Windows files, run axmol's own setup script first:

```powershell
cd C:\axmol
python setup.py
```

(Read what that script does first — it modifies environment
variables and downloads engine dependencies.)

### 4.7 Headers that don't exist on Windows

Likely offenders surfaced by the first build attempt:

| POSIX header | Windows replacement |
|---|---|
| `<unistd.h>` | use `<io.h>` + `<process.h>` for the bits you need |
| `<sys/wait.h>` | not needed if you don't `wait()` |
| `<sys/socket.h>` (used by `InspectServer`) | `<winsock2.h>` + `<ws2tcpip.h>` + link `ws2_32.lib` |
| `<arpa/inet.h>`, `<netinet/in.h>` | as above, all unified under WinSock |
| `<pthread.h>` | use `std::thread` (it's already used elsewhere) or `<windows.h>` threads |

InspectServer.cpp uses BSD sockets directly. Will need
`#if defined(_WIN32)` wraps to call `WSAStartup`, swap `close()`
for `closesocket()`, and link `ws2_32` in CMake.

---

## 5. CMakeLists changes

Top-level `CMakeLists.txt` already gates `.mm` files via
`if(APPLE)`. Add a similar block for Windows-specific link libs at
the bottom:

```cmake
if(WIN32)
    target_link_libraries(${APP_NAME} PRIVATE ws2_32 shell32)
endif()
```

GLFW + OpenGL are already pulled in by axmol's setup for Windows.

---

## 6. CI workflow extension

Add a third matrix entry in `.github/workflows/build.yml`. Locate
the `matrix.include` list and add:

```yaml
- os: windows-2022
  label: windows-x64
  artifact: Walshard-windows-x64.zip
```

Then add platform-conditional install + package steps. Reference
implementation snippets:

```yaml
- name: Install Windows deps
  if: matrix.os == 'windows-2022'
  run: |
    choco install python --version=3.11.0 -y
    # rsvg-convert: ship via MSYS2 or skip in CI; if we have it
    # bundled in packaging/win/rsvg/, no install step needed

- name: Configure (Windows)
  if: matrix.os == 'windows-2022'
  run: cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DAX_ROOT=${{ runner.workspace }}/axmol

- name: Package (Windows)
  if: matrix.os == 'windows-2022'
  shell: pwsh
  run: |
    cd build/bin/Walshard/Release
    Compress-Archive -Path * -DestinationPath ../../../../${{ matrix.artifact }}
```

`actions/cache@v4` for axmol works identically on Windows; just
make sure the cache key includes the matrix label so caches don't
cross-contaminate.

---

## 7. Smoke test the Windows build

After your first successful build:

```powershell
cd build\bin\Walshard\Release
.\Walshard.exe ..\..\..\..\..\path\to\some\scene.csd
```

Verify:

1. Window opens, ImGui dockspace renders.
2. The provided `.csd` loads and the canvas shows the scene root.
3. File → Save works (writes back to the same `.csd`).
4. File → Export → CSB writes a `.csb` next to the `.csd`.
5. Dragging a tab out of the window spawns a new `Walshard.exe`
   process (Task Manager confirms).
6. Closing the app cleanly removes `%TEMP%\walshard\<pid>.window`
   and friends.

If any of those fail, debug with the Visual Studio debugger
attached. The InspectServer TCP port (default 9876, override with
`OCS_INSPECT_PORT`) is the easiest way to drive the editor for
scripted smoke tests once it's running.

---

## 8. Do not regress macOS or Linux

Every Windows-specific code change must be inside
`#if defined(_WIN32)`. After each edit, mentally check: would this
file still compile on Apple clang and on Linux gcc/clang? If
unsure, build at least one of those locally before continuing.

The macOS CI runner is `macos-14`; Linux is `ubuntu-22.04`. If
your local Windows VM doesn't have a way to cross-test, push a
branch (the user pushes — see Section 2 standing rule) and let
GitHub Actions tell you. **Do not push directly to `main` from
the Windows side; create a branch and open a PR so the user can
review.**

---

## 9. Known unknowns

These are things this brief is unsure about. Investigate before
committing to an approach:

- **axmol's Windows toolchain on Apple Silicon Macs via ARM64 VM:**
  axmol may assume x86_64. Confirm by running `python setup.py`
  and watching for arch mismatches. If ARM64 Windows fails, fall
  back to GitHub Actions `windows-2022` (x86_64) for shipped
  builds and use the VM only for editing/debugging non-build
  code.
- **`rsvg-convert.exe` redistribution licensing:** librsvg is LGPL.
  Bundling the .exe is fine for distribution; bundling its `.dll`
  dependencies is fine as long as the LGPL notice ships with
  them. Confirm by reading librsvg's COPYING file when you fetch
  the binary.
- **Code signing / SmartScreen:** unsigned `.exe`s trigger
  Windows SmartScreen on first run. Out of scope for v0.1 port —
  document the "Run anyway" workaround in README. Microsoft Store
  submission ($19 one-time) or an OV/EV cert ($200-1000/yr) are
  future-work decisions, not blockers.

---

## 10. Reporting back

When you finish a logical chunk (a single Section 4 item, the
CMake edit, or the CI matrix entry), report to the user with:

1. Summary of what you changed (file + brief diff intent).
2. State of the build: configures? compiles? runs?
3. Anything you punted on or could not resolve.
4. **Do not commit.** Stage if it helps the user review, but the
   `git commit` is theirs.

Good luck.
