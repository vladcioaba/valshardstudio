// Scene tree panel — recursive ImGui tree of <ObjectData>/<AbstractNodeData>
// nodes with click-to-select, right-click context menu (Add/Duplicate/Delete),
// and drag-drop for reordering + reparenting. All structural mutations go
// through the Editor (which maintains the undo stack).

#include "UILayoutEditor/panels/Panels.h"
#include "UILayoutEditor/Editor.h"
#include "UILayoutEditor/Log.h"

#include "axmol.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <unordered_set>

namespace opencs
{

namespace
{
constexpr const char* kDragPayload = "CSD_NODE";

/// Custom padlock icon button. Hit area is `size × size` (full row
/// height), so it's easy to click; the icon itself takes ~70% of
/// that, leaving comfortable padding. Five visual states:
///   Propagating   — bright orange, closed shackle (locks descendants too)
///   SelfOnly      — yellow, closed shackle (this node only)
///   Inherited     — dim gray, closed shackle, non-clickable
///   Hover-unlocked— light gray, open shackle
///   Resting       — faint outline, open shackle
/// Returns true on a real click that the caller should act on
/// (false for inherited rows so the descendant row can't toggle a
/// lock that doesn't belong to it).
enum class LockVis { None, SelfOnly, Propagating, Inherited };
bool drawLockIcon(const void* id, LockVis state)
{
    const float size = ImGui::GetFrameHeight();
    ImGui::PushID(id);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##lock", ImVec2(size, size));
    const bool inherited = (state == LockVis::Inherited);
    const bool clicked   = !inherited && ImGui::IsItemClicked();
    const bool hovered   = ImGui::IsItemHovered();
    auto*      dl        = ImGui::GetWindowDrawList();
    const bool selfLocked = (state == LockVis::Propagating ||
                             state == LockVis::SelfOnly);

    ImU32 col;
    switch (state)
    {
        case LockVis::Propagating:
            col = IM_COL32(255, 170,  80, 255); break;     // orange
        case LockVis::SelfOnly:
            col = IM_COL32(245, 220,  90, 255); break;     // yellow
        case LockVis::Inherited:
            col = IM_COL32(160, 160, 160, 150); break;     // dim gray
        default:   // None
            col = hovered
                ? IM_COL32(200, 200, 200, 220)
                : IM_COL32(130, 130, 130, 110);
            break;
    }

    // Centre the padlock in the hit-area square.
    const float cx = p0.x + size * 0.5f;
    const float cy = p0.y + size * 0.5f;
    // Body proportions: a small rounded rect roughly 55% wide by
    // 35% tall, sitting just below centre.
    const float bodyHalfW = size * 0.26f;
    const float bodyTopY  = cy + size * 0.03f;
    const float bodyBotY  = cy + size * 0.34f;
    // Shackle: an arc whose diameter matches the body width minus
    // a small inset, anchored just above the body.
    const float shackleR  = size * 0.19f;
    const float shackleCy = bodyTopY;
    const float thick     = std::max(1.2f, size * 0.10f);

    // Body — filled (locked) or outlined (unlocked) rounded rect.
    if (selfLocked || inherited)
    {
        dl->AddRectFilled(ImVec2(cx - bodyHalfW, bodyTopY),
                          ImVec2(cx + bodyHalfW, bodyBotY),
                          col, size * 0.10f);
    }
    else
    {
        dl->AddRect(ImVec2(cx - bodyHalfW, bodyTopY),
                    ImVec2(cx + bodyHalfW, bodyBotY),
                    col, size * 0.10f, 0, thick);
    }

    // Shackle — closed arc when locked, open (3/4 arc tipped right)
    // when unlocked. Stroked only, never filled.
    dl->PathClear();
    if (selfLocked || inherited)
        dl->PathArcTo(ImVec2(cx, shackleCy), shackleR,
                      3.14159265358979323846f, 3.14159265358979323846f * 2.0f, 14);
    else
        dl->PathArcTo(ImVec2(cx, shackleCy), shackleR,
                      3.14159265358979323846f, 3.14159265358979323846f * 1.75f, 12);
    dl->PathStroke(col, ImDrawFlags_None, thick);

    // Tooltip when hovered — explain the state + the Ctrl modifier.
    if (hovered)
    {
        const char* tip = nullptr;
        switch (state)
        {
            case LockVis::Propagating:
                tip = "Locked (this node + descendants)\n"
                      "Click to unlock  ·  Ctrl-click to lock self only"; break;
            case LockVis::SelfOnly:
                tip = "Locked (this node only)\n"
                      "Click to extend lock to descendants  ·  Ctrl-click to unlock"; break;
            case LockVis::Inherited:
                tip = "Locked by an ancestor"; break;
            default:
                tip = "Click to lock (with descendants)\n"
                      "Ctrl-click to lock just this node"; break;
        }
        ImGui::SetTooltip("%s", tip);
    }
    ImGui::PopID();
    return clicked;
}

/// Eye / hidden icon button. Three states:
///   Visible    — open eye, light gray fill (hover brightens)
///   Hidden     — closed eye + strike-through, red fill
///   Inherited  — open eye, dim gray (parent hides it). Click still
///                routes — toggling self when an ancestor hides will
///                feel a no-op until the user shows the ancestor.
enum class EyeVis { Visible, Hidden, Inherited };
bool drawEyeIcon(const void* id, EyeVis state)
{
    const float size = ImGui::GetFrameHeight();
    ImGui::PushID(id);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##eye", ImVec2(size, size));
    const bool clicked = ImGui::IsItemClicked();
    const bool hovered = ImGui::IsItemHovered();
    auto* dl = ImGui::GetWindowDrawList();

    const float cx = p0.x + size * 0.5f;
    const float cy = p0.y + size * 0.5f;
    const float eyeHalfW = size * 0.32f;
    const float eyeHalfH = size * 0.19f;
    const float pupilR   = size * 0.10f;
    const float thick    = std::max(1.2f, size * 0.10f);

    ImU32 col;
    switch (state)
    {
    case EyeVis::Hidden:
        col = hovered ? IM_COL32(220, 130, 130, 230)
                      : IM_COL32(170, 100, 100, 220);
        break;
    case EyeVis::Inherited:
        col = IM_COL32(120, 120, 120,  90);   // dim, non-interactive look
        break;
    default:   // Visible
        col = hovered ? IM_COL32(220, 220, 220, 230)
                      : IM_COL32(140, 140, 140, 130);
        break;
    }

    constexpr float kPi = 3.14159265358979323846f;
    dl->PathClear();
    dl->PathArcTo(ImVec2(cx, cy + eyeHalfH * 0.5f), eyeHalfW,
                  kPi * 1.10f, kPi * 1.90f, 16);
    dl->PathStroke(col, ImDrawFlags_None, thick);
    dl->PathClear();
    dl->PathArcTo(ImVec2(cx, cy - eyeHalfH * 0.5f), eyeHalfW,
                  kPi * 0.10f, kPi * 0.90f, 16);
    dl->PathStroke(col, ImDrawFlags_None, thick);

    if (state == EyeVis::Visible || state == EyeVis::Inherited)
        dl->AddCircleFilled(ImVec2(cx, cy), pupilR, col, 12);

    if (state == EyeVis::Hidden)
    {
        dl->AddLine(ImVec2(cx - eyeHalfW, cy - eyeHalfH * 0.9f),
                    ImVec2(cx + eyeHalfW, cy + eyeHalfH * 0.9f),
                    col, thick);
    }

    if (hovered)
    {
        const char* tip = nullptr;
        switch (state)
        {
        case EyeVis::Hidden:    tip = "Hidden — click to show"; break;
        case EyeVis::Inherited: tip = "Hidden by an ancestor"; break;
        default:                tip = "Visible — click to hide"; break;
        }
        ImGui::SetTooltip("%s", tip);
    }
    ImGui::PopID();
    return clicked;
}

// Known Cocos Studio UI ctypes the Add-Child selector offers. Ordered by
// expected frequency-of-use, not alphabetical. Extend over time.
constexpr const char* kNodeTypes[] = {
    "NodeObjectData",
    "SingleNodeObjectData",
    "SpriteObjectData",
    "ImageViewObjectData",
    "ButtonObjectData",
    "PanelObjectData",
    "TextObjectData",
    "TextBMFontObjectData",
    "TextAtlasObjectData",
    "TextFieldObjectData",
    "CheckBoxObjectData",
    "SliderObjectData",
    "LoadingBarObjectData",
    "ScrollViewObjectData",
    "ListViewObjectData",
    "PageViewObjectData",
    "LayoutObjectData",
    "ParticleObjectData",
    "GameMapObjectData",
    "ProjectNodeObjectData",
};

/// Context menu opened by the right-click popup. Returns after the popup
/// closes; mutations are queued until after we've finished walking the tree.
struct RowDropInfo
{
    ImVec2          rowMin;
    ImVec2          rowMax;
    pugi::xml_node  el;
    float           subtreeEndY = 0.0f;  // bottom of last descendant row
};

// drawNode pushes each rendered row into this vector while
// drawSceneTreeBody owns its lifetime. Pointer is set non-null
// only during the walk + nulled after, so out-of-band drawNode
// calls (none today, but guarded against) just skip the record.
static std::vector<RowDropInfo>* sGapRows = nullptr;

struct PendingMutation
{
    enum Kind {
        None, AddChild, Duplicate, Delete, MoveChild, MoveSibling,
        MoveAsFirstChild, MoveBefore,
        ZForward, ZBackward, ZFront, ZBack,
        Group, Ungroup
    };
    Kind kind = None;
    Node subject;       // node the user acted on
    Node target;        // for Move ops
};

void drawNode(Editor& editor, const Node& node, PendingMutation& pending,
              const std::unordered_set<void*>& forceOpen)
{
    const auto children = node.children();
    // Auto-expand ancestors of a freshly-changed selection so the
    // SetScrollHereY call below has a row to scroll TO. Without this,
    // selecting a node whose ancestor was collapsed (canvas click,
    // RPC `select`, Tab cycle) would have no rendered row, and the
    // tree would stay scrolled where it was.
    if (forceOpen.count(node.element().internal_object()))
        ImGui::SetNextItemOpen(true);
    // Highlight every node in the multi-selection set, not only the
    // primary `selected()`. Shift-click / rubber-band marquee leaves
    // multiple nodes in `_selection` and the user expects to see them
    // all marked in the tree.
    const bool selected = editor.isSelected(node);
    const bool isPrimary =
        node.element() == editor.selected().element();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen;
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (selected) flags |= ImGuiTreeNodeFlags_Selected;

    void* id = node.element().internal_object();

    // Lock-icon column on the LEFT, at a fixed X regardless of tree
    // depth. Right-edge alignment got the icon clipped off-screen
    // when the panel was narrow + the row's text was truncated, so we
    // pin it to the panel's left edge. The TreeNodeEx that follows
    // shifts to (lockColumnEnd + gap) so the disclosure arrow + label
    // never overlap the lock icon — the row is offset uniformly across
    // all depths, then the tree's own indent applies on top of that.
    const float rowOriginX = ImGui::GetCursorPosX();
    const float iconBtnW   = ImGui::GetFrameHeight();
    // Eye + lock columns on the LEFT, fixed X regardless of depth.
    // Layout: [inset][eye][gap][lock][gap][label].
    constexpr float kIconInsetPx = 2.0f;
    constexpr float kIconGapPx   = 2.0f;
    const float eyeStartX  = kIconInsetPx;
    const float lockStartX = eyeStartX + iconBtnW + kIconGapPx;
    const float labelMinX  = lockStartX + iconBtnW + kIconGapPx;

    // Capture the panel-left X (cursor minus the current tree indent)
    // ONCE at the top of the row. Re-reading after SameLine subtracts
    // the same indent from a cursor that's already past the eye, which
    // gave the root row's lock a 21 px rightward drift compared to
    // deeper rows where DC.Indent.x is non-zero.
    const float panelLeftX =
        std::max(0.0f,
                 ImGui::GetCursorPosX() - ImGui::GetCurrentWindow()->DC.Indent.x);

    // --- Eye icon: visibility toggle. Renders the node's `Visible`
    // attribute and toggles via Cocos Studio's native attr so the
    // change round-trips through the engine. The ax counterpart is
    // setVisible'd immediately so the canvas updates without
    // requiring a structural refresh.
    {
        ImGui::SetCursorPosX(panelLeftX + eyeStartX);
        const bool selfVisible = node.visible();
        const bool effVisible  = editor.isEffectivelyVisible(node);
        EyeVis vis = EyeVis::Visible;
        if      (!selfVisible)             vis = EyeVis::Hidden;
        else if (!effVisible)              vis = EyeVis::Inherited;
        if (drawEyeIcon(node.element().internal_object(), vis))
        {
            editor.pushUndo();
            Node nn = node;
            const bool nowVisible = !selfVisible;
            nn.setVisible(nowVisible);
            if (editor.doc()) editor.doc()->dirty = true;
            // Cascade ax-tree visibility on hide (and on show) so
            // ProjectNodeObjectData embeds — whose loaded children
            // live outside the pugi tree — also flip. Without this
            // cascade the user can toggle the eye and still see the
            // sprite because the embed root is a separate ax node
            // not reached by syncNodeCsdToAx alone.
            editor.syncNodeCsdToAx(nn);
            if (auto* axN = editor.axForCsd(nn))
            {
                std::function<void(ax::Node*)> walk = [&](ax::Node* p) {
                    if (!p) return;
                    for (auto* c : p->getChildren())
                    { c->setVisible(nowVisible); walk(c); }
                };
                walk(axN);
            }
        }
        ImGui::SameLine();
    }

    // --- Lock icon ----------------------------------------------------
    {
        const auto mode = node.lockMode();
        const bool inherited = (mode == Node::LockMode::None) &&
                               editor.isEffectivelyLocked(node);
        LockVis vis = LockVis::None;
        if      (inherited)                              vis = LockVis::Inherited;
        else if (mode == Node::LockMode::Propagating)    vis = LockVis::Propagating;
        else if (mode == Node::LockMode::Self)           vis = LockVis::SelfOnly;

        ImGui::SetCursorPosX(panelLeftX + lockStartX);
        if (drawLockIcon(node.element().internal_object(), vis))
        {
            const auto& io = ImGui::GetIO();
            const bool ctrl = io.KeyCtrl || io.KeySuper;
            Node::LockMode next;
            if (ctrl)
                next = (mode == Node::LockMode::Self)
                    ? Node::LockMode::None : Node::LockMode::Self;
            else
                next = (mode == Node::LockMode::Propagating)
                    ? Node::LockMode::None : Node::LockMode::Propagating;
            editor.pushUndo();
            Node nn = node;
            nn.setLockMode(next);
            if (editor.doc()) editor.doc()->dirty = true;
        }
        ImGui::SameLine();
        // Push the tree row past both icon columns so the disclosure
        // arrow + label never overlap. Depth-indented origin is the
        // floor for deeper nodes.
        ImGui::SetCursorPosX(std::max(rowOriginX, labelMinX));
    }

    // Brighter, more saturated row highlight when selected. Default
    // Header color is a muted blue that's easy to miss; primary
    // selection gets a vivid orange and multi-select extras get a
    // slightly lighter variant so the user can distinguish "the one
    // the Properties panel is editing" from the wider set.
    int popColors = 0;
    if (selected)
    {
        const ImVec4 primary(1.00f, 0.92f, 0.45f, 0.85f);  // light yellow
        const ImVec4 extra  (0.98f, 0.88f, 0.40f, 0.55f);  // dimmer yellow
        const ImVec4 hl = isPrimary ? primary : extra;
        // Default text is near-white, which becomes invisible on a
        // light yellow background. Force a dark text color for selected
        // rows so the label stays readable.
        const ImVec4 darkText(0.10f, 0.10f, 0.12f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Header,        hl);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, hl);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  hl);
        ImGui::PushStyleColor(ImGuiCol_Text,          darkText);
        popColors = 4;
    }
    const bool open =
        ImGui::TreeNodeEx(id, flags, "%s", node.displayLabel().c_str());
    if (popColors) ImGui::PopStyleColor(popColors);

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
    {
        const auto& io = ImGui::GetIO();
        if (io.KeyShift)
        {
            // Range select: anchor at the current primary selection,
            // pick every node up to and including this one in tree
            // (document pre-order) order. Falls back to single-select
            // when there's no anchor.
            editor.selectRange(editor.selected(), node);
        }
        else if (io.KeyCtrl || io.KeySuper)
        {
            // Toggle this row in the multi-selection set without
            // disturbing other members.
            editor.toggleSelected(node);
        }
        else
        {
            editor.setSelected(node);
        }
    }

    // When the selection just changed (tree click, canvas click, RPC
    // `select`, Tab cycle, ...), scroll the row into view. Use the
    // sticky `selectionScrollRequested` flag instead of the per-frame
    // `selectionChanged` — canvas clicks fire AFTER this panel
    // already rendered, so a per-frame flag would never reach us on
    // the same frame the user clicked. The Editor lifts the sticky
    // flag at end-of-render from `_selectionChanged`, so it survives
    // into next frame.
    if (selected && editor.selectionScrollRequested())
    {
        ImGui::SetScrollHereY(0.5f);
    }

    // Right-click context menu.
    if (ImGui::BeginPopupContextItem())
    {
        // Only collapse the selection to this one row when the user
        // right-clicked OUTSIDE the existing multi-select. Right-
        // clicking a row that's already in the set keeps every other
        // selected row intact, so "multi-select → right-click →
        // Group" stays a single fluid gesture.
        if (!editor.isSelected(node)) editor.setSelected(node);
        if (ImGui::MenuItem("Add Child"))
        {
            pending = {PendingMutation::AddChild, node, {}};
        }
        if (ImGui::MenuItem("Duplicate", nullptr, false,
                            node.element() != editor.doc()->rootNode.element()))
        {
            pending = {PendingMutation::Duplicate, node, {}};
        }
        if (ImGui::MenuItem("Delete", nullptr, false,
                            node.element() != editor.doc()->rootNode.element()))
        {
            pending = {PendingMutation::Delete, node, {}};
        }

        if (node.element() != editor.doc()->rootNode.element())
        {
            ImGui::Separator();
            if (ImGui::MenuItem("Bring to Front", "Cmd+Shift+]"))
                pending = {PendingMutation::ZFront, node, {}};
            if (ImGui::MenuItem("Bring Forward",  "Cmd+]"))
                pending = {PendingMutation::ZForward, node, {}};
            if (ImGui::MenuItem("Send Backward",  "Cmd+["))
                pending = {PendingMutation::ZBackward, node, {}};
            if (ImGui::MenuItem("Send to Back",   "Cmd+Shift+["))
                pending = {PendingMutation::ZBack, node, {}};
        }

        // Group / Ungroup. Group is only meaningful when the canvas
        // selection has 2+ members (the tree context-menu subject may
        // differ from the primary selection — the editor operates on
        // the full _selection set regardless). Ungroup applies to this
        // node if it has children and isn't root.
        {
            const bool canGroup   = editor.selection().size() >= 2;
            const bool canUngroup = !node.children().empty() &&
                node.element() != editor.doc()->rootNode.element();
            if (canGroup || canUngroup) ImGui::Separator();
            if (canGroup && ImGui::MenuItem("Group", "Cmd+G"))
                pending = {PendingMutation::Group, node, {}};
            if (canUngroup && ImGui::MenuItem("Ungroup", "Cmd+Shift+G"))
                pending = {PendingMutation::Ungroup, node, {}};
        }
        ImGui::EndPopup();
    }

    // Drag source: the subject is this node. Don't allow dragging root.
    if (node.element() != editor.doc()->rootNode.element() &&
        ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover))
    {
        pugi::xml_node el = node.element();
        ImGui::SetDragDropPayload(kDragPayload, &el, sizeof(el));
        ImGui::TextUnformatted(node.displayLabel().c_str());
        ImGui::EndDragDropSource();
    }

    // ---- Drop targets ---------------------------------------------------
    //
    // Horizontal-position-aware reparent. While a row drag is active:
    //   * Cursor in MIDDLE 50% of the row vertically → drop AS CHILD
    //     of this row.
    //   * Cursor in the TOP / BOTTOM 25% bands → insertion line
    //     between rows. The cursor's X position selects the target
    //     ancestor:
    //       - X >= row's text origin   → sibling of THIS row.
    //       - Every kIndentStep px LEFT of that origin walks one
    //         ancestor up: sibling of parent, then grandparent, ...
    //   * Right edge of the row (top/bottom band, X past the row
    //     width) → still drop-as-child fallback.
    // A thick blue line is drawn at the computed insert position so
    // the user sees exactly where the drop will land before releasing.
    // Use ImGui's own indent spacing so the preview line lines up
    // exactly with the row indentation the panel renders. Falls back
    // to a sane default if Style is mid-init.
    const float kIndentStep =
        std::max(8.0f, ImGui::GetStyle().IndentSpacing);
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    const float rowH = rowMax.y - rowMin.y;
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool dragActive = ImGui::IsDragDropActive();
    const bool rowHovered = dragActive &&
        mouse.y >= rowMin.y && mouse.y <= rowMax.y;

    enum class DropKind { None, AsChild, InsertBeforeAnc, InsertAfterAnc,
                          InsertAsFirstChild };
    DropKind dropKind = DropKind::None;
    int  ancStep   = 0;   // how many parents up — 0 = this row's parent
    float insertY  = 0.0f;
    float insertX0 = rowMin.x;
    if (rowHovered)
    {
        const float band = rowH * 0.25f;
        if (mouse.y < rowMin.y + band)
            dropKind = DropKind::InsertBeforeAnc;
        else if (mouse.y > rowMax.y - band)
            dropKind = DropKind::InsertAfterAnc;
        else
            dropKind = DropKind::AsChild;

        if (dropKind != DropKind::AsChild)
        {
            // Ancestor step keyed off cursor X measured against the
            // row's ACTUAL left edge (rowMin.x already reflects the
            // tree indent depth at draw time). Each full kIndentStep
            // of LEFT travel walks one ancestor up. Small +/-2 px
            // dead-zone so jittery cursors don't bump the step.
            const float dxLeft = rowMin.x - mouse.x;
            if (dxLeft > 2.0f)
                ancStep = std::min((int)(dxLeft / kIndentStep), 32);
            insertY  = (dropKind == DropKind::InsertBeforeAnc)
                ? rowMin.y : rowMax.y;
            // Line position matches the chosen ancestor depth: walk
            // back from the row's drawn label X by ancStep indent
            // columns. For ancStep=0 this is the row itself.
            insertX0 = rowMin.x - ancStep * kIndentStep;
        }
    }

    if (ImGui::BeginDragDropTarget())
    {
        if (auto* payload =
                ImGui::AcceptDragDropPayload(kDragPayload))
        {
            pugi::xml_node dragged;
            std::memcpy(&dragged, payload->Data, sizeof(dragged));
            // Diagnostic: surface every drop attempt so the user can
            // tell whether nothing happened because the gesture was
            // dropped (no payload accept) or because the move target
            // resolved to the same node (mEl == tEl bail).
            const Node draggedN(dragged);
            OCS_LOG_INFO("scene-tree",
                "drop: kind=%d ancStep=%d subject=%s target=%s",
                (int)dropKind, ancStep,
                draggedN.name().c_str(), node.name().c_str());
            if (dropKind == DropKind::AsChild)
                pending = {PendingMutation::MoveChild, Node(dragged), node};
            else
            {
                // Walk up ancestors by ancStep to pick the target.
                pugi::xml_node t = node.element();
                for (int i = 0; i < ancStep && t; ++i)
                {
                    auto p = t.parent();      // <Children>
                    if (!p) break;
                    auto grand = p.parent();  // ancestor abstract-node
                    if (!grand || grand == editor.doc()->rootNode.element())
                        break;
                    t = grand;
                }
                // InsertBefore: pugi's insert_copy_before lands the
                // node EXACTLY before `t`, regardless of where the
                // moving node currently lives. Direct verb avoids
                // the prev-sibling lookup pitfall where the dragged
                // node IS the prev sibling and the lookup returned
                // self → moveNode bailed on mEl==tEl.
                if (dropKind == DropKind::InsertBeforeAnc)
                    pending = {PendingMutation::MoveBefore,
                               Node(dragged), Node(t)};
                else
                    pending = {PendingMutation::MoveSibling,
                               Node(dragged), Node(t)};
            }
        }
        // Asset-browser payload — unchanged. Same MoveChild fallback
        // because dragging an image onto an arbitrary insertion line
        // is ambiguous; child semantics is the safe default.
        if (auto* payload =
                ImGui::AcceptDragDropPayload(kAssetDragPayloadId))
        {
            AssetDragPayload p{};
            std::memcpy(&p, payload->Data, sizeof(p));
            namespace fs = std::filesystem;
            const fs::path root = editor.assetsRoot();
            std::error_code ec;
            const fs::path imgRel =
                root.empty() ? fs::path(p.absPath)
                             : fs::relative(p.absPath, root, ec);
            const fs::path plistRel =
                (root.empty() || p.atlasPlistPath[0] == '\0')
                    ? fs::path(p.atlasPlistPath)
                    : fs::relative(p.atlasPlistPath, root, ec);
            editor.addImageChild(node,
                                 imgRel.generic_string(),
                                 plistRel.generic_string(),
                                 p.frameName);
        }
        ImGui::EndDragDropTarget();
    }

    // Insert-line preview, drawn on the foreground draw list so it
    // sits above the highlight + bg of every row.
    if (dragActive && rowHovered && dropKind != DropKind::AsChild)
    {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImU32 col = IM_COL32(60, 140, 240, 220);
        fg->AddLine(ImVec2(insertX0, insertY),
                    ImVec2(rowMax.x,  insertY),
                    col, 2.5f);
        fg->AddCircleFilled(ImVec2(insertX0, insertY), 3.5f, col);
    }
    else if (dragActive && rowHovered && dropKind == DropKind::AsChild)
    {
        // Drop AS CHILD — highlight the target row. Full-subtree
        // highlight would need a pre-pass for subtreeEndY which is
        // only known AFTER children draw; the per-frame end-of-frame
        // highlight from the math-gap detector covers the wider
        // subtree case anyway.
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImU32 hi   = IM_COL32(60, 140, 240, 60);
        const ImU32 edge = IM_COL32(60, 140, 240, 200);
        fg->AddRectFilled(rowMin, rowMax, hi);
        fg->AddRect(rowMin, rowMax, edge, 2.0f, 0, 1.5f);
    }

    // Record row info BEFORE recursion so DFS-order in gapRows
    // matches the on-screen order (parent first, then descendants).
    // subtreeEndY is patched in after the recursive draw finishes
    // — captures cursor.y at that point as the bottom of the
    // parent's full subtree, used by the drag-drop highlight rect.
    std::size_t myGapIdx = (std::size_t)-1;
    if (sGapRows)
    {
        myGapIdx = sGapRows->size();
        sGapRows->push_back(
            {rowMin, rowMax, node.element(), rowMax.y});
    }
    if (open)
    {
        for (const auto& child : children)
            drawNode(editor, child, pending, forceOpen);
        ImGui::TreePop();
    }
    if (sGapRows && myGapIdx != (std::size_t)-1)
        (*sGapRows)[myGapIdx].subtreeEndY =
            ImGui::GetCursorScreenPos().y;
}
}  // namespace

void drawSceneTreeBody(Editor& editor)
{
    auto* doc = editor.doc();
    if (!doc)
    {
        ImGui::TextWrapped("No document loaded.");
        if (!editor.loadError().empty())
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                               "%s", editor.loadError().c_str());
        }
        return;
    }

    ImGui::Text("%s  (%s)",
                editor.csdPath().filename().string().c_str(),
                doc->fileType().c_str());
    ImGui::TextDisabled("drag to reparent · drop-gap to reorder · right-click for actions");
    ImGui::Separator();

    // Publish focus state so the global arrow-key handler skips node nudges
    // while the user is navigating the tree.
    const bool panelFocused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    editor.setSceneTreeFocused(panelFocused);

    // Up / Down moves the selection through the flat DFS order while the
    // panel has focus. Properties + Layout panels follow automatically.
    if (panelFocused)
    {
        const bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
        const bool up   = ImGui::IsKeyPressed(ImGuiKey_UpArrow,   true);
        if (down || up)
        {
            auto all = editor.doc()->allNodes();
            if (!all.empty())
            {
                int idx = -1;
                for (int i = 0; i < (int)all.size(); ++i)
                    if (all[i].element() == editor.selected().element())
                    { idx = i; break; }
                const int step = down ? 1 : -1;
                const int n = (int)all.size();
                const int next = ((idx < 0 ? 0 : idx) + step + n) % n;
                editor.setSelected(all[next]);
            }
        }
    }

    PendingMutation pending;
    // Collect every drawn row for the math-based gap-drop detector.
    std::vector<RowDropInfo> gapRows;
    sGapRows = &gapRows;
    // Build the set of ancestors of the currently-selected node so
    // drawNode can force-open them this frame. Only fires when the
    // sticky scroll-request flag is set; on subsequent frames the
    // user's manual collapse/expand state is left alone.
    std::unordered_set<void*> forceOpen;
    if (editor.selectionScrollRequested() && editor.selected().valid())
    {
        auto path = editor.pathTo(editor.selected());
        Node cur = doc->rootNode;
        // Include the root + every ancestor of the selected node, but
        // NOT the selected node itself — leave its own kids' state
        // alone.
        forceOpen.insert(cur.element().internal_object());
        for (std::size_t i = 0; i + 1 < path.size(); ++i)
        {
            auto kids = cur.children();
            if (path[i] >= kids.size()) break;
            cur = kids[path[i]];
            forceOpen.insert(cur.element().internal_object());
        }
    }
    drawNode(editor, doc->rootNode, pending, forceOpen);
    sGapRows = nullptr;

    // Math-based gap detection: zero layout space. Walks the
    // collected per-row rects, identifies which between-rows gap
    // (or below-the-last-row tail) the cursor sits in, and on a
    // mouse-release with an active drag payload performs the move
    // via MoveSibling against the chosen ancestor (cursor-X
    // determines the climb depth).
    if (ImGui::IsDragDropActive() && !gapRows.empty())
    {
        const ImGuiPayload* peek = ImGui::GetDragDropPayload();
        const bool isOurPayload = peek &&
            peek->IsDataType(kDragPayload);
        if (isOurPayload)
        {
            const ImVec2 mp = ImGui::GetMousePos();
            // Find which between-rows gap contains the cursor.
            // Band lives STRICTLY between rowMax[i] and rowMin[i+1]
            // — no 3 px overlap into the rows themselves (the row
            // itself owns its top/bottom drop bands via the existing
            // BeginDragDropTarget). Tail zone past the last row
            // extends to the panel's bottom content edge so a drop
            // anywhere below the tree resolves to "after last row".
            const float panelBottom =
                ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
            int idx = -1;
            float gapY = 0.0f;
            for (std::size_t i = 0; i < gapRows.size(); ++i)
            {
                const float topY = gapRows[i].rowMax.y;
                const float botY = (i + 1 < gapRows.size())
                    ? gapRows[i + 1].rowMin.y
                    : panelBottom;
                if (botY <= topY) continue;  // adjacent rows touch
                if (mp.y >= topY && mp.y <= botY)
                {
                    idx  = (int)i;
                    gapY = (i + 1 < gapRows.size())
                        ? (gapRows[i].rowMax.y + gapRows[i+1].rowMin.y) * 0.5f
                        : gapRows[i].rowMax.y + 4.0f;
                    break;
                }
            }
            if (idx >= 0)
            {
                const auto& upper = gapRows[idx];
                const float kStep = std::max(
                    8.0f, ImGui::GetStyle().IndentSpacing);
                const float dxLeft = upper.rowMin.x - mp.x;
                int step = 0;
                if (dxLeft > 2.0f)
                    step = std::min((int)(dxLeft / kStep), 32);
                const float x0 = upper.rowMin.x - step * kStep;
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                const ImU32 col = IM_COL32(60, 140, 240, 220);
                fg->AddLine(ImVec2(x0, gapY),
                            ImVec2(upper.rowMax.x, gapY),
                            col, 2.5f);
                fg->AddCircleFilled(ImVec2(x0, gapY), 3.5f, col);

                // Highlight rect: the destination parent + its full
                // subtree. For "sibling of t" the destination parent
                // is t's csd parent — find that node's entry in
                // gapRows by walking pugi parents up two levels
                // (abstract → <Children> → abstract parent).
                pugi::xml_node t = upper.el;
                for (int s = 0; s < step && t; ++s)
                {
                    auto pp = t.parent();
                    if (!pp) break;
                    auto gg = pp.parent();
                    if (!gg || gg == editor.doc()->rootNode.element())
                        break;
                    t = gg;
                }
                pugi::xml_node parentAbs = t.parent().parent();  // <Children>.parent()
                for (const auto& r : gapRows)
                {
                    if (r.el == parentAbs)
                    {
                        const ImU32 hi =
                            IM_COL32(60, 140, 240, 60);
                        const ImU32 he =
                            IM_COL32(60, 140, 240, 160);
                        fg->AddRectFilled(
                            ImVec2(r.rowMin.x, r.rowMin.y),
                            ImVec2(r.rowMax.x, r.subtreeEndY),
                            hi);
                        fg->AddRect(
                            ImVec2(r.rowMin.x, r.rowMin.y),
                            ImVec2(r.rowMax.x, r.subtreeEndY),
                            he, 2.0f, 0, 1.5f);
                        break;
                    }
                }

                // On release, do the move. Manual accept since we
                // bypassed BeginDragDropTarget — ImGui treats
                // unaccepted drops as cancelled, which is fine for
                // the source (we already mutated the doc).
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                {
                    pugi::xml_node dragged;
                    std::memcpy(&dragged, peek->Data, sizeof(dragged));
                    pugi::xml_node t = upper.el;
                    for (int i = 0; i < step && t; ++i)
                    {
                        auto p = t.parent();
                        if (!p) break;
                        auto g = p.parent();
                        if (!g ||
                            g == editor.doc()->rootNode.element())
                            break;
                        t = g;
                    }
                    OCS_LOG_INFO("scene-tree",
                        "math-gap drop: step=%d subject=%s target=%s",
                        step, Node(dragged).name().c_str(),
                        Node(t).name().c_str());
                    pending = {PendingMutation::MoveSibling,
                               Node(dragged), Node(t)};
                }
            }
        }
    }
    // Consume the sticky request so the auto-scroll only fires once
    // per selection change. Without this, the panel would re-scroll
    // every frame and fight the user's manual scroll.
    editor.consumeSelectionScroll();

    // Delete key binding: while this panel has focus and a non-root node is
    // selected, Delete / Backspace removes it.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
    {
        const bool wantDel = ImGui::IsKeyPressed(ImGuiKey_Delete,    false) ||
                             ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
        if (wantDel)
        {
            Node sel = editor.selected();
            if (sel.valid() && sel.element() != doc->rootNode.element())
            {
                pending.kind = PendingMutation::Delete;
                pending.subject = sel;
            }
        }
    }

    // Add-child opens a type-selector modal instead of immediately appending
    // a generic NodeObjectData. The chosen ctype is queued as a second-pass
    // mutation.
    static bool         sOpenTypePicker = false;
    static Node         sTypePickerParent;
    static int          sTypePickerChoice = 0;

    if (pending.kind == PendingMutation::AddChild)
    {
        sTypePickerParent = pending.subject;
        sTypePickerChoice = 0;
        sOpenTypePicker   = true;
        pending.kind      = PendingMutation::None;   // handled below
    }

    switch (pending.kind)
    {
        case PendingMutation::Duplicate:   editor.duplicateNode(pending.subject); break;
        case PendingMutation::Delete:      editor.deleteNode(pending.subject); break;
        case PendingMutation::MoveChild:   editor.moveNode(pending.subject, pending.target, /*asChild=*/true);  break;
        case PendingMutation::MoveSibling: editor.moveNode(pending.subject, pending.target, /*asChild=*/false); break;
        case PendingMutation::MoveAsFirstChild:
            editor.moveNodeAsFirstChild(pending.subject, pending.target); break;
        case PendingMutation::MoveBefore:
            editor.moveNodeBefore(pending.subject, pending.target); break;
        case PendingMutation::ZForward:    editor.bringForward(pending.subject); break;
        case PendingMutation::ZBackward:   editor.sendBackward(pending.subject); break;
        case PendingMutation::ZFront:      editor.bringToFront(pending.subject); break;
        case PendingMutation::ZBack:       editor.sendToBack(pending.subject);   break;
        case PendingMutation::Group:       editor.groupSelected();    break;
        case PendingMutation::Ungroup:
            editor.setSelected(pending.subject);
            editor.ungroupSelected();
            break;
        default: break;
    }

    if (sOpenTypePicker) { ImGui::OpenPopup("Add node"); sOpenTypePicker = false; }
    ImVec2 pc = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(pc, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Add node", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextDisabled(
            "Child of: %s",
            sTypePickerParent.valid() ? sTypePickerParent.name().c_str() : "?");
        ImGui::Separator();

        constexpr int kCount = (int)(sizeof(kNodeTypes) / sizeof(kNodeTypes[0]));
        if (ImGui::BeginListBox("##types", ImVec2(-FLT_MIN, 16 * ImGui::GetTextLineHeightWithSpacing())))
        {
            for (int i = 0; i < kCount; ++i)
            {
                const bool sel = (i == sTypePickerChoice);
                if (ImGui::Selectable(kNodeTypes[i], sel)) sTypePickerChoice = i;
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                {
                    if (sTypePickerParent.valid())
                        editor.addChildNode(sTypePickerParent, kNodeTypes[i]);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndListBox();
        }
        ImGui::Spacing();
        if (ImGui::Button("Add", ImVec2(120, 0)))
        {
            if (sTypePickerParent.valid() &&
                sTypePickerChoice >= 0 && sTypePickerChoice < kCount)
                editor.addChildNode(sTypePickerParent,
                                    kNodeTypes[sTypePickerChoice]);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

}  // namespace opencs
