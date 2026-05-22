// Layout-constraints panel — Cocos Studio's auto-layout / "anchor magnet"
// properties that describe how a node positions and sizes itself relative
// to its parent when the parent resizes.
//
// The csd stores these as a mix of top-level attributes on the
// AbstractNodeData (PositionPercentXEnabled, PercentWidthEnabled, margins)
// and typed sub-elements (<PrePosition X Y/>, <PreSize X Y/>). We expose
// them in one place so the user doesn't have to hunt them down in the raw
// attributes table.

#include "UILayoutEditor/panels/Panels.h"
#include "UILayoutEditor/Editor.h"

#include "imgui/imgui.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace opencs
{

namespace
{
/// Edit a bool attribute stored as "True"/"False" on the node.
bool drawBoolAttr(Node& n, const char* label, const char* key)
{
    const std::string cur = n.attr(key, "False");
    bool v = (cur == "True");
    if (ImGui::Checkbox(label, &v))
    {
        n.setAttr(key, v ? "True" : "False");
        return true;
    }
    return false;
}

/// Edit a float attribute stored as "%.4f" on the node.
bool drawFloatAttr(Editor& editor, const char* label, const char* key,
                   float step = 1.0f, float fastStep = 10.0f)
{
    Node n = editor.selected();
    const std::string cur = n.attr(key, "0");
    float v = std::strtof(cur.c_str(), nullptr);
    ImGui::PushID(key);
    const bool changed = ImGui::InputFloat(label, &v, step, fastStep, "%.4f");
    ImGui::PopID();
    if (changed)
    {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.4f", v);
        n.setAttr(key, buf);
    }
    return changed;
}

/// Edit a Vec2 sub-element (e.g. <PrePosition X="" Y=""/>).
bool drawVec2Sub(Editor& editor, const char* label, const char* subTag,
                 const char* xKey, const char* yKey, const char* fmt = "%.4f")
{
    Node n = editor.selected();
    auto attrs = n.subAttrs(subTag);
    float vals[2] = {0, 0};
    for (const auto& [k, v] : attrs)
    {
        if      (k == xKey) vals[0] = std::strtof(v.c_str(), nullptr);
        else if (k == yKey) vals[1] = std::strtof(v.c_str(), nullptr);
    }
    ImGui::PushID(subTag);
    const bool changed = ImGui::InputFloat2(label, vals, fmt);
    ImGui::PopID();
    if (changed)
    {
        std::vector<std::pair<std::string, std::string>> next;
        char bx[32], by[32];
        std::snprintf(bx, sizeof(bx), "%.4f", vals[0]);
        std::snprintf(by, sizeof(by), "%.4f", vals[1]);
        bool wX = false, wY = false;
        for (const auto& [k, v] : attrs)
        {
            if      (k == xKey) { next.emplace_back(k, bx); wX = true; }
            else if (k == yKey) { next.emplace_back(k, by); wY = true; }
            else                  next.emplace_back(k, v);
        }
        if (!wX) next.emplace_back(xKey, bx);
        if (!wY) next.emplace_back(yKey, by);
        n.setSubAttrs(subTag, next);
    }
    return changed;
}
}  // namespace

void drawLayoutBody(Editor& editor)
{
    Node sel = editor.selected();
    if (!sel.valid())
    {
        ImGui::TextDisabled("No selection");
        return;
    }

    bool anyChanged = false;

    ImGui::TextDisabled(
        "Proportional placement relative to the parent — "
        "use these instead of absolute Position / Size when the parent may resize.");
    ImGui::Separator();

    // -- Percent toggles --
    ImGui::Text("Percent enabled");
    if (drawBoolAttr(sel, "Position X (%)",   "PositionPercentXEnabled")) anyChanged = true;
    ImGui::SameLine();
    if (drawBoolAttr(sel, "Position Y (%)",   "PositionPercentYEnabled")) anyChanged = true;
    if (drawBoolAttr(sel, "Size W (%)",       "PercentWidthEnable"))      anyChanged = true;
    ImGui::SameLine();
    if (drawBoolAttr(sel, "Size H (%)",       "PercentHeightEnable"))     anyChanged = true;

    ImGui::Separator();

    // -- Proportional position / size (0..1 of parent) --
    ImGui::Text("Proportional values");
    if (drawVec2Sub(editor, "PrePosition (0..1)", "PrePosition", "X", "Y")) anyChanged = true;
    if (drawVec2Sub(editor, "PreSize (0..1)",     "PreSize",     "X", "Y")) anyChanged = true;

    ImGui::Separator();

    // -- Margins (pixels from each edge of the parent) --
    ImGui::Text("Margins");
    if (drawFloatAttr(editor, "Left",   "LeftMargin"))   anyChanged = true;
    if (drawFloatAttr(editor, "Right",  "RightMargin"))  anyChanged = true;
    if (drawFloatAttr(editor, "Top",    "TopMargin"))    anyChanged = true;
    if (drawFloatAttr(editor, "Bottom", "BottomMargin")) anyChanged = true;

    if (anyChanged)
    {
        if (auto* d = editor.doc()) d->dirty = true;
        editor.syncNodeCsdToAx(sel);
    }
}

}  // namespace opencs
