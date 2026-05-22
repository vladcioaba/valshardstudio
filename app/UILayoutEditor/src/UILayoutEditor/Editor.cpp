#include "UILayoutEditor/Editor.h"
#include "UILayoutEditor/VectorMapBinding.h"
#include "UILayoutEditor/LitSpriteBinding.h"

#include "UILayoutEditor/Log.h"
#include "UILayoutEditor/PlistAtlas.h"
#include "UILayoutEditor/panels/Panels.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"  // DockBuilder API + central-node lookup

#include "axmol.h"
#include "ui/UIWidget.h"
#include "ui/UILayout.h"
#include "cocostudio/FlatBuffersSerialize.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>

namespace fs = std::filesystem;

namespace opencs
{

namespace
{
PickImageFn gImagePicker;   // set by the host at launch
DetachPanelFn gDetachPanelFn;
}

void setImagePicker(PickImageFn fn) { gImagePicker = std::move(fn); }
void setDetachPanelFn(DetachPanelFn fn) { gDetachPanelFn = std::move(fn); }

namespace
{
/// Render the small ↗ "detach" button in the top-right corner of the
/// current panel body. Call right after ImGui::Begin, before any
/// other body content. When clicked, invokes gDetachPanelFn with the
/// panel's current ImGui window size so the detached process can
/// open its OS window at the same footprint.
void drawPanelDetachButton(const char* panelName)
{
    if (!gDetachPanelFn) return;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float  btnW  = 22.0f;
    ImGui::SetCursorPosX(
        ImGui::GetCursorPosX() + std::max(0.0f, avail.x - btnW));
    ImGui::PushID(panelName);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 0));
    if (ImGui::SmallButton("↗"))
    {
        int w = 0, h = 0;
        if (auto* win = ImGui::FindWindowByName(panelName))
        {
            // Use the panel's actual ImGui Size — the new OS window
            // opens at exactly the footprint the user sees here.
            w = (int)win->Size.x;
            h = (int)win->Size.y;
        }
        if (w <= 0 || h <= 0) { w = 360; h = 420; }
        gDetachPanelFn(panelName, w, h);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Detach %s into its own OS window", panelName);
    ImGui::PopStyleVar();
    ImGui::PopID();
}
}  // namespace
std::string pickImage()
{
    if (!gImagePicker) return {};
    try { return gImagePicker(); } catch (...) { return {}; }
}

namespace
{
/// Walk up from the .csd to find an ancestor directory that looks like
/// the project's asset root. Tried in order:
///   1. A sibling named "Resources" / "resources" — Cocos Studio default.
///   2. A sibling named "assets" / "Assets" — common in Godot/Unity ports.
///   3. An ancestor literally NAMED one of {Resources, assets, cocos}
///      (case-insensitive). This catches projects where the .csd lives
///      under e.g. `assets/cocos/scenes/`: walking up hits `cocos` and
///      we use that as the root so images sitting under `assets/cocos/
///      images/` show up in the panel.
///   4. Fallback: the .csd's own parent directory, so at least the
///      sibling files are visible rather than an empty panel.
fs::path guessResourcesDir(const fs::path& csd)
{
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    auto isAssetishName = [&](const fs::path& dir) {
        const std::string n = lower(dir.filename().string());
        return n == "resources" || n == "assets" || n == "cocos";
    };

    std::error_code ec;
    // Pass 1: sibling named Resources / resources / assets / Assets.
    for (fs::path cur = csd.parent_path();
         !cur.empty() && cur != cur.parent_path();
         cur = cur.parent_path())
    {
        for (const char* name : {"Resources", "resources", "assets", "Assets"})
        {
            auto candidate = cur / name;
            if (fs::is_directory(candidate, ec)) return candidate;
        }
    }
    // Pass 2: ancestor literally named one of the asset-ish tokens.
    for (fs::path cur = csd.parent_path();
         !cur.empty() && cur != cur.parent_path();
         cur = cur.parent_path())
    {
        if (isAssetishName(cur) && fs::is_directory(cur, ec))
            return cur;
    }
    // Pass 3: .csd's own parent — better than an empty panel.
    fs::path parent = csd.parent_path();
    if (!parent.empty() && fs::is_directory(parent, ec)) return parent;
    return {};
}

bool isImageExtension(const fs::path& ext)
{
    auto s = ext.string();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    // .svg counts as an image asset: the Sprite Catalog + asset
    // browser surface it, the svg-bake RPC rasterizes to a sibling
    // .png at export, and the runtime loads the baked PNG.
    return s == ".png" || s == ".jpg" || s == ".jpeg" || s == ".webp" ||
           s == ".bmp" || s == ".tga" || s == ".svg";
}

/// Write an X/Y pair into a <Tag> sub-element, preserving any other attrs.
/// Defined here (forward reachable) so Editor::render's arrow-key handler
/// can call it; the mirror definition further down in the file is the body.
void writeXYSub(opencs::Node& n, const char* tag, float x, float y);

constexpr ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoCollapse;

/// Builds the initial VS-style dock layout on first run:
///   +---------+----------+---------+
///   |         |          |         |
///   | Scene   | (central |Propertie|
///   |         |  empty)  |         |
///   +---------+          +---------+
///   | Assets  |          |         |
///   +---------+----------+---------+
void buildDefaultLayout(ImGuiID dockspaceId)
{
    const ImVec2 vp = ImGui::GetMainViewport()->Size;
    OCS_LOG_TRACE("layout",
        "buildDefaultLayout: vp=%.0fx%.0f dock=0x%08X (full rebuild)",
        vp.x, vp.y, dockspaceId);
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(
        dockspaceId,
        ImGuiDockNodeFlags_PassthruCentralNode |
            ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, vp);

    // Top strip for the mode bar (Edit / View / Side / Top). Sized
    // to the button height plus exactly 2px top + 2px bottom of
    // padding (the Mode panel itself contributes that padding), so
    // nothing shows above or below the button row.
    ImGuiID topStrip, mainArea;
    ImGui::DockBuilderSplitNode(
        dockspaceId, ImGuiDir_Up, 0.06f, &topStrip, &mainArea);
    const ImVec2 vpSize = ImGui::GetMainViewport()->Size;
    // Strip = default button row + a tiny pad so the Edit / View
    // buttons (which fill 100% of the strip's content region — see
    // Mode panel render below) read as a single solid chip with no
    // visible cut at the top or bottom.
    // stripH = GetFrameHeight() / 0.8 plus 2 px breathing room —
    // smallest height where an 80%-of-content button equals the
    // natural ImGui FrameHeight (label + frame padding fit
    // without clipping), with a couple of pixels so the button
    // background isn't flush against the dock chrome.
    const float  stripH = ImGui::GetFrameHeight() / 0.84f;
    ImGui::DockBuilderSetNodeSize(topStrip, ImVec2(vpSize.x, stripH));
    ImGui::DockBuilderSetNodeSize(mainArea, ImVec2(vpSize.x, vpSize.y - stripH));
    if (auto* node = ImGui::DockBuilderGetNode(topStrip))
        node->LocalFlags |= ImGuiDockNodeFlags_NoTabBar |
                            ImGuiDockNodeFlags_NoResize |
                            ImGuiDockNodeFlags_NoDockingSplit;

    // Timeline takes the bottom 25% of mainArea, full width — animation
    // editing wants horizontal space more than anything else, so we
    // claim a strip across the entire bottom and split the upper part
    // into the usual left / central / right layout below it.
    ImGuiID timelineDock, mainTop;
    ImGui::DockBuilderSplitNode(
        mainArea, ImGuiDir_Down, 0.25f, &timelineDock, &mainTop);

    ImGuiID left, right, central;
    ImGui::DockBuilderSplitNode(
        mainTop, ImGuiDir_Left, 0.22f, &left, &central);
    ImGui::DockBuilderSplitNode(
        central, ImGuiDir_Right, 0.28f, &right, &central);

    ImGuiID bottomLeft, topLeft;
    ImGui::DockBuilderSplitNode(
        left, ImGuiDir_Down, 0.40f, &bottomLeft, &topLeft);

    ImGuiID bottomRight, topRight;
    ImGui::DockBuilderSplitNode(
        right, ImGuiDir_Down, 0.40f, &bottomRight, &topRight);

    ImGui::DockBuilderDockWindow("Mode",       topStrip);
    ImGui::DockBuilderDockWindow("Scene",      topLeft);
    ImGui::DockBuilderDockWindow("Assets",     bottomLeft);
    ImGui::DockBuilderDockWindow("Properties", topRight);
    ImGui::DockBuilderDockWindow("Layout",     bottomRight);
    // Messages shares the bottom-right slot with Layout as a tab. It's
    // a diagnostic / log view so sitting alongside Layout keeps it out
    // of the way but one click away.
    ImGui::DockBuilderDockWindow("Messages",   bottomRight);
    ImGui::DockBuilderDockWindow("Timeline",   timelineDock);

    ImGui::DockBuilderFinish(dockspaceId);
}

/// Comma-joined DFS path like "0,2,1" or "(root)". Mirrors the helper in
/// EditorInspect.cpp — the inspect channel and the overlay's click-log
/// both want the same human-readable form.
std::string pathToString(const std::vector<std::size_t>& p)
{
    if (p.empty()) return "(root)";
    std::string s;
    for (size_t i = 0; i < p.size(); ++i)
    {
        if (i) s.push_back(',');
        s += std::to_string(p[i]);
    }
    return s;
}

}  // namespace

Editor::Editor(const fs::path& csdPath) : _csdPath(csdPath)
{
    loadRecents();
    try
    {
        auto d = loadCsd(csdPath);
        _doc = std::make_unique<CsdDocument>(std::move(d));
        _selected = _doc->rootNode;
        pushRecent(csdPath);   // current file counts as most-recent
    }
    catch (const std::exception& e)
    {
        _loadError = e.what();
    }
    scanAssets();

    // Dev-only inspect channel. Start iff the port env var is set.
    if (const char* p = std::getenv("OCS_INSPECT_PORT"))
    {
        int port = std::atoi(p);
        if (port > 0) _inspect.start(port);
    }
}

Editor::~Editor() = default;

namespace
{
/// DFS walk that mirrors the real filesystem under `dir` into an AssetEntry
/// subtree. At each level we pair a `<name>.plist` with `<name>.png` when
/// PlistAtlas recognizes it as a Cocos2d texture atlas — the pair becomes
/// a single Atlas pseudo-folder with one AtlasFrame child per sprite, and
/// the raw .png is NOT emitted separately (it's reachable through the
/// atlas). Loose .png / .jpg / … files stay as File entries; real
/// subdirectories become Folder entries and recurse. Hidden files
/// (dot-prefix) and non-image leaf files are skipped.
void scanDirectory(const fs::path& dir,
                   std::vector<opencs::AssetEntry>& out)
{
    namespace fs = std::filesystem;
    using opencs::AssetEntry;
    using opencs::AssetKind;
    std::error_code ec;

    // First pass: collect sub-entries in this directory, record paired
    // plist-png bases so we can skip the .png when it's part of an atlas.
    std::vector<fs::directory_entry> subdirs;
    std::vector<fs::path>            images;
    std::vector<fs::path>            plists;
    for (auto& e : fs::directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec))
    {
        const auto& p = e.path();
        if (p.filename().string().rfind('.', 0) == 0) continue;  // hidden
        if (e.is_directory(ec))   subdirs.push_back(e);
        else if (e.is_regular_file(ec))
        {
            if (isImageExtension(p.extension())) images.push_back(p);
            else if (p.extension() == ".plist")  plists.push_back(p);
        }
    }

    // Identify atlases — a .plist whose sibling <basename>.png exists AND
    // which parses as a Cocos2d texture atlas.
    std::unordered_set<std::string> atlasPngs;  // absolute path keys
    std::vector<AssetEntry> atlasEntries;
    for (const auto& pl : plists)
    {
        auto pngCandidate = pl; pngCandidate.replace_extension(".png");
        if (!fs::exists(pngCandidate, ec)) continue;
        if (!opencs::PlistAtlas::isAtlasPlist(pl)) continue;

        AssetEntry atlas;
        atlas.kind           = AssetKind::Atlas;
        atlas.absPath        = pngCandidate;          // image file on disk
        atlas.atlasPlistPath = pl;
        atlas.displayName    = pl.stem().string();    // drop extension
        atlas.sizeBytes      = fs::file_size(pngCandidate, ec);

        for (auto& [name, frame] : opencs::PlistAtlas::listFrames(pl))
        {
            AssetEntry f;
            f.kind           = AssetKind::AtlasFrame;
            f.absPath        = pngCandidate;  // atlas png (for preview)
            f.atlasPlistPath = pl;
            f.displayName    = name;
            atlas.children.push_back(std::move(f));
        }

        atlasPngs.insert(pngCandidate.string());
        atlasEntries.push_back(std::move(atlas));
    }

    // Folders first (recurse), then atlases, then loose files. Each group
    // is sorted alphabetically so the panel has a stable order.
    std::vector<AssetEntry> folders;
    for (const auto& d : subdirs)
    {
        AssetEntry folder;
        folder.kind        = AssetKind::Folder;
        folder.absPath     = d.path();
        folder.displayName = d.path().filename().string();
        scanDirectory(d.path(), folder.children);
        // Hide empty folders — they'd clutter the tree without offering
        // any selectable content. User can still create files via the OS
        // file dialog and they'll appear on the next rescan.
        if (!folder.children.empty())
            folders.push_back(std::move(folder));
    }

    std::vector<AssetEntry> files;
    for (const auto& ip : images)
    {
        if (atlasPngs.count(ip.string())) continue;  // already in an atlas
        AssetEntry f;
        f.kind        = AssetKind::File;
        f.absPath     = ip;
        f.displayName = ip.filename().string();
        f.sizeBytes   = fs::file_size(ip, ec);
        files.push_back(std::move(f));
    }

    auto byName = [](const AssetEntry& a, const AssetEntry& b) {
        return a.displayName < b.displayName;
    };
    std::sort(folders.begin(),      folders.end(),      byName);
    std::sort(atlasEntries.begin(), atlasEntries.end(), byName);
    std::sort(files.begin(),        files.end(),        byName);

    out.reserve(folders.size() + atlasEntries.size() + files.size());
    for (auto& e : folders)      out.push_back(std::move(e));
    for (auto& e : atlasEntries) out.push_back(std::move(e));
    for (auto& e : files)        out.push_back(std::move(e));
}

}  // namespace


void Editor::scanAssets()
{
    _assets.clear();
    _assetsRoot = guessResourcesDir(_csdPath);
    if (_assetsRoot.empty()) return;
    // Stale plist entries would out-live a TexturePacker re-run — wipe
    // them so the newly-authored atlases re-parse on next tree render.
    PlistAtlas::clearCache();
    scanDirectory(_assetsRoot, _assets);
}

static void applyBlueprintStyle()
{
    ImGuiStyle& s = ImGui::GetStyle();

    // Neon-steel palette — matches the Edit / View chip + dock icon.
    // Bg tones run cool dark blue-gray (icon backdrop), title bars get
    // a brighter brushed-steel highlight, every window carries a thin
    // semi-transparent cyan border so panels read as discrete neon
    // chips against the canvas.
    const ImVec4 bgDeep   (0.040f, 0.055f, 0.075f, 1.00f);  // dock + window
    const ImVec4 bgPanel  (0.075f, 0.090f, 0.115f, 1.00f);  // panel surface
    const ImVec4 bgInput  (0.115f, 0.135f, 0.165f, 1.00f);  // text inputs / frames
    const ImVec4 bgHover  (0.160f, 0.190f, 0.225f, 1.00f);  // hover state
    const ImVec4 bgActive (0.220f, 0.260f, 0.310f, 1.00f);  // pressed state
    // Title-bar highlight: brushed steel top tone, matches the
    // gradient used inside the Edit/View buttons (78,82,92).
    const ImVec4 bgTitleHi(0.305f, 0.320f, 0.360f, 1.00f);
    // Neon border — cyan with low alpha; reads as a glow line at the
    // panel edge rather than a hard rule.
    const ImVec4 border   (0.430f, 0.920f, 1.000f, 0.45f);
    const ImVec4 accent   (0.30f, 0.78f, 0.95f, 1.00f);   // cyan, primary
    const ImVec4 accentHi (0.55f, 0.92f, 1.00f, 1.00f);   // cyan, active
    const ImVec4 accentDim(0.30f, 0.78f, 0.95f, 0.30f);   // accent fill bg
    const ImVec4 text     (0.93f, 0.95f, 0.98f, 1.00f);
    const ImVec4 textDim  (0.55f, 0.60f, 0.66f, 1.00f);

    // Geometry: tighter rounding + breathing-room padding so the UI
    // feels modern without being airy. WindowBorderSize=1.5 paints
    // the neon edge on every dockable panel.
    s.WindowRounding     = 6.0f;
    s.ChildRounding      = 4.0f;
    s.FrameRounding      = 4.0f;
    s.PopupRounding      = 6.0f;
    s.ScrollbarRounding  = 4.0f;
    s.GrabRounding       = 3.0f;
    s.TabRounding        = 4.0f;
    s.WindowBorderSize   = 1.5f;
    s.ChildBorderSize    = 1.0f;
    s.FrameBorderSize    = 0.0f;
    s.PopupBorderSize    = 1.0f;
    s.WindowPadding      = ImVec2(10.0f, 8.0f);
    s.FramePadding       = ImVec2(8.0f, 4.0f);
    s.ItemSpacing        = ImVec2(8.0f, 6.0f);
    s.ItemInnerSpacing   = ImVec2(5.0f, 4.0f);
    s.IndentSpacing      = 18.0f;
    s.ScrollbarSize      = 12.0f;
    s.GrabMinSize        = 10.0f;

    s.Colors[ImGuiCol_WindowBg]           = bgDeep;
    s.Colors[ImGuiCol_ChildBg]            = bgPanel;
    s.Colors[ImGuiCol_PopupBg]            = bgPanel;
    s.Colors[ImGuiCol_Border]             = border;
    s.Colors[ImGuiCol_BorderShadow]       = ImVec4(0,0,0,0);
    s.Colors[ImGuiCol_FrameBg]            = bgInput;
    s.Colors[ImGuiCol_FrameBgHovered]     = bgHover;
    s.Colors[ImGuiCol_FrameBgActive]      = bgActive;
    // Brushed-steel top: TitleBg darker, Active brighter so a focused
    // panel reads as the polished face under the cursor.
    s.Colors[ImGuiCol_TitleBg]            = bgInput;
    s.Colors[ImGuiCol_TitleBgActive]      = bgTitleHi;
    s.Colors[ImGuiCol_TitleBgCollapsed]   = bgDeep;
    s.Colors[ImGuiCol_MenuBarBg]          = bgPanel;
    s.Colors[ImGuiCol_ScrollbarBg]        = bgDeep;
    s.Colors[ImGuiCol_ScrollbarGrab]      = bgHover;
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = bgActive;
    s.Colors[ImGuiCol_ScrollbarGrabActive]  = accent;
    s.Colors[ImGuiCol_CheckMark]          = accent;
    s.Colors[ImGuiCol_SliderGrab]         = accent;
    s.Colors[ImGuiCol_SliderGrabActive]   = accentHi;
    s.Colors[ImGuiCol_Button]             = bgInput;
    s.Colors[ImGuiCol_ButtonHovered]      = bgHover;
    s.Colors[ImGuiCol_ButtonActive]       = accent;
    s.Colors[ImGuiCol_Header]             = bgInput;
    s.Colors[ImGuiCol_HeaderHovered]      = bgHover;
    s.Colors[ImGuiCol_HeaderActive]       = accentDim;
    s.Colors[ImGuiCol_Separator]          = border;
    s.Colors[ImGuiCol_SeparatorHovered]   = accent;
    s.Colors[ImGuiCol_SeparatorActive]    = accentHi;
    s.Colors[ImGuiCol_ResizeGrip]         = bgInput;
    s.Colors[ImGuiCol_ResizeGripHovered]  = bgHover;
    s.Colors[ImGuiCol_ResizeGripActive]   = accent;
    s.Colors[ImGuiCol_Tab]                = bgPanel;
    s.Colors[ImGuiCol_TabHovered]         = bgHover;
    s.Colors[ImGuiCol_TabSelected]        = bgInput;
    s.Colors[ImGuiCol_TabDimmed]          = bgPanel;
    s.Colors[ImGuiCol_TabDimmedSelected]  = bgPanel;
    s.Colors[ImGuiCol_DockingPreview]     = accentDim;
    s.Colors[ImGuiCol_DockingEmptyBg]     = bgDeep;
    s.Colors[ImGuiCol_Text]               = text;
    s.Colors[ImGuiCol_TextDisabled]       = textDim;
    s.Colors[ImGuiCol_TextSelectedBg]     = accentDim;
    s.Colors[ImGuiCol_TableHeaderBg]      = bgPanel;
    s.Colors[ImGuiCol_TableBorderLight]   = border;
    s.Colors[ImGuiCol_TableBorderStrong]  = border;
    s.Colors[ImGuiCol_TableRowBg]         = bgDeep;
    s.Colors[ImGuiCol_TableRowBgAlt]      = bgPanel;
    s.Colors[ImGuiCol_NavCursor]          = accent;
    s.Colors[ImGuiCol_DragDropTarget]     = accent;
}

void Editor::render()
{
    // Advance any in-flight visual-preview jobs from prior frames
    // BEFORE draining new commands. Rolling back the doc here means
    // the new frame paints the post-rollback state instead of the
    // mutated one, so the flash is bounded to ~2 frames total.
    advanceVisualPreviews();

    // Drain any queued inspect-server commands first so their effect (mode
    // flips, mock-click injection, selection changes) is visible this frame.
    // Handler may call `reply.defer()` for multi-frame work; in that case
    // we leave the reply un-sent and the editor-side state machine will
    // fulfil the PendingReply on a later frame.
    _inspect.drain([this](const std::string& line, InspectReply& reply) {
        std::string s = handleInspectCommand(line, &reply);
        if (!reply.delivered()) reply.send(std::move(s));
    });

    // Timeline playback: advance the playhead, fire any event keyframes
    // we just crossed, then push interpolated keyframe values onto the
    // live axmol tree. Cheap when no <Animation> is present.
    tickTimelinePlayback();
    fireEventKeyframesIfCrossed();
    evaluateTimelineAtCurrentFrame();

    // One-shot theme initialization.
    static bool sStyled = false;
    if (!sStyled) { applyBlueprintStyle(); sStyled = true; }

    // Docking must be enabled on the IO config. The ImGuiPresenter owns the
    // context, so the flag can only be set from here.
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Raw click counter — bumped BEFORE any gate decisions so the "mouse"
    // inspect command can answer: did a user click reach ImGui at all?
    if (ImGui::IsMouseClicked(0)) ++_totalClicks;

    // Menu bar is native (macOS NSMenu wired up by the host app). ImGui
    // main menu bar intentionally not drawn.

    // PassthruCentralNode keeps the middle region transparent so the axmol
    // scene shows through. The dockspace covers the full main viewport and
    // automatically tracks window resizes.
    const ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(
        0, ImGui::GetMainViewport(),
        ImGuiDockNodeFlags_PassthruCentralNode);
    _dockspaceId = dockspaceId;

    if (!_layoutBuilt)
    {
        // Always rebuild once per session so the default docking picks up
        // new panels (e.g., Mode bar, Layout) even when a stale .ini from
        // a prior layout is on disk.
        OCS_LOG_TRACE("layout",
            "buildDefaultLayout triggered (_layoutBuilt was false)");
        buildDefaultLayout(dockspaceId);
        _layoutBuilt = true;
    }

// Undo / redo. Cmd (macOS) or Ctrl (Win/Linux) + Z, +Shift+Z for redo.
    // Check globally — panels themselves don't need to capture typing.
    if (!io.WantTextInput)
    {
        const bool cmdOrCtrl = io.KeyCtrl || io.KeySuper;
        if (cmdOrCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
        {
            if (io.KeyShift) redo();
            else             undo();
        }
        // Ctrl+Y (Windows convention) / Cmd+Y (macOS) as an alternate redo
        // binding alongside Ctrl/Cmd+Shift+Z.
        if (cmdOrCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();

        // Cmd/Ctrl+G   → group the current selection into a new parent.
        // Cmd/Ctrl+Shift+G → ungroup the selected node (children reparent
        // up). Both preserve visual positions via world-space math in
        // groupSelected / ungroupSelected.
        if (cmdOrCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false))
        {
            if (io.KeyShift) ungroupSelected();
            else             groupSelected();
        }

        // Z-order: Cmd+] forward, Cmd+[ backward, +Shift to front / back.
        // Standard Figma / Photoshop convention. Selection must be valid
        // and not the root.
        if (cmdOrCtrl && _selected.valid() && _doc &&
            _selected.element() != _doc->rootNode.element())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false))
            {
                if (io.KeyShift) bringToFront(_selected);
                else             bringForward(_selected);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false))
            {
                if (io.KeyShift) sendToBack(_selected);
                else             sendBackward(_selected);
            }
        }

        // Tab / Shift+Tab cycles selection through all csd nodes in DFS
        // order (same order the Scene tree lists them). Skipped while any
        // text field is being typed into.
        if (_mode == Mode::Edit && _doc &&
            ImGui::IsKeyPressed(ImGuiKey_Tab, false))
        {
            auto all = _doc->allNodes();
            if (!all.empty())
            {
                int idx = -1;
                for (int i = 0; i < (int)all.size(); ++i)
                {
                    if (all[i].element() == _selected.element()) { idx = i; break; }
                }
                const int step = io.KeyShift ? -1 : 1;
                const int n    = (int)all.size();
                const int next = ((idx < 0 ? 0 : idx) + step + n) % n;
                setSelected(all[next]);
                _selectionChanged = true;
            }
        }

        // Arrow-key nudges on the selected node. Plain arrows move by 1 pt,
        // Shift+arrow by 10 pt. Skipped when any panel has focus so panel
        // navigation (tree selection, scroll, property text input) isn't
        // hijacked.
        if (_mode == Mode::Edit && _selected.valid() &&
            !_sceneTreeFocused && !_assetPanelFocused &&
            !_propertiesFocused &&
            _selected.element() != (_doc ? _doc->rootNode.element()
                                         : pugi::xml_node()))
        {
            const float step = io.KeyShift ? 10.0f : 1.0f;
            auto arrow = [&](ImGuiKey k) {
                return ImGui::IsKeyPressed(k, true);   // auto-repeat
            };
            float ddx = 0, ddy = 0;
            if (arrow(ImGuiKey_RightArrow)) ddx += step;
            if (arrow(ImGuiKey_LeftArrow))  ddx -= step;
            if (arrow(ImGuiKey_UpArrow))    ddy += step;   // ax Y grows up
            if (arrow(ImGuiKey_DownArrow))  ddy -= step;

            // Initial-press snapshot so Cmd+Z reverts this nudge. Auto-
            // repeats during a key hold don't push another snapshot.
            const bool arrowInitial =
                ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ||
                ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  false) ||
                ImGui::IsKeyPressed(ImGuiKey_UpArrow,    false) ||
                ImGui::IsKeyPressed(ImGuiKey_DownArrow,  false);
            if (arrowInitial && (ddx != 0 || ddy != 0)) pushUndo();

            if (ddx != 0 || ddy != 0)
            {
                auto it = _csdToAx.find(_selected.element().internal_object());
                // Move the selected node.
                auto ps = _selected.subAttrs("Position");
                float px = 0, py = 0;
                for (auto& [k, v] : ps) {
                    if      (k == "X") px = std::strtof(v.c_str(), nullptr);
                    else if (k == "Y") py = std::strtof(v.c_str(), nullptr);
                }
                px += ddx; py += ddy;
                if (it != _csdToAx.end() && it->second)
                    it->second->setPosition(px, py);
                writeXYSub(_selected, "Position", px, py);
                if (_doc) _doc->dirty = true;
            }
        }
    }

    // Each panel takes an `*open` pointer so clicking the titlebar X
    // flips the visibility flag the same way the View menu does.
    // The rising-edge "put the panel back in its old slot" logic
    // runs earlier (in the host's detach-sync block, via
    // Editor::reattachPanel) — by the time we're here, DockBuilder
    // has already written the dock assignment, so a plain Begin is
    // enough to place the window correctly.
    if (_showScenePanel)
    {
        if (ImGui::Begin("Scene", &_showScenePanel, kPanelFlags))
        {
            drawPanelDetachButton("Scene");
            drawSceneTreeBody(*this);
        }
        ImGui::End();
    }
    if (_showAssetsPanel)
    {
        if (ImGui::Begin("Assets", &_showAssetsPanel, kPanelFlags))
        {
            drawPanelDetachButton("Assets");
            drawAssetBrowserBody(*this);
        }
        ImGui::End();
    }
    if (_showPropertiesPanel)
    {
        if (ImGui::Begin("Properties", &_showPropertiesPanel, kPanelFlags))
        {
            drawPanelDetachButton("Properties");
            drawPropertiesBody(*this);
        }
        ImGui::End();
    }
    if (_showLayoutPanel)
    {
        if (ImGui::Begin("Layout", &_showLayoutPanel, kPanelFlags))
        {
            drawPanelDetachButton("Layout");
            drawLayoutBody(*this);
        }
        ImGui::End();
    }
    if (_showTimelinePanel)
    {
        if (ImGui::Begin("Timeline", &_showTimelinePanel, kPanelFlags))
        {
            drawPanelDetachButton("Timeline");
            drawTimelineBody(*this);
        }
        ImGui::End();
    }
    // Sprite Editor + Sprite Catalog: not docked by default
    // (`buildDefaultLayout` doesn't register them), so first
    // appearance is a floating window. ImGui's dock semantics still
    // apply, so the user can drag them into any panel region.
    if (_showSpriteEditor)
    {
        ImGui::SetNextWindowSize(ImVec2(420, 280), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Sprite Editor", &_showSpriteEditor, kPanelFlags))
            drawSpriteEditorBody(*this);
        ImGui::End();
    }
    if (_showSpriteCatalog)
    {
        ImGui::SetNextWindowSize(ImVec2(460, 360), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Sprite Catalog", &_showSpriteCatalog, kPanelFlags))
            drawSpriteCatalogBody(*this);
        ImGui::End();
    }
    if (_showMessagesPanel) drawMessagesPanel();

    // Capture the current DockId of each visible panel so the host's
    // reattachPanel() can later restore it on a hide→show transition.
    auto recordDock = [&](const char* name, bool shown) {
        if (!shown) return;
        if (auto* win = ImGui::FindWindowByName(name))
            if (win->DockId != 0)
                _lastDockIdByPanel[name] = win->DockId;
    };
    recordDock("Scene",      _showScenePanel);
    recordDock("Assets",     _showAssetsPanel);
    recordDock("Properties", _showPropertiesPanel);
    recordDock("Layout",     _showLayoutPanel);
    recordDock("Messages",   _showMessagesPanel);
    recordDock("Timeline",   _showTimelinePanel);

    // Mode bar — a proper docked panel in the top strip, completely
    // outside the canvas so nothing overlays the scene. Tight vertical
    // padding so the strip hugs the button row — buildDefaultLayout
    // already sized the topStrip to GetFrameHeight()+4 for this.
    if (_showModePanel)
    {
        // Zero vertical padding so the buttons can fill the strip's
        // content region corner-to-corner (the strip is 20% shorter
        // than the default button row — see buildDefaultLayout).
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 0));
        if (ImGui::Begin("Mode", &_showModePanel,
                         kPanelFlags | ImGuiWindowFlags_NoTitleBar))
        {
            constexpr float kBtnW = 80.0f;
            constexpr float kGap  = 4.0f;
            // Only Edit + View live up here now — Side and Top are
            // orientation presets, so they moved onto the globe
            // widget next to the rotation gizmo they control.
            constexpr float kBar  = kBtnW * 2 + kGap * 1;

            // No explicit button height — ImGui sizes it from the
            // current FrameHeight (label + frame padding).
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            if (avail.x > kBar)
                ImGui::SetCursorPosX(
                    ImGui::GetCursorPosX() + (avail.x - kBar) * 0.5f);
            // Centre the chip vertically in the strip so the neon ring
            // has equal clearance top and bottom — clear of the dock
            // divider above and the canvas top below.
            const float chipTargetH = ImGui::GetFontSize() * 1.6f;
            ImGui::SetCursorPosY(
                ImGui::GetCursorPosY() +
                std::max(0.0f, (avail.y - chipTargetH) * 0.5f));

            // Mode button: flat gray field, neon-cyan ring + neon
            // text when active, plain white text when inactive. No
            // gradients, no scan lines — keep it readable at a
            // glance.
            const float btnH = chipTargetH;
            auto modeButton = [&](const char* label, bool active) -> bool
            {
                ImGui::PushID(label);
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const ImVec2 p1(p0.x + kBtnW, p0.y + btnH);
                ImGui::InvisibleButton(
                    "##mode", ImVec2(kBtnW, btnH));
                const bool clicked = ImGui::IsItemClicked();
                const bool hovered = ImGui::IsItemHovered();
                const bool held    = ImGui::IsItemActive();

                ImDrawList* dl = ImGui::GetWindowDrawList();

                // Flat gray field. Held darkens slightly; hover
                // brightens by the same amount so the chip
                // responds to the cursor without visual noise.
                int base = 68;
                if (held)    base = 55;
                else if (hovered) base = 80;
                dl->AddRectFilled(p0, p1,
                    IM_COL32(base, base + 2, base + 6, 255), 3.0f);

                // Neon-cyan ring on active only. Inactive chip is
                // just the gray field with a single 1 px dark edge
                // so it still reads as a button.
                const ImVec2 rA(p0.x + 1.0f, p0.y + 1.0f);
                const ImVec2 rB(p1.x - 1.0f, p1.y - 1.0f);
                if (active)
                {
                    dl->AddRect(rA, rB,
                        IM_COL32(70, 200, 240, 80),  3.0f, 0, 3.0f);
                    dl->AddRect(rA, rB,
                        IM_COL32(110, 235, 255, 255), 3.0f, 0, 1.5f);
                }
                else
                {
                    dl->AddRect(rA, rB,
                        IM_COL32(0, 0, 0, 90), 3.0f, 0, 1.0f);
                }

                // Centred label. Two-stamp horizontal thicken so the
                // text reads as bold without a bundled bold face.
                // Active = neon cyan; inactive = white; hover keeps
                // the inactive state but brightens by 10 %.
                const ImVec2 ts = ImGui::CalcTextSize(label);
                const float  tx = std::floor(p0.x + (kBtnW - ts.x) * 0.5f);
                const float  ty = std::floor(p0.y + (btnH  - ts.y) * 0.5f);
                const ImU32 ink = active
                    ? IM_COL32(110, 235, 255, 255)
                    : (hovered ? IM_COL32(255, 255, 255, 255)
                               : IM_COL32(225, 230, 235, 255));
                dl->AddText(ImVec2(tx,        ty), ink, label);
                dl->AddText(ImVec2(tx + 1.0f, ty), ink, label);

                ImGui::PopID();
                return clicked;
            };
            if (modeButton("Edit", _mode == Mode::Edit)) setMode(Mode::Edit);
            ImGui::SameLine(0.0f, kGap);
            if (modeButton("View", _mode == Mode::View)) setMode(Mode::View);
        }
        ImGui::End();
        ImGui::PopStyleVar();        // WindowPadding
    }

    // Center the rendered scene inside the dockspace's central node by
    // applying a position offset to the running scene. Shift equals
    // (centralCenter - windowCenter) converted from points to design units.
    //
    // Recomputed every frame: on macOS live window resize, glfw framebuffer
    // events can batch and the DisplaySize-change gate would miss per-frame
    // updates, leaving the canvas visibly lagging until the drag ends. The
    // cost is trivial. Splitter-drag-induced canvas shift is acceptable — the
    // user explicitly asked for per-frame reshape.
    // 5% padding top+bottom so the canvas has breathing room from the
    // splitters and the Zoom bar on the bottom-right. The mode bar lives
    // in its own top dock panel, so no toolbar reservation is needed.
    if (auto* central = ImGui::DockBuilderGetCentralNode(dockspaceId))
    {
        const float padY = std::max(8.0f, central->Size.y * 0.05f);
        const ImVec2 cp(central->Pos.x,
                        central->Pos.y + padY);
        const ImVec2 cs(central->Size.x,
                        std::max(1.0f, central->Size.y - padY * 2.0f));
        if (cs.x > 1.0f && cs.y > 1.0f &&
            io.DisplaySize.x > 1.0f && io.DisplaySize.y > 1.0f)
        {
            auto* view = ax::Director::getInstance()->getRenderView();
            const float scaleX = view->getScaleX();  // points per design-px
            const float scaleY = view->getScaleY();

            // ImGui uses top-left origin; axmol (GL) is bottom-left.
            const float dx = (cp.x + cs.x * 0.5f) - io.DisplaySize.x * 0.5f;
            const float dy = io.DisplaySize.y * 0.5f - (cp.y + cs.y * 0.5f);

            // dx/dy are in points (DisplaySize units); scaleX is
            // points-per-design-px, so dx/scaleX = design-px offset.
            // No FbScale composition (axmol's scale is already
            // points-based on macOS).
            _cachedSceneX = dx / scaleX;
            _cachedSceneY = dy / scaleY;
            _lastWinW = io.DisplaySize.x;
            _lastWinH = io.DisplaySize.y;

            // Remember how the central node sits inside the main viewport as
            // a fraction, so EVENT_WINDOW_RESIZED can predict the new central
            // rect before ImGui runs this frame.
            _centralFracL =  cp.x                 / io.DisplaySize.x;
            _centralFracT =  cp.y                 / io.DisplaySize.y;
            _centralFracR = (cp.x + cs.x)         / io.DisplaySize.x;
            _centralFracB = (cp.y + cs.y)         / io.DisplaySize.y;
        }
    }

    if (auto* scene = ax::Director::getInstance()->getRunningScene())
    {
        // The wrapper node that carries zoom/pan/rotation. AppDelegate
        ax::Node* panNode = scene->getChildByTag(0xCADCA12A);
        // Auto-fit scale: blow up the scene so the authored design resolution
        // fills the central dock instead of leaving the window-level letter-
        // box bars inside our panels. fitK=1 means "scene fills central";
        // _sceneZoom multiplies that so the slider / Ctrl+wheel still work
        // as expected (1.0x = auto-fit, 2.0x = double auto-fit).
        float fitK = 1.0f;
        if (auto* central = ImGui::DockBuilderGetCentralNode(dockspaceId))
        {
            auto* rv  = ax::Director::getInstance()->getRenderView();
            const float sx  = rv->getScaleX();
            const float sy  = rv->getScaleY();
            if (sx > 0 && sy > 0)
            {
                // axmol's _screenSize is in *points* on macOS (set
                // from glfwGetWindowSize), so sx/sy are already
                // points-per-design-pixel. central->Size is also in
                // points. No FbScale composition needed — that was
                // double-counting DPI on Retina (fitK 2× too big).
                const float centralWDesign = central->Size.x / sx;
                const float centralHDesign = central->Size.y / sy;

                // Read the scene's authored design size from the root node.
                float designW = centralWDesign;
                float designH = centralHDesign;
                if (_doc && _doc->rootNode.valid())
                {
                    for (const auto& [k, v] :
                         _doc->rootNode.subAttrs("Size"))
                    {
                        if      (k == "X") designW = std::strtof(v.c_str(), nullptr);
                        else if (k == "Y") designH = std::strtof(v.c_str(), nullptr);
                    }
                }
                if (designW > 1 && designH > 1)
                {
                    // Leave a little breathing room so the canvas doesn't
                    // touch the splitters.
                    fitK = std::min(centralWDesign / designW,
                                    centralHDesign / designH) * 0.95f;
                    if (fitK <= 0.01f) fitK = 1.0f;
                }
            }
        }

        // Apply view transforms to the shared top-root (a plain Node
        // under the Scene) instead of the Scene itself. AppDelegate
        // parents both loadedRoot AND the overlay DrawNode under it,
        // so any pan / zoom / rotation3D propagates to both in
        // lockstep — no more overlay-vs-render desync when the view
        // changes. Scene itself stays identity.
        ax::Node* viewRoot = panNode ? panNode : scene;
        viewRoot->setPosition(_cachedSceneX + _panOffsetX,
                              _cachedSceneY + _panOffsetY);
        // 1.0x on the zoom slider now means "fit-to-canvas with
        // breathing room" — the auto-fit pass below computes the
        // zoom that actually fits the content bounding box and
        // stores it as `_initialZoom`; that value typically lands
        // around 1.0, so the slider reads intuitively.
        constexpr float kBaseScale = 1.0f;

        // One-time fit-to-content on scene load. We measure the bbox
        // in LOADEDROOT-LOCAL coords (not world) so the measurement
        // is invariant to topRoot's current scale — the raw authored
        // bounds come straight out. Walk every tracked descendant of
        // loadedRoot; for each, convert its four local corners
        // through the parent chain back into loadedRoot's local
        // frame, and union them.
        if (_needsFitToContent && _axmolRoot && !_axOrderDfs.empty())
        {
            float minX =  std::numeric_limits<float>::max();
            float minY =  std::numeric_limits<float>::max();
            float maxX = -std::numeric_limits<float>::max();
            float maxY = -std::numeric_limits<float>::max();
            for (ax::Node* n : _axOrderDfs)
            {
                if (!n || !n->isVisible()) continue;
                const auto cs = n->getContentSize();
                if (cs.width <= 0 || cs.height <= 0) continue;
                const ax::Vec2 localCorners[4] = {
                    ax::Vec2(0, 0),
                    ax::Vec2(cs.width, 0),
                    ax::Vec2(cs.width, cs.height),
                    ax::Vec2(0, cs.height),
                };
                for (const auto& lc : localCorners)
                {
                    const ax::Vec2 w = n->convertToWorldSpace(lc);
                    const ax::Vec2 inRoot = _axmolRoot->convertToNodeSpace(w);
                    minX = std::min(minX, inRoot.x);
                    minY = std::min(minY, inRoot.y);
                    maxX = std::max(maxX, inRoot.x);
                    maxY = std::max(maxY, inRoot.y);
                }
            }
            const float bboxW = maxX - minX;
            const float bboxH = maxY - minY;
            if (bboxW > 1.0f && bboxH > 1.0f)
            {
                auto* central = ImGui::DockBuilderGetCentralNode(dockspaceId);
                if (central)
                {
                    auto* rv    = ax::Director::getInstance()->getRenderView();
                    // points / points-per-design-px = design-px.
                    // No FbScale composition (see same fix above).
                    const float cwD = central->Size.x / rv->getScaleX();
                    const float chD = central->Size.y / rv->getScaleY();
                    const float fitScale =
                        std::min(cwD / bboxW, chD / bboxH) * 0.90f;
                    const float newZoom = fitScale /
                        std::max(0.0001f, fitK * kBaseScale);
                    _sceneZoom   = std::clamp(newZoom, 0.1f, 8.0f);
                    _initialZoom = _sceneZoom;

                    // Pan offset to recentre on the BBOX center
                    // rather than the design-size center (pan=0
                    // alone only puts designSize/2 at canvas
                    // center, which leaves off-centre content
                    // visibly offset). viewRoot.setPosition takes
                    // parent-local (design) units; a local shift
                    // of N renders as N*viewRootScale in world, so
                    // we multiply the desired local offset by the
                    // applied scale.
                    const auto cs = viewRoot->getContentSize();
                    const float appliedScale =
                        fitK * _sceneZoom * kBaseScale;
                    const float bboxCX = (minX + maxX) * 0.5f;
                    const float bboxCY = (minY + maxY) * 0.5f;
                    _panOffsetX = (cs.width  * 0.5f - bboxCX) * appliedScale;
                    _panOffsetY = (cs.height * 0.5f - bboxCY) * appliedScale;
                    _initialPanX = _panOffsetX;
                    _initialPanY = _panOffsetY;
                    OCS_LOG_INFO("fit",
                        "bbox=(%.1f..%.1f, %.1f..%.1f) size=%.1fx%.1f "
                        "canvas=%.1fx%.1f fitK=%.3f zoom=%.3f pan=(%.1f,%.1f)",
                        minX, maxX, minY, maxY, bboxW, bboxH, cwD, chD,
                        fitK, _sceneZoom, _panOffsetX, _panOffsetY);
                }
            }
            _needsFitToContent = false;
        }

        viewRoot->setScale(std::clamp(
            fitK * _sceneZoom * kBaseScale, 0.05f, 20.0f));
        // [scene-rotation experiment] — drop when reverting. setRotation3D
        // lets the canvas tilt around X / Y (perspective) in addition to
        // the usual Z spin, and the per-layer Z padding spreads children
        // along the depth axis so tilted views show distinct layers.
        viewRoot->setRotation3D(
            ax::Vec3(_sceneRotationXDeg, _sceneRotationYDeg, _sceneRotationDeg));

        const float spacing = _sceneZLayerSpacing;
        for (std::size_t i = 0; i < _axOrderDfs.size(); ++i)
        {
            ax::Node* n = _axOrderDfs[i];
            if (!n) continue;
            n->setPositionZ(spacing * static_cast<float>(i));
        }
    }

    drawSceneOverlay();

    // --- Zoom controls ----------------------------------------------------
    // 1. Ctrl + MouseWheel anywhere over the canvas → zoom in/out.
    // 2. Small floating bar in the bottom-right of the central dock with a
    //    slider + stepper buttons.
    if (auto* central = ImGui::DockBuilderGetCentralNode(dockspaceId))
    {
        // --- Selection widget (L3) --------------------------------------
        // Dedicated ImGui window covering the entire canvas rect with a
        // single InvisibleButton that owns canvas mouse input. Submitted
        // BEFORE the floating overlays (zoom bar / nav globe) so those
        // sit above it in z-order — hovering them correctly reports the
        // selection widget as not-hovered, and clicks on them don't
        // start a rubber-band.
        //
        // Widget-local coords: (0, 0) is the top-left of the canvas in
        // ImGui space; (width, height) the bottom-right. ImGui uses a
        // top-left origin, so the cursor starts at (0, 0) and the
        // InvisibleButton's size matches central->Size exactly. The
        // window auto-tracks central->Pos / Size every frame, so a host
        // window resize or panel-splitter drag instantly re-stretches
        // the selection layer to the new canvas bounds.
        _canvasRectX = central->Pos.x;
        _canvasRectY = central->Pos.y;
        _canvasRectW = central->Size.x;
        _canvasRectH = central->Size.y;
        ImGui::SetNextWindowPos(central->Pos);
        ImGui::SetNextWindowSize(central->Size);
        constexpr ImGuiWindowFlags kSelFlags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoMove       | ImGuiWindowFlags_NoResize   |
            ImGuiWindowFlags_NoScrollbar  | ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoDocking    | ImGuiWindowFlags_NoBringToFrontOnFocus;
        // Zero out padding AND border so the InvisibleButton can fill
        // the full central rect corner to corner — otherwise the
        // window's content region is smaller than central->Size by the
        // padding + border, leaving dead strips along the edges that
        // don't route clicks.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        if (ImGui::Begin("##selectionLayer", nullptr, kSelFlags))
        {
            // Anchor the cursor at (0,0) in window-local space so the
            // InvisibleButton rect starts at the window's top-left
            // corner. Size to the full content region so it extends to
            // the bottom-right corner regardless of any residual padding.
            ImGui::SetCursorPos(ImVec2(0, 0));
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            ImGui::InvisibleButton("##canvasSelect",
                                   ImVec2(std::max(avail.x, 1.0f),
                                          std::max(avail.y, 1.0f)));
            _canvasHovered = ImGui::IsItemHovered();
            _canvasActive  = ImGui::IsItemActive();
        }
        else
        {
            _canvasHovered = _canvasActive = false;
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        // ----------------------------------------------------------------

        const ImVec2 mouse = ImGui::GetMousePos();
        const bool mouseInCanvas =
            mouse.x >= central->Pos.x &&
            mouse.x <  central->Pos.x + central->Size.x &&
            mouse.y >= central->Pos.y &&
            mouse.y <  central->Pos.y + central->Size.y;
        const bool modHeld = io.KeyCtrl || io.KeySuper;
        if (mouseInCanvas && modHeld && io.MouseWheel != 0.0f)
        {
            // Exponential step: each wheel notch = ±10 % of current zoom,
            // so zoom feels symmetric in both directions.
            _sceneZoom *= std::pow(1.10f, io.MouseWheel);
            _sceneZoom = std::clamp(_sceneZoom, 0.1f, 8.0f);
        }

        // Pan: Space + LMB drag, or middle-mouse drag. Offset is applied
        // on top of the auto-centering position so a pan survives window
        // resizes and panel re-layouts. Ctrl/Cmd/Alt mouse drags are
        // reserved for node manipulation and never trigger a pan.
        const bool spaceHeld =
            !io.WantTextInput && ImGui::IsKeyDown(ImGuiKey_Space);
        const bool lmbDown  = ImGui::IsMouseDown(0);
        const bool mmbDown  = ImGui::IsMouseDown(2);
        const bool wantPan  = (spaceHeld && lmbDown) || mmbDown;

        if (wantPan && (mouseInCanvas || _panActive))
        {
            if (!_panActive)
            {
                _panActive       = true;
                _panStartMouseX  = mouse.x;
                _panStartMouseY  = mouse.y;
                _panStartOffsetX = _panOffsetX;
                _panStartOffsetY = _panOffsetY;
            }
            // Mouse delta is in points (ImGui's coord system); the
            // pan offset feeds setPosition() which lives in
            // design-px (same units as _cachedSceneX/Y, computed
            // upstream as dx/scaleX). Without dividing by scale the
            // canvas moves more than the cursor when scaleX > 1
            // (a zoomed-in or Retina view).
            auto* view = ax::Director::getInstance()->getRenderView();
            const float sX = std::max(1e-4f, view->getScaleX());
            const float sY = std::max(1e-4f, view->getScaleY());
            _panOffsetX = _panStartOffsetX + (mouse.x - _panStartMouseX) / sX;
            _panOffsetY = _panStartOffsetY - (mouse.y - _panStartMouseY) / sY;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
        else
        {
            _panActive = false;
            if (spaceHeld && mouseInCanvas)
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }

        // Zoom bar, initially pinned to the bottom-right corner of the
        // central dock but draggable. Position is stored as a fraction
        // of the central rect (see _zoomBarRel*) and re-applied every
        // frame so a main-window resize moves the panel proportionally
        // instead of stranding it below the new bottom edge.
        constexpr float kBarW = 230.0f;
        constexpr float kBarH = 36.0f;
        // NoDecoration keeps the bar compact (no title bar), but
        // leaving NoMove OFF lets ImGui drag the window from any empty
        // background region. Hovering an interactive child (slider,
        // button) still captures normally — ImGui only starts a drag
        // when the mouse-down lands on the window's background.
        constexpr ImGuiWindowFlags kBarFlags =
            ImGuiWindowFlags_NoDecoration   | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoSavedSettings|
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

        // Position a floating panel from its stored ratio (fraction of
        // the central dock rect), clamped so the entire panel stays
        // inside the rect. Returns the resolved pixel position. Caller
        // feeds the result into SetNextWindowPos before Begin, so the
        // window renders at the correct spot on the very same frame —
        // no flicker after a resize and no reliance on Begin returning
        // true to clamp.
        auto resolveFloatingPos =
            [&](const ImGuiDockNode* c, float& relX, float& relY,
                float defaultW, float defaultH,
                float defaultRelX, float defaultRelY,
                const char* winName) -> ImVec2
        {
            // Window size: prefer last-known from ImGui's internal
            // state (panel auto-resizes), fall back to caller defaults
            // on the very first frame before ImGui has measured it.
            float panelW = defaultW, panelH = defaultH;
            if (auto* win = ImGui::FindWindowByName(winName))
            {
                if (win->Size.x > 0) panelW = win->Size.x;
                if (win->Size.y > 0) panelH = win->Size.y;
            }
            if (relX < 0.0f || relY < 0.0f)
            {
                relX = defaultRelX;
                relY = defaultRelY;
            }
            // Cap ratio so panel-top-left + panel-size fits in central.
            // c->Size minus panelSize can go negative when the dock is
            // tinier than the panel — clamp to zero to avoid pinning
            // the panel above the central rect.
            const float maxRelX = c->Size.x > panelW
                ? (c->Size.x - panelW) / c->Size.x : 0.0f;
            const float maxRelY = c->Size.y > panelH
                ? (c->Size.y - panelH) / c->Size.y : 0.0f;
            relX = std::clamp(relX, 0.0f, maxRelX);
            relY = std::clamp(relY, 0.0f, maxRelY);
            return ImVec2(c->Pos.x + relX * c->Size.x,
                          c->Pos.y + relY * c->Size.y);
        };

        // After Begin/End, record the panel's current position back as
        // a ratio so a user drag this frame survives into the next.
        // Skipped when central is null or has zero area.
        auto recordFloatingRatio =
            [&](const ImGuiDockNode* c, float& relX, float& relY,
                const char* winName)
        {
            if (!c || c->Size.x <= 0.0f || c->Size.y <= 0.0f) return;
            auto* win = ImGui::FindWindowByName(winName);
            if (!win) return;
            relX = (win->Pos.x - c->Pos.x) / c->Size.x;
            relY = (win->Pos.y - c->Pos.y) / c->Size.y;
        };

        // Inside-Begin clamp: pin the panel inside the central rect.
        // Called every frame so an ImGui mouse-drag that would push
        // the window outside the dock snaps back to the boundary —
        // visually the panel sticks at the edge while the user keeps
        // dragging further. Necessary because the pre-Begin
        // `SetNextWindowPos(Always)` would otherwise fight the drag,
        // so we only set NextWindowPos on the resize edge and rely
        // on this inside-scope clamp for the drag case.
        auto clampInsideBegin =
            [&](const ImGuiDockNode* c)
        {
            if (!c) return;
            const ImVec2 sz  = ImGui::GetWindowSize();
            const ImVec2 pos = ImGui::GetWindowPos();
            // max{Pos+Size-sz, Pos} avoids std::clamp(lo>hi) UB when
            // the panel is taller / wider than the central dock.
            const float maxX =
                std::max(c->Pos.x, c->Pos.x + c->Size.x - sz.x);
            const float maxY =
                std::max(c->Pos.y, c->Pos.y + c->Size.y - sz.y);
            const ImVec2 q(std::clamp(pos.x, c->Pos.x, maxX),
                           std::clamp(pos.y, c->Pos.y, maxY));
            if (q.x != pos.x || q.y != pos.y)
                ImGui::SetWindowPos(q);
        };

        // Resize edge: central rect moved or resized since last frame.
        // Both panels reposition via SetNextWindowPos when this fires
        // — otherwise we leave the position alone so ImGui drag works.
        const bool centralChanged =
            central->Pos.x  != _lastCentralX ||
            central->Pos.y  != _lastCentralY ||
            central->Size.x != _lastCentralW ||
            central->Size.y != _lastCentralH;
        _lastCentralX = central->Pos.x;
        _lastCentralY = central->Pos.y;
        _lastCentralW = central->Size.x;
        _lastCentralH = central->Size.y;

        // Prominent accent border so floating controls read clearly
        // against the axmol canvas. Thicker still while the user is
        // holding a drag on the window so the active gesture has
        // obvious feedback.
        auto pushFloatingStyle = [](bool dragging) {
            const ImU32 col = dragging
                ? IM_COL32(150, 200, 255, 255)
                : IM_COL32(90, 140, 220, 220);
            ImGui::PushStyleColor(ImGuiCol_Border, col);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,
                                dragging ? 2.5f : 1.5f);
        };
        auto popFloatingStyle = []() {
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        };

        // Drag-eligibility latches for the two floating panels.
        // Set true the instant the user presses on the window's
        // background (no sub-item hovered), cleared on mouse
        // release. This covers the ENTIRE drag action — from
        // mouse-down, through the pre-threshold "about to drag"
        // state, all the way until the button is released —
        // rather than only the frames ImGui has flagged as an
        // actual drag. Used for border prominence.
        static bool sZoomBarDragging  = false;
        static bool sNavGlobeDragging = false;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            sZoomBarDragging  = false;
            sNavGlobeDragging = false;
        }

        // Default position: pinned to bottom-right of the central dock
        // with a 12 px margin. Expressed as a ratio so it survives the
        // first-frame seed path.
        const float zoomDefaultRelX = central->Size.x > kBarW
            ? (central->Size.x - kBarW - 12.0f) / central->Size.x : 0.0f;
        const float zoomDefaultRelY = central->Size.y > kBarH
            ? (central->Size.y - kBarH - 12.0f) / central->Size.y : 0.0f;
        // Only override pos on the resize edge (or first frame). Off
        // that edge ImGui drag handles position — pushing a window
        // pos every frame would override the drag and the panel
        // would not be movable at all.
        if (centralChanged || _zoomBarRelX < 0.0f)
        {
            const ImVec2 zoomPos = resolveFloatingPos(
                central, _zoomBarRelX, _zoomBarRelY,
                kBarW, kBarH, zoomDefaultRelX, zoomDefaultRelY,
                "##zoombar");
            ImGui::SetNextWindowPos(zoomPos, ImGuiCond_Always);
        }
        ImGui::SetNextWindowBgAlpha(0.85f);
        pushFloatingStyle(sZoomBarDragging);
        if (ImGui::Begin("##zoombar", nullptr, kBarFlags))
        {
            if (ImGui::Button("-")) _sceneZoom =
                std::clamp(_sceneZoom / 1.10f, 0.1f, 8.0f);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::SliderFloat("##zoom", &_sceneZoom, 0.1f, 4.0f, "%.2fx",
                               ImGuiSliderFlags_AlwaysClamp);
            ImGui::SameLine();
            if (ImGui::Button("+")) _sceneZoom =
                std::clamp(_sceneZoom * 1.10f, 0.1f, 8.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton("1:1")) _sceneZoom = 1.0f;
            // Latch the dragging indicator the moment the user
            // presses on empty window background (not on a
            // sub-widget). Stays latched until mouse release
            // (handled above in the shared unlatch block).
            if (ImGui::IsWindowHovered() &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                !ImGui::IsAnyItemHovered())
                sZoomBarDragging = true;
            // Mouse wheel while the cursor is over the zoom bar
            // scrolls zoom in/out without needing Ctrl. Matches the
            // behaviour users expect from clicking into a "zoom
            // widget". Same ±10% step the +/- buttons use.
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                ImGui::GetIO().MouseWheel != 0.0f)
            {
                _sceneZoom = std::clamp(
                    _sceneZoom * std::pow(1.10f,
                                          ImGui::GetIO().MouseWheel),
                    0.1f, 8.0f);
            }
            // Mid-drag clamp: pin the panel inside the central rect.
            // ImGui's MovingWindow logic happens in NewFrame before
            // user code; this clamp runs after, so a drag that would
            // push the panel off the dock snaps back to the boundary
            // every frame the cursor stays out. User has to bring
            // the cursor back inside the rect to keep dragging.
            clampInsideBegin(central);
        }
        ImGui::End();
        // Record post-drag position OUTSIDE the Begin scope so it
        // runs even when ImGui culls the window (Begin returns false)
        // — that's the case the user hit when a resize pushed the
        // panel fully below the new central rect.
        recordFloatingRatio(central, _zoomBarRelX, _zoomBarRelY, "##zoombar");
        popFloatingStyle();

        // ---------------- [scene-rotation experiment] ----------------
        // Four stacked sliders in the bottom-left: rotate the whole scene
        // around X / Y / Z via setRotation3D, plus a "Z padding" row that
        // offsets each child along Z by (index × spacing) so tilted views
        // show distinct layers. Remove this block + the _sceneRotation*Deg
        // and _sceneZLayerSpacing fields to back out.
        // ---------------- Navigator globe ----------------
        //
        // Compact 3ds-Max/Maya-style view gizmo that replaces the old
        // 4-slider bar. A wireframe sphere with three coloured axes
        // (X=red, Y=green, Z=blue) is rendered with the current scene
        // rotation. Dragging inside rotates the view; Shift+drag rolls
        // (Z-axis); scroll zooms. A reset button snaps back to the
        // straight-on orientation.
        //
        // The sphere is drawn directly on ImGui's list — no axmol
        // texture round-trip — because all we need is a UI preview,
        // not a picking surface.
        // Navigator globe + rotation sliders + Z-pad + Layer rects
        // toggle. Total height is the globe box (kGlobeSize) plus the
        // sliders / button rows (~kControlsH). Positioning from the
        // BOTTOM of the central dock so the widget bottom sits above
        // the bottom splitter and nothing gets clipped below.
        constexpr float kGlobeSize  = 120.0f;
        constexpr float kControlsH  = 170.0f;   // 3 rot sliders + zoom row
                                                // + reset btn + zpad slider
                                                // + "Layer rects" checkbox
        constexpr float kWidgetH    = kGlobeSize + kControlsH + 24.0f;
        constexpr float kWidgetW    = kGlobeSize + 24.0f;
        // Default position: bottom-left of the central dock with a
        // 12 px margin, expressed as a ratio so the resize/clamp
        // pipeline can re-derive a pixel pos every frame.
        const float globeDefaultRelX = central->Size.x > kWidgetW
            ? 12.0f / central->Size.x : 0.0f;
        const float globeDefaultRelY = central->Size.y > kWidgetH
            ? (central->Size.y - kWidgetH - 12.0f) / central->Size.y : 0.0f;
        if (centralChanged || _navGlobeRelX < 0.0f)
        {
            const ImVec2 globePos = resolveFloatingPos(
                central, _navGlobeRelX, _navGlobeRelY,
                kWidgetW, kWidgetH, globeDefaultRelX, globeDefaultRelY,
                "##navglobe");
            ImGui::SetNextWindowPos(globePos, ImGuiCond_Always);
        }
        ImGui::SetNextWindowBgAlpha(0.85f);
        pushFloatingStyle(sNavGlobeDragging);
        if (ImGui::Begin("##navglobe", nullptr, kBarFlags))
        {
            // Side/Top/Front/Back presets live as clickable cardinal
            // pins directly ON the globe now — visually obvious and
            // one click away — so the header button row is gone.
            drawNavigatorGlobe(kGlobeSize);
            // Latch the dragging indicator on mouse-down-on-bg
            // (same semantics as zoombar — see comment there).
            if (ImGui::IsWindowHovered() &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                !ImGui::IsAnyItemHovered())
                sNavGlobeDragging = true;
            clampInsideBegin(central);
        }
        ImGui::End();
        recordFloatingRatio(central, _navGlobeRelX, _navGlobeRelY,
                            "##navglobe");
        popFloatingStyle();
        // -------------------------------------------------
    }

    // Preferences modal — rendered last so it sits on top of everything.
    drawPreferencesModal();

    // RPC `watch` channel: emit unsolicited events for state the
    // agent cares about. Cheap when no subscribers — publishEvent
    // bails on an empty subscriber set.
    if (_selectionChanged)
    {
        if (_selected.valid())
        {
            auto p = pathTo(_selected);
            std::string body = "selection ";
            for (std::size_t i = 0; i < p.size(); ++i)
            { if (i) body.push_back(','); body += std::to_string(p[i]); }
            if (p.empty()) body += "(root)";
            body.push_back('\t'); body += _selected.name();
            body.push_back('\t'); body += _selected.ctype();
            emitEvent(body);
        }
        else
            emitEvent("selection (none)");
    }
    const bool nowDirty = _doc && _doc->dirty;
    if (nowDirty != _lastEmittedDirty)
    {
        emitEvent(std::string("dirty ") + (nowDirty ? "1" : "0"));
        _lastEmittedDirty = nowDirty;
    }

    // Hand off the change to the sticky scroll-request flag so the
    // Scene tree panel sees it on NEXT frame even when the change
    // came from the canvas-click handler (which runs AFTER the panel
    // rendered this frame). The panel calls consumeSelectionScroll()
    // once it has acted on the request.
    if (_selectionChanged) _selectionScrollRequested = true;
    _selectionChanged = false;
}

void Editor::drawPreferencesModal()
{
    // A one-time OpenPopup edge-trigger: we only want to pop the modal
    // when _showPreferences *goes* true, not each frame it stays true.
    static bool sPoppedOnThisOpen = false;
    if (_showPreferences && !sPoppedOnThisOpen)
    {
        ImGui::OpenPopup("Preferences");
        sPoppedOnThisOpen = true;
    }
    if (!_showPreferences) sPoppedOnThisOpen = false;

    const ImVec2 vpCenter = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(vpCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_Appearing);

    bool openFlag = _showPreferences;
    if (!ImGui::BeginPopupModal("Preferences", &openFlag,
                                ImGuiWindowFlags_NoCollapse))
    {
        _showPreferences = openFlag;
        return;
    }

    // Table-style shortcut list, grouped by section. Each group has a
    // single tab; making sections tabs (instead of one long list) keeps
    // the dialog compact and readable.
    struct Row { const char* keys; const char* what; };
    auto section = [&](const char* title,
                       std::initializer_list<Row> rows)
    {
        if (!ImGui::BeginTabItem(title)) return;
        if (ImGui::BeginTable("##kb", 2,
                              ImGuiTableFlags_Borders |
                              ImGuiTableFlags_RowBg   |
                              ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Shortcut",
                                    ImGuiTableColumnFlags_WidthFixed, 180);
            ImGui::TableSetupColumn("Action");
            ImGui::TableHeadersRow();
            for (const auto& r : rows)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(r.keys);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(r.what);
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    };

    if (ImGui::BeginTabBar("##prefsTabs"))
    {
        section("Canvas", {
            {"LMB drag body",         "Select topmost node under cursor"},
            {"Drag blue +",           "Move (Position)"},
            {"Cmd/Ctrl + drag blue +","Anchor edit — element stays, pivot follows cursor"},
            {"Drag orange ↘",         "Resize (Size attribute)"},
            {"Cmd/Ctrl + drag orange","Scale-only — Size / Position frozen, Scale grows"},
            {"Shift + drag orange",   "Aspect-lock while resizing"},
            {"Alt + drag orange",     "Rotate around the node origin"},
            {"Shift while rotating",  "Snap rotation to 15°"},
            {"Ctrl/Cmd + MouseWheel", "Zoom canvas"},
            {"Space + LMB drag",      "Pan canvas"},
            {"Middle mouse drag",     "Pan canvas"},
        });
        section("Node editing", {
            {"Arrow keys",            "Nudge selected node ±1 pt"},
            {"Shift + Arrows",        "Nudge ±10 pt"},
            {"Tab / Shift+Tab",       "Cycle selection through scene tree (DFS order)"},
            {"Delete / Backspace",    "Delete selected (tree or assets panel focused)"},
            {"Cmd/Ctrl + ]",          "Bring forward (Z-order +1)"},
            {"Cmd/Ctrl + [",          "Send backward (Z-order −1)"},
            {"Cmd/Ctrl + Shift + ]",  "Bring to front"},
            {"Cmd/Ctrl + Shift + [",  "Send to back"},
        });
        section("History", {
            {"Cmd/Ctrl + Z",          "Undo"},
            {"Cmd/Ctrl + Shift + Z",  "Redo"},
            {"Cmd/Ctrl + Y",          "Redo (alternate)"},
        });
        section("File", {
            {"Cmd + N",               "New project (native folder picker)"},
            {"Cmd + O",               "Open .csd (restart-based, Phase 2 will hot-reload)"},
            {"Cmd + S",               "Save .csd"},
            {"Cmd + Shift + S",       "Save As…"},
            {"Cmd + Shift + E",       "Export .csb"},
            {"Cmd + ,",               "Preferences"},
        });
        section("View modes", {
            {"Edit / View / Side / Top",  "Click buttons in the top Mode panel"},
            {"Side / Top",                "Scene rotated via setRotation3D + Z padding 50 px"},
            {"Layer rects checkbox",      "Toggle the colored quads over layers"},
            {"1:1 in zoom bar",           "Reset zoom to 100%"},
            {"0° per row (rotation bar)", "Reset that axis to 0°"},
        });
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120, 0)))
    {
        _showPreferences = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled(
        "Editable bindings coming in a later pass. Report shortcut "
        "ideas in the project tracker.");

    ImGui::EndPopup();
    // Sync the modal-close (via window X) back to the flag.
    if (!openFlag) _showPreferences = false;
}

void Editor::saveToCurrent()
{
    if (!_doc || _csdPath.empty()) return;
    try
    {
        opencs::saveCsd(*_doc);
        _doc->dirty = false;
        removeAutosave();   // sidecar no longer needed; edits are in the real file
        _statusMsg = "saved " + _csdPath.filename().string();
        emitEvent("saved " + _csdPath.string());
    }
    catch (const std::exception& e)
    {
        _statusMsg = std::string("save failed: ") + e.what();
    }
}

std::string Editor::exportCsb(const std::filesystem::path& outPath)
{
    namespace fs = std::filesystem;

    if (!_doc || _csdPath.empty())
    {
        _statusMsg = "export: no document loaded";
        return _statusMsg;
    }

    // Make sure disk has the latest edits — FlatBuffersSerialize reads from
    // file, not from our in-memory doc.
    try { opencs::saveCsd(*_doc); }
    catch (const std::exception& e)
    {
        _statusMsg = std::string("export: save failed: ") + e.what();
        return _statusMsg;
    }

    // Resolve output path: sibling .csb next to the .csd by default.
    fs::path target = outPath;
    if (target.empty())
    {
        target = _csdPath;
        target.replace_extension(".csb");
    }

    // Delegate the actual schema-aware serialization to axmol. The function
    // returns an empty string on success; non-empty on error.
    auto* fbs = cocostudio::FlatBuffersSerialize::getInstance();
    const std::string err = fbs->serializeFlatBuffersWithXMLFile(
        _csdPath.string(), target.string());

    if (!err.empty())
    {
        _statusMsg = std::string("export failed: ") + err;
        return err;
    }

    // Write a `<basename>.vmaps.json` sidecar listing every
    // VectorMapObjectData node + its layer data. The runtime
    // calls `ocs::ext::applyVectorMapsFromJson(scene, sidecar)`
    // after CSLoader::createNode to attach the runtime VectorMapNode
    // children. Decoupled from the .csb so we don't need to extend
    // the cocostudio flatbuffers schema.
    {
        std::ostringstream js;
        js << "{\n  \"scene\":\""
           << target.stem().string() << "\",\n  \"vmaps\":[";
        bool first = true;
        std::function<void(const Node&, const std::string&)> walk =
            [&](const Node& n, const std::string& parentPath)
        {
            const std::string namePart = n.name();
            const std::string here = parentPath.empty()
                ? namePart : (parentPath + "/" + namePart);
            if (n.ctype() == "VectorMapObjectData")
            {
                if (!first) js << ',';
                first = false;
                js << "\n    {\"path\":\"" << here
                   << "\",\"name\":\"" << namePart
                   << "\",\"layers\":[";
                auto vmd = n.element().child("VectorMapData");
                bool firstL = true;
                for (auto layerEl = vmd.child("Layer"); layerEl;
                     layerEl = layerEl.next_sibling("Layer"))
                {
                    if (!firstL) js << ',';
                    firstL = false;
                    const std::string lname(layerEl.attribute("Name").as_string(""));
                    const std::string vis(
                        layerEl.attribute("Visible").as_string("True"));
                    js << "{\"name\":\"" << lname
                       << "\",\"visible\":"
                       << ((vis == "False") ? "false" : "true")
                       << ",\"primitives\":[";
                    bool firstP = true;
                    for (auto p = layerEl.first_child(); p;
                         p = p.next_sibling())
                    {
                        const std::string tag(p.name());
                        const char* kind = nullptr;
                        if (tag == "Polygon")  kind = "Polygon";
                        else if (tag == "Polyline") kind = "Polyline";
                        else if (tag == "Circle")   kind = "Circle";
                        else continue;
                        if (!firstP) js << ',';
                        firstP = false;
                        js << "{\"kind\":\"" << kind << "\"";
                        auto colorArr = [&](const char* attr,
                                            const char* key,
                                            const char* dflt) {
                            const std::string v(p.attribute(attr).as_string(dflt));
                            if (v.empty()) return;
                            js << ",\"" << key << "\":[";
                            std::string tok;
                            bool firstC = true;
                            auto flush = [&]() {
                                if (tok.empty()) return;
                                if (!firstC) js << ',';
                                firstC = false;
                                js << tok; tok.clear();
                            };
                            for (char c : v)
                            {
                                if (c == ',') { flush(); }
                                else if (std::isdigit((unsigned char)c) ||
                                         c == '-') tok.push_back(c);
                            }
                            flush();
                            js << ']';
                        };
                        colorArr("Fill",   "fill",   "");
                        colorArr("Stroke", "stroke", "");
                        if (p.attribute("StrokeWidth"))
                            js << ",\"strokeWidth\":"
                               << p.attribute("StrokeWidth").as_float(1.0f);
                        if (std::string(kind) == "Circle")
                        {
                            js << ",\"x\":"    << p.attribute("X").as_float(0)
                               << ",\"y\":"    << p.attribute("Y").as_float(0)
                               << ",\"radius\":"<< p.attribute("Radius").as_float(0);
                        }
                        else
                        {
                            // "x,y x,y" → [[x,y],[x,y]]
                            const std::string pts(p.attribute("Points").as_string(""));
                            js << ",\"points\":[";
                            std::string pair;
                            bool firstPt = true;
                            auto flushPt = [&]() {
                                if (pair.empty()) return;
                                const auto comma = pair.find(',');
                                if (comma != std::string::npos)
                                {
                                    if (!firstPt) js << ',';
                                    firstPt = false;
                                    js << '[' << pair.substr(0, comma) << ','
                                       << pair.substr(comma + 1) << ']';
                                }
                                pair.clear();
                            };
                            for (char c : pts)
                            {
                                if (std::isspace((unsigned char)c)) flushPt();
                                else pair.push_back(c);
                            }
                            flushPt();
                            js << ']';
                        }
                        js << '}';
                    }
                    js << "]";
                    // <Animation> block. Each keyframe becomes a row
                    // with primIdx, attr, frame, value preserved as
                    // a string (runtime parser switches by attr).
                    if (auto anim = layerEl.child("Animation"))
                    {
                        js << ",\"anim\":{";
                        js << "\"duration\":" << anim.attribute("Duration").as_int(60);
                        js << ",\"fps\":" << anim.attribute("FPS").as_int(30);
                        const std::string lp(anim.attribute("Loop").as_string("True"));
                        js << ",\"loop\":" << ((lp == "False") ? "false" : "true");
                        js << ",\"keyframes\":[";
                        bool firstK = true;
                        for (auto kf = anim.child("Keyframe"); kf;
                             kf = kf.next_sibling("Keyframe"))
                        {
                            if (!firstK) js << ',';
                            firstK = false;
                            js << "{\"frame\":" << kf.attribute("Frame").as_int(0)
                               << ",\"primIdx\":" << kf.attribute("PrimIdx").as_int(0)
                               << ",\"attr\":\""  << kf.attribute("Attr").as_string("")
                               << "\",\"value\":\"" << kf.attribute("Value").as_string("")
                               << "\"}";
                        }
                        js << "]}";
                    }
                    js << "}";
                }
                js << "]}";
            }
            for (auto& c : n.children()) walk(c, here);
        };
        walk(_doc->rootNode, std::string());
        js << "\n  ]\n}\n";
        if (!first)   // at least one VectorMap was written
        {
            fs::path side = target;
            side.replace_extension(".vmaps.json");
            std::ofstream out(side, std::ios::trunc);
            if (out) out << js.str();
        }
    }

    // Bindings sidecar (`<basename>.bindings.json`). Same shape
    // motivation as the vmaps sidecar — out-of-band because the
    // cocostudio flatbuffers schema does not carry our event-binding
    // table. Runtime helper `ocs::ext::applyBindingsFromJson` walks
    // it and hands each binding to a caller-supplied dispatch lambda.
    {
        std::ostringstream js;
        js << "{\n  \"scene\":\"" << target.stem().string()
           << "\",\n  \"bindings\":[";
        bool first = true;
        std::function<void(const Node&, const std::string&)> walk =
            [&](const Node& n, const std::string& parentPath)
        {
            const std::string here = parentPath.empty()
                ? n.name() : (parentPath + "/" + n.name());
            auto root = n.element().child("OCSBindings");
            if (root)
            {
                for (auto b = root.child("Binding"); b;
                     b = b.next_sibling("Binding"))
                {
                    if (!first) js << ',';
                    first = false;
                    const std::string params(
                        b.attribute("Params").as_string(""));
                    js << "\n    {\"path\":\"" << here
                       << "\",\"event\":\""
                       << b.attribute("Event").as_string("")
                       << "\",\"method\":\""
                       << b.attribute("Method").as_string("")
                       << "\"";
                    if (!params.empty())
                        js << ",\"params\":\"" << params << "\"";
                    js << '}';
                }
            }
            for (auto& c : n.children()) walk(c, here);
        };
        walk(_doc->rootNode, std::string());
        js << "\n  ]\n}\n";
        if (!first)   // at least one binding present
        {
            fs::path side = target;
            side.replace_extension(".bindings.json");
            std::ofstream out(side, std::ios::trunc);
            if (out) out << js.str();
        }
    }

    // Lights sidecar (`<basename>.lights.json`) — runtime calls
    // ocs::ext::applyLitSpritesFromJson(scene, sidecar) after
    // CSLoader::createNode to attach LitSpriteNode children with
    // texture + ambient + light list.
    {
        std::ostringstream js;
        js << "{\n  \"scene\":\"" << target.stem().string()
           << "\",\n  \"lits\":[";
        bool first = true;
        std::function<void(const Node&, const std::string&)> walk =
            [&](const Node& n, const std::string& parentPath)
        {
            const std::string here = parentPath.empty()
                ? n.name() : (parentPath + "/" + n.name());
            if (n.ctype() == "LitSpriteObjectData")
            {
                auto fd = n.element().child("FileData");
                std::string tex(fd ? fd.attribute("Path").as_string("") : "");
                auto lroot = n.element().child("OCSLights");
                if (!first) js << ',';
                first = false;
                js << "\n    {\"path\":\"" << here
                   << "\",\"texture\":\"" << tex << "\",\"ambient\":[";
                int amb[4] = {60,60,80,255};
                if (lroot)
                {
                    const std::string ambS(
                        lroot.attribute("Ambient").as_string(""));
                    std::sscanf(ambS.c_str(),
                        "%d,%d,%d,%d", amb, amb+1, amb+2, amb+3);
                }
                js << amb[0]<<','<<amb[1]<<','<<amb[2]<<','<<amb[3]
                   << "],\"lights\":[";
                bool firstL = true;
                if (lroot)
                    for (auto L = lroot.child("Light"); L;
                         L = L.next_sibling("Light"))
                    {
                        if (!firstL) js << ',';
                        firstL = false;
                        int c[4] = {255,255,255,255};
                        const std::string cs(
                            L.attribute("Color").as_string(""));
                        std::sscanf(cs.c_str(),
                            "%d,%d,%d,%d", c, c+1, c+2, c+3);
                        float dx = 0, dy = 0;
                        const std::string ds(
                            L.attribute("Direction").as_string(""));
                        std::sscanf(ds.c_str(), "%f,%f", &dx, &dy);
                        js << "{\"x\":" << L.attribute("X").as_float(0)
                           << ",\"y\":" << L.attribute("Y").as_float(0)
                           << ",\"r\":" << L.attribute("Radius").as_float(100)
                           << ",\"color\":[" << c[0]<<','<<c[1]<<','<<c[2]<<','<<c[3]
                           << "],\"falloff\":" << L.attribute("Falloff").as_float(1.0f)
                           << ",\"dir\":[" << dx << ',' << dy << ']'
                           << ",\"cone\":" << L.attribute("ConeDeg").as_float(360.0f)
                           << ",\"soft\":" << L.attribute("ConeSoftness").as_float(0.2f)
                           << "}";
                    }
                js << "]}";
            }
            for (auto& c : n.children()) walk(c, here);
        };
        walk(_doc->rootNode, std::string());
        js << "\n  ]\n}\n";
        if (!first)
        {
            fs::path side = target;
            side.replace_extension(".lights.json");
            std::ofstream out(side, std::ios::trunc);
            if (out) out << js.str();
        }
    }

    _statusMsg = "exported " + target.filename().string();
    emitEvent("exported " + target.string());
    return {};
}

void Editor::saveAs(const std::filesystem::path& newPath)
{
    if (!_doc) return;
    try
    {
        // Drop the sidecar for the OLD path before switching — it
        // would otherwise be orphaned with no document tracking it.
        removeAutosave();
        auto p = newPath;
        opencs::saveCsd(*_doc, &p);
        _csdPath = p;
        _doc->path = p;
        _doc->dirty = false;
        // And the new path's sidecar, if the user happened to Save As
        // over a file that had its own in-flight autosave.
        removeAutosave();
        pushRecent(p);
        _statusMsg = "saved " + p.filename().string();
        emitEvent("saved " + p.string());
    }
    catch (const std::exception& e)
    {
        _statusMsg = std::string("save failed: ") + e.what();
    }
}

void Editor::emitEvent(const std::string& body)
{
    // Best-effort fan-out to RPC subscribers. No-op when nobody has
    // sent `watch on`. The server snapshots its subscriber set under
    // its own lock; this call is cheap when the set is empty.
    _inspect.publishEvent(body);
}

void Editor::advanceVisualPreviews()
{
    // State machine for `preview-visual`. Captures happen at end of
    // frame N (when the live renderer flushed the mutated state);
    // this runs at the top of frame N+1+ and rolls the doc back as
    // soon as the captureScreen callback set `captured=true`. Reply
    // is fulfilled with the absolute path of the saved PNG.
    if (_visualPreviews.empty()) return;
    for (auto it = _visualPreviews.begin(); it != _visualPreviews.end(); )
    {
        auto& job = **it;
        if (job.captured && !job.rolledBack && _doc)
        {
            try { _doc->reloadFromString(job.snapXml); }
            catch (const std::exception& e)
            {
                job.reply.send(std::string("ERR: rollback failed: ") +
                               e.what() + "\n");
                job.rolledBack = true;
                it = _visualPreviews.erase(it);
                continue;
            }
            // Trim undo/redo growth from the mutation, restore dirty.
            while (_undoStack.size() > job.undoSize) _undoStack.pop_back();
            while (_redoStack.size() > job.redoSize) _redoStack.pop_back();
            _doc->dirty = job.dirtyBefore;
            _selected = _doc->rootNode;
            _selectionChanged = true;
            refreshAxFromCsd();
            job.reply.send(std::string("ok ") + job.outPath + "\n");
            job.rolledBack = true;
        }
        if (job.rolledBack) it = _visualPreviews.erase(it);
        else ++it;
    }
}

std::string Editor::openFromPath(const std::filesystem::path& p)
{
    namespace fs = std::filesystem;
    if (p.empty()) return "empty path";
    if (!fs::exists(p)) return "file not found: " + p.string();
    if (_doc && _doc->dirty)
        return "current doc has unsaved edits (save or discard first)";
    try
    {
        auto fresh = opencs::loadCsd(p);
        _doc = std::make_unique<opencs::CsdDocument>(std::move(fresh));
    }
    catch (const std::exception& e)
    {
        _statusMsg = std::string("open failed: ") + e.what();
        return e.what();
    }
    _csdPath = p;
    _selected = _doc->rootNode;
    _selection.clear();
    _undoStack.clear();
    _redoStack.clear();
    _selectionChanged = true;
    _loadError.clear();
    removeAutosave();
    scanAssets();
    refreshAxFromCsd();
    pushRecent(p);
    _statusMsg = "opened " + p.filename().string();
    emitEvent("opened " + p.string());
    return {};
}

// ----- recents persistence ------------------------------------------------

std::filesystem::path Editor::recentsFile() const
{
    namespace fs = std::filesystem;
    // Plain text, newline-separated.
    //
    // Per-platform location:
    //   macOS:   ~/Library/Application Support/Walshard Studio/recents.txt
    //   Linux:   $XDG_DATA_HOME/Walshard Studio/recents.txt
    //              or ~/.local/share/Walshard Studio/recents.txt
    //   Windows: %APPDATA%\Walshard Studio\recents.txt
    //   other:   ~/.walshard/recents.txt
    //
    // macOS only: one-time migration from the pre-rename
    // "OpenCocosStudio" directory.
    std::error_code ec;
    fs::path dir;
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (appdata && *appdata)
        dir = fs::path(appdata) / "Walshard Studio";
    else
    {
        const char* home = std::getenv("USERPROFILE");
        if (!home || !*home) return fs::path("walshard_recents.txt");
        dir = fs::path(home) / "Walshard Studio";
    }
#else
    const char* home = std::getenv("HOME");
    if (!home || !*home) return fs::path("walshard_recents.txt");
#  if defined(__APPLE__)
    fs::path appSupport = fs::path(home) / "Library" / "Application Support";
    dir = appSupport / "Walshard Studio";
    if (!fs::exists(dir, ec))
    {
        // Try each pre-rename location, newest first.
        for (const char* legacyName : {"Valshard Studio", "OpenCocosStudio"})
        {
            fs::path legacy = appSupport / legacyName;
            if (fs::exists(legacy, ec))
            {
                fs::rename(legacy, dir, ec);  // best-effort
                break;
            }
        }
    }
#  elif defined(__linux__)
    const char* xdg = std::getenv("XDG_DATA_HOME");
    dir = (xdg && *xdg) ? fs::path(xdg) / "Walshard Studio"
                        : fs::path(home) / ".local" / "share" / "Walshard Studio";
#  else
    dir = fs::path(home) / ".walshard";
#  endif
#endif
    fs::create_directories(dir, ec);
    return dir / "recents.txt";
}

void Editor::loadRecents()
{
    namespace fs = std::filesystem;
    _recents.clear();
    std::ifstream f(recentsFile());
    if (!f) return;
    std::string line;
    while (std::getline(f, line))
    {
        if (line.empty()) continue;
        fs::path p(line);
        // Skip entries that have vanished since the last session so
        // the menu doesn't surface dead paths.
        std::error_code ec;
        if (!fs::exists(p, ec)) continue;
        _recents.push_back(std::move(p));
        if (_recents.size() >= kRecentsCap) break;
    }
}

void Editor::saveRecents() const
{
    std::ofstream f(recentsFile(), std::ios::trunc);
    if (!f) return;
    for (const auto& p : _recents)
        f << p.string() << '\n';
}

void Editor::pushRecent(const std::filesystem::path& p)
{
    namespace fs = std::filesystem;
    if (p.empty()) return;
    // De-dupe by canonical form so the same file via different
    // relative paths doesn't fragment the list.
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(p, ec);
    if (ec) canon = p;
    auto sameAs = [&](const fs::path& q) {
        std::error_code e;
        auto qc = fs::weakly_canonical(q, e);
        if (e) qc = q;
        return qc == canon;
    };
    _recents.erase(
        std::remove_if(_recents.begin(), _recents.end(), sameAs),
        _recents.end());
    _recents.insert(_recents.begin(), canon);
    if (_recents.size() > kRecentsCap) _recents.resize(kRecentsCap);
    saveRecents();
}

void Editor::drawMenuBar()
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Open…", "Cmd+O"))
            {
                std::snprintf(_pathBuf, sizeof(_pathBuf), "%s",
                              _csdPath.string().c_str());
                _openDialogOpen = true;
            }
            if (ImGui::BeginMenu("Open Recent",
                                 /*enabled=*/!_recents.empty()))
            {
                // Most-recent first. Each row labels with filename for
                // readability + full path on hover so duplicates from
                // sibling dirs can still be told apart.
                for (const auto& p : _recents)
                {
                    const std::string label = p.filename().string();
                    if (ImGui::MenuItem(label.c_str()))
                    {
                        auto err = openFromPath(p);
                        if (!err.empty())
                            _statusMsg = "open failed: " + err;
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", p.string().c_str());
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Clear list"))
                {
                    _recents.clear();
                    saveRecents();
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Save", "Cmd+S", false,
                                _doc != nullptr && !_csdPath.empty()))
            {
                try
                {
                    opencs::saveCsd(*_doc);
                    _doc->dirty = false;
                    _statusMsg = "saved " + _csdPath.filename().string();
                }
                catch (const std::exception& e)
                {
                    _statusMsg = std::string("save failed: ") + e.what();
                }
            }
            if (ImGui::MenuItem("Save As…", nullptr, false, _doc != nullptr))
            {
                std::snprintf(_pathBuf, sizeof(_pathBuf), "%s",
                              _csdPath.string().c_str());
                _saveAsDialogOpen = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Cmd+Q"))
            {
                ax::Director::getInstance()->end();
            }
            ImGui::EndMenu();
        }

        // Status text on the right-hand side of the menu bar.
        if (!_statusMsg.empty())
        {
            ImGui::Separator();
            ImGui::TextUnformatted(_statusMsg.c_str());
        }
        if (_doc && _doc->dirty)
        {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1, 0.75f, 0.2f, 1), "[modified]");
        }
        ImGui::EndMainMenuBar();
    }

    // Open / Save As modal path inputs. Native file dialogs deferred.
    auto modal = [&](const char* title, bool& open, bool isSave)
    {
        if (open) { ImGui::OpenPopup(title); open = false; }
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(600, 0), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal(title, nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##path", _pathBuf, sizeof(_pathBuf));
            ImGui::Spacing();
            if (ImGui::Button(isSave ? "Save" : "Open", ImVec2(120, 0)))
            {
                if (isSave && _doc)
                {
                    try
                    {
                        std::filesystem::path p(_pathBuf);
                        opencs::saveCsd(*_doc, &p);
                        _csdPath = p;
                        _doc->path = p;
                        _doc->dirty = false;
                        _statusMsg = "saved " + p.filename().string();
                    }
                    catch (const std::exception& e)
                    {
                        _statusMsg = std::string("save failed: ") + e.what();
                    }
                }
                else if (!isSave)
                {
                    auto err = openFromPath(std::filesystem::path(_pathBuf));
                    if (!err.empty())
                        _statusMsg = "open failed: " + err;
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    };
    modal("Open .csd", _openDialogOpen, false);
    modal("Save As", _saveAsDialogOpen, true);
}

// handleInspectCommand + its tokenize / parsePath / pathToString helpers
// live in EditorInspect.cpp to keep this translation unit focused on the
// Editor's render loop and panel orchestration. The split was a pure code
// move — no behavior change.

std::vector<Node> Editor::findAssetReferences(const std::string& basename) const
{
    std::vector<Node> refs;
    if (!_doc || basename.empty()) return refs;

    // Normalize for comparison. An asset's `basename` is the filename on disk
    // (e.g. "card_back.png"); Cocos Studio Path attrs typically carry a
    // project-relative path like "images/cards/card_back.png". Matching =
    // filename component equality, case-insensitive.
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
        return s;
    };
    const std::string wantFilename = lower(basename);

    auto filenameOf = [](const std::string& p) -> std::string {
        auto pos = p.find_last_of("/\\");
        return pos == std::string::npos ? p : p.substr(pos + 1);
    };

    for (auto& n : _doc->allNodes())
    {
        for (const auto& ref : n.imageRefs())
        {
            if (!ref.path.empty() &&
                lower(filenameOf(ref.path)) == wantFilename)
            { refs.push_back(n); break; }
            // Plist-driven sprite frames reference the atlas via Plist="…plist"
            // and the frame name in Path. Catch plist filename matches too.
            if (!ref.plist.empty() &&
                lower(filenameOf(ref.plist)) == wantFilename)
            { refs.push_back(n); break; }
        }
    }
    return refs;
}

void Editor::setMode(Mode m)
{
    const Mode prev = _mode;
    _mode = m;
    applyModeToScene();
    if (prev != m)
    {
        const char* name = "edit";
        switch (m) {
            case Mode::Edit: name = "edit"; break;
            case Mode::View: name = "view"; break;
            case Mode::LayerSide: name = "side"; break;
            case Mode::LayerTop:  name = "top";  break;
        }
        emitEvent(std::string("mode ") + name);
    }

    // View mode auto-plays the timeline (and loops) so the user can
    // preview the animation without touching the timeline panel. Edit
    // mode pauses so authoring isn't disrupted by the playhead. Only
    // toggle on a real mode transition — re-entering the same mode
    // shouldn't restart playback.
    if (prev != m)
    {
        if (m == Mode::View)
        {
            _tlPlaying = true;
            _tlLoop    = true;
            _tlLastTickTime = 0.0;
            _tlAccumFrames  = 0.0;
        }
        else if (prev == Mode::View)
        {
            _tlPlaying = false;
        }
    }

    // Re-centre the scene on every mode change — we restore the
    // fitted pan so the bounding-box centre lands at the canvas
    // centre regardless of which mode we're entering. The user's
    // current zoom is deliberately NOT reset: if they chose a
    // specific zoom level they want it preserved across mode flips;
    // `Reset view` remains the explicit way to restore fit zoom.
    _panOffsetX = _initialPanX;
    _panOffsetY = _initialPanY;

    // Side / Top modes reuse the scene-rotation experiment: they preset the
    // rotation axes + Z-padding so the same 3D rig that the experiment
    // sliders drive also produces the side / top projection. Edit and View
    // reset those back to zero so the canvas lies flat again.
    switch (m)
    {
    case Mode::LayerSide:
        _sceneRotationXDeg  =  0.0f;
        _sceneRotationYDeg  = 80.0f;
        _sceneRotationDeg   =  0.0f;
        _sceneZLayerSpacing = 50.0f;
        break;
    case Mode::LayerTop:
        _sceneRotationXDeg  = 80.0f;
        _sceneRotationYDeg  =  0.0f;
        _sceneRotationDeg   =  0.0f;
        _sceneZLayerSpacing = 50.0f;
        break;
    case Mode::Edit:
    case Mode::View:
        _sceneRotationXDeg  = 0.0f;
        _sceneRotationYDeg  = 0.0f;
        _sceneRotationDeg   = 0.0f;
        _sceneZLayerSpacing = 0.0f;
        break;
    }
}

void Editor::applyModeToScene()
{
    const bool view = (_mode == Mode::View);
    for (ax::Node* n : _axOrderDfs)
    {
        if (auto* w = dynamic_cast<ax::ui::Widget*>(n))
        {
            // Disable touch in Edit mode so the editor doesn't fight the
            // underlying button/rollover handlers. In View mode the
            // components behave as they would in a real scene.
            w->setTouchEnabled(view);
        }
    }
}

int Editor::getZOrder(const Node& n) const
{
    if (!n.valid()) return 0;
    const auto s = n.attr("ZOrder", "");
    return s.empty() ? 0 : std::atoi(s.c_str());
}

void Editor::setZOrder(const Node& n, int z)
{
    if (!n.valid()) return;
    pushUndo();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", z);
    // n is const-ref but setAttr mutates the underlying pugi element, which
    // is owned by the document, not by the Node wrapper. Cast away const on
    // the wrapper to call the mutator.
    const_cast<Node&>(n).setAttr("ZOrder", buf);
    auto it = _csdToAx.find(n.element().internal_object());
    if (it != _csdToAx.end() && it->second)
        it->second->setLocalZOrder(z);
    if (_doc) _doc->dirty = true;
    _statusMsg = std::string("z = ") + buf;
}

void Editor::bringForward(const Node& n) { setZOrder(n, getZOrder(n) + 1); }
void Editor::sendBackward(const Node& n) { setZOrder(n, getZOrder(n) - 1); }
void Editor::bringToFront(const Node& n) { setZOrder(n,  1000000); }
void Editor::sendToBack (const Node& n)  { setZOrder(n, -1000000); }

void Editor::syncNodeCsdToAx(const Node& n)
{
    if (!n.valid()) return;
    auto it = _csdToAx.find(n.element().internal_object());
    if (it == _csdToAx.end() || !it->second) return;
    ax::Node* ax = it->second;

    // OCSExtension binding: VectorMapObjectData isn't in the cocostudio
    // flatbuffers schema, so CSLoader produces a plain ax::Node. The
    // editor attaches a `VectorMapNode` runtime child here and
    // re-populates layers from the XML on every sync — so a setattr /
    // panel edit reflects on the canvas without an export round-trip.
    syncVectorMapToAx(n, ax);
    // Same pattern for LitSpriteObjectData.
    syncLitSpriteToAx(n, ax, _assetsRoot);

    auto readXY = [](const opencs::Node& nn, const char* tag,
                     const char* kX, const char* kY,
                     float& outX, float& outY) {
        for (const auto& [k, v] : nn.subAttrs(tag))
        {
            if      (k == kX) outX = std::strtof(v.c_str(), nullptr);
            else if (k == kY) outY = std::strtof(v.c_str(), nullptr);
        }
    };

    float px = ax->getPositionX(), py = ax->getPositionY();
    readXY(n, "Position", "X", "Y", px, py);
    ax->setPosition(px, py);

    const auto cs = ax->getContentSize();
    float sw = cs.width, sh = cs.height;
    readXY(n, "Size", "X", "Y", sw, sh);
    ax->setContentSize(ax::Size(sw, sh));

    float scx = ax->getScaleX(), scy = ax->getScaleY();
    readXY(n, "Scale", "ScaleX", "ScaleY", scx, scy);
    ax->setScaleX(scx);
    ax->setScaleY(scy);

    const auto aP = ax->getAnchorPoint();
    float axf = aP.x, ayf = aP.y;
    readXY(n, "AnchorPoint", "ScaleX", "ScaleY", axf, ayf);
    ax->setAnchorPoint(ax::Vec2(axf, ayf));

    const auto rot = n.attr("Rotation", "");
    ax->setRotation(rot.empty() ? 0.0f : std::strtof(rot.c_str(), nullptr));

    const auto z = n.attr("ZOrder", "");
    ax->setLocalZOrder(z.empty() ? 0 : std::atoi(z.c_str()));

    // Visibility — `Visible="False"` (or our editor toggle) hides the
    // node in the canvas. Default (attribute absent) is visible. Read
    // through the model accessor so the eye-icon panel toggle and the
    // Cocos Studio runtime end up using identical state.
    ax->setVisible(n.visible());

    // CColor — modulate color sub-element. Missing sub-element → leave
    // ax's color alone (common case for new nodes that inherit the
    // default white). Present-but-partial → fill missing channels from
    // the ax node's current color so we don't stomp to black.
    {
        auto col = ax->getColor();
        int R = col.r, G = col.g, B = col.b;
        for (const auto& [k, v] : n.subAttrs("CColor"))
        {
            const int iv = std::atoi(v.c_str());
            if      (k == "R") R = iv;
            else if (k == "G") G = iv;
            else if (k == "B") B = iv;
        }
        ax->setColor(ax::Color3B((uint8_t)R, (uint8_t)G, (uint8_t)B));
    }

    // Opacity — Alpha attribute, 0-255. Absent attr → keep current.
    const auto alpha = n.attr("Alpha", "");
    if (!alpha.empty())
        ax->setOpacity((uint8_t)std::clamp(std::atoi(alpha.c_str()), 0, 255));

    // Panel background fill — only meaningful for ax::ui::Layout (what
    // CSLoader instantiates for Panel/Layer ObjectData). For any other
    // subclass the cast returns null and we silently skip. Mode mapping
    // matches Cocos Studio's ComboBoxIndex: 0 = none, 1 = solid, 2 =
    // gradient.
    if (auto* layout = dynamic_cast<ax::ui::Layout*>(ax))
    {
        auto readRGBA = [&](const char* tag,
                            uint8_t& r, uint8_t& g,
                            uint8_t& b, uint8_t& a)
        {
            r = g = b = 255; a = 255;
            for (const auto& [k, v] : n.subAttrs(tag))
            {
                const int iv = std::atoi(v.c_str());
                if      (k == "R") r = (uint8_t)std::clamp(iv, 0, 255);
                else if (k == "G") g = (uint8_t)std::clamp(iv, 0, 255);
                else if (k == "B") b = (uint8_t)std::clamp(iv, 0, 255);
                else if (k == "A") a = (uint8_t)std::clamp(iv, 0, 255);
            }
        };
        int mode = 0;
        const auto cbi = n.attr("ComboBoxIndex", "");
        if (!cbi.empty()) mode = std::atoi(cbi.c_str());
        else if (!n.subAttrs("FirstColor").empty()) mode = 2;
        else if (!n.subAttrs("SingleColor").empty()) mode = 1;

        if (mode == 1)
        {
            uint8_t r, g, b, a;
            readRGBA("SingleColor", r, g, b, a);
            const auto bcA = n.attr("BackColorAlpha", "");
            if (!bcA.empty()) a = (uint8_t)std::clamp(std::atoi(bcA.c_str()), 0, 255);
            layout->setBackGroundColorType(
                ax::ui::Layout::BackGroundColorType::SOLID);
            layout->setBackGroundColor(ax::Color3B(r, g, b));
            layout->setBackGroundColorOpacity(a);
        }
        else if (mode == 2)
        {
            uint8_t fr, fg, fb, fa, er, eg, eb, ea;
            readRGBA("FirstColor", fr, fg, fb, fa);
            readRGBA("EndColor",   er, eg, eb, ea);
            const auto bcA = n.attr("BackColorAlpha", "");
            const uint8_t a = bcA.empty() ? fa :
                (uint8_t)std::clamp(std::atoi(bcA.c_str()), 0, 255);
            float vx = 0.0f, vy = -1.0f;
            for (const auto& [k, v] : n.subAttrs("ColorVector"))
            {
                const float f = std::strtof(v.c_str(), nullptr);
                if      (k == "ScaleX") vx = f;
                else if (k == "ScaleY") vy = f;
            }
            layout->setBackGroundColorType(
                ax::ui::Layout::BackGroundColorType::GRADIENT);
            layout->setBackGroundColor(
                ax::Color3B(fr, fg, fb), ax::Color3B(er, eg, eb));
            layout->setBackGroundColorVector(ax::Vec2(vx, vy));
            layout->setBackGroundColorOpacity(a);
        }
        else
        {
            layout->setBackGroundColorType(
                ax::ui::Layout::BackGroundColorType::NONE);
        }
    }
}

void Editor::rebuildAxMapsAndSync()
{
    if (!_axmolRoot || !_doc) return;
    _axToCsd.clear();
    _csdToAx.clear();
    _axOrderDfs.clear();

    auto readXY = [](const opencs::Node& n, const char* tag,
                     const char* kX, const char* kY,
                     float& outX, float& outY) {
        for (const auto& [k, v] : n.subAttrs(tag))
        {
            if      (k == kX) outX = std::strtof(v.c_str(), nullptr);
            else if (k == kY) outY = std::strtof(v.c_str(), nullptr);
        }
    };

    std::function<void(ax::Node*, const Node&)> walk =
        [&](ax::Node* ax, const Node& csd)
    {
        _axToCsd[ax] = csd;
        _csdToAx[csd.element().internal_object()] = ax;
        _axOrderDfs.push_back(ax);

        // Apply csd geometry to the live ax node so the scene snaps back to
        // the snapshot state.
        float px = ax->getPositionX(), py = ax->getPositionY();
        readXY(csd, "Position", "X", "Y", px, py);
        ax->setPosition(px, py);

        const auto cs = ax->getContentSize();
        float sw = cs.width, sh = cs.height;
        readXY(csd, "Size", "X", "Y", sw, sh);
        ax->setContentSize(ax::Size(sw, sh));

        float scx = ax->getScaleX(), scy = ax->getScaleY();
        readXY(csd, "Scale", "ScaleX", "ScaleY", scx, scy);
        ax->setScaleX(scx);
        ax->setScaleY(scy);

        const auto aP = ax->getAnchorPoint();
        float ax_frac = aP.x, ay_frac = aP.y;
        readXY(csd, "AnchorPoint", "ScaleX", "ScaleY", ax_frac, ay_frac);
        ax->setAnchorPoint(ax::Vec2(ax_frac, ay_frac));

        // ALWAYS set rotation — even if the attribute is absent in the new
        // snapshot, we need to zero it out so undoing a rotate doesn't
        // leave the ax node with its mid-drag angle.
        const auto rot = csd.attr("Rotation", "");
        ax->setRotation(rot.empty() ? 0.0f : std::strtof(rot.c_str(), nullptr));

        const auto z = csd.attr("ZOrder", "");
        ax->setLocalZOrder(z.empty() ? 0 : std::atoi(z.c_str()));

        const auto& axKids = ax->getChildren();
        auto csdKids = csd.children();
        const size_t n = std::min<size_t>(axKids.size(), csdKids.size());
        for (size_t i = 0; i < n; ++i)
            walk(axKids.at((ssize_t)i), csdKids[i]);
    };
    walk(_axmolRoot, _doc->rootNode);
    applyModeToScene();
}

void Editor::refreshAxFromCsd()
{
    // Without a host hook we can only resync attributes — structural
    // additions / removals won't be reflected on the canvas. The
    // truncating walk in rebuildAxMapsAndSync makes this a best-effort
    // fallback; structurally-impacted callers should ensure the host
    // has registered the hook.
    if (!_refreshAxRootFn)
    {
        rebuildAxMapsAndSync();
        return;
    }
    ax::Node* fresh = _refreshAxRootFn();
    if (!fresh)
    {
        // Compile or load failed — keep the current ax root rather than
        // blanking the canvas. Map resync against the old tree may now
        // be off (csd has structurally diverged) but the user still sees
        // the previous scene rather than a black canvas.
        rebuildAxMapsAndSync();
        return;
    }
    setAxmolRoot(fresh);
    // setAxmolRoot sets _needsFitToContent for the initial open path,
    // which would re-fit the canvas after every structural mutation —
    // i.e. zoom would jump on every add/delete. Cancel it here so the
    // user's current zoom/pan survive the refresh.
    _needsFitToContent = false;
}

namespace
{
/// Locate the TexturePacker CLI. Returns empty if not found.
std::string findTexturePacker()
{
    if (const char* env = std::getenv("OCS_TEXTUREPACKER"))
    {
        if (*env && std::filesystem::exists(env)) return env;
    }
    for (const char* p :
         {"/usr/local/bin/TexturePacker",
          "/opt/homebrew/bin/TexturePacker",
          "/Applications/TexturePacker.app/Contents/MacOS/TexturePacker"})
    {
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
    }
    // Fallback: hope it's on PATH. We'll just invoke by name.
    return "TexturePacker";
}

/// Shell-quote a path so it survives the /bin/sh invocation used by
/// popen/system. Single-quote wrapping with embedded ' escaped as '\''.
std::string shellQuote(const std::string& s)
{
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else           out += c;
    }
    out += "'";
    return out;
}
}

std::string Editor::packFolderWithTexturePacker(
    const std::filesystem::path& folder,
    const std::string& atlasName)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(folder, ec))
        return "not a directory";

    std::vector<fs::path> pngs;
    for (auto it = fs::directory_iterator(folder, ec);
         !ec && it != fs::directory_iterator(); ++it)
    {
        if (!it->is_regular_file(ec)) continue;
        auto ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        if (ext == ".png") pngs.push_back(it->path());
    }
    if (pngs.empty()) return "no .png files in folder";

    const fs::path sheet = folder / (atlasName + ".png");
    const fs::path data  = folder / (atlasName + ".plist");
    const std::string bin = findTexturePacker();

    std::string cmd = shellQuote(bin);
    cmd += " --format cocos2d";
    cmd += " --data "  + shellQuote(data.string());
    cmd += " --sheet " + shellQuote(sheet.string());
    for (const auto& p : pngs) cmd += " " + shellQuote(p.string());
    cmd += " 2>&1";

    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return "popen failed";
    std::string out;
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) out += buf;
    const int rc = pclose(f);
    if (rc != 0)
    {
        std::string msg = "TexturePacker exit=" + std::to_string(rc);
        if (!out.empty()) msg += "\n" + out.substr(0, 300);
        _statusMsg = msg;
        return msg;
    }
    rescanAssets();
    _statusMsg = "packed " + std::to_string(pngs.size()) +
                 " images → " + sheet.filename().string();
    return {};
}


std::string Editor::packFilesIntoAtlas(
    const std::vector<std::filesystem::path>& files,
    const std::string& atlasName,
    const std::filesystem::path& outDir)
{
    namespace fs = std::filesystem;
    if (files.empty())     return "no files selected";
    if (atlasName.empty()) return "empty atlas name";

    std::error_code ec;
    if (!fs::is_directory(outDir, ec))
        return "output directory does not exist";

    // Stage the selected files into a throwaway dir so the existing
    // folder packer (which sheets every .png in a dir) has a clean set of
    // inputs. Using a PID + wall-time suffix keeps concurrent packs from
    // colliding if the editor ever fires two in parallel.
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const fs::path staging =
        fs::temp_directory_path() /
        ("opencs-pack-" + std::to_string(::getpid()) +
         "-" + std::to_string(ts));
    fs::create_directories(staging, ec);
    if (ec) return "failed to create staging dir: " + ec.message();

    // Copy (not move) — source files are still in the project and the
    // user hasn't opted in to deleting them. Collisions get a numeric
    // suffix so two files with the same basename don't clobber.
    std::unordered_map<std::string, int> seen;
    for (const auto& src : files)
    {
        auto base = src.filename().stem().string();
        auto ext  = src.filename().extension().string();
        auto dst  = staging / (base + ext);
        while (fs::exists(dst, ec))
        {
            ++seen[base];
            dst = staging / (base + "_" + std::to_string(seen[base]) + ext);
        }
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            { std::error_code _ec; fs::remove_all(staging, _ec); }
            return "copy failed: " + ec.message();
        }
    }

    // Run the existing folder-packer on the staging dir.
    const std::string err = packFolderWithTexturePacker(staging, atlasName);
    if (!err.empty())
    {
        { std::error_code _ec; fs::remove_all(staging, _ec); }
        return err;
    }

    // Move the produced atlas + plist out of the staging dir into the
    // user-chosen output dir. If anything goes wrong past this point we
    // keep the staging dir around so the user can grab the outputs
    // manually — the error message points at it.
    const fs::path srcPng   = staging / (atlasName + ".png");
    const fs::path srcPlist = staging / (atlasName + ".plist");
    const fs::path dstPng   = outDir  / (atlasName + ".png");
    const fs::path dstPlist = outDir  / (atlasName + ".plist");
    fs::copy_file(srcPng,   dstPng,   fs::copy_options::overwrite_existing, ec);
    if (ec) return "move atlas png failed: " + ec.message() +
                   " (leftover at " + staging.string() + ")";
    fs::copy_file(srcPlist, dstPlist, fs::copy_options::overwrite_existing, ec);
    if (ec) return "move atlas plist failed: " + ec.message() +
                   " (leftover at " + staging.string() + ")";

    { std::error_code _ec; fs::remove_all(staging, _ec); }
    rescanAssets();
    _statusMsg = "packed " + std::to_string(files.size()) +
                 " images → " + dstPng.filename().string();
    return {};
}


void Editor::onWindowResized(float newWinW, float newWinH)
{
    // Runs on the main thread inside axmol's EVENT_WINDOW_RESIZED dispatch,
    // BEFORE axmol visits the scene. Uses the central-rect fractions cached
    // during the last ImGui render to predict the new central rect, then
    // applies scene->setPosition so the canvas is centered correctly on the
    // very frame the listener triggers via drawScene().
    OCS_LOG_TRACE("layout",
        "onWindowResized: %.0fx%.0f (cFrac=L%.3f T%.3f R%.3f B%.3f)",
        newWinW, newWinH,
        _centralFracL, _centralFracT, _centralFracR, _centralFracB);
    if (newWinW <= 1.0f || newWinH <= 1.0f) return;
    if (_centralFracR - _centralFracL <= 0.01f ||
        _centralFracB - _centralFracT <= 0.01f) return;   // no fractions yet

    const float cpx = _centralFracL * newWinW;
    const float cpy = _centralFracT * newWinH;
    const float csx = (_centralFracR - _centralFracL) * newWinW;
    const float csy = (_centralFracB - _centralFracT) * newWinH;

    auto* view = ax::Director::getInstance()->getRenderView();
    const float scaleX = view->getScaleX();
    const float scaleY = view->getScaleY();
    if (scaleX <= 0 || scaleY <= 0) return;

    const float dx = (cpx + csx * 0.5f) - newWinW * 0.5f;
    const float dy = newWinH * 0.5f - (cpy + csy * 0.5f);
    // Same fix as the per-frame canvas-fit path: axmol's scale is
    // already points-per-design-px on macOS, no FbScale needed.
    _cachedSceneX = dx / scaleX;
    _cachedSceneY = dy / scaleY;

    if (auto* scene = ax::Director::getInstance()->getRunningScene())
    {
        ax::Node* viewRoot = scene->getChildByTag(0xCADCA12A);
        if (!viewRoot) viewRoot = scene;
        viewRoot->setPosition(_cachedSceneX, _cachedSceneY);
    }
}

void Editor::setCanvasActive(bool active)
{
    // Clear any leftover commands from the previous frame and
    // toggle visibility. Drawing goes into this same node again
    // on the next render frame when active == true, so there's no
    // stale-state risk.
    if (_overlayDraw)
    {
        _overlayDraw->clear();
        _overlayDraw->setVisible(active);
    }
}

namespace
{
/// Parse "0,1,3" into vector<size_t>. Tolerates "(root)" from
/// pathToString by yielding an empty vector. Shared by
/// applyMirrorEdit + selectByPath.
std::vector<std::size_t> parseMirrorPath(const std::string& pathStr)
{
    std::vector<std::size_t> idxs;
    std::size_t i = 0;
    while (i < pathStr.size())
    {
        std::size_t j = pathStr.find(',', i);
        if (j == std::string::npos) j = pathStr.size();
        const std::string piece = pathStr.substr(i, j - i);
        if (piece != "(root)" && !piece.empty())
            idxs.push_back((std::size_t)std::atoll(piece.c_str()));
        i = j + 1;
    }
    return idxs;
}
}  // namespace

std::string Editor::serializePanelMirror(const std::string& name) const
{
    if (name == "Scene")
    {
        // Depth-prefixed DFS of the whole CSD tree: each line is
        //   <depth>\t<name>\t<ctype>\t<pathStr>
        // plus a header `selected=<pathStr>`. Detached Scene renders
        // this as an indented tree; clicking a row sends a `select`
        // request back to the focused main-app.
        std::string out;
        out.reserve(1024);
        if (_selected.valid())
            out += "selected=" + pathToString(pathTo(_selected)) + "\n";
        else
            out += "selected=\n";
        if (!_doc) return out;
        auto root = _doc->rootNode;
        if (!root.valid()) return out;
        std::function<void(Node, int, std::vector<std::size_t>&)> walk;
        walk = [&](Node n, int depth, std::vector<std::size_t>& path) {
            const std::string p = pathToString(path);
            out += std::to_string(depth) + "\t" +
                   n.name() + "\t" + n.ctype() + "\t" + p + "\n";
            auto kids = n.children();
            for (std::size_t i = 0; i < kids.size(); ++i)
            {
                path.push_back(i);
                walk(kids[i], depth + 1, path);
                path.pop_back();
            }
        };
        std::vector<std::size_t> path;
        walk(root, 0, path);
        return out;
    }
    if (name == "Assets")
    {
        // One line per asset:  <kind>\t<label>\t<absPath>
        // Kind as "File" / "Folder" / "Atlas" / "AtlasFrame".
        std::string out;
        out.reserve(256 + _assets.size() * 64);
        if (!_selectedAsset.empty())
            out += "selected=" + _selectedAsset.string() + "\n";
        else
            out += "selected=\n";
        for (const auto& a : _assets)
        {
            const char* kind = "File";
            switch (a.kind)
            {
                case AssetKind::File:       kind = "File";       break;
                case AssetKind::Folder:     kind = "Folder";     break;
                case AssetKind::Atlas:      kind = "Atlas";      break;
                case AssetKind::AtlasFrame: kind = "AtlasFrame"; break;
            }
            out += std::string(kind) + "\t" + a.displayName + "\t" +
                   a.absPath.string() + "\n";
        }
        return out;
    }
    if (name == "Layout")
    {
        // Layout panel: simple snapshot of document-level state that
        // isn't tied to a specific node. The docked panel shows the
        // design resolution + dirty flag + a few other knobs — this
        // mirror reflects that set.
        std::string out;
        if (!_doc) return out;
        auto root = _doc->rootNode;
        if (root.valid())
        {
            auto size = root.subAttrs("Size");
            for (auto& [k, v] : size)
                out += std::string("size.") + k + "=" + v + "\n";
            out += "ctype="   + root.ctype() + "\n";
            out += "name="    + root.name() + "\n";
        }
        out += std::string("dirty=") + (_doc->dirty ? "1" : "0") + "\n";
        return out;
    }
    if (name == "Properties")
    {
        if (!_selected.valid()) return {};
        auto path = pathTo(_selected);
        std::string out;
        out.reserve(512);
        out += "path=" + pathToString(path) + "\n";
        out += "display=" + _selected.displayLabel() + "\n";
        // Top-level attributes — dump everything pugi has. Prefix
        // with `attr.` so the writeback parser knows which setter to
        // call. We skip empty values just to keep the body compact;
        // an empty attribute is indistinguishable from "attribute not
        // set" and neither side cares.
        for (auto a : _selected.element().attributes())
        {
            const std::string k(a.name());
            const std::string v(a.value());
            if (k.empty()) continue;
            out += "attr." + k + "=" + v + "\n";
        }
        // Common editable sub-elements. We list them explicitly rather
        // than walking all children because the CSD has many non-attr
        // sub-elements (Children, Timeline, …) that aren't props.
        static const char* const kSubs[] = {
            "Position", "Size", "Scale",
            "AnchorPoint", "PrePosition", "PreSize",
            "CColor", "BlendFunc"
        };
        for (const char* tag : kSubs)
        {
            auto subs = _selected.subAttrs(tag);
            for (const auto& [k, v] : subs)
                out += std::string("sub.") + tag + "." + k + "=" + v + "\n";
        }
        return out;
    }
    if (name == "Messages")
    {
        // Replicate the click-log table the in-window Messages panel
        // shows: newest-first, with kind / coords / hoverWin / hit.
        std::string out;
        out.reserve(64 * _eventLog.size());
        char line[256];
        for (const auto& ev : _eventLog)
        {
            std::snprintf(line, sizeof(line),
                "%7.2fs (%.0f,%.0f) %-22s win=%s%s%s%s\n",
                ev.time, ev.x, ev.y, ev.kind ? ev.kind : "?",
                ev.hoverWinName.empty() ? "(null)"
                                        : ev.hoverWinName.c_str(),
                ev.hitName.empty()  ? "" : "  hit=",
                ev.hitName.c_str(),
                ev.hitCtype.empty() ? "" :
                    (std::string(" (") + ev.hitCtype + ")").c_str());
            out.append(line);
        }
        return out;
    }
    // Properties / Scene / Assets / Layout serializers land here in
    // later phases. Returning empty tells the host "no mirror update
    // this frame" — readers keep showing the last value.
    return {};
}

void Editor::applyMirrorEdit(const std::string& pathStr,
                             const std::string& key,
                             const std::string& value)
{
    Node n = nodeAtPath(parseMirrorPath(pathStr));
    if (!n.valid()) return;

    pushUndo();
    if (key.rfind("attr.", 0) == 0)
    {
        n.setAttr(key.substr(5), value);
    }
    else if (key.rfind("sub.", 0) == 0)
    {
        const auto rest = key.substr(4);
        const auto dot  = rest.find('.');
        if (dot == std::string::npos) return;
        const std::string tag    = rest.substr(0, dot);
        const std::string subKey = rest.substr(dot + 1);
        auto attrs = n.subAttrs(tag);
        bool wrote = false;
        for (auto& [k, v] : attrs)
            if (k == subKey) { v = value; wrote = true; break; }
        if (!wrote) attrs.emplace_back(subKey, value);
        n.setSubAttrs(tag, attrs);
    }
    else
    {
        return;   // unknown prefix
    }
    if (_doc) _doc->dirty = true;
    // Re-sync the axmol tree so the live scene reflects the attr change.
    rebuildAxMapsAndSync();
}

void Editor::selectByPath(const std::string& pathStr)
{
    Node n = nodeAtPath(parseMirrorPath(pathStr));
    if (!n.valid()) return;
    setSelected(n);
}

void Editor::selectRange(const Node& anchor, const Node& target)
{
    if (!_doc || !target.valid()) return;
    // Walk the csd document in pre-order (matches the Scene tree's
    // visible order) and collect every node into a flat list so we
    // can pull the contiguous range between anchor and target.
    std::vector<Node> flat;
    std::function<void(pugi::xml_node)> walk = [&](pugi::xml_node el) {
        if (!el) return;
        Node nd(el);
        if (nd.valid() &&
            nd.element().name() == pugi::string_view_t(tags::kAbstractNode))
            flat.push_back(nd);
        else if (nd.valid() &&
                 nd.element().name() == pugi::string_view_t(tags::kObjectData))
            flat.push_back(nd);
        // Recurse into <Children> so the order matches what drawNode
        // emits row-by-row.
        auto kids = el.child(tags::kChildren);
        if (kids)
            for (auto c = kids.first_child(); c; c = c.next_sibling())
                walk(c);
    };
    walk(_doc->rootNode.element());

    int aIdx = -1, bIdx = -1;
    auto* anchorObj = anchor.valid() ? anchor.element().internal_object() : nullptr;
    auto* targetObj = target.element().internal_object();
    for (int i = 0; i < (int)flat.size(); ++i)
    {
        if (anchorObj && flat[i].element().internal_object() == anchorObj) aIdx = i;
        if (flat[i].element().internal_object() == targetObj)              bIdx = i;
    }
    if (bIdx < 0) return;
    // No anchor (or invalid) — fall back to single-select target.
    if (aIdx < 0) { setSelected(target); return; }
    if (aIdx > bIdx) std::swap(aIdx, bIdx);

    _selection.clear();
    for (int i = aIdx; i <= bIdx; ++i) _selection.push_back(flat[i]);
    _selected = target;
    _selectionChanged = true;
    _lastTouched = LastTouched::Node;
}

bool Editor::isEffectivelyLocked(const Node& n) const
{
    if (!n.valid()) return false;
    // Self lock-mode counts no matter which kind (Self / Propagating);
    // ancestor lock-modes only count when Propagating.
    if (n.lockMode() != Node::LockMode::None) return true;
    auto path = pathTo(n);
    if (path.empty()) return false;
    path.pop_back();   // start at the parent
    while (true)
    {
        Node cur = nodeAtPath(path);
        if (cur.valid() && cur.lockMode() == Node::LockMode::Propagating)
            return true;
        if (path.empty()) break;
        path.pop_back();
    }
    return false;
}

bool Editor::isEffectivelyVisible(const Node& n) const
{
    if (!n.valid()) return false;
    if (!n.visible()) return false;
    auto path = pathTo(n);
    if (path.empty()) return true;
    path.pop_back();
    while (true)
    {
        Node cur = nodeAtPath(path);
        if (cur.valid() && !cur.visible()) return false;
        if (path.empty()) break;
        path.pop_back();
    }
    return true;
}

void Editor::appendInfoLog(const char* kind, const std::string& detail)
{
    ClickEvent ev;
    ev.time          = ImGui::GetTime();
    ev.x             = 0;
    ev.y             = 0;
    ev.inCentral     = false;
    ev.hoverIsPanel  = false;
    ev.anyItemHovered = false;
    ev.passedGate    = false;
    ev.kind          = kind;     // expected static string literal
    ev.hitName       = detail;
    _eventLog.push_back(std::move(ev));
    while (_eventLog.size() > kEventLogCap) _eventLog.pop_front();
}

void Editor::requestLayoutRebuild()
{
    OCS_LOG_TRACE("layout",
        "requestLayoutRebuild (full default-layout pass on next frame)");
    _layoutBuilt = false;
    // Rebuild tears down the current dock tree, so any DockIds we
    // captured for hide→show restoration become invalid. Drop them —
    // the new buildDefaultLayout pass will assign fresh ones which
    // get captured on the next record pass.
    _lastDockIdByPanel.clear();
}

void Editor::resnapDockToViewport()
{
    if (_dockspaceId == 0) return;
    const ImVec2 vp = ImGui::GetMainViewport()->Size;
    if (vp.x <= 0 || vp.y <= 0) return;
    OCS_LOG_TRACE("layout",
        "resnapDockToViewport: vp=%.0fx%.0f dock=0x%08X",
        vp.x, vp.y, _dockspaceId);
    ImGui::DockBuilderSetNodeSize(_dockspaceId, vp);
    ImGui::DockBuilderFinish(_dockspaceId);
}

void Editor::reattachPanel(const std::string& name)
{
    auto it = _lastDockIdByPanel.find(name);
    const ImGuiID stored = (it == _lastDockIdByPanel.end()) ? 0 : it->second;
    if (stored != 0 && ImGui::DockBuilderGetNode(stored))
    {
        // Stored dock node is still alive — re-associate the window
        // with it. DockBuilderDockWindow writes to ImGui's persistent
        // window settings so the next Begin picks up the new DockId
        // without us having to SetNextWindowDockID on every frame.
        ImGui::DockBuilderDockWindow(name.c_str(), stored);
    }
    else
    {
        // Dock node has been collapsed/merged away (typical when a
        // panel was the only tab in its node and got hidden during
        // a detach round-trip). Rebuild the layout from defaults so
        // the panel has a valid slot to land in — next frame's
        // buildDefaultLayout runs because _layoutBuilt is false.
        requestLayoutRebuild();
    }
}

void Editor::setAxmolRoot(ax::Node* root)
{
    _axmolRoot = root;
    _axToCsd.clear();
    _csdToAx.clear();
    _axOrderDfs.clear();
    // Detach the old overlay DrawNode from its parent (gTopRoot) so it
    // doesn't accumulate as an orphaned child every time we refresh the
    // ax tree on a structural mutation. drawEditOverlay re-creates it
    // lazily on the next pass.
    if (_overlayDraw)
    {
        _overlayDraw->removeFromParent();
        _overlayDraw = nullptr;
    }
    _needsFitToContent = true;
    if (!_doc || !root) return;

    // Walk both trees in parallel DFS. CSLoader builds the axmol tree from
    // the compiled .csb in the same structural order as the .csd, so the
    // i-th child on each side should correspond. If the counts differ we
    // just stop — mismatched subtrees are silently skipped.
    std::function<void(ax::Node*, const Node&)> walk =
        [&](ax::Node* ax, const Node& csd)
    {
        _axToCsd[ax] = csd;
        _csdToAx[csd.element().internal_object()] = ax;
        _axOrderDfs.push_back(ax);

        const auto& axKids = ax->getChildren();
        auto csdKids = csd.children();
        const size_t n = std::min<size_t>(axKids.size(), csdKids.size());
        for (size_t i = 0; i < n; ++i)
        {
            walk(axKids.at((ssize_t)i), csdKids[i]);
        }
    };
    walk(root, _doc->rootNode);
    // OCSExtension: post-walk attach `VectorMapNode` instances to any
    // ax node whose csd counterpart is `VectorMapObjectData`. Same
    // call runs from `syncNodeCsdToAx` on incremental edits; this
    // batch handles the initial load + structural-mutation rebuilds.
    for (auto& [axN, csdN] : _axToCsd)
    {
        syncVectorMapToAx(csdN, axN);
        syncLitSpriteToAx(csdN, axN, _assetsRoot);
    }
    applyModeToScene();
}

namespace
{
/// Write an X/Y pair into a <Tag> sub-element, preserving any other attrs
/// already present.
void writeXYSub(opencs::Node& n, const char* tag, float x, float y)
{
    char bx[32], by[32];
    std::snprintf(bx, sizeof(bx), "%.4f", x);
    std::snprintf(by, sizeof(by), "%.4f", y);
    auto attrs = n.subAttrs(tag);
    std::vector<std::pair<std::string, std::string>> next;
    bool wX = false, wY = false;
    for (const auto& [k, v] : attrs)
    {
        if (k == "X")       { next.emplace_back(k, bx); wX = true; }
        else if (k == "Y")  { next.emplace_back(k, by); wY = true; }
        else                  next.emplace_back(k, v);
    }
    if (!wX) next.emplace_back("X", bx);
    if (!wY) next.emplace_back("Y", by);
    n.setSubAttrs(tag, next);
}
}  // namespace

// Forward-decl for the anon-namespace helper defined further down,
// so drawSceneOverlay can call it when the mode switches away from
// LayerSide/Top and any per-node layer-rect children need wiping.
namespace { void clearLayerProjectionOverlays(
    const std::vector<ax::Node*>& order); }

void Editor::drawSceneOverlay()
{
    // Wipe any lines left over from the previous frame/mode so View
    // mode has NO overlay (pure passthrough) and Side/Top modes don't
    // leak the axis-aligned Edit-mode boxes into the tilted view.
    // DrawNode accumulates draw calls until explicitly cleared —
    // without this line the last set of Edit-mode rectangles would
    // stay rendered after a mode switch.
    if (_overlayDraw) _overlayDraw->clear();

    if (!_axmolRoot)            { _editLastBail = "no-axmol-root"; return; }
    if (_axOrderDfs.empty())    { _editLastBail = "empty-dfs";     return; }
    // View mode is pure passthrough — no editor-side overlays at
    // all, so make sure we zero out anything a previous Side/Top
    // mode left attached to individual scene nodes.
    if (_mode == Mode::View)
    {
        clearLayerProjectionOverlays(_axOrderDfs);
        return;
    }
    if (_mode == Mode::LayerSide) { drawLayerSideOverlay(); return; }
    if (_mode == Mode::LayerTop)  { drawLayerTopOverlay();  return; }
    // Edit mode: wipe any Side/Top layer-rect leftovers too so
    // switching from a tilted view back to Edit doesn't leave a
    // rainbow of projection quads pinned to the scene nodes.
    clearLayerProjectionOverlays(_axOrderDfs);
    drawEditOverlay();
}

void Editor::drawEditOverlay()
{
    ++_editOverlayRuns;

    auto* view = ax::Director::getInstance()->getRenderView();
    const float scaleX = view->getScaleX();
    const float scaleY = view->getScaleY();
    const auto vpRect  = view->getViewPortRect();
    const auto fbSize  = view->getFrameSize();
    ImGuiIO& io = ImGui::GetIO();
    const float fbs = io.DisplayFramebufferScale.x;
    if (scaleX <= 0 || scaleY <= 0 || fbs <= 0) return;

    // Strip/restore the shared top-root's transform so clicks and
    // world coords live in the same frame. The linear viewport math
    // below maps between design-coords (= topRoot's local frame) and
    // display pixels; `convertToWorldSpace` on any scene node walks
    // through topRoot.transform, so its output is post-transform
    // world. We bridge the two by composing topRoot.
    ax::Node* topRootForMath = nullptr;
    if (auto* s = ax::Director::getInstance()->getRunningScene())
        topRootForMath = s->getChildByTag(0xCADCA12A);
    // Scene-world ↔ screen-points conversion. The view's `getScaleX/Y`
    // and `getViewPortRect` map SCENE-world coords (which already
    // include gTopRoot's zoom/pan transform) directly to framebuffer
    // OS-points — they are NOT a design-space-to-pixel scale on top
    // of which we'd add gTopRoot's scale. Two earlier bugs lived
    // together here:
    //   (1) `convertToNodeSpace(w)` undid gTopRoot's transform, so the
    //       view scale was applied to pre-zoom design coords. With any
    //       zoom != 1 the projected rect drifted from the rendered
    //       position by exactly the zoom factor.
    //   (2) Dividing/multiplying by `DisplayFramebufferScale` halved
    //       (or doubled) every result on Retina, since axmol's view
    //       reports its frame numbers in OS points already.
    // Skip both: feed the world coord straight through the viewport
    // affine and we end up in the same screen-points space as ImGui's
    // mouse position.
    (void)topRootForMath;
    (void)fbs;
    auto worldToScreen = [&](const ax::Vec2& w) -> ImVec2
    {
        const float fbX = w.x * scaleX + vpRect.origin.x;
        const float fbY = w.y * scaleY + vpRect.origin.y;
        return ImVec2(fbX, fbSize.height - fbY);
    };
    auto screenToWorld = [&](const ImVec2& s) -> ax::Vec2
    {
        const float fbX = s.x;
        const float fbY = fbSize.height - s.y;
        return ax::Vec2((fbX - vpRect.origin.x) / scaleX,
                        (fbY - vpRect.origin.y) / scaleY);
    };

    auto* dl = ImGui::GetBackgroundDrawList();

    // Ground-truth overlay via axmol DrawNode. Lives under the shared
    // top-root (tag 0xCADCA12A, a sibling of loadedRoot) so it picks
    // up every transform applied to the canvas. Lines drawn in that
    // node's local frame render through the same pipeline as the
    // scene content, no linear-projection shortcut involved.
    ax::Node* topRoot = nullptr;
    if (auto* s = ax::Director::getInstance()->getRunningScene())
        topRoot = s->getChildByTag(0xCADCA12A);
    if (topRoot && !_overlayDraw)
    {
        _overlayDraw = ax::DrawNode::create();
        _overlayDraw->setLocalZOrder(0x7fffffff);  // front of siblings
        _overlayDraw->setGlobalZOrder(100.0f);     // on top of content
        topRoot->addChild(_overlayDraw);
    }
    // Clear is done once per frame by drawSceneOverlay, so anything we
    // append below is the complete state for this pass.

    const ImU32 kNormal   = IM_COL32(144, 238, 144, 255);  // light green
    const ImU32 kSelected = IM_COL32(230,  40,  40, 255);  // red — primary
    const ImU32 kMultiSel = IM_COL32(245, 140,  60, 255);  // amber — extra
    const ImU32 kMoveIcon = IM_COL32( 60, 140, 240, 255);  // blue
    const ImU32 kSizeIcon = IM_COL32(240, 160,  20, 255);  // orange
    const ImU32 kIconEdge = IM_COL32(  0,   0,   0, 220);  // dark outline
    const ImU32 kRubber   = IM_COL32( 90, 160, 255, 120);  // rubber-band fill
    // Target handle radius in DISPLAY pixels. We divide by the
    // effective draw scale below, so the rendered dot always measures
    // this many screen pixels regardless of zoom.
    constexpr float kIconR  = 6.0f;
    // Rectangle-outline thickness in DISPLAY pixels. Same story —
    // `drawSegment` takes a radius in local units; we convert from
    // target pixels via the effective scale so the stroke stays
    // visually constant when you zoom in or out.
    constexpr float kRectPx = 2.0f;

    // Screen-space rects for the two handles of the selected node — computed
    // during the draw pass so the mouse hit-test below uses the exact same
    // geometry the user sees.
    bool haveHandles = false;
    ImVec2 moveHandle(0, 0), sizeHandle(0, 0);

    for (ax::Node* node : _axOrderDfs)
    {
        if (!node->isVisible()) continue;
        const auto cs = node->getContentSize();
        if (cs.width <= 0 || cs.height <= 0) continue;

        // Four local-space corners (BL, BR, TR, TL) → world-space via the
        // node's full transform (position + rotation + scale) so the overlay
        // quad tracks rotation instead of staying axis-aligned.
        const auto w0 = node->convertToWorldSpace(ax::Vec2(0,         0        ));
        const auto w1 = node->convertToWorldSpace(ax::Vec2(cs.width,  0        ));
        const auto w2 = node->convertToWorldSpace(ax::Vec2(cs.width,  cs.height));
        const auto w3 = node->convertToWorldSpace(ax::Vec2(0,         cs.height));
        const ImVec2 s0 = worldToScreen(w0);
        const ImVec2 s1 = worldToScreen(w1);
        const ImVec2 s2 = worldToScreen(w2);
        const ImVec2 s3 = worldToScreen(w3);

        // Three-way outline: primary selection (red + thick), extra
        // selection member from shift-click/rubber-band (amber + thick),
        // everything else (green + thin). Only the primary gets handles.
        const Node& asCsd = _axToCsd[node];
        const bool isPrimary = _selected.valid() &&
            _selected.element() == asCsd.element();
        const bool isExtra = !isPrimary && isSelected(asCsd);
        const ImU32 col =
            isPrimary ? kSelected : (isExtra ? kMultiSel : kNormal);
        const float thickness = (isPrimary || isExtra) ? 2.5f : 1.5f;
        // Per-node handles render only for single-selection. Multi-
        // select gets ONE combined handle (the blue dot at the
        // bbox center) drawn after the loop. Suppress per-node
        // primary handles when the selection set has more than one
        // member so the user doesn't see redundant dots.
        const bool selected =
            isPrimary && _selection.size() <= 1;
        // Selection outlines render through the axmol DrawNode below —
        // its lines compose with the same transform chain the content
        // does, so nothing drifts out of alignment under pan/zoom/
        // rotation. ImGui's line drawlist is kept only for the handles
        // (blue/orange dots) that need UI-level hit testing.
        (void)dl; (void)s0; (void)s1; (void)s2; (void)s3;
        (void)col; (void)thickness;
        // Per-node overlay DrawNode — a tagged child of the scene
        // node itself, so it inherits every parent-chain transform
        // (including the widget renderer's effective z=0 once we
        // cancel the node's own positionZ). Same tag as LayerSide/
        // Top: only one mode is active at a time and each draw is
        // an explicit clear + redraw.
        constexpr int kOverlayTag = 0x4C594F56;   // 'LYOV'
        ax::DrawNode* over = dynamic_cast<ax::DrawNode*>(
            node->getChildByTag(kOverlayTag));
        if (!over)
        {
            over = ax::DrawNode::create();
            over->setTag(kOverlayTag);
            over->setLocalZOrder(0x7fffffff);
            over->setGlobalZOrder(100.0f);
            node->addChild(over);
        }
        over->clear();
        over->setVisible(true);
        // Cancel out the node's positionZ so the overlay renders at
        // world-z = 0 — matching axmol's widget renderer which
        // issues its draw commands at a flat depth regardless of
        // parent positionZ. Keeps rotation visually in sync.
        over->setPositionZ(-node->getPositionZ());

        const ax::Color4F axCol = isPrimary
            ? ax::Color4F(0.90f, 0.16f, 0.16f, 1.0f)     // red
            : (isExtra
                ? ax::Color4F(0.96f, 0.55f, 0.24f, 1.0f) // amber
                : ax::Color4F(0.56f, 0.93f, 0.56f, 1.0f)); // green
        // Cumulative node-to-world scale — walking the parent chain
        // so the per-node overlay compensates for nested node scales
        // (widget/image have their own Scale attribute, distinct
        // from gTopRoot's view zoom). Display-pixel size is held
        // constant by dividing target sizes by this.
        auto nodeWorldScaleX = [](ax::Node* n) {
            float s = 1.0f;
            for (ax::Node* cur = n; cur; cur = cur->getParent())
                s *= cur->getScaleX();
            return s;
        };
        const float nodeWS   = nodeWorldScaleX(node);
        const float pxPerLocal =
            (nodeWS * scaleX / fbs > 0.0001f)
                ? nodeWS * scaleX / fbs : 1.0f;

        // Use drawSegment instead of drawLine for a consistent
        // thickness — drawLine is always 1 GL pixel (hairline at
        // any zoom); drawSegment takes a radius we scale against
        // the display-to-world factor so the stroke stays visually
        // constant under zoom AND under node scale.
        const float segR = (kRectPx * 0.5f) / pxPerLocal;
        const ax::Vec2 p0(0,        0        );
        const ax::Vec2 p1(cs.width, 0        );
        const ax::Vec2 p2(cs.width, cs.height);
        const ax::Vec2 p3(0,        cs.height);
        over->drawSegment(p0, p1, segR, axCol);
        over->drawSegment(p1, p2, segR, axCol);
        over->drawSegment(p2, p3, segR, axCol);
        over->drawSegment(p3, p0, segR, axCol);

        if (selected)
        {
            // Blue dot sits at the node's anchor point — this is axmol's
            // actual pivot for rotation and scaling, so the dot stays put
            // during both operations (which was the user's expectation).
            // The orange resize grip stays at the local (w, h) corner.
            const auto anchorLocal = node->getAnchorPointInPoints();
            const auto wA = node->convertToWorldSpace(anchorLocal);
            moveHandle = worldToScreen(wA);                   // blue dot
            sizeHandle = s2;                                  // orange dot
            haveHandles = true;

            // Hover-scale: when the cursor is over a handle (or the handle
            // is actively being dragged), render it 10% larger as a visual
            // cue that the action is available. Distance check uses the
            // same radius tolerance as the click test.
            const ImVec2 mouse = ImGui::GetMousePos();
            auto distSqr = [&](const ImVec2& c) {
                return (mouse.x - c.x)*(mouse.x - c.x) +
                       (mouse.y - c.y)*(mouse.y - c.y);
            };
            const float hoverR2 = (kIconR + 3.0f) * (kIconR + 3.0f);
            const bool moveHot =
                distSqr(moveHandle) <= hoverR2 ||
                _dragMode == DragMode::Move       ||
                _dragMode == DragMode::AnchorEdit;
            const bool sizeHot =
                distSqr(sizeHandle) <= hoverR2 ||
                _dragMode == DragMode::ResizeTL   ||
                _dragMode == DragMode::ResizeTR   ||
                _dragMode == DragMode::ResizeBR   ||
                _dragMode == DragMode::ResizeBL   ||
                _dragMode == DragMode::Rotate;

            // Control bubbles render through ImGui's foreground draw
            // list at their already-computed screen positions, so
            // they sit above ALL axmol scene content unconditionally
            // — including elements with globalZOrder ≥ 100 that would
            // otherwise occlude a per-node DrawNode rendering. The
            // rectangle outline still draws into the per-node
            // DrawNode (above), so the box follows the node's
            // rotation; only the move + resize dots float on top.
            //
            // Clip the foreground emissions to the canvas central
            // rect so they don't bleed over the surrounding panels
            // when a node sits near the edge of the canvas.
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const ImVec2 canvasMin(_canvasRectX, _canvasRectY);
            const ImVec2 canvasMax(_canvasRectX + _canvasRectW,
                                    _canvasRectY + _canvasRectH);
            const bool canvasRectValid =
                (_canvasRectW > 0 && _canvasRectH > 0);
            if (canvasRectValid)
                fg->PushClipRect(canvasMin, canvasMax, true);
            auto drawDotScreen = [&](const ImVec2& s, ImU32 fill, bool hot) {
                const float rr = (hot ? 1.1f : 1.0f) * kIconR;
                fg->AddCircleFilled(s, rr, fill, 18);
                fg->AddCircle(s, rr,
                              IM_COL32(0, 0, 0, 220), 18, 1.5f);
            };
            drawDotScreen(moveHandle, kMoveIcon, moveHot);
            drawDotScreen(sizeHandle, kSizeIcon, sizeHot);

            // X overlay when the selected node has auto-computed
            // contentSize and the user just plain-clicked the orange
            // handle. Warns that Size-based resize is a no-op here —
            // they need Ctrl+drag. Drawn on the foreground draw list
            // for the same "always on top" reason as the dots.
            if (ImGui::GetTime() < _orangeFlashUntil)
            {
                const float r = 0.8f * kIconR;
                const ImU32 col = IM_COL32(230, 40, 40, 255);
                fg->AddLine(ImVec2(sizeHandle.x - r, sizeHandle.y - r),
                            ImVec2(sizeHandle.x + r, sizeHandle.y + r),
                            col, 2.0f);
                fg->AddLine(ImVec2(sizeHandle.x + r, sizeHandle.y - r),
                            ImVec2(sizeHandle.x - r, sizeHandle.y + r),
                            col, 2.0f);
            }

            // Alt held while an item is selected → show a rotation-hint
            // icon (curved arrow) next to the orange dot so the user knows
            // clicking will start a rotate drag instead of a resize.
            const auto& hIO = ImGui::GetIO();
            const bool cmdLike = hIO.KeyCtrl || hIO.KeySuper;
            // Cmd/Ctrl held → anchored-scale hint. Blue disk with a
            // diagonal double-arrow (↔ rotated) so the user can tell
            // scaling will be constrained (Size + Position unchanged, only
            // Scale sub-element moves).
            // Modifier hints render as proper icons in the dot's
            // colour (no disk background) so they feel like glyphs
            // rather than dots. Each is drawn twice — first a thick
            // dark "halo" stroke for visibility against bright sprite
            // backgrounds, then the coloured stroke on top. Footprint
            // matches the kIconR move/resize dots.
            if (cmdLike && !hIO.KeyAlt)
            {
                const float r = kIconR;
                const ImVec2 c(sizeHandle.x + kIconR + r + 4.0f,
                               sizeHandle.y - kIconR - r + 2.0f);
                const ImU32 blue   = IM_COL32( 60, 140, 240, 255);
                const ImU32 halo   = IM_COL32(  0,   0,   0, 200);

                // Scale icon = double-headed diagonal arrow ↗↙.
                const float d  = r * 0.95f;
                const float ah = r * 0.50f;
                const ImVec2 tipNE(c.x + d, c.y - d);
                const ImVec2 tipSW(c.x - d, c.y + d);
                auto stroke = [&](ImU32 col, float thick) {
                    fg->AddLine(tipSW, tipNE, col, thick);
                    fg->AddTriangleFilled(
                        tipNE,
                        ImVec2(tipNE.x - ah, tipNE.y),
                        ImVec2(tipNE.x,      tipNE.y + ah), col);
                    fg->AddTriangleFilled(
                        tipSW,
                        ImVec2(tipSW.x + ah, tipSW.y),
                        ImVec2(tipSW.x,      tipSW.y - ah), col);
                };
                stroke(halo, 3.0f);
                stroke(blue, 1.6f);
            }

            if (hIO.KeyAlt)
            {
                const float r = kIconR;
                const ImVec2 c(sizeHandle.x + kIconR + r + 4.0f,
                               sizeHandle.y - kIconR - r + 2.0f);
                const ImU32 red    = IM_COL32(230,  40,  40, 255);
                const ImU32 halo   = IM_COL32(  0,   0,   0, 200);

                // Rotate icon = 3/4 circular arrow ↻.
                constexpr float kPi  = 3.14159265f;
                const float    arcR = r * 0.95f;
                const float    a0   = -kPi * 0.20f;
                const float    a1   =  kPi * 1.25f;
                const float    cosT = std::cos(a1);
                const float    sinT = std::sin(a1);
                const ImVec2   tip(c.x + arcR * cosT, c.y + arcR * sinT);
                const float    tang = a1 + kPi * 0.5f;
                const float    tCos = std::cos(tang);
                const float    tSin = std::sin(tang);
                const float    hs   = r * 0.55f;
                const ImVec2   ah1(tip.x + tCos * hs - sinT * hs,
                                   tip.y + tSin * hs + cosT * hs);
                const ImVec2   ah2(tip.x + tCos * hs + sinT * hs,
                                   tip.y + tSin * hs - cosT * hs);
                auto stroke = [&](ImU32 col, float thick) {
                    fg->PathArcTo(c, arcR, a0, a1, 22);
                    fg->PathStroke(col, 0, thick);
                    fg->AddTriangleFilled(tip, ah1, ah2, col);
                };
                stroke(halo, 3.0f);
                stroke(red,  1.6f);
            }

            // Match the PushClipRect that wraps every bubble
            // emission so the foreground draw list returns to its
            // unclipped state for any later consumers.
            if (canvasRectValid) fg->PopClipRect();
        }
    }

    // --- Multi-selection group handle ---------------------------------------
    // When 2+ nodes are selected, the per-node primary handles are
    // suppressed (`selected = isPrimary && _selection.size() <= 1`
    // above). Instead, draw ONE blue dot at the centre of the
    // combined world-space AABB of the whole selection. Clicking +
    // dragging that dot moves the entire group via the existing
    // Move drag-mode (which already supports group translate via
    // _dragStartPositions).
    ImVec2 multiHandle(0, 0);
    bool   haveMultiHandle = false;
    if (_selection.size() > 1)
    {
        float mnX =  FLT_MAX, mnY =  FLT_MAX;
        float mxX = -FLT_MAX, mxY = -FLT_MAX;
        bool any = false;
        for (const auto& sn : _selection)
        {
            auto it = _csdToAx.find(sn.element().internal_object());
            if (it == _csdToAx.end() || !it->second) continue;
            ax::Node* n = it->second;
            const auto cs = n->getContentSize();
            const ax::Vec2 corners[4] = {
                n->convertToWorldSpace(ax::Vec2(0,         0)),
                n->convertToWorldSpace(ax::Vec2(cs.width,  0)),
                n->convertToWorldSpace(ax::Vec2(cs.width,  cs.height)),
                n->convertToWorldSpace(ax::Vec2(0,         cs.height)),
            };
            for (auto& c : corners)
            {
                mnX = std::min(mnX, c.x); mnY = std::min(mnY, c.y);
                mxX = std::max(mxX, c.x); mxY = std::max(mxY, c.y);
            }
            any = true;
        }
        if (any)
        {
            const ax::Vec2 centerW((mnX + mxX) * 0.5f,
                                   (mnY + mxY) * 0.5f);
            multiHandle = worldToScreen(centerW);
            haveMultiHandle = true;

            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const ImVec2 canvasMin(_canvasRectX, _canvasRectY);
            const ImVec2 canvasMax(_canvasRectX + _canvasRectW,
                                    _canvasRectY + _canvasRectH);
            const bool canvasRectValid =
                (_canvasRectW > 0 && _canvasRectH > 0);
            if (canvasRectValid)
                fg->PushClipRect(canvasMin, canvasMax, true);

            const ImVec2 mouse = ImGui::GetMousePos();
            const float dx = mouse.x - multiHandle.x;
            const float dy = mouse.y - multiHandle.y;
            const float hoverR = kIconR + 4.0f;
            const bool hot = (dx*dx + dy*dy) <= hoverR*hoverR ||
                             _dragMode == DragMode::Move;
            const float rr = (hot ? 1.15f : 1.0f) * (kIconR + 2.0f);
            // Slightly larger than the per-node move dot so the group
            // handle reads as the dominant control. Same blue tone.
            fg->AddCircleFilled(multiHandle, rr, kMoveIcon, 20);
            fg->AddCircle(multiHandle, rr,
                          IM_COL32(0, 0, 0, 220), 20, 1.8f);

            // Faint AABB outline to show the group extent.
            const ImVec2 a = worldToScreen(ax::Vec2(mnX, mnY));
            const ImVec2 b = worldToScreen(ax::Vec2(mxX, mxY));
            const ImVec2 aabbMin(std::min(a.x, b.x), std::min(a.y, b.y));
            const ImVec2 aabbMax(std::max(a.x, b.x), std::max(a.y, b.y));
            fg->AddRect(aabbMin, aabbMax,
                        IM_COL32(60, 140, 240, 180), 0.0f, 0, 1.0f);

            if (canvasRectValid) fg->PopClipRect();
        }
    }

    // --- Mouse pick + drag --------------------------------------------------
    // The dockspace host window covers the full viewport and has
    // PassthruCentralNode, so io.WantCaptureMouse reports true even over the
    // central region. Instead, gate on: inside-central AND no ImGui item or
    // non-host window is hovered under the cursor.
    auto* central = ImGui::DockBuilderGetCentralNode(_dockspaceId);
    if (!central) { ++_editOverlayBails; _editLastBail = "no-central-node"; return; }

    const ImVec2 m = ImGui::GetMousePos();
    const bool inCentral =
        m.x >= central->Pos.x && m.x < central->Pos.x + central->Size.x &&
        m.y >= central->Pos.y && m.y < central->Pos.y + central->Size.y;

    // On macOS the OS translates Ctrl+left-click into right-click, so
    // treat button 1 as a click too when Ctrl/Cmd is held. Without this the
    // orange dot never starts a drag under Ctrl.
    const auto& cIO = ImGui::GetIO();
    const bool cmdLikeHeld = cIO.KeyCtrl || cIO.KeySuper;
    // Pan gesture owns the click when active — skip all handle / body /
    // click routing so Space+drag and middle-drag don't also start a
    // node edit.
    const bool panGesture =
        (!cIO.WantTextInput && ImGui::IsKeyDown(ImGuiKey_Space)
         && ImGui::IsMouseDown(0)) ||
        ImGui::IsMouseDown(2);
    const bool clickedThisFrame =
        !panGesture &&
        (ImGui::IsMouseClicked(0) ||
         (cmdLikeHeld && ImGui::IsMouseClicked(1)));
    const bool anyHover = ImGui::IsAnyItemHovered();
    auto* hoverWin = ImGui::GetCurrentContext()->HoveredWindow;
    const bool isDockspaceHost = hoverWin ?
        ((hoverWin->Flags & ImGuiWindowFlags_DockNodeHost) != 0) : true;

    auto recordEvent = [&](const char* kind, const char* hitPath = "",
                           const char* hitName = "") {
        if (!clickedThisFrame) return;
        ClickEvent ev;
        ev.time = ImGui::GetTime();
        ev.x = m.x; ev.y = m.y;
        ev.inCentral = inCentral;
        ev.hoverIsPanel = (hoverWin && !isDockspaceHost);
        ev.anyItemHovered = anyHover;
        ev.passedGate = true;
        ev.hitPath = hitPath;
        ev.hitName = hitName;
        ev.kind = kind;
        ev.hoverWinName = hoverWin ? hoverWin->Name : "";
        ev.hoveredId = ImGui::GetCurrentContext()->HoveredId;
        _eventLog.push_back(std::move(ev));
        while (_eventLog.size() > kEventLogCap) _eventLog.pop_front();
    };

    // Gate: the selection-layer InvisibleButton (submitted earlier in
    // this frame, see the central-node block) tells us directly whether
    // the canvas is the active input target. Hovered = cursor is on the
    // canvas and no floating widget / panel sits above it. When a drag
    // is already in-flight we keep processing regardless.
    if (_dragMode == DragMode::None && !_canvasHovered)
    {
        recordEvent("gate:canvas-not-hovered"); return;
    }

    auto onIcon = [&](const ImVec2& c) {
        const float dx = m.x - c.x, dy = m.y - c.y;
        return (dx * dx + dy * dy) <= (kIconR + 3.0f) * (kIconR + 3.0f);
    };

    if (clickedThisFrame)
    {
        _dragMode = DragMode::None;
        const ax::Vec2 world = screenToWorld(m);

        // 0. Multi-selection group handle? Click anywhere within the
        // blue dot drags every selected node by the same world-space
        // delta — reuses the per-node Move drag-mode plumbing, which
        // already records starting positions for the whole _selection
        // set via _dragStartPositions.
        if (haveMultiHandle && _selected.valid() && onIcon(multiHandle))
        {
            auto it = _csdToAx.find(_selected.element().internal_object());
            if (it != _csdToAx.end() && it->second)
            {
                pushUndo();
                const auto anchorWorld = it->second->convertToWorldSpace(
                    it->second->getAnchorPointInPoints());
                _dragPickupOffsetX = world.x - anchorWorld.x;
                _dragPickupOffsetY = world.y - anchorWorld.y;
                _dragMode = DragMode::Move;
                _dragStartMouseX = world.x;  _dragStartMouseY = world.y;
                _dragStartNodeX  = it->second->getPositionX();
                _dragStartNodeY  = it->second->getPositionY();
                _dragStartPositions.clear();
                for (const auto& sn : _selection)
                {
                    if (sn.element() == _selected.element()) continue;
                    auto cit = _csdToAx.find(sn.element().internal_object());
                    if (cit == _csdToAx.end() || !cit->second) continue;
                    _dragStartPositions.emplace_back(
                        cit->second,
                        std::make_pair(cit->second->getPositionX(),
                                       cit->second->getPositionY()));
                }
                recordEvent("multi-move");
            }
        }
        // 1. Anchor / Move icon?
        else if (haveHandles && _selected.valid() && onIcon(moveHandle))
        {
            auto it = _csdToAx.find(_selected.element().internal_object());
            if (it != _csdToAx.end() && it->second)
            {
                pushUndo();   // one snapshot per drag so Cmd+Z reverts it.

                // Pickup offset: distance from the cursor to the node's
                // anchor-world-position at click-time. We keep this offset
                // constant during the drag so the node doesn't snap to the
                // cursor — the exact grab point rides under the mouse.
                const auto anchorWorld = it->second->convertToWorldSpace(
                    it->second->getAnchorPointInPoints());
                _dragPickupOffsetX = world.x - anchorWorld.x;
                _dragPickupOffsetY = world.y - anchorWorld.y;

                if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper)
                {
                    // Cmd/Ctrl held → AnchorEdit. Capture the element's
                    // origin in PARENT-local coords (not world): axmol's
                    // setPosition operates in parent-local, so keeping the
                    // origin stable there is what actually holds the visual
                    // still — world coords drift whenever any ancestor has
                    // a transform, which is especially common for Text
                    // nodes that sit inside nested panels.
                    _dragMode = DragMode::AnchorEdit;
                    _dragStartMouseX = world.x;  _dragStartMouseY = world.y;
                    const auto originWorld = it->second->convertToWorldSpace(
                        ax::Vec2(0, 0));
                    ax::Node* par = it->second->getParent();
                    const ax::Vec2 originParent = par
                        ? par->convertToNodeSpace(originWorld)
                        : originWorld;
                    _dragStartNodeX  = originParent.x;
                    _dragStartNodeY  = originParent.y;
                    const auto scs = it->second->getContentSize();
                    _dragStartSizeW  = scs.width;
                    _dragStartSizeH  = scs.height;
                    _dragStartScaleX = std::fabs(it->second->getScaleX());
                    _dragStartScaleY = std::fabs(it->second->getScaleY());
                    if (_dragStartScaleX <= 0.01f) _dragStartScaleX = 1.0f;
                    if (_dragStartScaleY <= 0.01f) _dragStartScaleY = 1.0f;
                    recordEvent("anchor-edit");
                }
                else
                {
                    _dragMode = DragMode::Move;
                    _dragStartMouseX = world.x;  _dragStartMouseY = world.y;
                    _dragStartNodeX  = it->second->getPositionX();
                    _dragStartNodeY  = it->second->getPositionY();
                    // Group translate: snapshot every OTHER selected node's
                    // starting position so the Move update can shift all of
                    // them by the same world-space delta.
                    _dragStartPositions.clear();
                    for (const auto& sn : _selection)
                    {
                        if (sn.element() == _selected.element()) continue;
                        auto cit = _csdToAx.find(sn.element().internal_object());
                        if (cit == _csdToAx.end() || !cit->second) continue;
                        _dragStartPositions.emplace_back(
                            cit->second,
                            std::make_pair(cit->second->getPositionX(),
                                           cit->second->getPositionY()));
                    }
                    recordEvent("move-icon");
                }
            }
        }
        // 2. Resize icon?
        else if (haveHandles && _selected.valid() && onIcon(sizeHandle))
        {
            auto it = _csdToAx.find(_selected.element().internal_object());
            if (it != _csdToAx.end() && it->second)
            {
                const auto cs = it->second->getContentSize();
                _dragStartMouseX = world.x;  _dragStartMouseY = world.y;
                _dragStartSizeW  = cs.width;
                _dragStartSizeH  = cs.height;
                _dragStartNodeX  = it->second->getPositionX();
                _dragStartNodeY  = it->second->getPositionY();
                _dragStartScaleX = std::fabs(it->second->getScaleX());
                _dragStartScaleY = std::fabs(it->second->getScaleY());
                if (_dragStartScaleX <= 0.01f) _dragStartScaleX = 1.0f;
                if (_dragStartScaleY <= 0.01f) _dragStartScaleY = 1.0f;
                // Snapshot on-screen size for the Cmd/Ctrl scale path
                // so the per-px-of-mouse → per-px-of-sprite-growth
                // ratio stays constant across the drag (otherwise
                // screenSize grows with scale and the bubble drifts
                // ahead of the cursor).
                {
                    const auto w0 = it->second->convertToWorldSpace(
                        ax::Vec2(0, 0));
                    const auto w1 = it->second->convertToWorldSpace(
                        ax::Vec2(cs.width, cs.height));
                    const ImVec2 s0 = worldToScreen(w0);
                    const ImVec2 s1 = worldToScreen(w1);
                    _dragStartScreenSizeW = std::max(20.0f,
                        std::fabs(s1.x - s0.x));
                    _dragStartScreenSizeH = std::max(20.0f,
                        std::fabs(s1.y - s0.y));
                }

                pushUndo();   // one snapshot per drag so Cmd+Z reverts it.
                // Modifiers are latched at click-time so toggling them
                // mid-drag doesn't swap modes unexpectedly.
                const auto& cio = ImGui::GetIO();
                if (cio.KeyAlt)
                {
                    const auto origin = it->second->convertToWorldSpace(ax::Vec2(0, 0));
                    _dragStartAngleRad =
                        std::atan2f(world.y - origin.y, world.x - origin.x);
                    _dragStartRotDeg = it->second->getRotation();
                    _dragMode = DragMode::Rotate;
                    recordEvent("rotate-icon");
                }
                else
                {
                    _dragMode       = DragMode::ResizeBR;
                    _dragScaleOnly  = cio.KeyCtrl || cio.KeySuper;
                    recordEvent(_dragScaleOnly ? "scale-only-icon"
                                               : "resize-icon");
                    // Nodes whose ctype auto-computes its contentSize
                    // (Text / Label varieties) will ignore Size changes
                    // made by the plain-resize drag. Flash an X over the
                    // orange dot for a moment so the user knows they need
                    // Ctrl (scale-only) for these.
                    if (!_dragScaleOnly)
                    {
                        const auto ct = _selected.ctype();
                        const bool autoSized =
                            ct.find("Text") != std::string::npos ||
                            ct.find("Label") != std::string::npos;
                        if (autoSized)
                            _orangeFlashUntil = ImGui::GetTime() + 0.6;
                    }
                }
            }
        }
        // 3. Body click → always arm the selection marquee at the click
        //    point. The selection widget (L3) owns every non-handle
        //    click on the canvas now, regardless of whether a scene leaf
        //    happens to be under the cursor. The click-vs-drag decision
        //    (single-click-select-leaf vs commit-rubber-band) is
        //    deferred to release, so:
        //      · release without moving  → treat as a single click,
        //        hit-test the start position and select the topmost
        //        leaf there (or clear selection on empty / additive-on).
        //      · release after a drag    → commit the marquee overlap
        //        as the selection.
        //    Modifier (Shift / Ctrl / Cmd) is latched here and honoured
        //    on release so the gesture stays consistent even if the
        //    user drops the modifier mid-drag.
        else
        {
            const auto& io = ImGui::GetIO();
            _rubberAdditive     = io.KeyShift || io.KeyCtrl || io.KeySuper;
            _rubberActive       = true;
            _rubberStartScreenX = m.x;
            _rubberStartScreenY = m.y;
            recordEvent("rubber-armed");
        }
    }

    // Drag updates (move or resize).
    const bool anyDown =
        ImGui::IsMouseDown(0) ||
        (cmdLikeHeld && ImGui::IsMouseDown(1));
    if (_dragMode != DragMode::None && anyDown && _selected.valid())
    {
        auto iter = _csdToAx.find(_selected.element().internal_object());
        if (iter != _csdToAx.end() && iter->second)
        {
            ax::Node* axNode = iter->second;
            const ax::Vec2 world = screenToWorld(m);

            // The modifier (Cmd/Ctrl) is latched at click-time. During the
            // drag itself we no longer flip between Move and AnchorEdit —
            // that caused the blue dot to "separate" from the sprite when
            // the user pressed Cmd while moving it.

            const float dx = world.x - _dragStartMouseX;
            const float dy = world.y - _dragStartMouseY;

            if (_dragMode == DragMode::Move)
            {
                // Target anchor-world = cursor − pickupOffset (keeps the
                // grab point under the cursor, no snap on first frame).
                const ax::Vec2 targetAnchorWorld(
                    world.x - _dragPickupOffsetX,
                    world.y - _dragPickupOffsetY);
                // setPosition is in parent-local — convert so any ancestor
                // scale / rotation / translation is applied exactly once.
                ax::Node* par = axNode->getParent();
                const ax::Vec2 newPos = par
                    ? par->convertToNodeSpace(targetAnchorWorld)
                    : targetAnchorWorld;
                axNode->setPosition(newPos.x, newPos.y);
                writeXYSub(_selected, "Position", newPos.x, newPos.y);

                // Group translate: each captured member gets the same
                // world-space cursor delta applied to its origin, then
                // converted back into its own parent-local frame. Each
                // member may have a different parent, so the convert pair
                // has to run per-node.
                const float wdx = world.x - _dragStartMouseX;
                const float wdy = world.y - _dragStartMouseY;
                for (auto& [axN, p0] : _dragStartPositions)
                {
                    ax::Node* p = axN->getParent();
                    const ax::Vec2 origin0(p0.first, p0.second);
                    const ax::Vec2 origin0World = p
                        ? p->convertToWorldSpace(origin0)
                        : origin0;
                    const ax::Vec2 origin1World(origin0World.x + wdx,
                                                 origin0World.y + wdy);
                    const ax::Vec2 newP = p
                        ? p->convertToNodeSpace(origin1World)
                        : origin1World;
                    axN->setPosition(newP.x, newP.y);
                    auto bit = _axToCsd.find(axN);
                    if (bit != _axToCsd.end())
                        bit->second.writePair("Position",
                                              "X", newP.x, "Y", newP.y);
                }
            }
            else if (_dragMode == DragMode::AnchorEdit)
            {
                // Ctrl+drag on blue = pivot edit. Element must stay visually
                // static — only the AnchorPoint fraction slides inside the
                // shape and Position shifts to keep the visual pinned.
                //
                // Constants captured at click time:
                //   _dragStartNodeX/Y = origin_world at click (local (0,0))
                //   _dragStartSizeW/H = cs at click
                //   _dragStartScaleX/Y = |scale| at click
                //
                // We clamp the fraction to [0,1] so the cursor can only slide
                // the anchor inside the shape; past the edge the anchor
                // saturates and the visual remains fixed (no accidental
                // sprite drag).
                // Target anchor in PARENT-local coords. Everything below
                // runs in the same coord system as setPosition, so Text
                // widgets that recompute their cs don't drag the origin
                // around.
                const float targetAnchorXW = world.x - _dragPickupOffsetX;
                const float targetAnchorYW = world.y - _dragPickupOffsetY;
                ax::Node* par = axNode->getParent();
                const ax::Vec2 targetAnchorParent = par
                    ? par->convertToNodeSpace(
                          ax::Vec2(targetAnchorXW, targetAnchorYW))
                    : ax::Vec2(targetAnchorXW, targetAnchorYW);

                // Cursor's position inside the *click-time* shape, in the
                // node's own local-pixel space. _dragStartNode[XY] is the
                // origin in parent-local; dividing by click-time scale
                // gives local coords.
                const float sw = std::max(1.0f, _dragStartSizeW);
                const float sh = std::max(1.0f, _dragStartSizeH);
                const float localX = (targetAnchorParent.x - _dragStartNodeX) / _dragStartScaleX;
                const float localY = (targetAnchorParent.y - _dragStartNodeY) / _dragStartScaleY;
                // Anchor fractions are NOT clamped — axmol accepts values
                // outside [0, 1] (pivot sits outside the node's bounding
                // box), which is sometimes useful for off-center rotations.
                const ax::Vec2 newA(localX / sw, localY / sh);

                // Apply the anchor first so auto-sized widgets can recompute
                // their contentSize, then derive the position that keeps
                // the origin locked to _dragStartNode[XY] in parent-local.
                axNode->setAnchorPoint(newA);
                const auto csNow = axNode->getContentSize();
                const float sXNow = std::fabs(axNode->getScaleX());
                const float sYNow = std::fabs(axNode->getScaleY());
                const float newPosX = _dragStartNodeX + newA.x * csNow.width  * sXNow;
                const float newPosY = _dragStartNodeY + newA.y * csNow.height * sYNow;
                // setPosition is parent-local — we're already there.
                ax::Vec2 newPosParent(newPosX, newPosY);
                axNode->setPosition(newPosParent.x, newPosParent.y);

                char bx[32], by[32];
                std::snprintf(bx, sizeof(bx), "%.4f", newA.x);
                std::snprintf(by, sizeof(by), "%.4f", newA.y);
                auto prev = _selected.subAttrs("AnchorPoint");
                std::vector<std::pair<std::string, std::string>> next;
                bool wX = false, wY = false;
                for (const auto& [k, v] : prev)
                {
                    if      (k == "ScaleX") { next.emplace_back(k, bx); wX = true; }
                    else if (k == "ScaleY") { next.emplace_back(k, by); wY = true; }
                    else                      next.emplace_back(k, v);
                }
                if (!wX) next.emplace_back("ScaleX", bx);
                if (!wY) next.emplace_back("ScaleY", by);
                _selected.setSubAttrs("AnchorPoint", next);

                writeXYSub(_selected, "Position", newPosParent.x, newPosParent.y);
            }
            else if (_dragMode == DragMode::Rotate)
            {
                // Angle from node origin in world space; axmol's Rotation is
                // in degrees and clockwise-positive (opposite of math atan2).
                const auto origin = axNode->convertToWorldSpace(ax::Vec2(0, 0));
                const float curAng =
                    std::atan2f(world.y - origin.y, world.x - origin.x);
                const float deltaDeg =
                    (curAng - _dragStartAngleRad) * (180.0f / 3.14159265358979323846f);
                float newDeg = _dragStartRotDeg - deltaDeg;   // flip sign
                // Snap to 15° when Shift is held — standard nudge behavior.
                if (ImGui::GetIO().KeyShift)
                    newDeg = std::round(newDeg / 15.0f) * 15.0f;
                axNode->setRotation(newDeg);

                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.4f", newDeg);
                _selected.setAttr("Rotation", buf);
            }
            else  // any ResizeXX — we only use BR now
            {
                const auto an = axNode->getAnchorPoint();

                if (_dragScaleOnly)
                {
                    // Cmd/Ctrl+drag = scale-only. Size and Position stay;
                    // only the Scale sub-element moves. Axmol scales
                    // around the node's anchor so the anchor's parent
                    // position also stays exactly put.
                    //
                    // Scale rate is keyed off the sprite's ON-SCREEN
                    // pixel size at drag-start, not its contentSize.
                    // Previously the formula was world-coord based
                    // (dx_world / contentSize), which produced very
                    // slow scaling on large sprites because the
                    // canvas zoom + ancestor scale compress
                    // screen→world by 3–10×. Switching to screen-
                    // delta / screen-size makes 1 px of mouse travel
                    // map to 1 px of on-screen sprite growth.
                    // Use the drag-START screen size, NOT a live
                    // recomputation, so the scaling rate is constant
                    // across the drag — otherwise screenW grows
                    // with the sprite and the bubble drifts away
                    // from the cursor.
                    const float screenW = std::max(20.0f, _dragStartScreenSizeW);
                    const float screenH = std::max(20.0f, _dragStartScreenSizeH);
                    const ImVec2 mScreen = ImGui::GetMousePos();
                    const ImVec2 startScreen = worldToScreen(
                        ax::Vec2(_dragStartMouseX, _dragStartMouseY));
                    const float sdx = mScreen.x - startScreen.x;
                    const float sdy = mScreen.y - startScreen.y;
                    const float denX = screenW * std::max(0.05f, 1.0f - an.x);
                    const float denY = screenH * std::max(0.05f, 1.0f - an.y);
                    // Y in screen coords is top-down; world Y is up.
                    // sdy is positive when the user drags DOWN, which
                    // visually grows the BR corner — same sign as the
                    // old world-delta path.
                    float nsx = _dragStartScaleX + sdx / denX *
                                _dragStartScaleX;
                    float nsy = _dragStartScaleY + sdy / denY *
                                _dragStartScaleY;
                    const bool flipX = nsx < 0.0f;
                    const bool flipY = nsy < 0.0f;
                    nsx = (flipX ? -1.0f : 1.0f) *
                          std::max(0.01f, std::fabs(nsx));
                    nsy = (flipY ? -1.0f : 1.0f) *
                          std::max(0.01f, std::fabs(nsy));
                    axNode->setScaleX(nsx);
                    axNode->setScaleY(nsy);

                    char bx[32], by[32];
                    std::snprintf(bx, sizeof(bx), "%.4f", nsx);
                    std::snprintf(by, sizeof(by), "%.4f", nsy);
                    auto prev = _selected.subAttrs("Scale");
                    std::vector<std::pair<std::string, std::string>> next;
                    bool wX = false, wY = false;
                    for (const auto& [k, v] : prev)
                    {
                        if      (k == "ScaleX") { next.emplace_back(k, bx); wX = true; }
                        else if (k == "ScaleY") { next.emplace_back(k, by); wY = true; }
                        else                      next.emplace_back(k, v);
                    }
                    if (!wX) next.emplace_back("ScaleX", bx);
                    if (!wY) next.emplace_back("ScaleY", by);
                    _selected.setSubAttrs("Scale", next);
                    if (_doc) _doc->dirty = true;
                }
                else
                {
                // Resize: change Size only. Position is untouched (X/Y stay
                // exactly as authored), so axmol's anchor-based rendering
                // keeps the anchor pinned and the box grows/shrinks around
                // it naturally. Ctrl = aspect-lock.
                //
                // Screen-delta + snapshot screen size to immunise the
                // bubble velocity from ancestor scale chains (panels under
                // a 0.5x layer were resizing 2x slower than the cursor).
                // ratioX/Y account for the anchor — for anchor 0.5, BR
                // moves only half the size delta on screen, so we need
                // 2x the contentSize delta to keep the bubble under the
                // cursor.
                const auto anR = axNode->getAnchorPoint();
                const float ratioX = std::max(0.05f, 1.0f - anR.x);
                const float ratioY = std::max(0.05f, 1.0f - anR.y);
                const ImVec2 startScreen = worldToScreen(
                    ax::Vec2(_dragStartMouseX, _dragStartMouseY));
                const ImVec2 curScreen = ImGui::GetMousePos();
                const float sdx = curScreen.x - startScreen.x;
                // Screen Y is top-down; world Y bottom-up. The bubble
                // sits at the rendered TOP-right corner of the sprite
                // (anchor-relative). User convention is "drag DOWN-RIGHT
                // to grow" — so positive screen-Y delta (mouse down)
                // should GROW Height. Negate to undo the screen→world
                // Y flip the prior world-coord formula inherited.
                const float sdy = -(curScreen.y - startScreen.y);
                const float scrW = std::max(20.0f, _dragStartScreenSizeW);
                const float scrH = std::max(20.0f, _dragStartScreenSizeH);
                float signedW = _dragStartSizeW +
                    _dragStartSizeW * sdx / (ratioX * scrW);
                float signedH = _dragStartSizeH +
                    _dragStartSizeH * sdy / (ratioY * scrH);

                if (ImGui::GetIO().KeyCtrl &&
                    _dragStartSizeW > 0.01f && _dragStartSizeH > 0.01f)
                {
                    const float aspect = _dragStartSizeW / _dragStartSizeH;
                    const float relW = std::fabs(sdx) / scrW;
                    const float relH = std::fabs(sdy) / scrH;
                    if (relW >= relH) signedH = signedW / aspect;
                    else              signedW = signedH * aspect;
                }

                const bool flipX = signedW < 0.0f;
                const bool flipY = signedH < 0.0f;
                const float newW = std::max(1.0f, std::fabs(signedW));
                const float newH = std::max(1.0f, std::fabs(signedH));

                axNode->setContentSize(ax::Size(newW, newH));
                axNode->setScaleX((flipX ? -1.0f : 1.0f) * _dragStartScaleX);
                axNode->setScaleY((flipY ? -1.0f : 1.0f) * _dragStartScaleY);
                writeXYSub(_selected, "Size", newW, newH);
                // Position intentionally NOT modified — keep X/Y as-is.

                // Mirror the flip into the .csd via the Scale sub-element
                // (works on all Cocos Studio node types; FlipX/FlipY attrs
                // would be Sprite-only).
                const float sx = (flipX ? -1.0f : 1.0f) * _dragStartScaleX;
                const float sy = (flipY ? -1.0f : 1.0f) * _dragStartScaleY;
                char bx[32], by[32];
                std::snprintf(bx, sizeof(bx), "%.4f", sx);
                std::snprintf(by, sizeof(by), "%.4f", sy);
                auto prev = _selected.subAttrs("Scale");
                std::vector<std::pair<std::string, std::string>> next;
                bool wroteX = false, wroteY = false;
                for (const auto& [k, v] : prev)
                {
                    if      (k == "ScaleX") { next.emplace_back(k, bx); wroteX = true; }
                    else if (k == "ScaleY") { next.emplace_back(k, by); wroteY = true; }
                    else                      next.emplace_back(k, v);
                }
                if (!wroteX) next.emplace_back("ScaleX", bx);
                if (!wroteY) next.emplace_back("ScaleY", by);
                _selected.setSubAttrs("Scale", next);
                }  // end default-resize else
            }      // end of ResizeXX branch
            if (_doc) _doc->dirty = true;
        }
    }

    // Rubber-band marquee — pure screen-space math.  The rect is drawn
    // only once the cursor has moved past a small threshold from the
    // down-press, so a stationary click doesn't flash a 1×1 rectangle.
    // On release, distance decides:
    //   · < kClickThresholdPx  → treat the gesture as a single click,
    //                             hit-test the start position and
    //                             select the topmost non-structural leaf
    //                             (or clear the selection on empty /
    //                             non-additive).
    //   · ≥ kClickThresholdPx  → commit marquee overlap as selection.
    if (_rubberActive)
    {
        constexpr float kClickThresholdPx = 4.0f;
        const float ddx = m.x - _rubberStartScreenX;
        const float ddy = m.y - _rubberStartScreenY;
        const bool  didDrag =
            (ddx * ddx + ddy * ddy) >=
            (kClickThresholdPx * kClickThresholdPx);

        if (didDrag)
        {
            const ImVec2 tl(std::min(_rubberStartScreenX, m.x),
                            std::min(_rubberStartScreenY, m.y));
            const ImVec2 br(std::max(_rubberStartScreenX, m.x),
                            std::max(_rubberStartScreenY, m.y));
            dl->AddRectFilled(tl, br, kRubber);
            dl->AddRect(tl, br, IM_COL32(90, 160, 255, 220), 0.0f, 0, 1.5f);
        }

        if (!ImGui::IsMouseDown(0))
        {
            // Same structural-skip filter as the body-hit path — wrapper
            // containers (root / attach-at-layer-1 / game-scene / pure
            // Node/Layer types) aren't pickable by canvas click.
            auto isStructural = [](const std::string& ct) {
                return ct == "SingleNodeObjectData"
                    || ct == "GameNodeObjectData"
                    || ct == "ProjectNodeObjectData"
                    || ct == "GameLayerObjectData"
                    || ct == "LayerObjectData";
            };

            if (!didDrag)
            {
                // Single-click gesture: find the topmost non-structural
                // leaf whose world bbox contains the start screen point.
                // Recursive top-down walk that calls sortAllChildren at
                // each level so explicit localZOrder overrides (a button
                // on top of a panel that uses Z, not insertion order)
                // resolve correctly. Descendants are tested before the
                // parent — a button child renders on top of its parent
                // panel, so a click in the overlap belongs to the child.
                const ImVec2 pt(_rubberStartScreenX, _rubberStartScreenY);
                auto rectContainsScreen = [&](ax::Node* n) -> bool {
                    const auto cs = n->getContentSize();
                    if (cs.width <= 0 || cs.height <= 0) return false;
                    const ImVec2 p0 = worldToScreen(n->convertToWorldSpace(ax::Vec2(0, 0)));
                    const ImVec2 p1 = worldToScreen(n->convertToWorldSpace(ax::Vec2(cs.width, 0)));
                    const ImVec2 p2 = worldToScreen(n->convertToWorldSpace(ax::Vec2(cs.width, cs.height)));
                    const ImVec2 p3 = worldToScreen(n->convertToWorldSpace(ax::Vec2(0, cs.height)));
                    const float nx0 = std::min({p0.x, p1.x, p2.x, p3.x});
                    const float nx1 = std::max({p0.x, p1.x, p2.x, p3.x});
                    const float ny0 = std::min({p0.y, p1.y, p2.y, p3.y});
                    const float ny1 = std::max({p0.y, p1.y, p2.y, p3.y});
                    return pt.x >= nx0 && pt.x <= nx1 &&
                           pt.y >= ny0 && pt.y <= ny1;
                };
                std::function<ax::Node*(ax::Node*)> hitWalk =
                    [&](ax::Node* n) -> ax::Node* {
                    if (!n || !n->isVisible()) return nullptr;
                    // Sort so reverse iteration visits children in
                    // top-most-first order. axmol normally only sorts
                    // during draw; our hit-test runs every click, so
                    // forcing sortAllChildren here keeps us in sync
                    // with explicit localZOrder values.
                    n->sortAllChildren();
                    const auto& kids = n->getChildren();
                    for (auto it = kids.rbegin(); it != kids.rend(); ++it)
                    {
                        if (auto* h = hitWalk(*it)) return h;
                    }
                    // No descendant under the cursor — test self.
                    auto csdIt = _axToCsd.find(n);
                    if (csdIt == _axToCsd.end() || !csdIt->second.valid())
                        return nullptr;
                    if (isStructural(csdIt->second.ctype())) return nullptr;
                    if (isEffectivelyLocked(csdIt->second)) return nullptr;
                    return rectContainsScreen(n) ? n : nullptr;
                };
                ax::Node* hit = _axmolRoot ? hitWalk(_axmolRoot) : nullptr;
                if (hit)
                {
                    auto& csd = _axToCsd[hit];
                    if (_rubberAdditive) toggleSelected(csd);
                    else                 setSelected(csd);
                    auto path = pathToString(pathTo(csd));
                    recordEvent(_rubberAdditive ? "body-add" : "body-hit",
                                path.c_str(), csd.name().c_str());
                    if (!_eventLog.empty())
                        _eventLog.back().hitCtype = csd.ctype();
                }
                else if (!_rubberAdditive)
                {
                    clearSelection();
                    recordEvent("body-miss");
                }
            }
            else
            {
                // Marquee drag: collect every visible, non-structural
                // node whose projected screen AABB overlaps the rect.
                const ImVec2 tl(std::min(_rubberStartScreenX, m.x),
                                std::min(_rubberStartScreenY, m.y));
                const ImVec2 br(std::max(_rubberStartScreenX, m.x),
                                std::max(_rubberStartScreenY, m.y));
                if (!_rubberAdditive) _selection.clear();
                for (ax::Node* node : _axOrderDfs)
                {
                    if (!node->isVisible()) continue;
                    const auto cs = node->getContentSize();
                    if (cs.width <= 0 || cs.height <= 0) continue;
                    auto csdIt = _axToCsd.find(node);
                    if (csdIt != _axToCsd.end() && csdIt->second.valid() &&
                        isStructural(csdIt->second.ctype())) continue;
                    // Locked nodes (and descendants of locked
                    // ancestors) are excluded from rubber-band
                    // multi-select too.
                    if (csdIt != _axToCsd.end() && csdIt->second.valid() &&
                        isEffectivelyLocked(csdIt->second)) continue;
                    const auto w0 = node->convertToWorldSpace(ax::Vec2(0, 0));
                    const auto w1 = node->convertToWorldSpace(ax::Vec2(cs.width, 0));
                    const auto w2 = node->convertToWorldSpace(ax::Vec2(cs.width, cs.height));
                    const auto w3 = node->convertToWorldSpace(ax::Vec2(0, cs.height));
                    const ImVec2 p0 = worldToScreen(w0);
                    const ImVec2 p1 = worldToScreen(w1);
                    const ImVec2 p2 = worldToScreen(w2);
                    const ImVec2 p3 = worldToScreen(w3);
                    const float nx0 = std::min({p0.x, p1.x, p2.x, p3.x});
                    const float nx1 = std::max({p0.x, p1.x, p2.x, p3.x});
                    const float ny0 = std::min({p0.y, p1.y, p2.y, p3.y});
                    const float ny1 = std::max({p0.y, p1.y, p2.y, p3.y});
                    if (nx1 < tl.x || nx0 > br.x ||
                        ny1 < tl.y || ny0 > br.y) continue;
                    auto it = _axToCsd.find(node);
                    if (it != _axToCsd.end() && it->second.valid())
                        addToSelection(it->second);
                }
                recordEvent("rubber-commit");
            }
            _rubberActive = false;
        }
    }

    if (!ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(1))
    {
        _dragMode = DragMode::None;
        _dragStartPositions.clear();
    }
}


namespace
{
/// Small helper — convert an HSV triplet into an ImGui-packed color.
ImU32 hsv(float h, float s, float v)
{
    ImVec4 c = ImColor::HSV(h, s, v).Value;
    return IM_COL32((int)(c.x * 255), (int)(c.y * 255), (int)(c.z * 255), 255);
}
}  // namespace

namespace
{
/// Per-node overlay DrawNode is added as a tagged child of the scene
/// node it represents. Under perspective, any Z offset the parent
/// chain imposes (layer-rotation Z-padding) applies to the overlay
/// identically, so rotations stay perfectly in sync with the scene.
constexpr int kLayerOverlayTag = 0x4C594F56;   // 'LYOV'

/// Shared between LayerSide and LayerTop — draws a translucent
/// colored quad + outline around every visible node. Each overlay is
/// added as a child of the target node (tagged `kLayerOverlayTag`),
/// so inherited transforms — including the per-layer Z padding —
/// apply uniformly and the overlay tracks the scene through any 3D
/// rotation without drift.
void drawProjectedLayers(const std::vector<ax::Node*>& order)
{
    auto hsvToRgb = [](float h, float s, float v) -> ax::Color4F {
        float r = v, g = v, b = v;
        if (s > 0.0f)
        {
            const float hh = h * 6.0f;
            const int   i  = static_cast<int>(hh) % 6;
            const float f  = hh - static_cast<int>(hh);
            const float p  = v * (1.0f - s);
            const float q  = v * (1.0f - s * f);
            const float t  = v * (1.0f - s * (1.0f - f));
            switch (i)
            {
                case 0: r = v; g = t; b = p; break;
                case 1: r = q; g = v; b = p; break;
                case 2: r = p; g = v; b = t; break;
                case 3: r = p; g = q; b = v; break;
                case 4: r = t; g = p; b = v; break;
                case 5: r = v; g = p; b = q; break;
            }
        }
        return ax::Color4F(r, g, b, 1.0f);
    };
    const int n = static_cast<int>(order.size());
    for (int i = 0; i < n; ++i)
    {
        ax::Node* node = order[i];
        if (!node || !node->isVisible()) continue;
        const auto cs = node->getContentSize();
        if (cs.width <= 0 || cs.height <= 0) continue;

        ax::DrawNode* over = dynamic_cast<ax::DrawNode*>(
            node->getChildByTag(kLayerOverlayTag));
        if (!over)
        {
            over = ax::DrawNode::create();
            over->setTag(kLayerOverlayTag);
            over->setLocalZOrder(0x7fffffff);
            over->setGlobalZOrder(100.0f);
            node->addChild(over);
        }
        over->clear();
        over->setVisible(true);
        // Cancel out the node's positionZ so the overlay renders at
        // world-z = 0, matching axmol's widget renderer (which
        // ignores parent positionZ and emits draw commands at a
        // flat effective depth). Without this, perspective makes
        // the overlay sweep more screen distance per rotation
        // degree than the underlying sprites — i.e. "the boxes
        // rotate faster than the scene".
        over->setPositionZ(-node->getPositionZ());

        const float hue = static_cast<float>(i) / std::max(1, n);
        const ax::Color4F edge = hsvToRgb(hue, 0.85f, 0.95f);
        ax::Color4F fill = edge; fill.a = 0.38f;

        // Node-local corners — the overlay inherits the node's own
        // transform chain, so we just describe the rectangle in
        // local coordinates and every parent rotation/scale/Z
        // offset applies to us identically.
        const ax::Vec2 p0(0,        0       );
        const ax::Vec2 p1(cs.width, 0       );
        const ax::Vec2 p2(cs.width, cs.height);
        const ax::Vec2 p3(0,        cs.height);
        const ax::Vec2 poly[4] = { p0, p1, p2, p3 };
        over->drawSolidPoly(poly, 4, fill);
        constexpr float kEdgeRLocal = 1.0f;   // ~1 design px outline
        over->drawSegment(p0, p1, kEdgeRLocal, edge);
        over->drawSegment(p1, p2, kEdgeRLocal, edge);
        over->drawSegment(p2, p3, kEdgeRLocal, edge);
        over->drawSegment(p3, p0, kEdgeRLocal, edge);
    }
}

/// Clear the per-node overlays (hide + zero out draw buffers) so
/// stale quads don't linger when the user leaves LayerSide/Top mode
/// or deselects Layer-rects. The DrawNode children stay attached
/// for cheap re-use on the next activation.
void clearLayerProjectionOverlays(const std::vector<ax::Node*>& order)
{
    for (ax::Node* n : order)
    {
        if (!n) continue;
        if (auto* over = dynamic_cast<ax::DrawNode*>(
                n->getChildByTag(kLayerOverlayTag)))
        {
            over->clear();
            over->setVisible(false);
        }
    }
}
}  // namespace

void Editor::drawLayerSideOverlay()
{
    // Scene is pre-rotated Y=80° + Z-padded by setMode(LayerSide).
    // The per-node overlay children draw each rectangle in node-
    // local coords, so the full parent transform chain (including
    // each layer's Z offset) applies — rotation stays in sync with
    // the rendered scene even under perspective projection.
    if (!_showLayerRects)
    {
        clearLayerProjectionOverlays(_axOrderDfs);
        return;
    }
    drawProjectedLayers(_axOrderDfs);
}

void Editor::drawLayerTopOverlay()
{
    // Same as Side mode, only preset rotation axis differs (X=80°
    // vs Y=80°). Shares drawProjectedLayers which attaches per-node
    // child DrawNodes.
    if (!_showLayerRects)
    {
        clearLayerProjectionOverlays(_axOrderDfs);
        return;
    }
    drawProjectedLayers(_axOrderDfs);
}


void Editor::drawNavigatorGlobe(float size)
{
    // Rotation order: Z (roll) * Y (yaw) * X (pitch), applied to a unit
    // vector. Matches axmol's setRotation3D order so the globe preview
    // matches the actual scene tilt.
    constexpr float kDeg = 3.14159265358979323846f / 180.0f;
    auto rotate = [&](ax::Vec3 v) {
        const float rx = _sceneRotationXDeg * kDeg;
        const float ry = _sceneRotationYDeg * kDeg;
        const float rz = _sceneRotationDeg  * kDeg;
        // X (pitch)
        float y = v.y * std::cos(rx) - v.z * std::sin(rx);
        float z = v.y * std::sin(rx) + v.z * std::cos(rx);
        float x = v.x;
        // Y (yaw)
        float x2 = x * std::cos(ry) + z * std::sin(ry);
        float z2 = -x * std::sin(ry) + z * std::cos(ry);
        float y2 = y;
        // Z (roll)
        float x3 = x2 * std::cos(rz) - y2 * std::sin(rz);
        float y3 = x2 * std::sin(rz) + y2 * std::cos(rz);
        return ax::Vec3(x3, y3, z2);
    };

    // Interaction area — InvisibleButton gives us the exact drag/hover
    // semantics ImGui uses everywhere else.
    ImGui::Dummy(ImVec2(size, size));
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    const ImVec2 center((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    const float  radius = size * 0.42f;
    ImGui::SetCursorScreenPos(p0);
    // Allow subsequent items (the cardinal pins below) to overlap +
    // capture clicks that would otherwise be eaten by this drag
    // hitbox. ImGui renamed SetItemAllowOverlap → SetNextItem…
    // in recent versions; it applies to the NEXT submitted item.
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##globeHit", ImVec2(size, size));
    // Cursor position to return to once we're done submitting the
    // pin buttons. Without this, the next widget (rotation sliders)
    // would attach to wherever the last pin's InvisibleButton left
    // the cursor — which depends on which pin was rendered last and
    // therefore changes per rotation → the widget appears to resize.
    const ImVec2 afterGlobeCursor = ImGui::GetCursorScreenPos();
    const bool hot    = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    ImGuiIO& io = ImGui::GetIO();
    if (active)
    {
        // Shift+drag rolls; plain drag pitches / yaws.
        if (io.KeyShift)
        {
            _sceneRotationDeg  += io.MouseDelta.x * 0.5f;
        }
        else
        {
            // Both axes inverted so the globe rotates WITH the cursor:
            // drag right  → globe yaws left-toward-you (Y inverted);
            // drag down   → globe pitches so the top follows the
            //               cursor downward (X inverted).
            _sceneRotationYDeg += io.MouseDelta.x * 0.4f;
            _sceneRotationXDeg += io.MouseDelta.y * 0.4f;
        }
        auto clampDeg = [](float& v) {
            while (v >  180.0f) v -= 360.0f;
            while (v < -180.0f) v += 360.0f;
        };
        clampDeg(_sceneRotationXDeg);
        clampDeg(_sceneRotationYDeg);
        clampDeg(_sceneRotationDeg);
    }
    if (hot && io.MouseWheel != 0.0f)
    {
        _sceneZoom *= std::pow(1.10f, io.MouseWheel);
        _sceneZoom  = std::clamp(_sceneZoom, 0.1f, 8.0f);
    }

    // Double-click anywhere in the globe resets to the straight-on
    // orientation — fastest way to bail out of an awkward tilt.
    if (hot && ImGui::IsMouseDoubleClicked(0))
    {
        _sceneRotationXDeg = 0.0f;
        _sceneRotationYDeg = 0.0f;
        _sceneRotationDeg  = 0.0f;
    }

    auto* dl = ImGui::GetWindowDrawList();

    // Background circle / sphere silhouette. Using the window drawlist
    // means it composes with the rest of the floating bar's styling.
    dl->AddCircleFilled(center, radius, IM_COL32(20, 30, 50, 180), 48);
    dl->AddCircle(center, radius,
        hot ? IM_COL32(120, 180, 255, 255)
            : IM_COL32(90, 130, 200, 180),
        48, 1.5f);

    // Latitude / longitude wireframe. Two great circles (one vertical,
    // one horizontal through the equator) + the silhouette gives a
    // clear sense of which way is "up" at any rotation. Back-facing
    // portions render dimmer so the front face reads first.
    auto projAndSort = [&](ax::Vec3 v) {
        const auto r = rotate(v);
        const float sx = center.x + r.x * radius;
        const float sy = center.y - r.y * radius;  // screen-Y grows down
        return std::make_tuple(ImVec2(sx, sy), r.z > 0.0f);
    };
    auto drawCircle = [&](int axis) {
        // axis 0 = equator (lies in X-Z plane, Y=0)
        // axis 1 = meridian YZ plane, X=0
        // axis 2 = meridian XY plane, Z=0
        constexpr int kSegs = 48;
        ImVec2 prev;
        bool   prevFront = true;
        for (int i = 0; i <= kSegs; ++i)
        {
            const float t = (float)i / (float)kSegs * 2.0f * 3.14159265f;
            ax::Vec3 v;
            if      (axis == 0) v = ax::Vec3(std::cos(t), 0.0f, std::sin(t));
            else if (axis == 1) v = ax::Vec3(0.0f, std::cos(t), std::sin(t));
            else                v = ax::Vec3(std::cos(t), std::sin(t), 0.0f);
            const auto [pt, front] = projAndSort(v);
            if (i > 0)
            {
                const ImU32 col = (front && prevFront)
                    ? IM_COL32(180, 200, 230, 160)
                    : IM_COL32( 80,  90, 110,  90);
                dl->AddLine(prev, pt, col, 1.0f);
            }
            prev = pt; prevFront = front;
        }
    };
    drawCircle(0);
    drawCircle(1);
    drawCircle(2);

    // Six cardinal pins — one at each ±axis endpoint. Clicking a
    // pin snaps the scene rotation to look from that direction.
    // Axis lines span the full diameter so both halves are visible;
    // back-facing halves render dimmer to preserve depth cues.
    // Pins are drawn + hit-tested AFTER the existing drag
    // InvisibleButton (using SetItemAllowOverlap above so they can
    // capture clicks over the globe area) — a click on a pin snaps
    // rotation and never starts a drag.
    struct Cardinal {
        ax::Vec3 dir;
        float    rotX, rotY, rotZ;   // target scene rotation
        ImU32    bright, dim;
        const char* id;
        const char* label;
    };
    const Cardinal cards[6] = {
        { { 1, 0, 0},  0.0f,  -90.0f, 0.0f,
          IM_COL32(230, 80, 80, 255), IM_COL32(140, 40, 40, 160),
          "##pinPX", "X"  },
        { {-1, 0, 0},  0.0f,   90.0f, 0.0f,
          IM_COL32(230, 80, 80, 255), IM_COL32(140, 40, 40, 160),
          "##pinNX", "-X" },
        { { 0, 1, 0},  90.0f,   0.0f, 0.0f,
          IM_COL32(80, 220, 120, 255), IM_COL32(40, 110, 60, 160),
          "##pinPY", "Y"  },
        { { 0,-1, 0}, -90.0f,   0.0f, 0.0f,
          IM_COL32(80, 220, 120, 255), IM_COL32(40, 110, 60, 160),
          "##pinNY", "-Y" },
        { { 0, 0, 1},  0.0f,    0.0f, 0.0f,
          IM_COL32(90, 150, 240, 255), IM_COL32(50, 80, 140, 160),
          "##pinPZ", "Z"  },
        { { 0, 0,-1},  0.0f,  180.0f, 0.0f,
          IM_COL32(90, 150, 240, 255), IM_COL32(50, 80, 140, 160),
          "##pinNZ", "-Z" },
    };

    // Axis diameter lines — each axis drawn as TWO halves from the
    // centre, coloured bright if its tip is front-facing, dim if
    // back-facing. That gives a depth cue at a glance.
    const ax::Vec3 axisDirs[3] = {
        ax::Vec3(1,0,0), ax::Vec3(0,1,0), ax::Vec3(0,0,1)
    };
    const ImU32 axisBright[3] = {
        IM_COL32(230, 80, 80, 255),
        IM_COL32( 80,220,120, 255),
        IM_COL32( 90,150,240, 255),
    };
    const ImU32 axisDim[3] = {
        IM_COL32(140, 40, 40, 160),
        IM_COL32( 40,110, 60, 160),
        IM_COL32( 50, 80,140, 160),
    };
    for (int a = 0; a < 3; ++a)
    {
        const auto [plusP,  plusFront]  = projAndSort( axisDirs[a]);
        const auto [minusP, minusFront] = projAndSort(-axisDirs[a]);
        dl->AddLine(center, plusP,
                    plusFront  ? axisBright[a] : axisDim[a], 2.0f);
        dl->AddLine(center, minusP,
                    minusFront ? axisBright[a] : axisDim[a], 2.0f);
    }

    // Pins + labels + click handling. Pin radius scales with the
    // globe so small globes still read. Front-facing pins render
    // slightly larger so they don't get lost behind the axis line.
    const float kPinR = std::max(6.0f, size * 0.055f);
    for (const auto& c : cards)
    {
        const auto [pinP, front] = projAndSort(c.dir);
        const ImU32 col  = front ? c.bright : c.dim;
        const float r    = front ? kPinR : kPinR * 0.75f;

        ImGui::SetCursorScreenPos(ImVec2(pinP.x - r, pinP.y - r));
        // Let later-submitted pins also overlap; otherwise two
        // pins that happen to land close together under an oblique
        // rotation would only the first be clickable.
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton(c.id, ImVec2(r * 2.0f, r * 2.0f));
        const bool pinHover = ImGui::IsItemHovered();
        const bool pinClick = ImGui::IsItemClicked(ImGuiMouseButton_Left);

        const float drawR = pinHover ? r * 1.25f : r;
        dl->AddCircleFilled(pinP, drawR, col, 16);
        dl->AddCircle     (pinP, drawR,
                           IM_COL32(0, 0, 0, 220), 16, 1.5f);

        // Label offset slightly outward from the tip so it doesn't
        // sit on top of the pin.
        const float lx = pinP.x + (pinP.x - center.x) * 0.20f;
        const float ly = pinP.y + (pinP.y - center.y) * 0.20f;
        dl->AddText(ImVec2(lx - 4.0f, ly - 7.0f),
                    front ? IM_COL32(255, 255, 255, 230)
                          : IM_COL32(160, 160, 160, 140),
                    c.label);

        if (pinHover) ImGui::SetTooltip("View %s", c.label);

        if (pinClick)
        {
            _sceneRotationXDeg = c.rotX;
            _sceneRotationYDeg = c.rotY;
            _sceneRotationDeg  = c.rotZ;
        }
    }
    // Pin InvisibleButtons leave the cursor at whatever pin was
    // drawn last — restore to right below the globe so the sliders
    // below attach in a stable position regardless of orientation.
    ImGui::SetCursorScreenPos(afterGlobeCursor);

    // Compact rotation sliders under the globe — a pure drag-only
    // sphere turned out harder to control than the old three sliders,
    // so the sliders are back in a tight layout (small "0" reset
    // button + narrow slider per axis). The globe sits above as the
    // visual indicator of the current tilt.
    auto rotRow = [&](const char* id, const char* label,
                      ImU32 col, float& value)
    {
        ImGui::PushID(id);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::Text("%s", label);
        ImGui::PopStyleColor();
        ImGui::SameLine(22.0f);
        if (ImGui::SmallButton("0")) value = 0.0f;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(size - 52.0f);
        ImGui::SliderFloat("##s", &value, -180.0f, 180.0f, "%.0f°",
                           ImGuiSliderFlags_AlwaysClamp);
        ImGui::PopID();
    };
    const ImU32 kAxisX = IM_COL32(230, 80, 80, 255);
    const ImU32 kAxisY = IM_COL32(80, 220, 120, 255);
    const ImU32 kAxisZ = IM_COL32(90, 150, 240, 255);
    rotRow("rotX", "X", kAxisX, _sceneRotationXDeg);
    rotRow("rotY", "Y", kAxisY, _sceneRotationYDeg);
    rotRow("rotZ", "Z", kAxisZ, _sceneRotationDeg);

    // Zoom readout + reset on one line. Double-clicking the globe
    // resets rotation only; this resets EVERYTHING that affects the
    // view — rotation, zoom, AND pan — so the scene snaps back to the
    // fit-on-screen starting frame. `_sceneZoom = 1` is the identity
    // in the `fitK * zoom` formula, and fitK is re-computed each
    // frame to match the central dock, so zoom=1 + pan=0 reliably
    // lands the scene centered and fitted.
    ImGui::TextDisabled("%.2fx", _sceneZoom);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset view"))
    {
        _sceneRotationXDeg = 0.0f;
        _sceneRotationYDeg = 0.0f;
        _sceneRotationDeg  = 0.0f;
        // Snap directly back to the values captured by the first
        // fit-to-content pass — zoom + pan together, so the bbox
        // lands centred in the canvas without a one-frame flicker.
        _sceneZoom  = _initialZoom;
        _panOffsetX = _initialPanX;
        _panOffsetY = _initialPanY;
    }

    // Z-layer spacing — slides each child forward along +Z by
    // (index × spacing) so the Side / Top tilted views spread into
    // distinct depth planes. 0 keeps everything coplanar. Paired
    // with the Layer-rects toggle that draws the coloured projected
    // quads in those modes.
    ImGui::PushID("zpad");
    ImGui::SetNextItemWidth(size);
    ImGui::SliderFloat("##zpad", &_sceneZLayerSpacing,
                       0.0f, 200.0f, "Z pad: %.0f px",
                       ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopID();
    ImGui::Checkbox("Layer rects", &_showLayerRects);
}

// Small floating diagnostic window. Purely read-only — shows the ImGui
// state the click gate reads ("is central region hovered? is any ImGui
// item hovered? which window owns the hover?"), plus the predicted
// gate decision if you clicked at the current mouse position, plus a
// tail of the last 20 clicks with their outcome. Nothing here affects
// editor behaviour; it's here only so we can see why a click is (or
// isn't) accepted for body-hit / rubber-band.
void Editor::drawMessagesPanel()
{
    if (!ImGui::Begin("Messages", &_showMessagesPanel))
    {
        ImGui::End();
        return;
    }
    drawPanelDetachButton("Messages");
    drawMessagesBody();
    ImGui::End();
}

void Editor::drawMessagesBody()
{
    const ImVec2 m = ImGui::GetMousePos();
    auto* ctx = ImGui::GetCurrentContext();
    auto* hoverWin = ctx->HoveredWindow;
    const bool isDockspaceHost = hoverWin ?
        ((hoverWin->Flags & ImGuiWindowFlags_DockNodeHost) != 0) : true;
    const char* hoverName = hoverWin ? hoverWin->Name : "(null)";

    const char* predicted = _canvasHovered
        ? "PASS (canvas hovered)"
        : "BLOCK: canvas not hovered";
    ImVec4 pCol = _canvasHovered
        ? ImVec4(0.45f, 0.90f, 0.45f, 1.0f)
        : ImVec4(0.95f, 0.55f, 0.40f, 1.0f);

    if (ImGui::CollapsingHeader("Live state",
            ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("mouse      : (%.0f, %.0f)", m.x, m.y);
        ImGui::Text("canvas rect: (%.0f,%.0f  %.0fx%.0f)",
                    _canvasRectX, _canvasRectY, _canvasRectW, _canvasRectH);
        ImGui::Text("hovered    : %s", _canvasHovered ? "YES" : "no");
        ImGui::Text("active     : %s", _canvasActive  ? "YES" : "no");
        ImGui::Text("hoverWin   : %s%s", hoverName,
                    isDockspaceHost ? "  [dockspace-host]" : "");
        ImGui::Text("HoveredId  : 0x%08X", ctx->HoveredId);
        ImGui::Text("ActiveId   : 0x%08X", ctx->ActiveId);
        ImGui::TextColored(pCol, "predicted  : %s", predicted);
    }

    // ---- Log viewer -----------------------------------------------------
    // Pulls from the process-wide RingSink so every OCS_LOG_* call lands
    // here regardless of which subsystem emitted it. Two filters:
    //   * Level — global Log::level. "Spam (Trace)" surfaces every
    //     layout / window-resize / dock-resnap call so the user can
    //     watch the rendering pipeline in real time.
    //   * Category / message substring — case-insensitive. Empty
    //     filter shows everything.
    if (ImGui::CollapsingHeader("Log",
            ImGuiTreeNodeFlags_DefaultOpen))
    {
        static char sFilterBuf[128]  = "";
        static char sExcludeBuf[128] = "";
        static bool sLogPrefsLoaded  = false;
        static bool sAutoScroll      = true;
        // Lazy-load persisted prefs the first time the panel renders.
        if (!sLogPrefsLoaded)
        {
            sAutoScroll   = getUiPrefBool("messages.log.autoscroll", true);
            sLogPrefsLoaded = true;
        }

        const LogLevel currentLv = Log::instance().level();
        const char* lvNames[] = {
            "Trace (spam)", "Debug", "Info", "Warn", "Error", "Off"
        };
        int lvIdx = static_cast<int>(currentLv);

        ImGui::SetNextItemWidth(150);
        if (ImGui::Combo("##loglevel", &lvIdx,
                         lvNames, IM_ARRAYSIZE(lvNames)))
            publishSharedLogLevel(static_cast<LogLevel>(lvIdx));
        ImGui::SameLine();
        if (ImGui::Checkbox("Auto-scroll", &sAutoScroll))
            setUiPrefBool("messages.log.autoscroll", sAutoScroll);
        ImGui::SameLine();
        // Force-scroll-to-bottom button. Sets a one-shot flag the
        // text pane consumes on the next frame; works even when
        // auto-scroll is off (e.g. user paused to inspect, then
        // wants to jump back to the latest event).
        static bool sScrollToBottomOnce = false;
        if (ImGui::Button("⤓"))
            sScrollToBottomOnce = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scroll to bottom");
        ImGui::SameLine();
        // Copy the visible (filtered) log body to the system
        // clipboard. Set later in the function once `body` has
        // been built — for now, queue a one-shot flag.
        static bool sCopyAllOnce = false;
        if (ImGui::Button("Copy"))
            sCopyAllOnce = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Copy visible log to clipboard");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##logfilter", "include (cat or msg)…",
                                 sFilterBuf, sizeof(sFilterBuf));
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##logexclude",
                                 "exclude (hide rows containing…)",
                                 sExcludeBuf, sizeof(sExcludeBuf));

        // Read from the cross-process message queue so the panel
        // shows records from this process AND any detached-panel
        // peers (or, when running detached, from the main app).
        // Each process appends to its own IPC log file; tailIpcLogs
        // merges them all by timestamp and returns the newest tail.
        std::vector<LogRecord> records = tailIpcLogs(200);

        // Lower-case helper so the filter is case-insensitive.
        auto lowerStr = [](const std::string& s) {
            std::string out; out.reserve(s.size());
            for (unsigned char c : s) out.push_back((char)std::tolower(c));
            return out;
        };
        std::string needle = lowerStr(sFilterBuf);

        // Build the visible text once per frame, applying both
        // filters. The pane is a read-only multiline InputText so
        // the user can drag-select / Cmd-C the lines (plain Text()
        // would not accept selection). We lose per-row colour but
        // gain native selection — the user asked for selection.
        std::string excl = lowerStr(sExcludeBuf);
        std::string body;
        body.reserve(records.size() * 80);
        std::size_t shown = 0;
        char rowBuf[1280];
        for (const auto& r : records)
        {
            const std::string& src = r.source;
            if (!needle.empty())
            {
                const std::string hayCat = lowerStr(
                    r.category ? r.category : "");
                const std::string hayMsg = lowerStr(r.message);
                const std::string haySrc = lowerStr(src);
                if (hayCat.find(needle) == std::string::npos &&
                    hayMsg.find(needle) == std::string::npos &&
                    haySrc.find(needle) == std::string::npos)
                    continue;
            }
            if (!excl.empty())
            {
                const std::string hayCat = lowerStr(
                    r.category ? r.category : "");
                const std::string hayMsg = lowerStr(r.message);
                const std::string haySrc = lowerStr(src);
                if (hayCat.find(excl) != std::string::npos ||
                    hayMsg.find(excl) != std::string::npos ||
                    haySrc.find(excl) != std::string::npos)
                    continue;
            }
            std::snprintf(rowBuf, sizeof(rowBuf),
                "%7.2fs %-5s %-22s %s\n",
                r.time, levelName(r.level),
                src.empty() ? (r.category ? r.category : "ocs")
                            : src.c_str(),
                r.message.c_str());
            body.append(rowBuf);
            ++shown;
        }
        if (shown == 0) body = "(no matching records)\n";

        if (sCopyAllOnce)
        {
            sCopyAllOnce = false;
            ImGui::SetClipboardText(body.c_str());
            OCS_LOG_INFO("ui",
                "copied %zu log row(s) to clipboard (%zu bytes)",
                shown, body.size());
        }

        // Persisted height + draggable splitter, same as before.
        static float sLogHeightPx = -1.0f;
        if (sLogHeightPx < 0)
            sLogHeightPx = getUiPrefF("messages.log.height", 220.0f);
        const float rowH = ImGui::GetTextLineHeightWithSpacing();
        sLogHeightPx = std::max(sLogHeightPx, rowH * 3.0f);

        // Auto-scroll runs ONLY on the frame where the body text
        // actually grew (new log lines arrived) or when the user
        // clicked ⤓. Firing every frame stomped the user's cursor
        // mid-drag so selection always restarted at the top —
        // gating on body-size change leaves the cursor alone for
        // the rest of the time so drag-select / cmd-click work.
        static std::size_t sLastBodySize = 0;
        const bool bodyGrew = (body.size() > sLastBodySize);
        sLastBodySize = body.size();
        struct ScrollState { bool fire; };
        ScrollState scrollState{
            sScrollToBottomOnce || (sAutoScroll && bodyGrew)
        };
        sScrollToBottomOnce = false;
        auto scrollCb = [](ImGuiInputTextCallbackData* d) -> int {
            auto* st = static_cast<ScrollState*>(d->UserData);
            if (st && st->fire)
            {
                d->CursorPos      = d->BufTextLen;
                d->SelectionStart = d->SelectionEnd = d->BufTextLen;
            }
            return 0;
        };

        ImGui::InputTextMultiline("##logtext",
            body.data(),
            body.size() + 1,                 // include the NUL
            ImVec2(-FLT_MIN, sLogHeightPx),
            ImGuiInputTextFlags_ReadOnly |
            ImGuiInputTextFlags_NoUndoRedo |
            ImGuiInputTextFlags_CallbackAlways,
            scrollCb,
            &scrollState);

        // Splitter: thin invisible button immediately below the
        // text pane. LMB-drag to adjust height. Persist the new
        // value when the drag releases.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::InvisibleButton("##logsplit", ImVec2(-FLT_MIN, 6.0f));
        const bool splitHov = ImGui::IsItemHovered();
        const bool splitAct = ImGui::IsItemActive();
        if (splitAct)
            sLogHeightPx += ImGui::GetIO().MouseDelta.y;
        if (splitHov || splitAct)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemDeactivatedAfterEdit() ||
            (splitAct && ImGui::IsMouseReleased(0)))
            setUiPrefF("messages.log.height", sLogHeightPx);
        const ImU32 hcol = (splitHov || splitAct)
            ? IM_COL32(120, 160, 230, 220)
            : IM_COL32( 90,  90, 100, 120);
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(mn.x, (mn.y + mx.y) * 0.5f - 1.0f),
            ImVec2(mx.x, (mn.y + mx.y) * 0.5f + 1.0f),
            hcol);
        ImGui::PopStyleVar();

        if (ImGui::Button("Clear log"))
        {
            // Truncate every per-process IPC log file. Each process
            // owns its own writer; truncating from any reader is
            // safe because the writer keeps its FILE* open in
            // append mode and just resumes appending.
            namespace fs = std::filesystem;
            std::error_code fsec;
            for (auto& e : fs::directory_iterator(
                     "/tmp/opencocosstudio", fsec))
            {
                if (fsec) break;
                const auto& p = e.path();
                const auto name = p.filename().string();
                if (name.size() > 13 &&
                    name.compare(name.size() - 13, 13,
                                 ".messages.log") == 0)
                {
                    if (auto* fp = std::fopen(p.c_str(), "w"))
                        std::fclose(fp);
                }
            }
            if (auto r = defaultRingSink()) r->clear();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%zu records", records.size());
    }

    // ---- Click log ------------------------------------------------------
    if (ImGui::CollapsingHeader("Click log (newest last)",
            ImGuiTreeNodeFlags_DefaultOpen))
    {
        const float rowH = ImGui::GetTextLineHeightWithSpacing();
        const float bottomReserve = rowH * 1.5f;
        if (ImGui::BeginChild("##clicklog", ImVec2(0, -bottomReserve),
                              true,
                              ImGuiWindowFlags_HorizontalScrollbar))
        {
            for (const auto& ev : _eventLog)
            {
                ImVec4 col(0.85f, 0.85f, 0.85f, 1.0f);
                if (ev.kind && std::strncmp(ev.kind, "gate:", 5) == 0)
                    col = ImVec4(0.95f, 0.55f, 0.40f, 1.0f);
                else if (ev.kind && std::strncmp(ev.kind, "body-", 5) == 0)
                    col = ImVec4(0.55f, 0.90f, 0.55f, 1.0f);
                std::string hitDesc;
                if (!ev.hitName.empty())
                {
                    hitDesc = "  hit=" + ev.hitName;
                    if (!ev.hitCtype.empty())
                        hitDesc += " (" + ev.hitCtype + ")";
                }
                ImGui::TextColored(col,
                    "%7.2fs (%.0f,%.0f) %-22s win=%s%s",
                    ev.time, ev.x, ev.y, ev.kind ? ev.kind : "?",
                    ev.hoverWinName.empty() ? "(null)"
                                            : ev.hoverWinName.c_str(),
                    hitDesc.c_str());
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - rowH)
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        if (ImGui::Button("Clear clicks")) _eventLog.clear();
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu events, cap %zu)",
            _eventLog.size(), kEventLogCap);
    }
}

}  // namespace opencs
