// Cross-process coordination between Walshard Studio instances.
//
// The protocol is entirely file-based under /tmp/walshard/, so
// it works without any networking stack, daemons, or platform
// abstractions. Each OCS process writes a small registry describing
// its window + tab bar every frame, scans peers to find drop targets,
// and drops one-shot request files into peer inboxes to trigger
// cross-window tab moves.
//
// File layout:
//   /tmp/walshard/<pid>.window       — this process's window + tab bar
//   /tmp/walshard/<pid>.hover        — a source is hovering our bar
//   /tmp/walshard/<pid>.inbox/*.req  — open-tab requests from peers
//
// Stale entries (from crashed instances) are cleaned up lazily during
// listPeers() by kill(pid, 0) testing.

#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ocs::ipc
{

struct PeerInfo
{
    int pid = 0;
    // Full window frame in screen coords (top-left origin, pixels).
    int frameX = 0, frameY = 0, frameW = 0, frameH = 0;
    // Tab bar rect in screen coords.
    int tabBarX0 = 0, tabBarY0 = 0, tabBarX1 = 0, tabBarY1 = 0;
    int tabCount = 0;
    // Per-tab X extents in screen coords, used by a dragging peer to
    // compute the insertion index when hovering over us.
    std::vector<std::pair<int, int>> tabRects;
};

struct SelfInfo
{
    // This process's window frame + tab bar rect in screen coords.
    int frameX, frameY, frameW, frameH;
    int tabBarX0, tabBarY0, tabBarX1, tabBarY1;
    int tabCount;
    std::vector<std::pair<int, int>> tabRects;
};

/// One-time setup: create the base directory, the inbox dir for this
/// PID, and schedule our registry file for removal at exit.
void init();

/// Force-remove this PID's registry + inbox + hover files.
void shutdown();

/// Refresh `/tmp/walshard/<pid>.window` from the caller's
/// current state. Called every frame from the render loop; writes are
/// cheap because the file is tiny.
void publish(const SelfInfo& info);

/// Scan the inbox for pending requests; invoke the matching callback
/// for each and delete the file afterwards so the request is
/// consumed. Currently understood request kinds:
///   `open <path>`                  — `onOpen(path, -1)`
///   `open_at <idx> <path>`         — `onOpen(path, idx)`
///   `reattach <panelName>`         — `onReattach(panelName)`
///   `edit <nodepath> <key>=<val>`  — `onEdit(nodepath, key, val)`
///   `select <nodepath>`            — `onSelect(nodepath)`
///   `selectasset <absPath>`        — `onSelectAsset(absPath)`
///   `replacedoc <xml-body>`        — `onReplaceDoc(xmlBody)`  (multi-line)
/// Any of the optional callbacks may be omitted; requests of that
/// kind are then consumed + ignored.
void drainInbox(
    const std::function<void(const std::string&, int /*insertIdx*/)>&
        onOpen,
    const std::function<void(const std::string& /*panelName*/)>&
        onReattach = {},
    const std::function<void(const std::string& /*nodepath*/,
                             const std::string& /*key*/,
                             const std::string& /*value*/)>&
        onEdit = {},
    const std::function<void(const std::string& /*nodepath*/)>&
        onSelect = {},
    const std::function<void(const std::string& /*absPath*/)>&
        onSelectAsset = {},
    const std::function<void(const std::string& /*xmlBody*/)>&
        onReplaceDoc = {});

/// Discover living peers. Stale entries (from processes that no longer
/// exist) are garbage-collected as a side effect.
std::vector<PeerInfo> listPeers();

/// Send `<targetPid>.inbox/<uuid>.req` containing the csd path so the
/// target process picks it up via drainInbox(). `insertIdx < 0`
/// means "append at end".
void sendOpen(int targetPid, const std::string& csdPath,
              int insertIdx = -1);

/// Ask the target main-app process to un-hide the named panel in its
/// docked layout — used when a detached panel's OS window is dragged
/// back over a main app window and dropped, or when "View → Dock
/// Back" is invoked.
void sendReattach(int targetPid, const std::string& panelName);

/// Forward an edit from a detached panel to the target main-app
/// process: `edit <nodepath> <key>=<value>\n`. Writes through the
/// same inbox as open / reattach so the ordering is preserved.
void sendEdit(int targetPid,
              const std::string& nodepath,
              const std::string& key,
              const std::string& value);

/// Tell the target main-app to make the node at `nodepath` the
/// active selection. Used by detached Scene-tree clicks.
void sendSelect(int targetPid, const std::string& nodepath);

/// Tell the target main-app to make `absPath` the active asset
/// selection. Used by detached Assets browser clicks.
void sendSelectAsset(int targetPid, const std::string& absPath);

/// Replace the target main-app's active document with the given
/// XML body. Used by detached panels that hold a local copy of the
/// peer's .csd: when the user mutates it via the real panel UI
/// (lock toggle, drag-drop, …), the whole document is forwarded so
/// the peer converges on the same state.
void sendReplaceDoc(int targetPid, const std::string& xmlBody);

/// Touch `<targetPid>.hover` while the source is hovering the peer's
/// tab bar. The cursor's screen X and the dragged tab's label travel
/// with the hover so the target can render a ghost in-place.
/// clearHover() removes the file when the hover ends.
void writeHoverSignal(int targetPid,
                      int sourcePid,
                      int cursorScreenX,
                      const std::string& dragLabel);
void clearHoverSignal(int targetPid);

/// If a peer is currently hovering this process's tab bar, return
/// true and write the source PID + cursor screen X + dragged label.
/// The file is stale-ignored if its mtime is older than 500 ms.
bool readHoverSignal(int& sourcePidOut,
                     int& cursorScreenXOut,
                     std::string& dragLabelOut);

// --- Detached panels --------------------------------------------------
// Each detached-panel process touches `/tmp/walshard/
// detached.<Name>` with its PID on startup, and removes the file on
// shutdown (best effort via atexit / signal handler). Main-app processes
// read `listDetachedPanels()` each frame to know which panels to hide
// from their own dock layout — the singleton rule: one detached panel
// per name across all OCS processes.
//
// Callers pass the panel name as-is (e.g. "Properties"). The file's
// content is a single ASCII PID, validated against processAlive() on
// read to GC stale entries.

/// Register this PID as the active detached panel of the given name.
/// Overwrites any previous registration (stale entries auto-GC'd).
void registerDetachedPanel(const std::string& panelName);

/// Remove this PID's detached-panel registration. Called at shutdown.
void unregisterDetachedPanel();

/// List panel names currently registered as detached by any living
/// peer. Callers use this to hide the corresponding docked copy.
std::vector<std::string> listDetachedPanels();

/// Best-effort SIGTERM to every living detached-panel process. Each
/// target's signal handler runs its own shutdown() which removes its
/// registry file, so the panels re-appear in main-app windows on
/// the next frame's listDetachedPanels() scan.
///
/// Used by the View → Reset Layout menu action to bring every panel
/// back to its home dock position across all OCS processes.
void terminateDetachedPanels();

/// SIGTERM the single detached-panel process owning the given panel
/// name, if any. Silent no-op when no peer owns the name.
void terminateDetachedPanel(const std::string& panelName);

// --- Focus + mirrored panel data --------------------------------------
// Main-app processes touch `<pid>.focus` whenever their OS window is
// key (updated every frame so mtime stays fresh while focused), and
// serialize per-panel data into `<pid>.<panel>.mirror` whenever the
// displayed state changes. Detached panels pick the most-recently-
// focused main-app peer by comparing focus-file mtimes, then read the
// corresponding .mirror file to render the mirrored content.

/// Refresh this process's focus mtime. Call each frame while focused;
/// stop calling when focus is lost (the file stays, but its mtime
/// becomes stale so readers pick a more recently-focused peer).
void publishFocus();

/// Find the pid with the freshest `.focus` mtime (main-app peer
/// ordering). Returns 0 when no peer has ever published focus.
int mostRecentlyFocusedPeer();

/// Write `<selfPid>.<panel>.mirror` atomically. `panel` must match
/// the detached-panel name ("Messages", "Properties", ...).
void publishMirror(const std::string& panel, const std::string& body);

/// Read the mirror body published by `peerPid` for the given panel,
/// or empty string when the file is missing / unreadable.
std::string readMirror(int peerPid, const std::string& panel);

/// Enumerate every living peer's `.events.mirror` body so the
/// Messages panel can show events from main-app + every detached
/// panel process in one merged view. Returns pairs of (pid, body),
/// sorted by ascending pid; self is excluded.
std::vector<std::pair<int, std::string>> listPeerEventMirrors();

}  // namespace ocs::ipc
