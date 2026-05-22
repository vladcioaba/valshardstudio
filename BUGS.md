# Known Bugs

Open issues we've noticed but haven't fixed yet. Closed bugs move to a
`## Closed` section at the bottom with the commit that fixed them.

Format:
- **Short title** — *status* (last seen / commit). One-paragraph description
  + repro steps if known. Add a "Hypothesis:" line for unverified causes.

## Open

### Detached panel window opens too big
*reported 2026-04-26* — *attempted fix in working tree*: reverted the
pre-drag snapshot capture in `AppDelegate.cpp` so we use the live
`win->Size` after `MovingWindow` flips again. ImGui has already moved
the panel into floating mode by then, so `Size` is the standalone
footprint, not the dock slot's full chrome. Needs user verification.

User: "open the window with the pane's size."

### Scene panel: lock icon hidden when narrow
*reported 2026-04-26* — *attempted fix in working tree*: lock now
renders at a fixed X column (panel left edge, computed by subtracting
the current ImGui indent from the row's start cursor) before the
TreeNodeEx label. Independent of tree depth or panel width.

### Edit/View buttons at center top are cut
*reported 2026-04-26* — *attempted fix in working tree*: button
height is now `avail.y * 0.90` (90% of the Mode panel's content
region) instead of the fixed `GetFrameHeight`, so the buttons can
never exceed the strip and get vertically clipped. Centering math
unchanged.

### TODO: licensing / format ownership of `.csd` and `.csb`
*added 2026-04-27*.

Verify whether the `.csd` (XML) and `.csb` (FlatBuffers) formats — and
their associated FlatBuffers schemas — are proprietary to Chukong /
Cocos. If they are, decide whether we keep using the cocostudio
extension that the Cocos team published (and inherit whatever licence
it ships under) or whether we need to fork / re-implement our own
schema + loader so OpenCocosStudio can ship under its own terms. The
axmol `extensions/cocostudio` tree is the current dependency surface
for `CSLoader::createNode` and `FlatBuffersSerialize::serializeFlatBuffersWithXMLBuffer`,
so any swap touches every code path that round-trips a scene.

### `refreshAxFromCsd` fallback drift
*latent* — only triggers when no host hook is registered.

`Editor::refreshAxFromCsd` falls back to `rebuildAxMapsAndSync` when the
host hasn't installed a `RefreshAxRootFn`. The fallback inherits the
truncating-parallel-walk drift it was supposed to replace: structural
csd mutations leave orphan ax nodes (delete) or invisible new ones
(add). The host (`AppDelegate`) does install the hook, so production
behaviour is fine — but anyone embedding the editor without the hook
will hit this. Either document it as a hard requirement or have the
fallback walk-and-prune.

## Closed

### Retina ↔ non-Retina drag shrinks/grows panels by FbScale
*resolved 2026-04-27* — user-confirmed.

Dragging the window between Retina (FbScale=2) and non-Retina
(FbScale=1) displays caused panels to render at 1/4 the size
(or 4× larger going the other way) until any other window
event (e.g. a 1px manual resize) triggered ImGui's dock layout
to recompute.

Diagnosis (via per-frame `io.DisplaySize` / `FramebufferScale`
tracing):
1. macOS preserves window size in *points* across the swap, so
   `glfwGetWindowSize` returns the same value on both displays.
2. ImGui's `imgui_impl_glfw.cpp:1050` writes `DisplaySize` from
   `glfwGetWindowSize` (points) and `DisplayFramebufferScale`
   from the framebuffer/window ratio. After the swap,
   `DisplaySize` is unchanged and only `FbScale` flips 2 ↔ 1.
3. ImGui's per-frame `DockNodeUpdate` short-circuits when the
   dockspace size hasn't changed, so the `FbScale`-only flip
   never re-runs the layout pass — leaves keep their old
   point sizes but render at the new physical-pixel density.

Two failed attempts before the fix:
- `requestPanelScale(ratio)` that multiplied each dock-leaf
  size by `prev/xs`. Compounded badly on tab-stacked panels
  (shared dock node visited multiple times) and was solving
  the wrong problem.
- `DockBuilderSetNodeSize(dockspace, current_size)` and a
  ±1 px in-ImGui nudge. `DockSpaceOverViewport` re-applies
  `viewport->Size` every frame, stomping the nudge before
  ImGui's layout pass sees it.

Fix (in AppDelegate's `windowContentScale` callback): defer
two real `glfwSetWindowSize` calls via
`Scheduler::runOnAxmolThread`. Frame N+1: `glfwSetWindowSize(
w, h+1)` — fires the full GLFW size pipeline, ImGui's
NewFrame reads the new DisplaySize, dock layout recomputes.
Frame N+2 (queued from inside the first deferred call):
`glfwSetWindowSize(w, h)` — restores the user's intended
size, ImGui recomputes again with proportional leaf
distribution. Calling `glfwSetWindowSize` directly from the
GLFW callback re-enters `ImGui::NewFrame` and asserts; the
two-frame defer keeps both calls outside any active ImGui
frame.

### Black screen + graph panel cut
*resolved 2026-04-27* — not reproduced.

Reported once on an early build, before the LayerSide/LayerTop
overlay was ported from ImGui drawlist to a per-node `_overlayDraw`
DrawNode. User confirmed they have not seen the symptom in
extensive subsequent testing across many builds in this session,
so the report is treated as fixed-in-passing (likely by one of
the rendering / refresh changes since `d05c586`).

### App becomes unresponsive after several screen swaps
*resolved 2026-04-27* — user-confirmed.

Two amplifiers compounded during a multi-monitor drag:
1. The `EVENT_WINDOW_RESIZED` listener called `Director::drawScene()`
   inside its own callback (rate-limited 16 ms, guarded by an
   atomic). On a screen swap GLFW emits a burst of size + framebuffer
   events; each `drawScene` polled pending GLFW events that re-fired
   the listener, starving the main loop even though no single call
   recursed.
2. New Trace logging on `EVENT_WINDOW_POSITIONED` fired on every
   GLFW position event during a drag — each call locked the global
   Log mutex and fanned out to all sinks, while the Messages panel
   re-copied the entire ring every frame.

Fix: drop the in-listener `drawScene` (the per-frame render keeps
the canvas live anyway), throttle the position-event Trace log to
≥100 ms intervals, cap the panel's per-frame ring snapshot at the
newest 200 records.

### Canvas controls: touch broken
*resolved 2026-04-27* — user-confirmed.

Two compounding bugs in `worldToScreen` / `screenToWorld`:
1. The math composed `gTopRoot->convertToNodeSpace` on top of the
   view's design-to-pixel scale, double-counting the canvas zoom
   whenever zoom != 1. Click rects landed off by the zoom factor.
2. The result was divided by `DisplayFramebufferScale` even though
   axmol's `view->getFrameSize()` already reports OS points on
   macOS — that halved every screen coord on Retina, making clicks
   "read at -50%".

Fix: feed scene-world coords straight through the viewport affine
without composing topRoot or applying fbs. Same change in the
inspect server's `hit/click/hover` handler. New recursive top-down
hit-test (calls `sortAllChildren` per level) so explicit
`localZOrder` overrides resolve descendants-first correctly.
