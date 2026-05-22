# OpenCocosStudio — Progress Log

## Primary Work
Continuation of OpenCocosStudio (C++/ImGui/axmol editor). Major thread: fixing overlay-vs-scene desync — selection rectangles and projected layer quads drifting away from rendered nodes under pan/zoom/3D-rotation.

## Architecture Established
- **Shared `gTopRoot`** (plain `ax::Node`, tag `0xCADCA12A`) inserted between Scene and loadedRoot in `AppDelegate.cpp`. Carries all view transforms (pan/zoom/rotation3D) so overlay and content share one transform chain.
- **`_overlayDraw`** (`ax::DrawNode`) parented to `gTopRoot` — selection rectangles drawn in topRoot-local coords via `topRoot->convertToNodeSpace(node->convertToWorldSpace(corner))`.
- **Auto-fit-to-content** on scene load: measures union bbox in loadedRoot-local coords, sets `_sceneZoom` and caches `_initialZoom` for Reset view.
- **Constant-size handles**: `localR = kIconR / effScale` so dots stay display-pixel-constant under zoom.
- **2px rectangle outlines** via `drawSegment` with `segR = (kRectPx * 0.5) / effScale`.

## Features Added
- 3D navigator globe (wireframe sphere + X/Y/Z axis drag + zoom readout + Reset)
- Modern logger (`Log.h`/`Log.cpp`) with levels + ring/stdout/file sinks + `dumpSceneGraph`
- Inspect server commands (`scene`, `canvas`, `roots`, `diff-graph`, `log`, `graph2`, `pan`, `zoom`, `hit`, `select`)

## Last Task (just finished)
**Live ax-tree refresh on structural mutations.** The parallel csd↔ax
walk in `rebuildAxMapsAndSync` truncates at `min(axKids, csdKids)`,
so any structural mutation drifted the tree:
- `addChildNode` left the new pugi node with no ax counterpart — sprite
  invisible until tab close/reopen.
- `deleteNode` left a trailing orphan ax::Node whenever a non-trailing
  child was deleted, and shifted attribute-resync onto siblings.
- `moveNode` only mutated csd; ax tree never reparented.
- `restoreSnapshot` (undo/redo) inherited the same drift.

Now `Editor::refreshAxFromCsd()` invokes a host hook that serializes the
in-memory pugi doc to XML, compiles via
`FlatBuffersSerialize::serializeFlatBuffersWithXMLBuffer` to a
per-process temp `.csb`, reloads via `CSLoader::createNode`, and swaps
the tab's `loadedRoot` under `gTopRoot`. Pugi handles in `_selected`
survive because the doc itself isn't reparsed. Wired into addChildNode,
deleteNode, duplicateNode, moveNode, and restoreSnapshot.

Side fixes:
- `setAxmolRoot` now `removeFromParent()`s the old `_overlayDraw` so
  refresh doesn't accumulate orphan overlay DrawNodes.
- `refreshAxFromCsd` cancels `_needsFitToContent` after `setAxmolRoot`
  so the user's zoom/pan survive structural edits (initial open
  still fits).
- Earlier in this session: tear-off panel-size snapshot fix
  (`AppDelegate.cpp` — capture `Size` before `MovingWindow` flips).

## Previous Unresolved (still open)
User reported after an earlier run: **"the screen is black, the graph
panel is cut"** — couldn't be diagnosed without runtime feedback.
Pivoted from this; revisit when the user provides a screenshot or
mode-toggle repro.

## Key Files Touched
- `app/AppDelegate.cpp` — refresh callback, FBS include, tear-off snapshot
- `app/UILayoutEditor/src/UILayoutEditor/Editor.h` —
  `RefreshAxRootFn`, `refreshAxFromCsd` decl, member field
- `app/UILayoutEditor/src/UILayoutEditor/Editor.cpp` —
  `refreshAxFromCsd` impl, overlay cleanup in `setAxmolRoot`
- `app/UILayoutEditor/src/UILayoutEditor/EditorDocument.cpp` —
  swapped `rebuildAxMapsAndSync` → `refreshAxFromCsd` in structural ops
