// Asset browser panel — tree view of images under the guessed Resources
// directory. Mirrors the real filesystem: real subdirs become expandable
// folder entries, loose PNG/JPG/… files are leaf rows, and a .plist + .png
// pair that parses as a Cocos2d texture-atlas becomes an expandable Atlas
// whose children are the sprite frames inside the atlas. Selecting any
// image publishes it to the Editor so the Properties panel can show
// details (format, size, where it's referenced).
//
// Tree rendering preserves the filter box by computing, for each subtree,
// whether it has any visible descendant before calling TreeNodeEx — so a
// filtered tree stays compact but still exposes deep matches.

#include "UILayoutEditor/panels/Panels.h"
#include "UILayoutEditor/Editor.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace opencs
{

namespace
{
char sFilter[128] = "";
bool sConfirmDelete = false;
// Anchor for Shift-range selection in the Assets tree. Plain-click and
// Cmd/Ctrl-click advance it; Shift-click keeps it fixed so a sequence of
// shift-clicks extends from the same pivot, matching OS file-manager UX.
std::filesystem::path sSelectionAnchor;

bool matches(const char* s, const char* needle)
{
    if (needle[0] == '\0') return true;
    // Case-insensitive substring.
    for (const char* p = s; *p; ++p)
    {
        const char* a = p;
        const char* b = needle;
        while (*a && *b && std::tolower((unsigned char)*a) ==
                                std::tolower((unsigned char)*b))
        { ++a; ++b; }
        if (*b == '\0') return true;
    }
    return false;
}

/// True if this entry's displayName matches the filter, OR the entry has
/// any descendant that does. Lets the tree hide whole subtrees that are
/// entirely filtered out while keeping matched leaves visible under their
/// parent chain.
bool subtreeMatches(const AssetEntry& e, const char* filter)
{
    if (filter[0] == '\0') return true;
    if (matches(e.displayName.c_str(), filter)) return true;
    for (const auto& c : e.children)
        if (subtreeMatches(c, filter)) return true;
    return false;
}

/// Walk visible leaves (File / AtlasFrame) in tree order into `out` for
/// keyboard arrow navigation. Folder and Atlas entries themselves are
/// NOT selectable leaves for nav — they're container rows.
void collectFlat(const AssetEntry& e,
                 const char* filter,
                 std::vector<std::filesystem::path>& out)
{
    if (!subtreeMatches(e, filter)) return;
    switch (e.kind)
    {
    case AssetKind::File:
    case AssetKind::AtlasFrame:
        if (matches(e.displayName.c_str(), filter)) out.push_back(e.absPath);
        break;
    case AssetKind::Folder:
    case AssetKind::Atlas:
        for (const auto& c : e.children) collectFlat(c, filter, out);
        break;
    }
}

/// File-only variant of collectFlat — skips AtlasFrame leaves. Used by
/// Shift-range selection, which operates over the file selection set
/// (frames alias onto the atlas png path so index lookups would be
/// ambiguous otherwise).
void collectFilesFlat(const AssetEntry& e,
                      const char* filter,
                      std::vector<std::filesystem::path>& out)
{
    if (!subtreeMatches(e, filter)) return;
    switch (e.kind)
    {
    case AssetKind::File:
        if (matches(e.displayName.c_str(), filter)) out.push_back(e.absPath);
        break;
    case AssetKind::Folder:
        for (const auto& c : e.children) collectFilesFlat(c, filter, out);
        break;
    default: break;  // Atlas / AtlasFrame excluded
    }
}

std::vector<std::filesystem::path>
fileLeavesInOrder(const std::vector<AssetEntry>& tree, const char* filter)
{
    std::vector<std::filesystem::path> out;
    for (const auto& e : tree) collectFilesFlat(e, filter, out);
    return out;
}

/// Enum is scoped inside an anonymous namespace so the switch across
/// request kinds stays local to the panel body.
enum class Pending
{
    None, Add, Delete, PackFolder, PackSelected, NewFolder
};
struct PendingOp
{
    Pending kind = Pending::None;
    std::filesystem::path target;   // asset / folder path the op keys on
};

/// Background-packer state shared between the worker thread and the UI
/// thread. The UI thread polls `state` each frame and flips the modal
/// lifecycle based on it. `result` is the error string (empty = success)
/// published when state transitions to Done/Failed.
enum class PackState { Idle, Running, Done, Failed };
struct PackJob
{
    std::atomic<PackState>  state{PackState::Idle};
    std::thread             thread;
    std::string             result;       // set before flipping to Done/Failed
    std::string             atlasName;
    std::filesystem::path   outDir;
    std::size_t             fileCount = 0;

    ~PackJob()
    {
        if (thread.joinable()) thread.join();
    }
};
PackJob& packJob()
{
    // Singleton — one pack at a time is plenty. The editor UI gates new
    // starts when `state == Running`.
    static PackJob j;
    return j;
}

/// Render one entry and its subtree. `pending` accumulates whichever
/// context-menu command the user picked this frame. `scrollToSel` is
/// flipped to true for the row that was just promoted by an arrow-key
/// nav so we can ScrollHereY on it.
void renderEntry(Editor& editor,
                 const AssetEntry& e,
                 const char* filter,
                 bool& scrollToSel,
                 PendingOp& pending)
{
    if (!subtreeMatches(e, filter)) return;

    // A row is "selected" if its path is anywhere in the multi-selection
    // set. The primary (selectedAsset()) gets the same highlight — the
    // panel doesn't try to visually distinguish primary from extra for
    // assets since there's no handle-bearing operation that cares.
    auto inSelectionSet = [&](const std::filesystem::path& p) {
        for (const auto& s : editor.selectedAssets())
            if (s == p) return true;
        return false;
    };
    const bool isSel = inSelectionSet(e.absPath);

    switch (e.kind)
    {
    case AssetKind::File:
    case AssetKind::AtlasFrame:
    {
        // Leaf row: filter-hide if its own name doesn't match (but its
        // subtree was expanded to get here via a parent match — that's
        // OK, the parent still renders).
        if (!matches(e.displayName.c_str(), filter)) return;
        ImGui::PushID(e.absPath.string().c_str());
        ImGui::PushID(e.displayName.c_str());
        const char* prefix =
            e.kind == AssetKind::AtlasFrame ? "\xE2\x94\x80 " : "";
        std::string label = std::string(prefix) + e.displayName;
        if (ImGui::Selectable(label.c_str(), isSel))
        {
            // Three click modes, all restricted to File rows (AtlasFrame
            // rows stay single-select — their grouping is at the atlas
            // level):
            //   * plain click            → single-select, reset anchor
            //   * Cmd/Ctrl+click         → toggle membership, move anchor
            //   * Shift+click            → extend range from anchor to
            //                               target in visible leaf order
            const auto& io = ImGui::GetIO();
            const bool fileRow = e.kind == AssetKind::File;
            if (fileRow && io.KeyShift)
            {
                // File-only flat list in render order. AtlasFrame leaves
                // are excluded — they'd alias onto the atlas PNG path
                // (multiple frames share the same absPath), breaking
                // index lookups.
                auto flat = fileLeavesInOrder(editor.assets(), filter);
                const auto& anchor = sSelectionAnchor;
                int a = -1, b = -1;
                for (int i = 0; i < (int)flat.size(); ++i)
                {
                    if (flat[i] == anchor)   a = i;
                    if (flat[i] == e.absPath) b = i;
                }
                if (a < 0) a = b;   // no prior anchor → just this row
                if (a > b) std::swap(a, b);
                std::vector<std::filesystem::path> range(
                    flat.begin() + a, flat.begin() + b + 1);
                editor.setSelectedAssetsRange(range);
                // Don't move the anchor — subsequent shift-clicks extend
                // from the SAME anchor, matching OS file-manager UX.
            }
            else if (fileRow && (io.KeyCtrl || io.KeySuper))
            {
                editor.toggleSelectedAsset(e.absPath);
                sSelectionAnchor = e.absPath;
            }
            else
            {
                editor.setSelectedAsset(e.absPath);
                sSelectionAnchor = e.absPath;
            }
        }
        if (isSel && scrollToSel) { ImGui::SetScrollHereY(0.5f);
                                     scrollToSel = false; }

        // Drag source: hand the path (and atlas info, when this is a
        // frame) over to drop targets in the Scene tree so the user
        // can drop an image onto a parent row to insert it as a
        // child ImageView.  Payload is a fixed-size POD so ImGui can
        // copy it through its internal buffer without extra
        // allocation. Paths > 1023 chars get truncated.
        if (ImGui::BeginDragDropSource(
                ImGuiDragDropFlags_SourceNoDisableHover))
        {
            opencs::AssetDragPayload payload{};
            payload.kind = (e.kind == AssetKind::AtlasFrame)
                ? opencs::AssetDragPayload::Kind::AtlasFrame
                : opencs::AssetDragPayload::Kind::File;
            std::snprintf(payload.absPath, sizeof(payload.absPath),
                          "%s", e.absPath.string().c_str());
            std::snprintf(payload.atlasPlistPath,
                          sizeof(payload.atlasPlistPath), "%s",
                          e.atlasPlistPath.string().c_str());
            std::snprintf(payload.frameName,
                          sizeof(payload.frameName), "%s",
                          e.kind == AssetKind::AtlasFrame
                              ? e.displayName.c_str() : "");
            ImGui::SetDragDropPayload(opencs::kAssetDragPayloadId,
                                      &payload, sizeof(payload));
            ImGui::TextUnformatted(e.displayName.c_str());
            ImGui::EndDragDropSource();
        }

        if (ImGui::IsItemHovered())
        {
            if (e.kind == AssetKind::AtlasFrame)
                ImGui::SetTooltip("frame %s\natlas: %s",
                                  e.displayName.c_str(),
                                  e.atlasPlistPath.string().c_str());
            else
                ImGui::SetTooltip("%s\n%.1f KiB",
                                  e.absPath.string().c_str(),
                                  e.sizeBytes / 1024.0);
        }
        if (e.kind == AssetKind::File &&
            ImGui::BeginPopupContextItem())
        {
            // Right-click on a row that's already part of the multi-
            // selection → keep the whole set. Right-click on a row that
            // ISN'T in the set → collapse to just that row before the
            // menu commands run, matching most OS file-manager behavior.
            if (!inSelectionSet(e.absPath))
                editor.setSelectedAsset(e.absPath);
            const std::size_t selCount = editor.selectedAssets().size();

            if (ImGui::MenuItem("Add here…"))
                pending = { Pending::Add, e.absPath.parent_path() };
            if (ImGui::MenuItem("Delete"))
                pending = { Pending::Delete, e.absPath };
            if (selCount >= 2)
            {
                ImGui::Separator();
                if (ImGui::MenuItem("Pack into Atlas…"))
                    pending = { Pending::PackSelected, e.absPath };
            }
            else
            {
                ImGui::Separator();
                ImGui::TextDisabled(
                    "Pack into Atlas… (select 2+ with Shift/Cmd)");
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        ImGui::PopID();
        break;
    }

    case AssetKind::Folder:
    case AssetKind::Atlas:
    {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
        // Auto-expand when filter is active and a descendant matches —
        // lets the user see their hit without clicking every arrow.
        if (filter[0] != '\0') flags |= ImGuiTreeNodeFlags_DefaultOpen;

        // Folders rely on ImGui's built-in tree arrow as the only
        // chevron — the previous custom ▸ glyph stacked a second
        // arrow on top. Atlases keep ◈ since it's a TYPE marker
        // (distinguishes them from regular folders), not a chevron.
        const char* glyph = e.kind == AssetKind::Atlas
            ? "\xE2\x97\x88 "   // ◈
            : "";
        ImGui::PushID(e.absPath.string().c_str());
        const bool open = ImGui::TreeNodeEx(
            e.displayName.c_str(),
            flags,
            "%s%s",
            glyph,
            e.displayName.c_str());

        if (ImGui::BeginPopupContextItem())
        {
            if (e.kind == AssetKind::Folder)
            {
                if (ImGui::MenuItem("Add here…"))
                    pending = { Pending::Add, e.absPath };
                if (ImGui::MenuItem("New subfolder…"))
                    pending = { Pending::NewFolder, e.absPath };
                ImGui::Separator();
                if (ImGui::MenuItem("Pack folder with TexturePacker…"))
                    pending = { Pending::PackFolder, e.absPath };
            }
            else  // Atlas
            {
                if (ImGui::MenuItem("Delete atlas"))
                    pending = { Pending::Delete, e.absPath };
                ImGui::TextDisabled("plist: %s",
                    e.atlasPlistPath.filename().string().c_str());
            }
            ImGui::EndPopup();
        }

        if (open)
        {
            for (const auto& c : e.children)
                renderEntry(editor, c, filter, scrollToSel, pending);
            ImGui::TreePop();
        }
        ImGui::PopID();
        break;
    }
    }
}

/// Count every File + AtlasFrame leaf in the whole tree for the summary
/// row. Shown in the panel header the same way the flat view used to
/// report "N image(s)".
std::size_t countLeaves(const std::vector<AssetEntry>& tree)
{
    std::size_t n = 0;
    for (const auto& e : tree)
    {
        if (e.kind == AssetKind::File || e.kind == AssetKind::AtlasFrame)
            ++n;
        n += countLeaves(e.children);
    }
    return n;
}

}  // namespace


void drawAssetBrowserBody(Editor& editor)
{
    const bool panelFocused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    editor.setAssetPanelFocused(panelFocused);

    const auto& assets = editor.assets();

    // Arrow-nav state — flipped to true after we move the selection so the
    // just-selected row scrolls into view on the next render pass.
    bool scrollToSel = false;
    if (panelFocused)
    {
        const bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
        const bool up   = ImGui::IsKeyPressed(ImGuiKey_UpArrow,   true);
        if (down || up)
        {
            std::vector<std::filesystem::path> flat;
            for (const auto& a : assets) collectFlat(a, sFilter, flat);
            if (!flat.empty())
            {
                int idx = -1;
                for (int i = 0; i < (int)flat.size(); ++i)
                    if (flat[i] == editor.selectedAsset()) { idx = i; break; }
                const int step = down ? 1 : -1;
                const int n = (int)flat.size();
                const int next = ((idx < 0 ? 0 : idx) + step + n) % n;

                // Shift+Arrow extends — select the contiguous range
                // between anchor and the new cursor. Plain Arrow moves
                // a single cursor selection.
                const auto& io = ImGui::GetIO();
                if (io.KeyShift)
                {
                    auto fileFlat = fileLeavesInOrder(assets, sFilter);
                    int a = -1, b = -1;
                    for (int i = 0; i < (int)fileFlat.size(); ++i)
                    {
                        if (fileFlat[i] == sSelectionAnchor) a = i;
                        if (fileFlat[i] == flat[next])       b = i;
                    }
                    if (a < 0) a = b;
                    if (a > b) std::swap(a, b);
                    if (a >= 0 && b >= 0)
                    {
                        std::vector<std::filesystem::path> range(
                            fileFlat.begin() + a, fileFlat.begin() + b + 1);
                        editor.setSelectedAssetsRange(range);
                    }
                }
                else
                {
                    editor.setSelectedAsset(flat[next]);
                    sSelectionAnchor = flat[next];
                }
                scrollToSel = true;
            }
        }
    }

    ImGui::Text("%zu image(s)", countLeaves(assets));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "filter…",
                             sFilter, sizeof(sFilter));
    ImGui::TextDisabled("right-click a row for add / delete");

    ImGui::Separator();

    PendingOp pending;

    if (ImGui::BeginChild("##list", ImVec2(0, 0), false,
                          ImGuiWindowFlags_HorizontalScrollbar))
    {
        for (const auto& a : assets)
            renderEntry(editor, a, sFilter, scrollToSel, pending);

        // Background right-click (empty area) → Add into the Resources
        // root. Useful when there are no entries yet or the user wants
        // to drop a file at the top level.
        if (ImGui::BeginPopupContextWindow("##bg",
                ImGuiPopupFlags_MouseButtonRight |
                ImGuiPopupFlags_NoOpenOverItems))
        {
            if (ImGui::MenuItem("Add here…"))
                pending = { Pending::Add, editor.assetsRoot() };
            if (ImGui::MenuItem("New subfolder…"))
                pending = { Pending::NewFolder, editor.assetsRoot() };
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();

    // State carried across frames for the modals below. These are static
    // so the modal stays open while the user types / confirms — a plain
    // local would reset every frame.
    static std::filesystem::path sDeleteTarget;
    static std::filesystem::path sPackFolder;
    static char sPackName[128]       = "atlas";
    static char sNewFolderName[128]  = "folder";
    static std::filesystem::path sNewFolderParent;
    static bool sOpenPackModal      = false;
    static bool sOpenNewFolderModal = false;

    if (pending.kind == Pending::Delete)
    {
        sDeleteTarget = pending.target;
        sConfirmDelete = true;
    }
    else if (pending.kind == Pending::PackFolder)
    {
        sPackFolder    = pending.target;
        sOpenPackModal = true;
    }
    else if (pending.kind == Pending::PackSelected)
    {
        // Open the pack-selected modal — it reads the editor's current
        // multi-select set when the user clicks Pack, so no extra state
        // snapshot needed here.
        static bool sOpenPackSelectedModal_unused = false;
        (void)sOpenPackSelectedModal_unused;
        ImGui::OpenPopup("Pack selection into atlas");
    }
    else if (pending.kind == Pending::NewFolder)
    {
        sNewFolderParent    = pending.target;
        sOpenNewFolderModal = true;
    }
    else if (pending.kind == Pending::Add)
    {
        // Copy the picked file into the right-clicked folder so the user's
        // intent ("put it HERE") is preserved. Falls back to the Resources
        // root if target is empty.
        const auto src = pickImage();
        if (!src.empty())
        {
            const auto dstFolder = pending.target.empty()
                ? editor.assetsRoot()
                : pending.target;
            if (!dstFolder.empty())
            {
                std::error_code ec;
                const auto dst = dstFolder /
                    std::filesystem::path(src).filename();
                std::filesystem::copy_file(
                    src, dst,
                    std::filesystem::copy_options::skip_existing, ec);
                editor.rescanAssets();
            }
        }
    }

    // Delete shortcut: Delete or Backspace with an asset selected + focus
    // on the list. Routes through the same confirm modal.
    if (!editor.selectedAsset().empty() &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
         ImGui::IsKeyPressed(ImGuiKey_Backspace, false)))
    {
        sDeleteTarget = editor.selectedAsset();
        sConfirmDelete = true;
    }

    // -- Delete confirmation ------------------------------------------------
    if (sConfirmDelete) { ImGui::OpenPopup("Delete image?"); sConfirmDelete = false; }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Delete image?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Remove %s from disk?",
            sDeleteTarget.filename().string().c_str());
        ImGui::TextDisabled("%s", sDeleteTarget.string().c_str());
        ImGui::Spacing();
        if (ImGui::Button("Delete", ImVec2(120, 0)))
        {
            std::error_code ec;
            // Atlas deletion: remove both the .png and its sibling .plist.
            // The Editor's assets vector keys atlases on the .png path, so
            // sDeleteTarget is the png here.
            auto sibling = sDeleteTarget; sibling.replace_extension(".plist");
            std::filesystem::remove(sDeleteTarget, ec);
            if (std::filesystem::exists(sibling, ec))
                std::filesystem::remove(sibling, ec);
            if (editor.selectedAsset() == sDeleteTarget)
                editor.setSelectedAsset({});
            editor.rescanAssets();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // -- New-subfolder modal ------------------------------------------------
    if (sOpenNewFolderModal)
    {
        ImGui::OpenPopup("New folder");
        sOpenNewFolderModal = false;
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("New folder", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Under: %s", sNewFolderParent.string().c_str());
        ImGui::Spacing();
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("##folder", sNewFolderName, sizeof(sNewFolderName));
        ImGui::Spacing();
        if (ImGui::Button("Create", ImVec2(120, 0)))
        {
            if (sNewFolderName[0] != '\0' && !sNewFolderParent.empty())
            {
                std::error_code ec;
                std::filesystem::create_directory(
                    sNewFolderParent / sNewFolderName, ec);
                editor.rescanAssets();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // -- Pack-with-TexturePacker modal -------------------------------------
    if (sOpenPackModal) { ImGui::OpenPopup("Pack atlas"); sOpenPackModal = false; }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Pack atlas", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Folder: %s", sPackFolder.string().c_str());
        ImGui::TextDisabled(
            "Runs TexturePacker on every .png in the folder.\n"
            "Override binary via env: OCS_TEXTUREPACKER=/path/to/TexturePacker");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("##packname", sPackName, sizeof(sPackName));
        ImGui::SameLine();
        ImGui::TextDisabled(".png + .plist");
        ImGui::Spacing();
        if (ImGui::Button("Pack", ImVec2(120, 0)))
        {
            editor.packFolderWithTexturePacker(
                sPackFolder, sPackName[0] ? sPackName : "atlas");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // -- Pack selection into a new atlas -----------------------------------
    //
    // Two-phase UX: the modal opens on name/output-dir entry, then flips
    // to a progress view once the worker thread is spawned. Cancel on the
    // entry phase just closes the dialog; Cancel during Running is a
    // best-effort — the packer runs to completion, we then delete the
    // stale output (real process-kill is deferred to a follow-up PR).
    static char sPackSelName[128]         = "atlas_01";
    static char sPackSelOutDir[1024]      = "";
    static bool sPackSelPrefillDone       = false;
    static bool sPackSelCancelRequested   = false;
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Pack selection into atlas", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        const auto& sel = editor.selectedAssets();
        const auto state = packJob().state.load();

        if (!sPackSelPrefillDone && state == PackState::Idle)
        {
            // Prefill the output dir to the deepest common parent of all
            // selected files. Falls back to assetsRoot if the common
            // parent can't be established (e.g. mix of roots).
            std::filesystem::path common;
            if (!sel.empty())
            {
                common = sel.front().parent_path();
                for (std::size_t i = 1; i < sel.size(); ++i)
                {
                    const auto& p = sel[i].parent_path();
                    // Walk common up until it's an ancestor of p.
                    while (!common.empty())
                    {
                        auto it = p.string().find(common.string());
                        if (it == 0) break;
                        common = common.parent_path();
                    }
                }
            }
            if (common.empty()) common = editor.assetsRoot();
            std::snprintf(sPackSelOutDir, sizeof(sPackSelOutDir),
                          "%s", common.string().c_str());
            sPackSelPrefillDone    = true;
            sPackSelCancelRequested = false;
        }

        if (state == PackState::Idle || state == PackState::Failed)
        {
            ImGui::Text("Pack %zu file(s) into:", sel.size());
            ImGui::Spacing();
            ImGui::SetNextItemWidth(360);
            ImGui::InputText("Atlas name", sPackSelName, sizeof(sPackSelName));
            ImGui::SetNextItemWidth(360);
            ImGui::InputText("Output dir", sPackSelOutDir, sizeof(sPackSelOutDir));
            if (state == PackState::Failed)
            {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1),
                    "Last run failed:");
                ImGui::TextWrapped("%s", packJob().result.c_str());
            }
            ImGui::Spacing();
            const bool canPack = sPackSelName[0] != '\0' &&
                                 sPackSelOutDir[0] != '\0' &&
                                 sel.size() >= 2;
            ImGui::BeginDisabled(!canPack);
            if (ImGui::Button("Pack", ImVec2(120, 0)))
            {
                // Snapshot inputs for the worker — we don't want the
                // worker to race with the UI mutating the selection set.
                auto files = sel;
                std::string name   = sPackSelName;
                std::filesystem::path outDir = sPackSelOutDir;
                packJob().state    = PackState::Running;
                packJob().result.clear();
                packJob().atlasName = name;
                packJob().outDir    = outDir;
                packJob().fileCount = files.size();
                if (packJob().thread.joinable())
                    packJob().thread.join();
                packJob().thread = std::thread([files, name, outDir, &editor]{
                    const std::string err =
                        editor.packFilesIntoAtlas(files, name, outDir);
                    packJob().result = err;
                    packJob().state  = err.empty()
                        ? PackState::Done : PackState::Failed;
                });
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0)))
            {
                sPackSelPrefillDone = false;
                ImGui::CloseCurrentPopup();
            }
        }
        else if (state == PackState::Running)
        {
            // Indeterminate progress — TexturePacker CLI doesn't emit
            // machine-parseable progress. Show a spinner built from a
            // rotating dot pattern + the file count so the user sees
            // proof of life.
            const char frames[] = "|/-\\";
            const int  fi = (int)(ImGui::GetTime() * 8) & 3;
            ImGui::Text("%c  Packing %zu files → %s.png",
                        frames[fi], packJob().fileCount,
                        packJob().atlasName.c_str());
            ImGui::TextDisabled(
                "TexturePacker runs synchronously; cancel is best-effort.");
            ImGui::Spacing();
            ImGui::BeginDisabled(sPackSelCancelRequested);
            if (ImGui::Button("Cancel", ImVec2(120, 0)))
            {
                sPackSelCancelRequested = true;
            }
            ImGui::EndDisabled();
        }
        else if (state == PackState::Done)
        {
            // Thread has published result. Finalize: surface the new atlas
            // by selecting its png path so the tree scrolls to it next
            // frame (rescanAssets already ran inside packFilesIntoAtlas).
            if (packJob().thread.joinable())
                packJob().thread.join();
            auto atlasPng = packJob().outDir /
                (packJob().atlasName + ".png");
            if (sPackSelCancelRequested)
            {
                // Honor the cancel — remove the (completed) output so the
                // cancel actually has an effect, even if delayed.
                std::error_code ec;
                std::filesystem::remove(atlasPng, ec);
                auto plist = atlasPng; plist.replace_extension(".plist");
                std::filesystem::remove(plist, ec);
                editor.rescanAssets();
            }
            else
            {
                editor.setSelectedAsset(atlasPng);
            }
            packJob().state      = PackState::Idle;
            sPackSelPrefillDone  = false;
            sPackSelCancelRequested = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

}  // namespace opencs
