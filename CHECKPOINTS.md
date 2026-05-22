# Checkpoints

Project is now under git (origin/main). Older checkpoints below were
tarballs in `/Users/Vlad/Work/GameDevBackup/` from before the repo was
initialised.

Restore an older tarball:
```
tar -xzf <file>.tar.gz -C <target-dir>
```

## 2026-04-27 — Retina-DPI + canvas fit fixes (git checkpoint)

State at this commit:
- **Retina ↔ non-Retina drag** no longer mis-sizes panels. Diagnosis:
  `DisplaySize` in points stays stable across the swap, only
  `FbScale` flips, and ImGui's per-frame `DockNodeUpdate` short-
  circuits when dockspace size hasn't changed. Fix in
  `AppDelegate.cpp`'s `windowContentScale` callback: defer two
  real `glfwSetWindowSize` calls via `Scheduler::runOnAxmolThread`
  — frame N+1 nudges height to H+1 (programmatic equivalent of a
  manual 1px drag), frame N+2 restores H. The two-frame defer
  keeps both calls outside any active ImGui frame so we don't
  re-enter `NewFrame`. Two earlier attempts (`requestPanelScale`
  and an in-ImGui `DockBuilderSetNodeSize` nudge) were dead ends
  and have been removed.
- **Canvas fit** no longer renders 2× on Retina / ¼ on non-Retina.
  The `centralWDesign = central->Size * fbs / scaleX` formula was
  composing `FbScale` on top of an already-points-based ratio
  (axmol's `_screenSize` is set from `glfwGetWindowSize` which is
  in logical points on macOS). Dropped the `* fbs` from all four
  sites: per-frame `fitK`, one-shot fit-to-content, and the two
  `_cachedSceneX/Y` scene-recenter computations.
- **Messages panel**: added a `Copy` button next to the scroll-to-
  bottom `⤓` that copies the currently filtered log body to the
  system clipboard.
- `BUGS.md`: closed the Retina drag bug with a full diagnosis
  writeup so the dead-end attempts don't get re-tried; removed
  the older "drag jerks panel layout" entry it superseded.
- Stale Python build at `dist/OpenCocosStudio.app` removed
  (untracked — was confusing macOS LaunchServices into activating
  the wrong binary when invoked by name).

Earlier work in this branch (since `2648012 Add BUGS.md`):
- Live ax-tree refresh on structural mutations (`refreshAxFromCsd`).
- Multi-select in Scene tree via Shift/Cmd-click + Cmd+G/Cmd+Shift+G
  group / ungroup; `editor.selectRange` / `toggleSelected`.
- Asset-browser drag-drop into Scene tree (image → ImageView child),
  via `AssetDragPayload` + `addImageChild`.
- Eye/visibility icon next to lock in Scene tree (Visible / Hidden /
  Inherited tri-state); Properties panel got a `Visible` checkbox
  that is parent-aware (shows "(hidden by ancestor)" indicator).
- Cross-process Messages queue: file-based IPC under
  `/tmp/opencocosstudio/<pid>.messages.log`; panel reader merges
  by timestamp so detached panels see main-app logs and vice-
  versa. Stale-pid GC at startup. `tailIpcLogs(N)` API.
- Messages panel as full debug viewer: include + exclude filters,
  level dropdown (publishes via `~/.opencocosstudio.loglevel`),
  auto-scroll toggle, persisted height + level + autoscroll
  (`~/.opencocosstudio.uiprefs`).
- Full GLFW window-event tracing (windowPos / windowSize /
  framebufferSize / windowContentScale / focus / iconify /
  maximize) at TRC level — opt-in via "Trace (spam)" log level.
- Globe rotation drag direction tweaks (Y, X both `+= dx*0.4f`).
- Mode-bar Edit/View geometry locked: stripH = `GetFrameHeight()/0.8 + 5`,
  button height `avail.y * 0.9`, ButtonTextAlign Y +6 px below
  centre, horizontally centred via `SetCursorPosX`.
- App-freeze on multi-monitor swap fixed: dropped the in-listener
  `Director::drawScene()` (per-frame render keeps canvas live
  anyway) and throttled position-event Trace logging to ≥100 ms.
- Asset-root heuristic extended: `Resources/`, `resources/`,
  `assets/`, `Assets/`, `cocos`.
- Canvas world↔screen math: `convertToNodeSpace`/
  `convertToWorldSpace` for `gTopRoot` removed (was double-
  counting the canvas zoom); `DisplayFramebufferScale` division
  removed (axmol's `getFrameSize` is already in OS points). Same
  fix in inspect-server `hit/click/hover` handler. New recursive
  top-down hit-test (calls `sortAllChildren` per level) so
  explicit `localZOrder` overrides resolve descendants-first.
- axmol patch: `RenderViewImpl::handleWindowSize` promoted from
  protected to public ([`OCS-PATCH`] in
  `core/platform/RenderViewImpl.h`) so OCS can drive a full
  window-size refresh on a DPI commit without going through the
  GLFW callback fan-out.

## 2026-04-26 23:29:17 — Pre-bubbles-on-top + canvas hit-test rebuild

File: `OpenCocosStudio-checkpoint-20260426-232917.tar.gz` (797 KB)

Snapshot taken before raising the move/resize/anchor "control bubbles"
to render on top of all scene Z layers. The user explicitly asked for
a checkpoint here in case the change breaks something.

State at this checkpoint:
- Live ax-tree refresh on structural mutations (`refreshAxFromCsd` —
  serialize csd → temp .csb → `CSLoader::createNode` → swap loadedRoot).
- Tear-off panel-size: snapshot fix reverted; uses live `win->Size`.
- Mode-bar Edit/View buttons sized to 90% of strip height, vertically
  centred so the strip can never clip them.
- Scene panel: lock + eye icons pinned at fixed columns on the LEFT
  ([2 px inset][eye][2 px gap][lock][2 px gap][label]) with row
  content shifted past both icons. Lock and eye both align across
  every depth (root + nested) — the `panelLeftX` is now captured
  once at row start instead of being recomputed after each `SameLine`.
- Selection highlight: light yellow row background with dark text;
  primary brighter than multi-select extras.
- Asset-root heuristic: `Resources/`, `resources/`, `assets/`,
  `Assets/` (sibling); ancestor literally named one of those plus
  `cocos`; final fallback to the .csd's parent directory.
- Canvas world↔screen math: `convertToNodeSpace`/`convertToWorldSpace`
  for gTopRoot stripped (it was double-counting the canvas zoom);
  `DisplayFramebufferScale` division removed (axmol's `getFrameSize`
  is already in OS points). Inspect `hit/click/hover` carries the same
  fix.
- Recursive top-down hit-test in the deferred-mouse-up branch — calls
  `sortAllChildren` per-node so explicit `localZOrder` resolves
  correctly and descendants are tested before their parent.
- Inspect server gained an `events` command that dumps the click
  ring + a one-line gate snapshot for remote triage.

## 2026-04-25 00:45:49 — Properties mirror + writeback

File: `OpenCocosStudio-checkpoint-20260425-004549.tar.gz` (797 KB)

Adds the first end-to-end data pipe for detached panels:
- `ocs::ipc::publishFocus()` / `mostRecentlyFocusedPeer()` — main-app
  touches `<pid>.focus` each frame while its OS window is key; detached
  panels pick the freshest peer by mtime.
- `ocs::ipc::publishMirror(panel, body)` / `readMirror(peerPid, panel)`
  — atomic write of panel state to `<pid>.<panel>.mirror`.
- `Editor::serializePanelMirror(name)` — emits Messages click-log and
  Properties attribute dump (`path=`, `display=`, `attr.<Name>=…`,
  `sub.<Tag>.<Key>=…`).
- `ocs::ipc::sendEdit(pid, nodepath, key, value)` + `drainInbox`'s new
  `onEdit` callback — writeback channel for detached Properties.
- `Editor::applyMirrorEdit(path, key, value)` resolves via `nodeAtPath`,
  wraps in `pushUndo()`, applies `setAttr` / `setSubAttrs`, and calls
  `rebuildAxMapsAndSync()` so live scene updates.
- Detached Properties renders parsed fields as `InputText` rows; Enter
  or deactivate-after-edit commits via `sendEdit`.

## 2026-04-25 00:25:44 — multi-window panels (phase 1–3)

File: `OpenCocosStudio-checkpoint-20260425-002544.tar.gz` (791 KB)

State at this checkpoint:
- Per-panel **detach into its own OS window** — menu
  (`View → Detach Panel → <Name>`), in-panel `↗` button, or drag
  the panel's title/tab past the main window's edge. Detached
  window opens at the panel's exact current footprint via
  `--panel=<Name> --size=WxH` args.
- **Singleton rule** across all OCS processes: IPC file
  `/tmp/opencocosstudio/detached.<Name>` owns one panel instance;
  every main-app hides its docked copy via `listDetachedPanels()`.
  Bidirectional sync keeps a `gSuppressedByDetach` set so
  user-toggled panels don't get auto-shown on reattach.
- **Close-to-reattach** (OS ✕): child process exits, registry
  GC'd, `Editor::reattachPanel(name)` re-associates the window
  with its **last dock node** via `DockBuilderDockWindow` — or
  requests a layout rebuild if the node got collapsed during the
  detach. Either way the panel lands in (or defaults to) its
  previous slot, not floating.
- **Detached-panel process** has a stripped-down menu: just
  **View → Reattach to Parent Window** (no panel list, no
  detach submenu, no reset). Full menu (File, View toggles,
  Detach Panel, Reset Layout) lives only in the main-app
  process.
- **View → Panel toggle** when the panel is detached acts as
  "bring it home" (closes the detached window, sync reattaches
  at last dock position). A second click hides; a third shows
  again at last dock.
- **View → Reset Layout** terminates every detached panel
  (SIGTERM + synchronous registry rm), clears
  `gSuppressedByDetach`, flips every `_show*Panel = true`, and
  calls `requestLayoutRebuild()` — all panels land in defaults
  on the next frame.
- Detached panel **auto-terminates** when no main-app peer
  remains (1.5 s grace for startup races) — no orphan panels.
- Mode-bar Edit / View buttons: 90% of strip height, vertically
  centered, strip itself 20% shorter than the default button
  row.
- Nav-globe drag: Y axis inverted so the globe yaws with the
  cursor; X axis re-flipped back to original per tuning
  feedback.
- "Canvas Debug" HUD renamed to **Messages**, made a regular
  dockable panel (default slot: tab with Layout in bottom-right).

New IPC functions (in `ocs::ipc`):
- `registerDetachedPanel(name)` / `unregisterDetachedPanel()`
- `listDetachedPanels()` with stale-pid GC
- `terminateDetachedPanels()` / `terminateDetachedPanel(name)`
- `sendReattach(targetPid, name)` + `drainInbox(onOpen, onReattach)`
  (reattach path is plumbed but currently unused — useful for
  future write-path refactoring)

New Editor surface:
- `reattachPanel(name)` — DockBuilderDockWindow if node alive,
  else requestLayoutRebuild().
- `_lastDockIdByPanel` — per-panel last-seen ImGui DockId.
- `setDetachPanelFn(fn)` host-installed hook used by the
  per-panel `↗` button.

## 2026-04-24 15:06:30 — selection-layer + deferred rubber-band

File: `OpenCocosStudio-checkpoint-20260424-150630.tar.gz` (779 KB)

State at this checkpoint:
- Canvas input routed through a dedicated **L3 selection widget** — an
  ImGui window (`##selectionLayer`) pinned to the central dock rect,
  padding/border zeroed, with an `InvisibleButton` filling
  `GetContentRegionAvail()` corner-to-corner. Auto-tracks
  `central->Pos` / `central->Size` every frame so OS window resizes
  and panel-splitter drags re-stretch it instantly.
- Input layering (draw bottom→top): L1 scene → L2 control graph →
  L3 selection → L4 floating widgets (zoom bar, nav globe).
  Hit-test order is inverse.
- Click gate collapsed to one line: `if (_dragMode == None &&
  !_canvasHovered) return;`. Old `inCentral + anyHover + hoverWin`
  gate is gone.
- **Rubber-band arms on every LMB-down on the canvas.** Click-vs-drag
  decision deferred to mouse-up:
  - release without moving (< 4 px) → single-click, hit-test the
    start point and select the topmost non-structural leaf, or clear
    selection on empty / non-additive.
  - release after a drag → marquee commit (overlap-test via
    `worldToScreen` on each node's 4 world corners — pure screen-space
    math, no round-trip drift).
- Structural container ctypes (`SingleNodeObjectData`,
  `GameNodeObjectData`, `ProjectNodeObjectData`,
  `GameLayerObjectData`, `LayerObjectData`) skipped in both
  single-click and marquee hit-test.
- **Canvas Debug HUD** (`_showCanvasDebug = true` by default) shows
  live `mouse`, `canvas rect`, `hovered`, `active`, `hoverWin`,
  `HoveredId` / `ActiveId`, predicted PASS/BLOCK, plus the last 32
  clicks with kind / hover-win / hit-name (ctype).
