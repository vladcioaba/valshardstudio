// Document + structural-mutation implementations extracted from Editor.cpp.
//
// Covers the pieces that form what could eventually be pulled into a
// DocumentStore service:
//   * map-invalidation hook (invalidateAxMaps)
//   * DFS path ↔ Node addressing (pathTo / nodeAtPath)
//   * undo/redo with full-doc snapshots (pushUndo, undo, redo, restoreSnapshot)
//   * structural edits (delete / duplicate / add / move)
//
// Keeping them in a separate TU lets the main Editor.cpp stay focused on
// render-loop orchestration. No behavior change — pure code move.

#include "UILayoutEditor/Editor.h"

#include "axmol.h"

#include <pugixml.hpp>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace opencs
{

namespace
{

/// Return the <Children> container under `n`, creating it if missing.
pugi::xml_node ensureChildrenContainer(pugi::xml_node n)
{
    auto c = n.child(tags::kChildren);
    if (!c) c = n.append_child(tags::kChildren);
    return c;
}

/// Is `ancestor` an ancestor of (or equal to) `descendant`?
bool isAncestor(pugi::xml_node ancestor, pugi::xml_node descendant)
{
    for (auto cur = descendant; cur; cur = cur.parent())
        if (cur == ancestor) return true;
    return false;
}

}  // namespace


bool Editor::isSelected(const Node& n) const
{
    if (!n.valid()) return false;
    auto el = n.element();
    for (const auto& s : _selection)
        if (s.element() == el) return true;
    return false;
}


void Editor::addToSelection(Node n)
{
    if (!n.valid()) return;
    if (!isSelected(n)) _selection.push_back(n);
    // Most-recent-add becomes the primary: this is what the properties panel
    // keys off of, and most apps treat "the one you just shift-clicked" as
    // the active one.
    _selected = n;
    _selectionChanged = true;
    _lastTouched = LastTouched::Node;
}


void Editor::toggleSelected(Node n)
{
    if (!n.valid()) return;
    auto el = n.element();
    for (auto it = _selection.begin(); it != _selection.end(); ++it)
    {
        if (it->element() == el)
        {
            _selection.erase(it);
            // Primary: last surviving member, or empty if nothing left.
            _selected = _selection.empty() ? Node() : _selection.back();
            _selectionChanged = true;
            _lastTouched = LastTouched::Node;
            return;
        }
    }
    addToSelection(n);
}


void Editor::clearSelection()
{
    if (!_selection.empty() || _selected.valid()) _selectionChanged = true;
    _selection.clear();
    _selected = Node();
}


void Editor::invalidateAxMaps()
{
    // After a structural mutation the csd pugi nodes referenced by our maps
    // may have been moved or erased, and the axmol tree now reflects a stale
    // structure. Dropping the maps disables the overlay + hit-test until the
    // user saves + restarts (live rebuild is a follow-up PR).
    _axmolRoot = nullptr;
    _axToCsd.clear();
    _csdToAx.clear();
    _axOrderDfs.clear();
    _dragMode = DragMode::None;
}


std::vector<std::size_t> Editor::pathTo(const Node& n) const
{
    std::vector<std::size_t> path;
    if (!_doc || !n.valid()) return path;
    // Walk parents up to root, recording each child's index among its siblings.
    auto el = n.element();
    auto rootEl = _doc->rootNode.element();
    while (el && el != rootEl)
    {
        auto parent = el.parent();  // <Children>
        if (!parent) return {};
        std::size_t idx = 0;
        bool found = false;
        for (auto c = parent.first_child(); c; c = c.next_sibling())
        {
            if (c == el) { found = true; break; }
            auto nameSv = c.name();
            if (nameSv == pugi::string_view_t(tags::kAbstractNode) ||
                nameSv == pugi::string_view_t(tags::kObjectData))
                ++idx;
        }
        if (!found) return {};
        path.push_back(idx);
        el = parent.parent();  // skip <Children> container
    }
    std::reverse(path.begin(), path.end());
    return path;
}


Node Editor::nodeAtPath(const std::vector<std::size_t>& path) const
{
    if (!_doc) return Node();
    Node cur = _doc->rootNode;
    for (auto idx : path)
    {
        auto children = cur.children();
        if (idx >= children.size()) return Node();
        cur = children[idx];
    }
    return cur;
}


void Editor::pushUndo()
{
    if (!_doc) return;
    // Stamp the current primary selection with a wall-clock unix
    // timestamp so the `tree-since <unix>` RPC can list nodes that
    // changed after a given moment. This is best-effort (a mutation
    // that targets a non-selected node would not be timestamped) but
    // covers the common case where the panel or RPC verb operates
    // on the active selection.
    if (_selected.valid())
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld",
                      (long long)std::time(nullptr));
        _selected.setAttr("OCSModified", buf);
    }
    std::ostringstream ss;
    _doc->doc.save(ss, "", pugi::format_raw | pugi::format_no_declaration);
    _undoStack.push_back(ss.str());
    _redoStack.clear();
    // Mirror the fresh snapshot to the sidecar so a crash mid-edit
    // doesn't lose work. saveToCurrent / saveAs clean the file up on
    // a successful real save.
    writeAutosave();
}

void Editor::writeAutosave() const
{
    if (!_doc || _csdPath.empty()) return;
    namespace fs = std::filesystem;
    fs::path backup = _csdPath;
    backup += ".1";   // e.g. Scene.csd → Scene.csd.1
    try
    {
        std::ostringstream ss;
        _doc->doc.save(ss, "",
                       pugi::format_raw | pugi::format_no_declaration);
        std::string xml = ss.str();
        std::ofstream out(backup, std::ios::binary | std::ios::trunc);
        if (out) out.write(xml.data(),
                           static_cast<std::streamsize>(xml.size()));
    }
    catch (...)
    {
        // Best-effort; never block editing because autosave failed.
    }
}

void Editor::removeAutosave() const
{
    if (_csdPath.empty()) return;
    namespace fs = std::filesystem;
    fs::path backup = _csdPath;
    backup += ".1";
    std::error_code ec;
    fs::remove(backup, ec);
}


void Editor::restoreSnapshot(const std::string& xml,
                             const std::vector<std::size_t>& selPath)
{
    if (!_doc) return;
    try { _doc->reloadFromString(xml); }
    catch (const std::exception& e)
    {
        _statusMsg = std::string("undo failed: ") + e.what();
        return;
    }
    // Pugi nodes are invalid after reload. Re-resolve selection via path;
    // fall back to root if the path is gone.
    _selected = nodeAtPath(selPath);
    if (!_selected.valid()) _selected = _doc->rootNode;
    _selectionChanged = true;
    _dragMode = DragMode::None;   // cancel any in-flight drag state

    // Rebuild the ax tree from the restored doc so structural undos
    // (add/delete/move/group) actually flip the canvas back. Falls back
    // to the in-place map resync when no host hook is registered.
    refreshAxFromCsd();
}


void Editor::undo()
{
    if (_undoStack.empty()) return;
    // Save current state onto the redo stack, then restore the previous one.
    std::ostringstream cur;
    _doc->doc.save(cur, "", pugi::format_raw | pugi::format_no_declaration);
    _redoStack.push_back(cur.str());
    auto selPath = pathTo(_selected);

    auto xml = std::move(_undoStack.back());
    _undoStack.pop_back();
    restoreSnapshot(xml, selPath);
    _statusMsg = "undo";
}


void Editor::redo()
{
    if (_redoStack.empty()) return;
    std::ostringstream cur;
    _doc->doc.save(cur, "", pugi::format_raw | pugi::format_no_declaration);
    _undoStack.push_back(cur.str());
    auto selPath = pathTo(_selected);

    auto xml = std::move(_redoStack.back());
    _redoStack.pop_back();
    restoreSnapshot(xml, selPath);
    _statusMsg = "redo";
}


void Editor::deleteNode(const Node& n)
{
    if (!_doc || !n.valid()) return;
    if (n.element() == _doc->rootNode.element()) return;  // never delete root

    pushUndo();
    auto el = n.element();
    auto parent = el.parent();     // <Children>
    const bool selWasThisOrDesc =
        _selected.valid() &&
        isAncestor(el, _selected.element());
    parent.remove_child(el);
    _doc->dirty = true;
    if (selWasThisOrDesc)
    {
        _selected = _doc->rootNode;
        _selectionChanged = true;
    }
    // Structural change → ask the host to rebuild the ax tree from the
    // mutated csd. The previous in-place map resync left orphan ax
    // nodes whenever a non-trailing child was deleted (the parallel
    // walk used min(axKids,csdKids) and skipped the tail).
    refreshAxFromCsd();
    _statusMsg = "deleted node";
}


Node Editor::duplicateNode(const Node& n)
{
    if (!_doc || !n.valid()) return Node();
    auto el = n.element();
    if (el == _doc->rootNode.element()) return Node();
    auto parent = el.parent();
    if (!parent) return Node();

    pushUndo();
    auto copy = parent.insert_copy_after(el, el);
    _doc->dirty = true;
    Node dup(copy);
    _selected = dup;
    _selectionChanged = true;
    // Rebuild the ax tree so the duplicated node renders. The
    // structural change wouldn't be picked up by the truncating
    // parallel walk in rebuildAxMapsAndSync.
    refreshAxFromCsd();
    _statusMsg = "duplicated node";
    return dup;
}


Node Editor::addChildNode(const Node& parent, const char* ctype)
{
    if (!_doc || !parent.valid()) return Node();

    pushUndo();
    auto childrenEl = ensureChildrenContainer(parent.element());
    auto newEl = childrenEl.append_child(tags::kAbstractNode);
    newEl.append_attribute("Name").set_value("NewNode");
    newEl.append_attribute("ctype").set_value(ctype ? ctype : "NodeObjectData");
    // Minimal default Size + Position so the overlay has something to draw
    // once the scene is reloaded.
    auto size = newEl.append_child("Size");
    size.append_attribute("X").set_value("100.0000");
    size.append_attribute("Y").set_value("100.0000");
    auto pos = newEl.append_child("Position");
    pos.append_attribute("X").set_value("0.0000");
    pos.append_attribute("Y").set_value("0.0000");

    _doc->dirty = true;
    Node created(newEl);
    _selected = created;
    _selectionChanged = true;
    // Rebuild the ax tree so the new node renders immediately. The host
    // hook serializes the in-memory pugi doc to XML and re-runs CSLoader,
    // so pre-existing pugi handles (including `created`) stay valid —
    // only the ax pointers rotate, and setAxmolRoot rebuilds the
    // csd↔ax maps.
    refreshAxFromCsd();
    _statusMsg = "added node";
    return created;
}


Node Editor::addImageChild(const Node& parent,
                           const std::string& imageRelPath,
                           const std::string& plistRel,
                           const std::string& frameName)
{
    if (!_doc || !parent.valid() || imageRelPath.empty()) return Node();

    pushUndo();
    auto childrenEl = ensureChildrenContainer(parent.element());
    auto newEl = childrenEl.append_child(tags::kAbstractNode);

    // Name = filename stem so the tree label reads sensibly. Falls back
    // to "NewImage" when the path has no stem (shouldn't happen, but
    // doesn't hurt).
    namespace fs = std::filesystem;
    fs::path img(imageRelPath);
    std::string stem = img.stem().string();
    if (stem.empty()) stem = "NewImage";
    newEl.append_attribute("Name").set_value(stem.c_str());
    newEl.append_attribute("ctype").set_value("ImageViewObjectData");

    // Modest default footprint so the new node is visible right away.
    auto size = newEl.append_child("Size");
    size.append_attribute("X").set_value("200.0000");
    size.append_attribute("Y").set_value("200.0000");
    auto pos = newEl.append_child("Position");
    pos.append_attribute("X").set_value("0.0000");
    pos.append_attribute("Y").set_value("0.0000");
    auto anchor = newEl.append_child("AnchorPoint");
    anchor.append_attribute("ScaleX").set_value("0.5000");
    anchor.append_attribute("ScaleY").set_value("0.5000");

    // FileData — Cocos Studio's standard image-reference sub-element.
    // MarkedSubImage when the asset comes from an atlas frame; Normal
    // for a loose file. CSLoader cares about the exact attribute order
    // here, so we emit Type / Path / Plist in that sequence.
    auto fd = newEl.append_child("FileData");
    const bool atlas = !plistRel.empty() && !frameName.empty();
    fd.append_attribute("Type").set_value(atlas ? "MarkedSubImage" : "Normal");
    fd.append_attribute("Path").set_value(
        atlas ? frameName.c_str() : imageRelPath.c_str());
    fd.append_attribute("Plist").set_value(atlas ? plistRel.c_str() : "");

    _doc->dirty = true;
    Node created(newEl);
    _selected = created;
    _selectionChanged = true;
    refreshAxFromCsd();
    _statusMsg = "added image";
    return created;
}


namespace
{

/// Rebuild the DFS draw order from a given ax root. The editor caches this
/// list for hit-tests; group/ungroup reshape the tree so it needs a fresh
/// walk. Uses a local recursive lambda, no heap allocs beyond the target.
void rebuildAxOrderDfs(ax::Node* root, std::vector<ax::Node*>& out)
{
    out.clear();
    if (!root) return;
    std::function<void(ax::Node*)> walk = [&](ax::Node* n) {
        if (!n) return;
        out.push_back(n);
        for (auto* c : n->getChildren()) walk(c);
    };
    walk(root);
}

}  // namespace


void Editor::groupSelected()
{
    if (!_doc || _selection.empty() || !_axmolRoot)
    {
        _statusMsg = "group: nothing to group";
        return;
    }
    // Every selected node must share the same csd parent. Otherwise the
    // group's placement is ambiguous — bail with a status-bar message.
    auto firstCsdParent = _selection.front().element().parent();
    if (!firstCsdParent) { _statusMsg = "group: can't wrap root"; return; }
    for (const auto& n : _selection)
    {
        if (n.element() == _doc->rootNode.element())
        { _statusMsg = "group: can't wrap root"; return; }
        if (n.element().parent() != firstCsdParent)
        { _statusMsg = "group: selection must share a parent"; return; }
    }

    // Common ax parent — pick the first selected node's parent; all
    // selection members share it per the check above.
    auto firstAxIt = _csdToAx.find(
        _selection.front().element().internal_object());
    if (firstAxIt == _csdToAx.end() || !firstAxIt->second)
    { _statusMsg = "group: no ax mapping"; return; }
    ax::Node* commonAxParent = firstAxIt->second->getParent();
    if (!commonAxParent) { _statusMsg = "group: no ax parent"; return; }

    pushUndo();

    // Capture each selected node's WORLD position BEFORE any structural
    // change — once we reparent the axmol nodes their parent chain
    // flips under them and convertToWorldSpace would return the new
    // (wrong) world value.
    std::vector<std::pair<ax::Node*, ax::Vec2>> worldAnchors;
    worldAnchors.reserve(_selection.size());
    for (const auto& n : _selection)
    {
        auto it = _csdToAx.find(n.element().internal_object());
        if (it == _csdToAx.end() || !it->second) continue;
        const auto localPos = it->second->getPosition();
        // Position is the anchor's coord in parent-local; convert via
        // the old parent's world transform to pin the world-anchor.
        const ax::Vec2 worldP =
            commonAxParent->convertToWorldSpace(localPos);
        worldAnchors.emplace_back(it->second, worldP);
    }
    if (worldAnchors.empty()) { _statusMsg = "group: no valid nodes"; return; }

    // Scaffold the new csd group: identity transform so the local coord
    // frame of the group equals its parent's (see math below). The
    // actual Position for each child is rewritten after the reparent.
    auto newCsdEl = firstCsdParent.append_child(tags::kAbstractNode);
    newCsdEl.append_attribute("Name").set_value("Group");
    newCsdEl.append_attribute("ctype").set_value("NodeObjectData");
    auto sizeEl = newCsdEl.append_child("Size");
    sizeEl.append_attribute("X").set_value("0.0000");
    sizeEl.append_attribute("Y").set_value("0.0000");
    auto posEl = newCsdEl.append_child("Position");
    posEl.append_attribute("X").set_value("0.0000");
    posEl.append_attribute("Y").set_value("0.0000");
    auto anchorEl = newCsdEl.append_child("AnchorPoint");
    anchorEl.append_attribute("ScaleX").set_value("0.0000");
    anchorEl.append_attribute("ScaleY").set_value("0.0000");
    auto newCsdChildren = newCsdEl.append_child(tags::kChildren);

    // Parallel ax node with matching identity transform. Retain across
    // the addChild so the local refcount stays balanced.
    ax::Node* newAx = ax::Node::create();
    newAx->setPosition(0.0f, 0.0f);
    newAx->setAnchorPoint(ax::Vec2(0.0f, 0.0f));
    commonAxParent->addChild(newAx);

    // Move every selected child — csd via append_move (preserves the pugi
    // internal_object ptr so our maps stay valid), axmol via retain/
    // removeChild/addChild (preserves pointer + internal state).
    for (const auto& [axN, worldP] : worldAnchors)
    {
        auto& csdNode = _axToCsd[axN];
        newCsdChildren.append_move(csdNode.element());

        axN->retain();
        commonAxParent->removeChild(axN, /*cleanup=*/false);
        newAx->addChild(axN);
        axN->release();

        // Recompute Position in the new parent's coord frame so the child
        // remains visually where it was before the reparent.
        const ax::Vec2 newLocal = newAx->convertToNodeSpace(worldP);
        axN->setPosition(newLocal);
        csdNode.writePair("Position",
                          "X", newLocal.x, "Y", newLocal.y);
    }

    Node newGroup(newCsdEl);
    _axToCsd[newAx] = newGroup;
    _csdToAx[newCsdEl.internal_object()] = newAx;
    rebuildAxOrderDfs(_axmolRoot, _axOrderDfs);

    _doc->dirty = true;
    _selected = newGroup;
    _selection.clear();
    _selection.push_back(newGroup);
    _selectionChanged = true;
    _statusMsg = "grouped " + std::to_string(worldAnchors.size()) + " nodes";
}


void Editor::ungroupSelected()
{
    if (!_doc || !_selected.valid() || !_axmolRoot)
    { _statusMsg = "ungroup: no selection"; return; }
    if (_selected.element() == _doc->rootNode.element())
    { _statusMsg = "ungroup: can't dissolve root"; return; }

    auto groupCsdEl       = _selected.element();
    auto groupCsdChildren = groupCsdEl.child(tags::kChildren);
    auto parentChildrenEl = groupCsdEl.parent();    // <Children> container
    if (!parentChildrenEl || !groupCsdChildren)
    { _statusMsg = "ungroup: nothing to dissolve"; return; }

    // Collect group's children by csd element order so insert_move_after
    // preserves sibling order in the grandparent.
    std::vector<Node> groupChildren;
    for (auto c = groupCsdChildren.first_child(); c; c = c.next_sibling())
    {
        auto sv = c.name();
        if (sv == pugi::string_view_t(tags::kAbstractNode) ||
            sv == pugi::string_view_t(tags::kObjectData))
            groupChildren.emplace_back(c);
    }
    if (groupChildren.empty())
    { _statusMsg = "ungroup: group has no children"; return; }

    auto axIt = _csdToAx.find(groupCsdEl.internal_object());
    if (axIt == _csdToAx.end() || !axIt->second)
    { _statusMsg = "ungroup: no ax mapping"; return; }
    ax::Node* groupAx  = axIt->second;
    ax::Node* parentAx = groupAx->getParent();
    if (!parentAx)
    { _statusMsg = "ungroup: no ax parent"; return; }

    pushUndo();

    // Capture world positions BEFORE the reparent. Use each child's axmol
    // transform so contentSize / anchor / scale are all accounted for.
    std::vector<std::pair<ax::Node*, ax::Vec2>> worldAnchors;
    worldAnchors.reserve(groupChildren.size());
    for (const auto& child : groupChildren)
    {
        auto it = _csdToAx.find(child.element().internal_object());
        if (it == _csdToAx.end() || !it->second) continue;
        const auto localPos = it->second->getPosition();
        const ax::Vec2 worldP = groupAx->convertToWorldSpace(localPos);
        worldAnchors.emplace_back(it->second, worldP);
    }

    // Walk children in original order. insert_move_after places each
    // unwrapped child immediately after the group in the grandparent's
    // child list — the reference anchor walks forward as we insert, so
    // the resulting order matches the in-group order.
    pugi::xml_node afterRef = groupCsdEl;
    for (auto& [axN, worldP] : worldAnchors)
    {
        auto& csdNode = _axToCsd[axN];
        afterRef = parentChildrenEl.insert_move_after(
            csdNode.element(), afterRef);

        axN->retain();
        groupAx->removeChild(axN, /*cleanup=*/false);
        parentAx->addChild(axN);
        axN->release();

        const ax::Vec2 newLocal = parentAx->convertToNodeSpace(worldP);
        axN->setPosition(newLocal);
        csdNode.writePair("Position",
                          "X", newLocal.x, "Y", newLocal.y);
    }

    // Drop the group's map entries before the csd/ax removal wipes them.
    _csdToAx.erase(groupCsdEl.internal_object());
    _axToCsd.erase(groupAx);
    parentChildrenEl.remove_child(groupCsdEl);
    parentAx->removeChild(groupAx, /*cleanup=*/true);

    rebuildAxOrderDfs(_axmolRoot, _axOrderDfs);

    // Select the grandparent (parent of the <Children> container) so the
    // properties panel has something useful to show after the dissolve.
    Node newPrimary(parentChildrenEl.parent());
    _selected = newPrimary;
    _selection.clear();
    if (newPrimary.valid()) _selection.push_back(newPrimary);
    _selectionChanged = true;
    _doc->dirty = true;
    _statusMsg = "ungrouped " + std::to_string(worldAnchors.size()) + " nodes";
}


void Editor::moveNode(const Node& moving, const Node& target, bool asChild)
{
    if (!_doc || !moving.valid() || !target.valid()) return;
    auto mEl = moving.element();
    auto tEl = target.element();
    if (mEl == tEl) return;
    if (mEl == _doc->rootNode.element()) return;      // can't move root
    if (isAncestor(mEl, tEl)) return;                 // would create cycle

    pushUndo();
    pugi::xml_node destParent;
    pugi::xml_node newEl;
    if (asChild)
    {
        // Reparent: insert as last child of target.
        destParent = ensureChildrenContainer(tEl);
        newEl = destParent.append_copy(mEl);
    }
    else
    {
        // Reorder / reparent-sibling: insert after target.
        destParent = tEl.parent();
        if (!destParent) { _undoStack.pop_back(); return; }
        newEl = destParent.insert_copy_after(mEl, tEl);
    }
    mEl.parent().remove_child(mEl);

    _doc->dirty = true;
    _selected = Node(newEl);
    _selectionChanged = true;
    // moveNode only mutates the csd doc; the ax tree isn't reparented
    // here. The previous in-place map resync produced wrong mappings
    // whenever the move shifted child indices (drift cascades through
    // every sibling after the moved node), so reload the ax tree from
    // csd to guarantee structure parity.
    refreshAxFromCsd();
    _statusMsg = asChild ? "reparented" : "reordered";
}

void Editor::moveNodeBefore(const Node& moving, const Node& target)
{
    if (!_doc || !moving.valid() || !target.valid()) return;
    auto mEl = moving.element();
    auto tEl = target.element();
    if (mEl == tEl) return;
    if (mEl == _doc->rootNode.element()) return;
    if (isAncestor(mEl, tEl)) return;

    pushUndo();
    auto destParent = tEl.parent();
    if (!destParent) { _undoStack.pop_back(); return; }
    auto newEl = destParent.insert_copy_before(mEl, tEl);
    mEl.parent().remove_child(mEl);

    _doc->dirty = true;
    _selected = Node(newEl);
    _selectionChanged = true;
    refreshAxFromCsd();
    _statusMsg = "moved (before)";
}

void Editor::moveNodeAsFirstChild(const Node& moving, const Node& parent)
{
    if (!_doc || !moving.valid() || !parent.valid()) return;
    auto mEl = moving.element();
    auto pEl = parent.element();
    if (mEl == pEl) return;
    if (mEl == _doc->rootNode.element()) return;
    if (isAncestor(mEl, pEl)) return;

    pushUndo();
    auto destParent = ensureChildrenContainer(pEl);
    // pugi has no insert_copy_first; prepend manually by inserting
    // before the current first child. Falls back to append when the
    // container is empty.
    pugi::xml_node newEl;
    auto firstAbs = destParent.first_child();
    while (firstAbs)
    {
        const auto nm = std::string(firstAbs.name());
        if (nm == "AbstractNodeData" || nm == "ObjectData") break;
        firstAbs = firstAbs.next_sibling();
    }
    if (firstAbs)
        newEl = destParent.insert_copy_before(mEl, firstAbs);
    else
        newEl = destParent.append_copy(mEl);
    mEl.parent().remove_child(mEl);

    _doc->dirty = true;
    _selected = Node(newEl);
    _selectionChanged = true;
    refreshAxFromCsd();
    _statusMsg = "moved";
}

}  // namespace opencs
