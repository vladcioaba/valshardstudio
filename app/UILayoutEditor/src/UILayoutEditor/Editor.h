// UILayoutEditor::Editor — owns the open .csd document + editor state and
// renders the three ImGui panels (scene tree, properties, asset browser)
// each frame.
//
// The host app wires a single instance: construct it with a .csd path, then
// pass its `render()` method to ImGuiPresenter::addRenderLoop. Everything
// else — selection, document mutation, asset scanning — stays inside.
//
// The editor does NOT own the axmol scene. The host app loads the .csb via
// CSLoader separately; the editor works on the .csd XML model. Phase 3 will
// link the two so selection highlights the axmol node.

#pragma once

#include "UILayoutEditor/CsdModel.h"
#include "UILayoutEditor/InspectServer.h"

#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

#include <functional>

namespace ax { class Node; class DrawNode; }

namespace opencs
{

/// Native-file-dialog integration point. The host app registers a picker at
/// startup (on macOS an NSOpenPanel wrapper lives in NativeMenu.mm); panels
/// that need to ask the user for a path — e.g. asset Add — call pickImage().
/// Returns empty string if no picker is registered or the user cancelled.
using PickImageFn = std::function<std::string()>;
void setImagePicker(PickImageFn fn);

// Host-installed hook for "detach this panel into its own OS window".
// Called by the tiny ↗ button rendered at the top of each panel body.
// The host (AppDelegate) is responsible for spawning the detached
// process and hiding the local copy.
//   name = "Scene" | "Assets" | "Properties" | "Layout" | "Mode" | "Messages"
//   widthPx / heightPx = the panel's current ImGui footprint in points,
//                        used by the spawned process to size its OS window.
using DetachPanelFn =
    std::function<void(const std::string& name, int widthPx, int heightPx)>;
void setDetachPanelFn(DetachPanelFn fn);
std::string pickImage();

}  // namespace opencs

namespace opencs
{

/// Kind of node in the Assets-panel tree.
///   File       - loose raw image (.png/.jpg/…) on disk
///   Folder     - real filesystem subdirectory under Resources/
///   Atlas      - pseudo-folder: a .plist + .png pair, children are frames
///   AtlasFrame - one sprite frame inside an Atlas (no standalone file)
enum class AssetKind { File, Folder, Atlas, AtlasFrame };

/// Sprite-search result row produced by a host-installed
/// `SpriteSearchProvider`. Kept at namespace scope so Editor's
/// member fields can use it before the inline class block defines it.
struct SpriteResult
{
    std::string label;
    std::string thumbUrl;       // small preview image (optional)
    std::string downloadUrl;    // full-resolution image
    std::string provider;       // human-readable source name
};

struct AssetEntry
{
    AssetKind kind = AssetKind::File;

    /// File / Atlas:   absolute path to the .png / .plist on disk.
    /// Folder:         absolute path to the subdirectory.
    /// AtlasFrame:     absolute path to the atlas .png (for thumbnail crop).
    std::filesystem::path absPath;

    /// Display text. Files / folders / atlases: basename (leaf). Frames:
    /// sprite-frame name from the plist (e.g. "card_back.png").
    std::string displayName;

    /// Only meaningful for File. Folder/Atlas aggregate size can be
    /// computed lazily by the panel if needed.
    std::uintmax_t sizeBytes = 0;

    /// For Atlas entries: absolute path to the sibling .plist. For every
    /// other kind: empty.
    std::filesystem::path atlasPlistPath;

    /// Children — populated for Folder and Atlas. Other kinds stay empty.
    std::vector<AssetEntry> children;
};

class Editor
{
public:
    /// Opens the .csd and scans for asset files under a guessed Resources dir.
    /// If the .csd can't be opened, the editor still runs in an empty state
    /// so the panels render and show the error.
    explicit Editor(const std::filesystem::path& csdPath);
    ~Editor();

    /// Draw all three panels. Call inside an ImGuiPresenter render loop.
    void render();

    // Accessors used by the panel free-functions in panels/*.
    CsdDocument* doc() { return _doc.get(); }
    Node selected() const { return _selected; }

    /// Which of the two parallel selection channels was touched most
    /// recently. The Properties panel uses this to decide whether to
    /// render node-editing controls or the asset preview when both a
    /// node and an asset happen to be selected — last click wins.
    enum class LastTouched { None, Asset, Node };
    LastTouched lastTouched() const { return _lastTouched; }
    /// Full selection set. Empty when nothing is selected; otherwise
    /// contains _selected plus zero or more extras from shift-click or
    /// rubber-band marquee. Single-selection-style operations (properties
    /// panel, rename) keep using `selected()`; set-based operations
    /// (group translate, multi-delete) iterate this vector.
    const std::vector<Node>& selection() const { return _selection; }
    void setSelected(Node n)
    {
        if (n.element() != _selected.element()) _selectionChanged = true;
        _selected = n;
        _selection.clear();
        if (n.valid())
        {
            _selection.push_back(n);
            _lastTouched = LastTouched::Node;
        }
    }
    /// Add `n` to the selection if it's not already there (and make it the
    /// new primary). No-op for an invalid node.
    void addToSelection(Node n);
    /// Toggle membership: shift-click behavior. If `n` is already selected
    /// it's removed; otherwise added and made primary.
    void toggleSelected(Node n);
    /// Clear both `_selected` and `_selection`.
    void clearSelection();
    bool isSelected(const Node& n) const;

    const std::filesystem::path& selectedAsset() const { return _selectedAsset; }
    void setSelectedAsset(const std::filesystem::path& p)
    {
        _selectedAsset = p;
        _selectedAssets.clear();
        if (!p.empty())
        {
            _selectedAssets.push_back(p);
            _lastTouched = LastTouched::Asset;
        }
    }
    /// Multi-select set of asset paths. `selectedAsset()` is always the
    /// primary (most-recent) member. Shift/Cmd-click in the panel toggles
    /// membership; plain click resets to {p}.
    const std::vector<std::filesystem::path>& selectedAssets() const
    { return _selectedAssets; }
    /// Replace the multi-selection set. Primary becomes `paths.back()`
    /// (matches most-recent-wins in toggleSelectedAsset). Empty clears.
    void setSelectedAssetsRange(const std::vector<std::filesystem::path>& paths)
    {
        _selectedAssets = paths;
        _selectedAsset = paths.empty()
            ? std::filesystem::path{} : paths.back();
        if (!paths.empty()) _lastTouched = LastTouched::Asset;
    }
    void toggleSelectedAsset(const std::filesystem::path& p)
    {
        if (p.empty()) return;
        _lastTouched = LastTouched::Asset;
        for (auto it = _selectedAssets.begin();
             it != _selectedAssets.end(); ++it)
        {
            if (*it == p)
            {
                _selectedAssets.erase(it);
                _selectedAsset = _selectedAssets.empty()
                    ? std::filesystem::path{}
                    : _selectedAssets.back();
                return;
            }
        }
        _selectedAssets.push_back(p);
        _selectedAsset = p;
    }
    /// Batch-pack `files` (each an absolute PNG path) into `<outDir>/<name>.png`
    /// + `<outDir>/<name>.plist`. Stages to a temp dir, runs TexturePacker
    /// via the existing folder path, moves the result. Returns empty string
    /// on success, otherwise an error message. Blocking — caller is expected
    /// to run it on a worker thread.
    std::string packFilesIntoAtlas(
        const std::vector<std::filesystem::path>& files,
        const std::string& atlasName,
        const std::filesystem::path& outDir);

    /// Return all csd nodes whose attributes reference an asset filename
    /// (basename match — e.g. "card_back.png" anywhere in an attribute value
    /// will count, whether the Path is "Resources/card_back.png" or just the
    /// filename). Walks the whole document.
    std::vector<Node> findAssetReferences(const std::string& basename) const;

    /// True exactly once per render for the frame in which the selection
    /// changed — panels read this to e.g. scroll to the new selection.
    bool selectionChanged() const { return _selectionChanged; }
    /// True the next time the Scene tree panel renders after a
    /// selection change. Survives across the end-of-render reset that
    /// drops `_selectionChanged` — necessary because canvas clicks
    /// fire AFTER the Scene panel has already rendered this frame, so
    /// the change would never reach the tree without a separate flag.
    /// Cleared by the panel via `consumeSelectionScroll`.
    bool selectionScrollRequested() const { return _selectionScrollRequested; }
    void consumeSelectionScroll() { _selectionScrollRequested = false; }

    const std::vector<AssetEntry>& assets() const { return _assets; }
    /// Absolute path of the Resources/ root the current scan keyed on.
    /// The Assets panel uses it as the drop target for "Add here…" on
    /// empty/background space and as the parent for new subfolders.
    const std::filesystem::path& assetsRoot() const { return _assetsRoot; }
    /// Re-walk the Resources dir and rebuild the asset list (after add/del).
    void rescanAssets() { scanAssets(); }
    const std::filesystem::path& csdPath() const { return _csdPath; }

    /// Run TexturePacker over every .png in `folder`, producing
    /// `<folder>/<atlasName>.png` + `.plist` in the Cocos2d format. Returns
    /// a human-readable status (blank on success beyond the status msg).
    /// Binary resolved from OCS_TEXTUREPACKER env var or PATH.
    std::string packFolderWithTexturePacker(
        const std::filesystem::path& folder,
        const std::string& atlasName);

    /// Panels set these each frame so the Editor's global keyboard handler
    /// (arrow nudges) knows to keep hands off when any panel has focus —
    /// otherwise arrows move the selected node *and* navigate the panel at
    /// the same time.
    void setSceneTreeFocused(bool f)  { _sceneTreeFocused = f; }
    void setAssetPanelFocused(bool f) { _assetPanelFocused = f; }
    void setPropertiesFocused(bool f) { _propertiesFocused = f; }
    const std::string& loadError() const { return _loadError; }

    /// File-menu actions. Saves/reloads the document in-place.
    void saveToCurrent();
    void saveAs(const std::filesystem::path& newPath);

    /// Replace the currently open document with the .csd at `p`. Mirrors
    /// the `reload` inspect command: re-reads from disk, clears
    /// undo/redo, drops the autosave sidecar, and asks the host to
    /// rebuild the ax tree. Pushes `p` onto the recents list on success.
    /// Returns empty string on success, otherwise an error message
    /// suitable for the status bar.
    std::string openFromPath(const std::filesystem::path& p);

    /// Recently opened .csd files (most-recent first, capped at 10).
    /// Backed by a text file in the user's app-support dir so the list
    /// survives restarts; load happens in ctor, write happens each
    /// time the list changes.
    const std::vector<std::filesystem::path>& recents() const { return _recents; }

    /// Compile the current .csd to a .csb via axmol's cocostudio
    /// FlatBuffersSerialize pipeline. If `outPath` is empty the result is
    /// written next to the .csd with the same basename + .csb extension.
    /// Returns an empty string on success; otherwise the error returned by
    /// axmol's serializer (also set on `_statusMsg`).
    std::string exportCsb(const std::filesystem::path& outPath = {});

    /// Host-installed hook that rebuilds this Editor's axmol root from the
    /// current in-memory csd document and returns the new ax::Node* (or
    /// nullptr on failure). Called after structural mutations (addChildNode,
    /// deleteNode) so the canvas reflects the new tree without a full
    /// open-tab round-trip. The host is responsible for compiling the
    /// document to .csb, swapping the loadedRoot under gTopRoot, and
    /// preserving visibility. Returning nullptr leaves the previous root
    /// in place.
    using RefreshAxRootFn = std::function<ax::Node*()>;
    void setRefreshAxRootFn(RefreshAxRootFn fn) { _refreshAxRootFn = std::move(fn); }

    /// Edit vs View mode. In Edit mode the editor draws selection overlays
    /// and handles click/drag (pick / move / resize). In View mode those
    /// editor overlays are hidden and the shadow-graph drives rollover +
    /// click feedback so the scene previews the way it does in the game.
    enum class Mode { Edit, View, LayerSide, LayerTop };
    Mode mode() const { return _mode; }
    void setMode(Mode m);

private:
    /// Disable touch/interaction on every ax::ui::Widget in the scene while
    /// in Edit mode, re-enable in View. Keeps accidental button clicks from
    /// firing game handlers when the user is trying to manipulate the node.
    void applyModeToScene();
public:

    /// Apply the scene-centering offset for a new window size BEFORE axmol
    /// visits the scene this frame. Called from the EVENT_WINDOW_RESIZED
    /// listener so live-resize reshapes the canvas in-step with the panels
    /// instead of lagging by one frame (or snapping at drag end).
    void onWindowResized(float newWinW, float newWinH);

    /// Hand the Editor the CSLoader-loaded axmol root so it can match each
    /// ax::Node against a csd XML node by parallel DFS and draw bounding-box
    /// overlays + route click/drag events back to the model.
    void setAxmolRoot(ax::Node* root);

    /// Show/hide + clear this Editor's overlay DrawNode. Called from
    /// the host on tab switch: when a tab deactivates, its last-frame
    /// overlay commands (selection rects, layer projections) would
    /// otherwise keep rendering even though the editor is no longer
    /// running its render() loop. This clears them and hides the
    /// DrawNode; the next active frame shows it again via the same
    /// method.
    void setCanvasActive(bool active);

    /// Bring a panel back to its last known dock slot. If the stored
    /// DockId still points to a live dock node, the window is re-
    /// associated with it (DockBuilderDockWindow); otherwise the
    /// whole dock tree is rebuilt from defaults so the panel has a
    /// valid slot to land in. Called by the host's singleton-detach
    /// sync on the rising-edge of "was suppressed, no longer detached".
    void reattachPanel(const std::string& name);

    /// Serialize the named panel's current display state into a plain-
    /// text body suitable for publishing over IPC to a detached-panel
    /// process. Empty string on unknown / un-mirrorable panel names.
    /// Called once per frame by the host; the return value is only
    /// re-written when it actually changed.
    std::string serializePanelMirror(const std::string& name) const;

    /// Apply a write-back edit received from a detached panel. `pathStr`
    /// is the comma-joined DFS path ("0,1,3") identifying the node;
    /// `key` is either `attr.<Name>` (top-level attribute) or
    /// `sub.<Tag>.<Key>` (sub-element attribute like Position.X).
    /// Wraps the mutation in pushUndo() and triggers
    /// rebuildAxMapsAndSync() so the live scene reflects the change.
    /// Silently ignores unknown paths / keys.
    void applyMirrorEdit(const std::string& pathStr,
                         const std::string& key,
                         const std::string& value);

    /// Resolve `pathStr` (comma-joined DFS indices) into a Node and
    /// mark it as the active selection. No-op on invalid paths.
    void selectByPath(const std::string& pathStr);

    /// Range-select between two nodes in document pre-order (the
    /// order rows are emitted in the Scene tree). Used by Shift-click
    /// in the panel: every AbstractNodeData / ObjectData between
    /// `anchor` (current primary, may be invalid) and `target`
    /// becomes part of the multi-select; `target` becomes primary.
    void selectRange(const Node& anchor, const Node& target);

    /// Push a free-form info log entry into the Messages panel ring
    /// buffer. `kind` becomes the row's label (must be a static
    /// string — stored as const char* without copying); `detail`
    /// is a per-row string (e.g. "640x480" for a window resize).
    void appendInfoLog(const char* kind, const std::string& detail);

    /// True if `n` is locked (OCSLocked attr) or any ancestor in its
    /// CSD path is. Used by canvas click-pick and rubber-band commit
    /// to skip locked nodes — locks propagate down so a locked
    /// "background" group hides its whole sub-tree from selection.
    bool isEffectivelyLocked(const Node& n) const;

    /// True if `n` and every ancestor in its CSD path are visible.
    /// Mirrors axmol's runtime behaviour where a hidden parent hides
    /// its entire subtree regardless of the children's own state.
    /// Used by the Scene-tree eye icon to dim rows that the user
    /// hid implicitly via an ancestor toggle, and by the Properties
    /// panel to label that case.
    bool isEffectivelyVisible(const Node& n) const;

    /// Resnap every dock node to the current main viewport size
    /// without rebuilding the tree. Used when the OS window's point
    /// dimensions changed (DPI cross) but the user's panel layout
    /// should be preserved — `requestLayoutRebuild` would also work
    /// but would tear the layout back to defaults, losing any
    /// panel-resize the user did since startup. SetNodeSize +
    /// Finish recomputes child sizes proportionally from SizeRef.
    void resnapDockToViewport();

    /// Multiply every panel dock-node's size along its parent's
    /// split axis by `ratio`. Used on a windowContentScale change
    /// (Retina ↔ non-Retina) so panels keep their physical-pixel
    /// area on the new display: when the scale halves (Retina →
/// Force a fresh `buildDefaultLayout` pass on the next render
    /// frame. Used by the host when the backing display scale
    /// changes (Retina ↔ non-Retina) so ImGui dock-node sizes get
    /// re-derived from the viewport's current DisplaySize instead
    /// of the stale values computed at app start.
    void requestLayoutRebuild();

    // --- Undo/redo (document snapshot ring, max 50 entries) ----------------
    void pushUndo();
    void undo();
    void redo();
    bool canUndo() const { return !_undoStack.empty(); }
    bool canRedo() const { return !_redoStack.empty(); }

    // --- Crash-safe autosave sidecar ---------------------------------------
    /// Serialize the current document to `<csdPath>.1` so uncommitted
    /// edits survive a crash. Called from pushUndo on every mutation;
    /// saveToCurrent / saveAs call removeAutosave() on success to clean
    /// the sidecar up once the edits are in the real file.
    void writeAutosave() const;
    void removeAutosave() const;

    // --- Structural mutations (each pushes undo + invalidates ax maps) -----
    /// Remove `n` from its parent's <Children>. No-op if `n` is the root.
    void deleteNode(const Node& n);
    /// Deep-copy `n` and append the copy next to it under the same parent.
    /// Returns the new node (or an invalid Node if `n` is the root).
    Node duplicateNode(const Node& n);
    /// Append an empty AbstractNodeData of the given ctype under `parent`.
    /// Returns the created node.
    Node addChildNode(const Node& parent, const char* ctype = "NodeObjectData");

    /// Append an ImageViewObjectData child under `parent` referencing the
    /// given asset. `imageRelPath` is the .png path relative to the asset
    /// root (e.g. "images/ui/foo.png"). For an atlas frame, pass non-empty
    /// `plistRel` and `frameName` — the FileData becomes
    /// Type="MarkedSubImage". For a loose file, leave them empty for
    /// Type="Normal". Returns the created Node (invalid on failure).
    Node addImageChild(const Node& parent,
                       const std::string& imageRelPath,
                       const std::string& plistRel,
                       const std::string& frameName);
    /// Move `moving` to become the immediate next sibling of `target`.
    /// If both already share a parent this is a reorder; otherwise it's a
    /// reparent. No-op if `moving` is an ancestor of `target`.
    void moveNode(const Node& moving, const Node& target, bool asChild);
    /// Move `moving` to become the FIRST child of `parent`. Used by
    /// the Scene-tree "insert before first sibling" drop. Distinct
    /// from moveNode(asChild=true) which appends.
    void moveNodeAsFirstChild(const Node& moving, const Node& parent);
    /// Move `moving` to be the IMMEDIATE PREVIOUS sibling of `target`.
    /// Direct equivalent to pugi `insert_copy_before` so the editor
    /// does not rely on a prev-sibling lookup that gets confused when
    /// `moving` IS the prev sibling already.
    void moveNodeBefore(const Node& moving, const Node& target);

    /// Wrap every currently-selected node in a new empty NodeObjectData
    /// parent. Requires all selected nodes to share a common parent —
    /// otherwise the scaffolding is ambiguous and the call is a no-op.
    /// Children retain their visual positions by converting through world
    /// space into the new group's local coord frame.
    void groupSelected();

    /// Inverse of groupSelected — the currently-selected node's children
    /// reparent to its parent (order preserved, positions visually stable
    /// via world-space math), and the selected node is deleted.
    /// No-op when the selection is the root or has no children.
    void ungroupSelected();

    /// Copy csd geometry attrs (Position / Size / Scale / AnchorPoint /
    /// Rotation) for a single node onto its mapped axmol node. Panels call
    /// this after a Property-field commit so the canvas updates live.
    void syncNodeCsdToAx(const Node& n);

    /// Mapped ax::Node for a csd node, or nullptr when unmapped.
    /// Public accessor so panels can drive low-level ax-side toggles
    /// (visibility cascade etc.) without touching `_csdToAx` directly.
    ax::Node* axForCsd(const Node& n) const
    {
        if (!n.valid()) return nullptr;
        auto it = _csdToAx.find(n.element().internal_object());
        return it == _csdToAx.end() ? nullptr : it->second;
    }

    // --- Z-order --------------------------------------------------------
    int  getZOrder(const Node& n) const;
    void setZOrder(const Node& n, int z);
    void bringForward(const Node& n);
    void sendBackward(const Node& n);
    void bringToFront(const Node& n);
    void sendToBack(const Node& n);

private:
    void scanAssets();

    std::filesystem::path _csdPath;
    std::unique_ptr<CsdDocument> _doc;
    Node _selected;
    std::vector<Node> _selection;
    // Group-move: each selected node's starting parent-local (x,y) captured
    // at drag-start so the Move branch can apply the same delta to all.
    // Cleared when a drag ends.
    std::vector<std::pair<ax::Node*, std::pair<float, float>>> _dragStartPositions;
    // Rubber-band marquee. Active while the user drags on empty canvas with
    // no modifier key. Anchor stored in ImGui screen coords — hit-test
    // projects each node's world AABB through worldToScreen to avoid any
    // screen↔world round-trip drift.
    bool  _rubberActive        = false;
    float _rubberStartScreenX  = 0.0f;
    float _rubberStartScreenY  = 0.0f;
    // Additive modifier (Shift / Ctrl / Cmd) latched at mouse-down so
    // the release resolution honours whatever was held when the drag
    // began, not whatever is held on release.
    bool  _rubberAdditive      = false;
    std::vector<AssetEntry> _assets;
    std::filesystem::path _assetsRoot;
    std::filesystem::path _selectedAsset;
    std::vector<std::filesystem::path> _selectedAssets;
    LastTouched _lastTouched = LastTouched::None;
    std::string _loadError;
    std::vector<std::filesystem::path> _recents;
    static constexpr std::size_t kRecentsCap = 10;
    std::filesystem::path recentsFile() const;
    void loadRecents();
    void saveRecents() const;
    void pushRecent(const std::filesystem::path& p);
    bool _layoutBuilt = false;
    // Last non-zero ImGui DockId observed for each panel while it
    // was visible. reattachPanel() consults this to put a panel
    // back into the dock node it lived in before the hide. Cleared
    // by requestLayoutRebuild() because a fresh dock tree has
    // fresh node IDs.
    std::map<std::string, unsigned int> _lastDockIdByPanel;

// Host-installed callback for "rebuild my axmol root from current csd".
    // Null until the host (AppDelegate) registers via setRefreshAxRootFn.
    // Invoked from structural mutations so the canvas stays live.
    RefreshAxRootFn _refreshAxRootFn;

    // Cached scene-centering offset. Recomputed only when the main window
    // size changes; splitter drags leave it alone.
    // ImGui dockspace id — cached from DockSpaceOverViewport's return each
    // frame. DockBuilderGetNode / GetCentralNode require this exact ID;
    // re-deriving it by hashing "DockSpaceViewport_11111111" with GetID
    // yields the wrong ID and GetCentralNode returns null.
    unsigned int _dockspaceId = 0;
    // Selection-layer state captured once per frame by the InvisibleButton
    // covering the central canvas rect. Read by drawEditOverlay as the
    // single source of truth for "is the canvas the active input target?"
    // — replaces the old inCentral / anyHover / hoverWin gate.
    bool  _canvasHovered = false;
    bool  _canvasActive  = false;
    float _canvasRectX   = 0.0f;  // central->Pos.x (ImGui points)
    float _canvasRectY   = 0.0f;
    float _canvasRectW   = 0.0f;  // central->Size.x
    float _canvasRectH   = 0.0f;
    float _lastWinW = 0.0f;
    float _lastWinH = 0.0f;
    float _cachedSceneX = 0.0f;
    float _cachedSceneY = 0.0f;
    // Central-node rect expressed as fractions of the main viewport. Updated
    // every render pass; used by onWindowResized to predict the new central
    // rect from a new window size before ImGui has run this frame.
    float _centralFracL = 0.0f;
    float _centralFracT = 0.0f;
    float _centralFracR = 1.0f;
    float _centralFracB = 1.0f;

    // Canvas zoom slider. Default 0.68 lands the whole authored scene
    // (including panels that extend past the root's authored size —
    // e.g. panelBackground_0 in the solitaire scene measures 1500×2001
    // against a 750×1334 Layer root) inside the central dock with a
    // small margin. Combined with the 0.5 base-scale multiplier in
    // the render loop, effective scale = fitK × 0.68 × 0.5 ≈ 0.34 ×
    // fitK, which empirically fits the authored assets.
    float _sceneZoom = 1.0f;

    // Floating overlay panel positions, stored as a fraction of the
    // central dock rect ([0..1] in each axis). The zoom bar and the
    // navigator globe are repositioned every frame via
    // `central->Pos + ratio * central->Size` so that:
    //   * a main-window resize keeps the panel at its relative position
    //     instead of leaving it stranded below the new bottom edge, and
    //   * a panel dragged outside the central rect is clamped back in
    //     even when ImGui culls its window (Begin returns false).
    // -1 sentinel = uninitialised; first frame seeds the ratio from the
    // default bottom-right / bottom-left pin position.
    float _zoomBarRelX  = -1.0f;
    float _zoomBarRelY  = -1.0f;
    float _navGlobeRelX = -1.0f;
    float _navGlobeRelY = -1.0f;

    // Previous-frame central dock rect. Used to detect a window
    // resize (or panel-layout shuffle): when these differ from the
    // current central rect we re-derive each floating panel's pixel
    // pos from its stored ratio. Outside the resize edge we leave
    // ImGui's normal drag/move logic alone so the user can move the
    // window — `SetNextWindowPos` every frame would override drag.
    float _lastCentralX = 0.0f;
    float _lastCentralY = 0.0f;
    float _lastCentralW = 0.0f;
    float _lastCentralH = 0.0f;

    // Zoom value computed by the first fit-to-content pass after a
    // scene loads. Stored so `Reset view` can snap straight back to
    // it without a one-frame flicker through 1.0.
    float _initialZoom = 1.0f;

    // Pan offsets computed alongside _initialZoom — they shift
    // viewRoot.position so the content bounding-box CENTER lands at
    // the canvas center (rather than the designSize center, which
    // is what pan=0 gives). Mode switches restore these so every
    // mode starts with the scene visually centered.
    float _initialPanX = 0.0f;
    float _initialPanY = 0.0f;

    // Set at scene load; the next render pass measures the visible
    // bounding box in loadedRoot-local coords (invariant to topRoot's
    // current scale) and solves for the `_sceneZoom` that fits the
    // whole thing inside the central dock with a 5% margin. Flag is
    // cleared after the fit runs.
    bool  _needsFitToContent = true;

    // Pan: user-driven X/Y offset applied ON TOP of the auto-centered
    // position (_cachedSceneX/Y). Activated by Space + LMB drag or middle
    // mouse drag. A dedicated `_panActive` state-machine tracks a live
    // drag so delta math uses the drag-start cursor + offset.
    float _panOffsetX = 0.0f;
    float _panOffsetY = 0.0f;
    bool  _panActive        = false;
    float _panStartMouseX   = 0.0f;
    float _panStartMouseY   = 0.0f;
    float _panStartOffsetX  = 0.0f;
    float _panStartOffsetY  = 0.0f;

    // EXPERIMENT — bottom-left sliders that rotate the scene around X, Y,
    // and Z (axmol's setRotation3D) + a display-only Z spacing that
    // spreads nodes along the Z axis by their draw order for a layered
    // "exploded" look at Y ≠ 0. Self-contained: remove these fields + the
    // block in render() marked "[scene-rotation experiment]" to revert.
    float _sceneRotationXDeg = 0.0f;
    float _sceneRotationYDeg = 0.0f;
    float _sceneRotationDeg  = 0.0f;   // Z (kept for backward-compat naming)
    float _sceneZLayerSpacing = 0.0f;  // px between successive layers
    bool  _showLayerRects = true;      // Side/Top mode: toggle colored quads

    // Scene-canvas overlay: bounding-box draw + pick/drag.
    ax::Node* _axmolRoot = nullptr;
    std::unordered_map<ax::Node*, Node> _axToCsd;   // axmol -> csd xml node
    std::unordered_map<void*, ax::Node*> _csdToAx;  // csd (internal_object) -> axmol
    std::vector<ax::Node*> _axOrderDfs;             // DFS order for picking (back-to-front)
    // Axmol-native overlay DrawNode. Lives as a sibling of loadedRoot
    // under the shared top-root. Lines drawn in loadedRoot-local coords
    // render through the exact same transform chain the real content
    // does, so they stay perfectly aligned no matter what pan / zoom /
    // rotation is applied.
    ax::DrawNode* _overlayDraw = nullptr;
    enum class DragMode { None, Move, ResizeTL, ResizeTR, ResizeBR, ResizeBL,
                          Rotate, AnchorEdit };
    DragMode _dragMode = DragMode::None;
    bool _selectionChanged = false;
    // Sticky flag for the Scene tree panel auto-scroll. See
    // selectionScrollRequested()/consumeSelectionScroll() — survives
    // the end-of-render reset that clears _selectionChanged.
    bool _selectionScrollRequested = false;
    float _dragStartMouseX = 0.0f;                  // design coords
    float _dragStartMouseY = 0.0f;
    float _dragStartNodeX  = 0.0f;
    float _dragStartNodeY  = 0.0f;
    float _dragStartSizeW  = 0.0f;
    float _dragStartSizeH  = 0.0f;
    // Absolute (unsigned) scale at drag-start — used so resize-flip can
    // toggle the sign without clobbering any user-authored scale magnitude.
    float _dragStartScaleX = 1.0f;
    float _dragStartScaleY = 1.0f;
    // Screen-space sprite size at drag-start, captured ONCE so the
    // scale formula does not drift as the sprite grows during the
    // drag. Used by the Cmd/Ctrl-scale path so 1 px of mouse maps to
    // 1 px of on-screen growth of the corner being dragged.
    float _dragStartScreenSizeW = 0.0f;
    float _dragStartScreenSizeH = 0.0f;
    // Pickup offset: cursor's position at click relative to the node's
    // anchor-world-position. Used so that during a drag the anchor tracks
    // "cursor − pickup offset" instead of the cursor itself — preserves the
    // exact grab point so the node doesn't jump on the first mouse-down.
    float _dragPickupOffsetX = 0.0f;
    float _dragPickupOffsetY = 0.0f;
    // Rotate drag: angle (radians, world) from node origin to mouse at start,
    // plus the node's existing Rotation attr in degrees.
    float _dragStartAngleRad = 0.0f;
    float _dragStartRotDeg   = 0.0f;
    // Captured at click-time so toggling Ctrl mid-drag doesn't switch the
    // resize mode mid-flight. True = scale-only (Scale sub-element); false =
    // content-size resize.
    bool  _dragScaleOnly     = false;
    // Short flash shown over the orange handle right after a plain-resize
    // click on a node whose contentSize is auto-computed (Text etc.) —
    // signals to the user that Size won't change unless they hold Ctrl for
    // the scale-only path.
    double _orangeFlashUntil = 0.0;

    void drawSceneOverlay();
    void drawEditOverlay();
    /// Experimental Z-ordered projections (axmol draw order = Z implicitly).
    /// Each element becomes a colored line representing its bounding extent
    /// along one axis; lines are laid out 5 px apart horizontally.
    void drawLayerSideOverlay();   // Y extents
    void drawLayerTopOverlay();    // X extents
    void drawMenuBar();

    // UI state for File menu text-input modals (native dialogs deferred).
    bool _openDialogOpen = false;
    bool _saveAsDialogOpen = false;
    char _pathBuf[1024] = {0};
    std::string _statusMsg;   // shown briefly in the menu bar after an action

    // Undo / redo: full-document XML snapshots. Cheap for .csd-sized files
    // (tens of kB) and immune to dangling pugi::xml_node pointers.
    std::deque<std::string> _undoStack;
    std::deque<std::string> _redoStack;
    // Unbounded — every edit snapshots the full doc XML. Scenes are a few
    // tens of KB apiece so even a long editing session stays well under a
    // GB; trim if that ever becomes a concern.

    Mode _mode = Mode::Edit;

    // Dev-only command/inspect channel. Enabled if OCS_INSPECT_PORT is set
    // in the environment at startup. When OCS_INSPECT_TOKEN is also set,
    // mutation commands are gated behind `auth <token>` until the editor
    // has authed once — cheap protection if the loopback port is ever
    // exposed beyond 127.0.0.1.
    InspectServer _inspect;
    bool          _rpcAuthed = false;

    // Sprite Editor / Catalog plug-points. Typedefs (SpriteNlpHandler /
    // SpriteSearchProvider) live in the public section further down for
    // host callers; the fields use the equivalent std::function spelling
    // directly so the class definition stays declaration-order valid.
    std::function<std::string(const std::string&, Editor&)>
        _spriteNlpHandler;
    std::function<std::vector<SpriteResult>(const std::string&)>
        _spriteSearchProvider;
    // Cached dirty flag for the `watch` event channel: render() emits
    // an `EVENT dirty <0|1>` when the doc's dirty state flips.
    bool          _lastEmittedDirty = false;
    void emitEvent(const std::string& body);
    // Handler returns an immediate-reply string for the common case.
    // When `reply` is non-null AND the handler decided to defer
    // (multi-frame work like visual-preview / screenshot-then-rollback),
    // it calls `reply->defer()` to take ownership of the promise and
    // returns an empty string; the drain caller then suppresses the
    // synchronous reply so the deferred token can fulfil it later.
public:
    /// Dispatch a single RPC-style command line. Same surface used by
    /// the inspect-server drain; exposed publicly so in-process panels
    /// (Sprite Editor, Sprite Catalog) can reuse the mutation
    /// vocabulary without re-implementing it. Pass nullptr for `reply`
    /// when no async-defer is needed (the panels never defer).
    std::string handleInspectCommand(const std::string& line,
                                     InspectReply* reply = nullptr);
private:

    // Pending visual-preview job. Drives the screenshot-then-rollback
    // state machine for the `preview-visual` RPC command: snapshot +
    // apply happen on frame N, captureScreen end-of-frame callback
    // fires on frame N+1 setting `captured=true`, advanceVisualPreviews
    // (top of render N+2) rolls the doc back and fulfils the reply.
    struct VisualPreviewJob
    {
        PendingReply reply;
        std::string  outPath;
        std::string  snapXml;
        std::size_t  undoSize  = 0;
        std::size_t  redoSize  = 0;
        bool         dirtyBefore = false;
        bool         captured    = false;
        bool         rolledBack  = false;
    };
    std::vector<std::shared_ptr<VisualPreviewJob>> _visualPreviews;
    void advanceVisualPreviews();
    // Simulated mouse state for inspect-driven testing (frame-valid).
    bool  _simMouse   = false;
    float _simMouseX  = 0.0f;
    float _simMouseY  = 0.0f;
    bool  _simClick   = false;

    // Per-frame click counter — incremented once at the top of Editor::render
    // whenever ImGui::IsMouseClicked(0) fires, BEFORE any gate decisions. If
    // a user click isn't showing up in the editor's pick path, compare this
    // to the number of recorded events to see whether the click reached
    // ImGui at all vs. was blocked by our gates.
    unsigned _totalClicks = 0;
    bool _sceneTreeFocused   = false;
    bool _assetPanelFocused  = false;
    bool _propertiesFocused  = false;

    // Panel visibility — toggled from the native View menu. All panels
    // start visible; closing one via its titlebar also flips the flag.
public:
    bool _showScenePanel      = true;
    bool _showAssetsPanel     = true;
    bool _showPropertiesPanel = true;
    bool _showLayoutPanel     = true;
    bool _showModePanel       = true;
    bool _showTimelinePanel   = true;
    // Sprite Editor + Sprite Catalog default to HIDDEN. User opens
    // them from the View menu; first appearance is a floating window
    // (no DockBuilderDockWindow in buildDefaultLayout) but they remain
    // dockable, so the user can drag them into any panel region.
    bool _showSpriteEditor    = false;
    bool _showSpriteCatalog   = false;
    bool _showPreferences     = false;

    // Sprite Editor: NLP command input + last response. The text input
    // is committed via the panel; commands are dispatched through
    // `dispatchSpriteNlp` which calls the registered handler (if any)
    // or falls back to a tiny local pattern matcher that translates
    // common verbs into existing RPC commands ("hide", "red", "rotate
    // 30", "scale 1.5", etc).
    char        _spriteNlpInput[1024] = {0};
    std::string _spriteNlpReply;
    using SpriteNlpHandler =
        std::function<std::string(const std::string& prompt,
                                  Editor& ed)>;
    void setSpriteNlpHandler(SpriteNlpHandler h)
    { _spriteNlpHandler = std::move(h); }
    /// Run `prompt` through the registered NLP handler. Returns the
    /// raw reply string (empty when no handler is set + no local
    /// fallback matched).
    std::string dispatchSpriteNlp(const std::string& prompt);

    // Sprite Catalog: search query + last results from a registered
    // online search provider. Provider is a host-installed function
    // that takes a query string and returns a list of result entries
    // (label + thumbnail URL + download URL). Default: unset → the
    // Online tab shows a placeholder asking the user to configure a
    // provider. Local-folder tab always works and lists `_assets`.
    char        _spriteSearchQuery[256] = {0};
    std::vector<SpriteResult> _spriteSearchResults;
    std::string               _spriteSearchStatus;
    using SpriteSearchProvider =
        std::function<std::vector<SpriteResult>(const std::string& query)>;
    void setSpriteSearchProvider(SpriteSearchProvider p)
    { _spriteSearchProvider = std::move(p); }
    bool hasSpriteSearchProvider() const
    { return static_cast<bool>(_spriteSearchProvider); }
    void runSpriteSearch();

    // --- Timeline / animation playhead state (M1: read-only display) -----
    // Per-document frame state for the Timeline panel. Indexes the
    // master <Animation Duration="N"> range. _tlAnimName is the
    // currently selected named range from <AnimationList>; empty
    // string means "Full" (0..duration).
    int         _tlFrame    = 0;       // current playhead frame
    bool        _tlPlaying  = false;
    bool        _tlLoop     = true;
    int         _tlFps      = 60;      // playback rate (cocos default)
    std::string _tlAnimName;           // "" = Full
    double      _tlLastTickTime = 0.0; // wall-clock of previous tick
    double      _tlAccumFrames  = 0.0; // sub-frame accumulator
    int         _tlLastEvaluatedFrame = -1; // for fire-once event-frame edges
    int         _tlEvaluatorAppliedFrame = -2; // last frame evaluator wrote — gates idle re-apply

    // Selected keyframe — backs the Properties-panel keyframe editor
    // and the drag/right-click handlers in TimelinePanel. A single
    // selection (not a set) for v1; multi-select can come later.
    // Both members are pugi::xml_node which is a value-type wrapper
    // around a stable internal pointer, so they survive across edits
    // as long as the underlying element isn't removed.
    struct SelectedKeyframe
    {
        pugi::xml_node timelineEl;
        pugi::xml_node frameEl;
        bool valid() const noexcept
        {
            return !timelineEl.empty() && !frameEl.empty();
        }
    };
    SelectedKeyframe _tlSelectedKf;
    void clearSelectedKeyframe() noexcept { _tlSelectedKf = {}; }

    // --- Timeline playback (EditorTimeline.cpp) ----------------------
public:
    /// Active animation range bounds. Empty _tlAnimName == [0..duration].
    /// Otherwise looked up from <AnimationList>. Returns [0,0] when the
    /// document has no <Animation> element.
    int timelineRangeStart() const;
    int timelineRangeEnd()   const;

    /// Advance _tlFrame by the wall-clock time elapsed since the last
    /// call when _tlPlaying is true. Loops within
    /// [timelineRangeStart(), timelineRangeEnd()] if _tlLoop, else
    /// clamps to the end and clears _tlPlaying. Cheap: bails fast when
    /// not playing.
    void tickTimelinePlayback();

    /// Walk every <Timeline> in the doc, find the bracket keyframes
    /// around _tlFrame, interpolate via the before-frame's easing,
    /// and apply the result to the matching axmol node looked up by
    /// ActionTag. Skips Event-property timelines (those fire through
    /// fireEventKeyframesIfCrossed instead). No-op when _doc is null.
    void evaluateTimelineAtCurrentFrame();

    /// Iterate Event-property timelines and fire each EventFrame whose
    /// frameIndex lies in (lastEvaluated, _tlFrame]. Tracks
    /// _tlLastEvaluatedFrame so each event fires exactly once per
    /// forward crossing; backward seeks reset the marker. Recognises
    /// "goto_stop:N", "goto_play:N", and otherwise logs the value to
    /// the Messages panel as a fired event.
    void fireEventKeyframesIfCrossed();
private:
    /// Draws the Preferences modal. Called every frame; shows itself only
    /// when _showPreferences is true. Tab-switched sections list every
    /// shortcut the editor handles (read-only reference for now).
    void drawPreferencesModal();
    /// View-gizmo / trackball. Renders a wireframe sphere plus the
    /// three coloured axes (X=red, Y=green, Z=blue) in an ImGui box
    /// of `size` pixels square, and routes drag / scroll / reset-click
    /// into the scene's X / Y / Z rotation angles and zoom distance.
    void drawNavigatorGlobe(float size);

public:
    // Frame-counter diagnostics: incremented wherever drawEditOverlay enters
    // or early-returns, so the "state" inspect command can tell us whether
    // the overlay's click handler is even running.
    unsigned _editOverlayRuns   = 0;
    unsigned _editOverlayBails  = 0;
    const char* _editLastBail   = "never";

    // Diagnostic ring buffer: every time the Edit overlay observes a real
    // mouse click it records what it saw, then the "events" inspect command
    // prints the tail. Lets us verify that user clicks actually reach the
    // editor's click handler.
    struct ClickEvent
    {
        double time;
        float  x, y;
        bool   inCentral;
        bool   hoverIsPanel;
        bool   anyItemHovered;
        bool   passedGate;
        std::string hitPath;    // "" on miss
        std::string hitName;
        const char* kind;       // "move-icon", "resize-icon", "body", "gated", "none"
        std::string hoverWinName;   // ImGui window under the cursor at click
        unsigned    hoveredId = 0;  // ImGui's HoveredId at click (0 = none)
        std::string hitCtype;       // ctype of the node under the cursor (if any)
    };
    static constexpr std::size_t kEventLogCap = 32;
    std::deque<ClickEvent> _eventLog;

    // Toggleable on-canvas debug HUD. Shows live mouse/hover state and
    // a tail of the click-event log so you can see exactly why a click
    // was (or wasn't) accepted as a body-click / rubber-band start.
    // Default ON while the canvas-gate logic is being iterated; close
    // the window's [x] to hide.
    bool _showMessagesPanel = true;
    void drawMessagesPanel();
    /// Render the Messages panel body (Live state + Log + Click log).
    /// Caller is responsible for the surrounding `ImGui::Begin/End`.
    /// Used both by the docked panel (drawMessagesPanel) and by the
    /// detached-panel render path so the detached window shows the
    /// same debug viewer as the in-app version.
    void drawMessagesBody();

    /// Invalidate axmol↔csd maps (no more overlay) after a structural edit,
    /// since our maps reference pugi nodes that may have been moved/erased
    /// and our axmol tree now reflects a stale structure.
    void invalidateAxMaps();
    /// Re-walk both trees (axmol + csd) in parallel DFS and resync both the
    /// maps AND the axmol node attributes (position / size / scale / rotation
    /// / anchor) from the csd snapshot. Used after undo/redo so the live
    /// scene reflects the restored document without reloading the .csb.
    /// Assumes structural parity — safe for attribute-only undos, best-effort
    /// for structural ones (mismatched subtrees are truncated).
    void rebuildAxMapsAndSync();

    /// Structural-change resync: ask the host to rebuild this Editor's
    /// axmol root from the current csd (compile + reload), then re-walk
    /// the new tree. Used after addChildNode / deleteNode / moveNode /
    /// group / ungroup / undo so the canvas reflects the new structure
    /// without needing a tab close/reopen. Falls back to
    /// rebuildAxMapsAndSync when no host hook is registered (best-effort
    /// — trailing csd-only nodes won't render).
    void refreshAxFromCsd();

    /// Child-index path from root to `n` (empty for root, empty+invalid for
    /// not-found). Used to preserve selection across snapshot/restore.
    std::vector<std::size_t> pathTo(const Node& n) const;
    Node nodeAtPath(const std::vector<std::size_t>& path) const;
    void restoreSnapshot(const std::string& xml,
                         const std::vector<std::size_t>& selPath);
};

}  // namespace opencs
