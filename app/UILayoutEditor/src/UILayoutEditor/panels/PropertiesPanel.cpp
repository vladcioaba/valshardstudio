// Properties panel — shows the selected node's geometry sub-elements
// (<Position>, <Size>, <AnchorPoint>, <Scale>) as editable floats, plus the
// raw attribute table. Edits write through to the XML model immediately and
// flag the document dirty.

#include "UILayoutEditor/panels/Panels.h"
#include "UILayoutEditor/Editor.h"
#include "UILayoutEditor/PlistAtlas.h"
#include "UILayoutEditor/Timeline.h"

#include <cstring>

#include "imgui/imgui.h"
#include "ImGui/ImGuiPresenter.h"
#include "axmol.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace opencs
{

namespace
{

std::string formatFloat(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    return std::string(buf);
}

/// Call immediately after drawing a property widget. If the widget was
/// just activated this frame (mouse-down, keyboard focus, first typed
/// char), snapshot the document for undo. That produces exactly one
/// undo entry per interaction — a drag on a ColorEdit or a typed
/// sequence in InputFloat doesn't flood the stack.
void snapshotOnActivation(Editor& editor)
{
    if (ImGui::IsItemActivated()) editor.pushUndo();
}

/// Render a `name: value` row — ImGui's built-in LabelText is value-on-left
/// which reads backwards. This keeps the property name on the left and the
/// value aligned in a consistent column to the right.
void nameValueRow(const char* name, const char* value)
{
    ImGui::TextUnformatted(name);
    ImGui::SameLine(130.0f);
    ImGui::TextUnformatted(value ? value : "");
}
void nameValueRowf(const char* name, const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    nameValueRow(name, buf);
}

/// Show two float fields for a sub-element like <Position X="" Y=""/>. Uses
/// xKey/yKey so we can reuse this for AnchorPoint's ScaleX/ScaleY etc.
void drawVec2SubElement(Editor& editor,
                        const char* label,
                        const char* subTag,
                        const char* xKey,
                        const char* yKey)
{
    Node n = editor.selected();
    auto attrs = n.subAttrs(subTag);
    float x = 0.0f, y = 0.0f;
    for (const auto& [k, v] : attrs)
    {
        if (k == xKey) x = std::strtof(v.c_str(), nullptr);
        else if (k == yKey) y = std::strtof(v.c_str(), nullptr);
    }

    float vals[2] = {x, y};
    ImGui::PushID(subTag);
    const bool changed = ImGui::InputFloat2(label, vals, "%.4f");
    snapshotOnActivation(editor);
    if (changed)
    {
        std::vector<std::pair<std::string, std::string>> next;
        // Preserve any other attributes that sub-element might carry.
        bool wroteX = false, wroteY = false;
        for (const auto& [k, v] : attrs)
        {
            if (k == xKey) { next.emplace_back(k, formatFloat(vals[0])); wroteX = true; }
            else if (k == yKey) { next.emplace_back(k, formatFloat(vals[1])); wroteY = true; }
            else next.emplace_back(k, v);
        }
        if (!wroteX) next.emplace_back(xKey, formatFloat(vals[0]));
        if (!wroteY) next.emplace_back(yKey, formatFloat(vals[1]));
        n.setSubAttrs(subTag, next);
        if (auto* d = editor.doc()) d->dirty = true;
        // Keep the live axmol node in step so the canvas reflects the edit.
        editor.syncNodeCsdToAx(n);
    }
    ImGui::PopID();
}

/// Aggregate stats for a multi-selection of assets: count, total bytes,
/// per-extension histogram. Shown instead of the single-image preview
/// when the user has shift/cmd-selected 2+ files so the panel stays
/// useful for bulk operations.
void drawAssetMultiStats(Editor& editor)
{
    const auto& sel = editor.selectedAssets();
    namespace fs = std::filesystem;

    ImGui::Text("%zu images selected", sel.size());
    ImGui::Separator();

    std::uintmax_t total = 0;
    std::unordered_map<std::string, int> byExt;
    int missing = 0;
    for (const auto& p : sel)
    {
        std::error_code ec;
        auto sz = fs::file_size(p, ec);
        if (ec) { ++missing; continue; }
        total += sz;
        auto ext = p.extension().string();
        for (auto& c : ext)
            c = static_cast<char>(std::tolower((unsigned char)c));
        ++byExt[ext];
    }
    nameValueRowf("total size", "%.1f KiB", total / 1024.0);
    if (missing > 0) nameValueRowf("missing", "%d", missing);

    ImGui::Spacing();
    ImGui::TextDisabled("By extension");
    if (ImGui::BeginTable("##ext", 2,
                          ImGuiTableFlags_Borders |
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("ext");
        ImGui::TableSetupColumn("count");
        ImGui::TableHeadersRow();
        for (const auto& [ext, n] : byExt)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(ext.empty() ? "(none)" : ext.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d", n);
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled(
        "Shift/Cmd/Ctrl-click an image (or Shift+Up/Down) to adjust the set.\n"
        "Right-click a row → \xE2\x80\x9CPack into Atlas\xE2\x80\xA6\xE2\x80\x9D to group.");
}

void drawAssetDetails(Editor& editor)
{
    const auto& p = editor.selectedAsset();
    namespace fs = std::filesystem;

    ImGui::Text("Image");
    ImGui::Separator();
    nameValueRow("name", p.filename().string().c_str());
    auto ext = p.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::toupper((unsigned char)c));
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    nameValueRow("format", ext.c_str());
    nameValueRow("path", p.string().c_str());

    std::error_code ec;
    auto bytes = fs::file_size(p, ec);
    if (!ec)
        nameValueRowf("size", "%.1f KiB", bytes / 1024.0);

    // -- Image preview with zoom slider.
    //
    // Cache the Texture2D* by path so we call TextureCache::addImage only
    // when the selected asset actually changes. Fetching every frame was
    // triggering a visible flicker — likely a frame-use bookkeeping race
    // between axmol's TextureCache and ImGuiPresenter::useTexture.
    {
        static std::filesystem::path sLoadedPath;
        static ax::Texture2D* sCachedTex = nullptr;
        if (p != sLoadedPath)
        {
            sCachedTex = ax::Director::getInstance()
                             ->getTextureCache()
                             ->addImage(p.string());
            sLoadedPath = p;
        }

        if (sCachedTex)
        {
            static float sZoom = 1.0f;
            ImGui::Spacing();
            ImGui::SetNextItemWidth(160);
            ImGui::SliderFloat("zoom", &sZoom, 0.1f, 4.0f, "%.2fx");

            const auto szPx = sCachedTex->getContentSizeInPixels();
            const float maxW = ImGui::GetContentRegionAvail().x;
            const float fitScale =
                (szPx.width > 0) ? std::min(1.0f, maxW / szPx.width) : 1.0f;
            const float w = szPx.width  * fitScale * sZoom;
            const float h = szPx.height * fitScale * sZoom;

            auto [texID, ref] =
                ax::extension::ImGuiPresenter::getInstance()->useTexture(sCachedTex);
            (void)ref;
            if (texID)
                ImGui::Image(texID, ImVec2(w, h));

            nameValueRowf("pixels", "%.0f x %.0f", szPx.width, szPx.height);
        }
    }

    ImGui::Spacing();

    const auto basename = p.filename().string();
    auto refs = editor.findAssetReferences(basename);
    ImGui::Text("References: %zu", refs.size());
    ImGui::Separator();
    if (refs.empty())
    {
        ImGui::TextDisabled("No csd nodes reference this image.");
    }
    else if (ImGui::BeginTable("##refs", 2,
                               ImGuiTableFlags_Borders |
                               ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Node");
        ImGui::TableSetupColumn("ctype");
        ImGui::TableHeadersRow();
        for (const auto& n : refs)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(n.element().internal_object());
            const auto name = n.name().empty() ? n.tag() : n.name();
            if (ImGui::Selectable(name.c_str(), false,
                                  ImGuiSelectableFlags_SpanAllColumns))
            {
                editor.setSelected(n);
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(n.ctype().c_str());
        }
        ImGui::EndTable();
    }
}

}  // namespace

void drawPropertiesBody(Editor& editor)
{
    editor.setPropertiesFocused(
        ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows));

    // Keyframe editor — when a keyframe is selected in the Timeline
    // panel, surface its frame index, tween flag, easing type, and
    // a type-aware value editor at the top. Node properties continue
    // to render below so the user keeps both contexts in view.
    if (editor._tlSelectedKf.valid())
    {
        TimelineRef tl(editor._tlSelectedKf.timelineEl);
        KeyframeRef kf(editor._tlSelectedKf.frameEl);
        const TimelineProperty prop = tl.property();

        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.55f, 1.0f),
            "Keyframe — %s @ %d",
            timelinePropertyDisplay(prop), kf.frameIndex());
        ImGui::SameLine();
        if (ImGui::SmallButton("X##kf-deselect"))
            editor.clearSelectedKeyframe();
        ImGui::Separator();

        int idx = kf.frameIndex();
        if (ImGui::DragInt("Frame##kf", &idx, 0.5f))
        {
            editor.pushUndo();
            kf.setFrameIndex(std::max(0, idx));
        }
        bool tw = kf.tween();
        if (ImGui::Checkbox("Tween##kf", &tw))
        {
            editor.pushUndo();
            kf.setTween(tw);
        }
        static const char* kEasingItems =
            "Linear\0Sine in\0Sine out\0Sine in/out\0"
            "Quad in\0Quad out\0Quad in/out\0"
            "Cubic in\0Cubic out\0Cubic in/out\0"
            "Quart in\0Quart out\0Quart in/out\0"
            "Quint in\0Quint out\0Quint in/out\0"
            "Expo in\0Expo out\0Expo in/out\0"
            "Circ in\0Circ out\0Circ in/out\0"
            "Elastic in\0Elastic out\0Elastic in/out\0"
            "Back in\0Back out\0Back in/out\0"
            "Bounce in\0Bounce out\0Bounce in/out\0\0";
        int ease = std::max(0, kf.easingType());
        if (ImGui::Combo("Easing##kf", &ease, kEasingItems))
        {
            editor.pushUndo();
            kf.setEasingType(ease);
        }

        // Type-aware value editor.
        KeyValue kv = kf.read(prop);
        bool changed = false;
        switch (prop)
        {
        case TimelineProperty::Position:
        case TimelineProperty::AnchorPoint:
        case TimelineProperty::Scale:
        case TimelineProperty::RotationSkew:
        case TimelineProperty::Skew:
        {
            float v[2] = { kv.vx, kv.vy };
            const float step =
                (prop == TimelineProperty::Scale) ? 0.01f : 0.5f;
            if (ImGui::DragFloat2("Value##kfv", v, step))
            {
                kv.vx = v[0]; kv.vy = v[1];
                changed = true;
            }
            break;
        }
        case TimelineProperty::Alpha:
        case TimelineProperty::ZOrder:
            if (ImGui::DragInt("Value##kfv", &kv.intVal, 0.5f))
                changed = true;
            break;
        case TimelineProperty::CColor:
        {
            float c[4] = {
                kv.r / 255.0f, kv.g / 255.0f,
                kv.b / 255.0f, kv.alphaAttr / 255.0f
            };
            if (ImGui::ColorEdit4("Color##kfv", c))
            {
                kv.r = (int)(c[0] * 255.0f);
                kv.g = (int)(c[1] * 255.0f);
                kv.b = (int)(c[2] * 255.0f);
                kv.alphaAttr = (int)(c[3] * 255.0f);
                kv.a = 255;
                changed = true;
            }
            break;
        }
        case TimelineProperty::VisibleForFrame:
            if (ImGui::Checkbox("Visible##kfv", &kv.boolVal))
                changed = true;
            break;
        case TimelineProperty::Event:
        {
            char buf[256] = {};
            std::strncpy(buf, kv.strA.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("Value##kfv", buf, sizeof(buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                kv.strA = buf;
                changed = true;
            }
            break;
        }
        case TimelineProperty::FileData:
        case TimelineProperty::InnerAction:
        case TimelineProperty::Unknown:
            ImGui::TextDisabled("(no inline editor for this property)");
            break;
        }
        if (changed)
        {
            editor.pushUndo();
            kf.write(prop, kv);
            // Force evaluator to re-apply on next render.
            editor._tlEvaluatorAppliedFrame = -2;
        }
        ImGui::Separator();
    }

    // Priority: show whichever channel the user touched most recently.
    // Clicking a scene node (tree / canvas / rubber-band) flips the
    // channel to Node and you get color pickers, gradient, text props.
    // Clicking an image row flips it to Asset so the image preview +
    // reference table show up. Either side can drive the panel without
    // the other disappearing behind an aggressive short-circuit.
    const auto last = editor.lastTouched();
    const bool preferAsset =
        last == Editor::LastTouched::Asset &&
        (!editor.selectedAsset().empty() ||
         !editor.selectedAssets().empty());

    Node sel = editor.selected();
    if (preferAsset)
    {
        if (editor.selectedAssets().size() >= 2)
            drawAssetMultiStats(editor);
        else if (!editor.selectedAsset().empty())
            drawAssetDetails(editor);
        return;
    }
    if (!sel.valid())
    {
        if (editor.selectedAssets().size() >= 2)
        {
            drawAssetMultiStats(editor);
            return;
        }
        if (!editor.selectedAsset().empty())
        {
            drawAssetDetails(editor);
            return;
        }
        ImGui::TextDisabled("No selection");
        return;
    }

    // -- Identity --
    {
        std::string name = sel.name();
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", name.c_str());
        const bool nameChanged = ImGui::InputText("Name", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue);
        snapshotOnActivation(editor);
        if (nameChanged)
        {
            sel.setName(buf);
            if (auto* d = editor.doc()) d->dirty = true;
        }

        nameValueRow("ctype", sel.ctype().c_str());
        nameValueRow("tag",   sel.tag().c_str());
        if (auto t = sel.nodeTag())   nameValueRowf("Tag",       "%d", *t);
        if (auto t = sel.actionTag()) nameValueRowf("ActionTag", "%d", *t);

        // Visible flag — same data the Scene-tree eye icon edits
        // (Cocos Studio's `Visible` attr, with `VisibleForFrame`
        // also cleared on show). Label leaks the inherited-hidden
        // case so the user knows why a self-visible node is dark
        // on the canvas.
        {
            bool selfVisible = sel.visible();
            ImGui::PushID("Visible");
            const bool changed = ImGui::Checkbox("Visible", &selfVisible);
            snapshotOnActivation(editor);
            if (changed)
            {
                Node nn = sel;
                nn.setVisible(selfVisible);
                if (auto* d = editor.doc()) d->dirty = true;
                editor.syncNodeCsdToAx(nn);
            }
            if (sel.visible() && !editor.isEffectivelyVisible(sel))
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.85f, 0.65f, 0.30f, 1.0f),
                                   "(hidden by ancestor)");
            }
            ImGui::PopID();
        }
    }

    ImGui::Separator();

    // -- Geometry --
    drawVec2SubElement(editor, "Position", "Position", "X", "Y");
    drawVec2SubElement(editor, "Size", "Size", "X", "Y");
    drawVec2SubElement(editor, "Scale", "Scale", "ScaleX", "ScaleY");
    drawVec2SubElement(editor, "Anchor", "AnchorPoint", "ScaleX", "ScaleY");
    drawVec2SubElement(editor, "PrePosition", "PrePosition", "X", "Y");
    drawVec2SubElement(editor, "PreSize", "PreSize", "X", "Y");

    // Rotation (single float, degrees). Reads the attribute directly off the
    // AbstractNodeData element (not a sub-element).
    {
        const auto rotAttr = sel.attr("Rotation", "");
        float r = rotAttr.empty() ? 0.0f : std::strtof(rotAttr.c_str(), nullptr);
        ImGui::PushID("Rotation");
        const bool rotChanged =
            ImGui::InputFloat("Rotation", &r, 1.0f, 15.0f, "%.4f");
        snapshotOnActivation(editor);
        if (rotChanged)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.4f", r);
            sel.setAttr("Rotation", buf);
            if (auto* d = editor.doc()) d->dirty = true;
            editor.syncNodeCsdToAx(sel);
        }
        ImGui::PopID();
    }

    // ZOrder — integer. Drives axmol's setLocalZOrder. Higher = drawn on
    // top of siblings with lower ZOrder.
    {
        int z = editor.getZOrder(sel);
        ImGui::PushID("ZOrder");
        const bool zChanged = ImGui::InputInt("ZOrder", &z, 1, 10);
        snapshotOnActivation(editor);
        if (zChanged)
        {
            editor.setZOrder(sel, z);
        }
        ImGui::PopID();
    }

    // Helper: look up the node's ax counterpart. Several of the sections
    // below write both csd (for persistence) and ax (for live preview in
    // the canvas) — nullptr-safe: ax paths are skipped if no mapping.
    ax::Node* axSel = nullptr;
    {
        if (auto* d = editor.doc())
        {
            (void)d;
            // The Editor owns the csd→ax map; expose via a minimal hop:
            // re-selecting the current node is a no-op but guarantees the
            // axSel lookup sees the current state.
        }
    }
    // Direct access — Editor exposes selected() but not the map. We
    // fall back to walking via the attribute write paths (which already
    // touch the ax side through syncNodeCsdToAx below where relevant).

    // Flags: TouchEnable (ui::Widget interactivity) and ClipAble (panel
    // clips its children to its bounds). Both stored as "True"/"False"
    // attrs — absent attrs default to False except TouchEnable which
    // Cocos nodes flip to True for any widget-derived node.
    {
        auto boolRow = [&](const char* label, const char* attr,
                           bool defaultVal)
        {
            const auto cur = sel.attr(attr, defaultVal ? "True" : "False");
            bool v = (cur == "True");
            ImGui::PushID(attr);
            const bool c = ImGui::Checkbox(label, &v);
            snapshotOnActivation(editor);
            if (c)
            {
                sel.setAttr(attr, v ? "True" : "False");
                if (auto* d = editor.doc()) d->dirty = true;
            }
            ImGui::PopID();
        };
        boolRow("Touch Enable", "TouchEnable", false);
        ImGui::SameLine();
        boolRow("Clip Able", "ClipAble", false);
    }

    // CColor (modulate) + Opacity (Alpha attr). Both live-preview on the
    // ax node via setColor / setOpacity — standard Node API, works for
    // any node subclass.
    {
        auto attrs = sel.subAttrs("CColor");
        float col[3] = { 1.0f, 1.0f, 1.0f };
        for (const auto& [k, v] : attrs)
        {
            const int n = std::atoi(v.c_str());
            if      (k == "R") col[0] = n / 255.0f;
            else if (k == "G") col[1] = n / 255.0f;
            else if (k == "B") col[2] = n / 255.0f;
        }
        ImGui::PushID("CColor");
        const bool ccColChanged = ImGui::ColorEdit3("Color", col,
                              ImGuiColorEditFlags_NoInputs);
        snapshotOnActivation(editor);
        if (ccColChanged)
        {
            auto byte = [](float f) {
                const int i = (int)std::lround(f * 255.0f);
                return i < 0 ? 0 : (i > 255 ? 255 : i);
            };
            char br[8], bg[8], bb[8];
            std::snprintf(br, sizeof(br), "%d", byte(col[0]));
            std::snprintf(bg, sizeof(bg), "%d", byte(col[1]));
            std::snprintf(bb, sizeof(bb), "%d", byte(col[2]));
            std::vector<std::pair<std::string, std::string>> next = {
                {"A", "255"}, {"R", br}, {"G", bg}, {"B", bb}
            };
            sel.setSubAttrs("CColor", next);
            if (auto* d = editor.doc()) d->dirty = true;
            // Live preview: apply to the ax node if mapped. findAx is a
            // small accessor we pull through sync — syncNodeCsdToAx
            // doesn't touch color, so do it directly here.
            // (We intentionally don't rescan the whole tree — just nudge
            //  the selected node's color.)
            editor.syncNodeCsdToAx(sel);
        }
        ImGui::PopID();

        // Opacity — Alpha attribute, 0-255. Slider keyed on the int so
        // drag feels integer-precise.
        int alpha = 255;
        const auto a = sel.attr("Alpha", "");
        if (!a.empty()) alpha = std::atoi(a.c_str());
        ImGui::PushID("Opacity");
        const bool opChanged = ImGui::SliderInt("Opacity", &alpha, 0, 255);
        snapshotOnActivation(editor);
        if (opChanged)
        {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", alpha);
            sel.setAttr("Alpha", buf);
            if (auto* d = editor.doc()) d->dirty = true;
            editor.syncNodeCsdToAx(sel);
        }
        ImGui::PopID();
    }

    // Background fill — Cocos Panel nodes can be painted with a solid
    // color or a two-stop gradient. The mode is stored as
    // `ComboBoxIndex` on the node (0 = Transparent, 1 = Solid color,
    // 2 = Gradient). We expose a combo to flip modes, then show ONLY
    // the controls relevant to the current mode so the panel isn't
    // cluttered with fields that don't paint anything.
    //
    // The section shows for container-like ctypes OR nodes that already
    // carry any BG data — users editing a third-party-authored file
    // shouldn't have their controls vanish because the ctype name is
    // non-standard.
    {
        const auto ct = sel.ctype();
        const bool containerCtype =
            ct.find("Panel")  != std::string::npos ||
            ct.find("Layer")  != std::string::npos ||
            ct.find("Layout") != std::string::npos ||
            ct.find("Scene")  != std::string::npos;
        const bool hasAnyBg =
            !sel.subAttrs("SingleColor").empty() ||
            !sel.subAttrs("FirstColor").empty()  ||
            !sel.subAttrs("EndColor").empty()    ||
            !sel.subAttrs("ColorVector").empty();

        if (containerCtype || hasAnyBg)
        {
            ImGui::Separator();
            ImGui::TextDisabled("Background");

            auto readColor = [](const Node& nn, const char* tag,
                                float out[4])
            {
                out[0] = out[1] = out[2] = 1.0f; out[3] = 1.0f;
                for (const auto& [k, v] : nn.subAttrs(tag))
                {
                    const int n = std::atoi(v.c_str());
                    if      (k == "R") out[0] = n / 255.0f;
                    else if (k == "G") out[1] = n / 255.0f;
                    else if (k == "B") out[2] = n / 255.0f;
                    else if (k == "A") out[3] = n / 255.0f;
                }
            };
            auto writeColor = [&](const char* tag, const float c[4])
            {
                auto byte = [](float f) {
                    const int i = (int)std::lround(f * 255.0f);
                    return i < 0 ? 0 : (i > 255 ? 255 : i);
                };
                char br[8], bg[8], bb[8], ba[8];
                std::snprintf(br, sizeof(br), "%d", byte(c[0]));
                std::snprintf(bg, sizeof(bg), "%d", byte(c[1]));
                std::snprintf(bb, sizeof(bb), "%d", byte(c[2]));
                std::snprintf(ba, sizeof(ba), "%d", byte(c[3]));
                sel.setSubAttrs(tag, {
                    {"A", ba}, {"R", br}, {"G", bg}, {"B", bb}
                });
            };

            // Current mode. Default to Transparent when ComboBoxIndex is
            // absent and no color data has been authored — matches the
            // Cocos "new Panel is transparent" convention.
            int mode = 0;
            const auto cbi = sel.attr("ComboBoxIndex", "");
            if (!cbi.empty()) mode = std::atoi(cbi.c_str());
            else if (!sel.subAttrs("FirstColor").empty()) mode = 2;
            else if (!sel.subAttrs("SingleColor").empty()) mode = 1;

            const char* kModes[] = { "Transparent", "Solid", "Gradient" };
            ImGui::PushID("BgMode");
            const bool bgModeChanged = ImGui::Combo("Mode", &mode, kModes, 3);
            snapshotOnActivation(editor);
            if (bgModeChanged)
            {
                char b[8]; std::snprintf(b, sizeof(b), "%d", mode);
                sel.setAttr("ComboBoxIndex", b);
                if (auto* d = editor.doc()) d->dirty = true;
                editor.syncNodeCsdToAx(sel);
            }
            ImGui::PopID();

            if (mode == 1)  // Solid
            {
                float col[4] = { 1, 1, 1, 1 };
                readColor(sel, "SingleColor", col);
                const auto bcAlpha = sel.attr("BackColorAlpha", "");
                if (!bcAlpha.empty())
                    col[3] = std::atoi(bcAlpha.c_str()) / 255.0f;
                ImGui::PushID("SingleColor");
                const bool scChanged = ImGui::ColorEdit4("Color", col,
                        ImGuiColorEditFlags_AlphaBar |
                        ImGuiColorEditFlags_NoInputs);
                snapshotOnActivation(editor);
                if (scChanged)
                {
                    writeColor("SingleColor", col);
                    auto byte = [](float f) {
                        const int i = (int)std::lround(f * 255.0f);
                        return i < 0 ? 0 : (i > 255 ? 255 : i);
                    };
                    char a[8]; std::snprintf(a, sizeof(a), "%d", byte(col[3]));
                    sel.setAttr("BackColorAlpha", a);
                    if (auto* d = editor.doc()) d->dirty = true;
                    editor.syncNodeCsdToAx(sel);
                }
                ImGui::PopID();
            }
            else if (mode == 2)  // Gradient
            {
                float first[4], last[4];
                readColor(sel, "FirstColor", first);
                readColor(sel, "EndColor",   last);

                ImGui::PushID("First");
                const bool fcChanged = ImGui::ColorEdit4("Start", first,
                        ImGuiColorEditFlags_AlphaBar |
                        ImGuiColorEditFlags_NoInputs);
                snapshotOnActivation(editor);
                if (fcChanged)
                {
                    writeColor("FirstColor", first);
                    if (auto* d = editor.doc()) d->dirty = true;
                    editor.syncNodeCsdToAx(sel);
                }
                ImGui::PopID();
                ImGui::PushID("End");
                const bool ecChanged = ImGui::ColorEdit4("End", last,
                        ImGuiColorEditFlags_AlphaBar |
                        ImGuiColorEditFlags_NoInputs);
                snapshotOnActivation(editor);
                if (ecChanged)
                {
                    writeColor("EndColor", last);
                    if (auto* d = editor.doc()) d->dirty = true;
                    editor.syncNodeCsdToAx(sel);
                }
                ImGui::PopID();

                // ColorVector: unit direction of the gradient. Read the
                // current (vx, vy), offer both a 2D vector field and an
                // angle slider as two views of the same value — edits on
                // either write both ColorVector and ColorAngle so the
                // .csb compiler and runtime readers agree.
                float vx = 0.0f, vy = -1.0f;   // default top→bottom
                for (const auto& [k, v] : sel.subAttrs("ColorVector"))
                {
                    const float n = std::strtof(v.c_str(), nullptr);
                    if      (k == "ScaleX") vx = n;
                    else if (k == "ScaleY") vy = n;
                }
                float angle = std::atan2(vy, vx) *
                    180.0f / 3.14159265358979323846f;
                const auto aAttr = sel.attr("ColorAngle", "");
                if (!aAttr.empty())
                    angle = std::strtof(aAttr.c_str(), nullptr);

                auto writeDir = [&](float newAngleDeg, float nvx, float nvy)
                {
                    const float rad = newAngleDeg *
                        3.14159265358979323846f / 180.0f;
                    if (nvx == 0 && nvy == 0)
                    { nvx = std::cos(rad); nvy = std::sin(rad); }
                    char bx[16], by[16], ba[16];
                    std::snprintf(bx, sizeof(bx), "%.4f", nvx);
                    std::snprintf(by, sizeof(by), "%.4f", nvy);
                    std::snprintf(ba, sizeof(ba), "%.4f", newAngleDeg);
                    sel.setSubAttrs("ColorVector", {
                        {"ScaleX", bx}, {"ScaleY", by}
                    });
                    sel.setAttr("ColorAngle", ba);
                };

                float vec[2] = { vx, vy };
                ImGui::PushID("ColorVector");
                const bool cvChanged = ImGui::InputFloat2("Vector", vec, "%.3f");
                snapshotOnActivation(editor);
                if (cvChanged)
                {
                    const float a = std::atan2(vec[1], vec[0]) *
                        180.0f / 3.14159265358979323846f;
                    writeDir(a, vec[0], vec[1]);
                    if (auto* d = editor.doc()) d->dirty = true;
                    editor.syncNodeCsdToAx(sel);
                }
                ImGui::PopID();
                ImGui::PushID("ColorAngle");
                const bool caChanged = ImGui::SliderFloat("Angle", &angle,
                                       -360.0f, 360.0f, "%.2f°");
                snapshotOnActivation(editor);
                if (caChanged)
                {
                    writeDir(angle, 0.0f, 0.0f);
                    if (auto* d = editor.doc()) d->dirty = true;
                    editor.syncNodeCsdToAx(sel);
                }
                ImGui::PopID();
            }
        }
    }

    // VectorMap type-specific properties — layer list, primitive
    // counts, add/remove. Mirrors the `<VectorMapData>` XML schema
    // parsed by VectorMapBinding. Designer-friendly first pass:
    // can add an empty layer, toggle visibility, delete; richer
    // primitive editing follows.
    if (sel.ctype() == "VectorMapObjectData")
    {
        ImGui::Separator();
        ImGui::TextDisabled("VectorMap");

        auto vmd = sel.element().child("VectorMapData");
        if (!vmd)
        {
            ImGui::TextWrapped(
                "<VectorMapData> sub-element missing — "
                "press Add Layer to seed one.");
        }
        else
        {
            // Iterate layers + show a collapsible row per layer with
            // per-primitive editor inside (CollapsingHeader). Each
            // primitive exposes its colors via ColorEdit4 and its
            // numeric attrs via small text inputs. Edits update the
            // XML and call syncNodeCsdToAx so the canvas reflects
            // the change live.
            int lidx = 0;
            for (auto layer = vmd.child("Layer"); layer; ++lidx)
            {
                auto next = layer.next_sibling("Layer");
                ImGui::PushID(lidx);
                const std::string lname(layer.attribute("Name").as_string("(unnamed)"));
                int primCount = 0;
                for (auto p = layer.first_child(); p; p = p.next_sibling())
                    ++primCount;
                bool vis = !(std::string(layer.attribute("Visible").as_string("True"))
                             == "False");
                if (ImGui::Checkbox("##vis", &vis))
                {
                    auto a = layer.attribute("Visible");
                    if (!a) a = layer.append_attribute("Visible");
                    a.set_value(vis ? "True" : "False");
                    if (editor.doc()) editor.doc()->dirty = true;
                    editor.syncNodeCsdToAx(sel);
                }
                ImGui::SameLine();
                char hdr[128];
                std::snprintf(hdr, sizeof(hdr), "%s (%d)##l",
                              lname.c_str(), primCount);
                const bool open = ImGui::CollapsingHeader(hdr);
                ImGui::SameLine();
                if (ImGui::SmallButton("X##layer"))
                { vmd.remove_child(layer);
                  if (editor.doc()) editor.doc()->dirty = true;
                  editor.syncNodeCsdToAx(sel);
                  layer = next; ImGui::PopID(); continue; }

                if (open)
                {
                    ImGui::Indent(12);
                    auto colorEditor = [&](pugi::xml_node prim,
                                           const char* attr,
                                           const char* label) {
                        const std::string s(prim.attribute(attr).as_string(""));
                        float c[4] = {1,1,1,1};
                        int parts[4] = {255,255,255,255};
                        std::sscanf(s.c_str(), "%d,%d,%d,%d",
                                    parts, parts+1, parts+2, parts+3);
                        for (int i = 0; i < 4; ++i) c[i] = parts[i] / 255.0f;
                        if (ImGui::ColorEdit4(label, c,
                                              ImGuiColorEditFlags_NoInputs |
                                              ImGuiColorEditFlags_AlphaBar))
                        {
                            char buf[64];
                            std::snprintf(buf, sizeof(buf),
                                "%d,%d,%d,%d",
                                (int)(c[0] * 255), (int)(c[1] * 255),
                                (int)(c[2] * 255), (int)(c[3] * 255));
                            auto a = prim.attribute(attr);
                            if (!a) a = prim.append_attribute(attr);
                            a.set_value(buf);
                            if (editor.doc()) editor.doc()->dirty = true;
                            editor.syncNodeCsdToAx(sel);
                        }
                    };
                    auto floatEditor = [&](pugi::xml_node prim,
                                            const char* attr,
                                            const char* label) {
                        float v = prim.attribute(attr).as_float(0.0f);
                        if (ImGui::DragFloat(label, &v, 0.5f, 0, 4096))
                        {
                            char buf[32];
                            std::snprintf(buf, sizeof(buf), "%.4f", v);
                            auto a = prim.attribute(attr);
                            if (!a) a = prim.append_attribute(attr);
                            a.set_value(buf);
                            if (editor.doc()) editor.doc()->dirty = true;
                            editor.syncNodeCsdToAx(sel);
                        }
                    };

                    int pidx = 0;
                    for (auto prim = layer.first_child(); prim; ++pidx)
                    {
                        auto pnext = prim.next_sibling();
                        const std::string tag(prim.name());
                        if (tag != "Polygon" && tag != "Polyline" && tag != "Circle")
                        { prim = pnext; continue; }
                        ImGui::PushID(pidx);
                        ImGui::Text("%s", tag.c_str());
                        ImGui::SameLine();
                        if (ImGui::SmallButton("X##prim"))
                        { layer.remove_child(prim);
                          if (editor.doc()) editor.doc()->dirty = true;
                          editor.syncNodeCsdToAx(sel);
                          prim = pnext; ImGui::PopID(); continue; }
                        if (tag != "Polyline") colorEditor(prim, "Fill",   "fill");
                        colorEditor(prim, "Stroke", "stroke");
                        floatEditor(prim, "StrokeWidth", "stroke w");
                        if (tag == "Circle")
                        {
                            floatEditor(prim, "X",      "x");
                            floatEditor(prim, "Y",      "y");
                            floatEditor(prim, "Radius", "radius");
                        }
                        else
                        {
                            char buf[1024];
                            std::snprintf(buf, sizeof(buf), "%s",
                                prim.attribute("Points").as_string(""));
                            if (ImGui::InputText("points", buf, sizeof(buf)))
                            {
                                auto a = prim.attribute("Points");
                                if (!a) a = prim.append_attribute("Points");
                                a.set_value(buf);
                                if (editor.doc()) editor.doc()->dirty = true;
                                editor.syncNodeCsdToAx(sel);
                            }
                        }
                        ImGui::Separator();
                        ImGui::PopID();
                        prim = pnext;
                    }
                    // Add-primitive row.
                    static int sKindIdx = 0;
                    const char* kinds[] = {"Polygon", "Polyline", "Circle"};
                    ImGui::SetNextItemWidth(110);
                    ImGui::Combo("##kind", &sKindIdx, kinds, 3);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Add##prim"))
                    {
                        editor.pushUndo();
                        auto np = layer.append_child(kinds[sKindIdx]);
                        if (sKindIdx == 2)  // Circle
                        {
                            np.append_attribute("X").set_value("0");
                            np.append_attribute("Y").set_value("0");
                            np.append_attribute("Radius").set_value("30");
                        }
                        else
                        {
                            np.append_attribute("Points")
                              .set_value("0,0 50,0 50,50");
                        }
                        if (sKindIdx != 1)
                            np.append_attribute("Fill").set_value("200,150,80,255");
                        np.append_attribute("Stroke")
                          .set_value("0,0,0,255");
                        np.append_attribute("StrokeWidth").set_value("2");
                        if (editor.doc()) editor.doc()->dirty = true;
                        editor.syncNodeCsdToAx(sel);
                    }
                    ImGui::Unindent(12);
                }
                ImGui::PopID();
                layer = next;
            }
        }

        if (ImGui::Button("Add Layer"))
        {
            editor.pushUndo();
            auto el = sel.element();
            auto vmdN = el.child("VectorMapData");
            if (!vmdN) vmdN = el.append_child("VectorMapData");
            auto nl = vmdN.append_child("Layer");
            char name[32];
            int n = 0;
            for (auto l = vmdN.child("Layer"); l; l = l.next_sibling("Layer")) ++n;
            std::snprintf(name, sizeof(name), "layer_%d", n);
            nl.append_attribute("Name").set_value(name);
            nl.append_attribute("Visible").set_value("True");
            // Seed with a small filled square so the layer is visible
            // immediately. Designer can edit / delete the primitive
            // via inline controls (TODO) or via RPC setattr.
            auto poly = nl.append_child("Polygon");
            poly.append_attribute("Points")
                .set_value("0,0 100,0 100,100 0,100");
            poly.append_attribute("Fill").set_value("220,160,90,255");
            poly.append_attribute("Stroke").set_value("0,0,0,255");
            poly.append_attribute("StrokeWidth").set_value("2");
            if (editor.doc()) editor.doc()->dirty = true;
            editor.syncNodeCsdToAx(sel);
        }
        ImGui::SameLine();
        ImGui::TextDisabled(
            "edit primitives via RPC setattr or hand-edit .csd");
    }

    // SvgImage / Lottie — both use a single Path attribute on a
    // FileData sub-element. Compact section: a path field + a hint
    // about the runtime semantics. Authoring beyond the path goes
    // through generic setattr for now.
    if (sel.ctype() == "SvgImageObjectData" ||
        sel.ctype() == "LottieObjectData")
    {
        ImGui::Separator();
        const bool svg = sel.ctype() == "SvgImageObjectData";
        ImGui::TextDisabled(svg ? "SVG" : "Lottie");
        auto fd = sel.element().child("FileData");
        char buf[1024];
        std::snprintf(buf, sizeof(buf), "%s",
            fd ? fd.attribute("Path").as_string("") : "");
        if (ImGui::InputText(svg ? ".svg path" : ".json path",
                              buf, sizeof(buf)))
        {
            auto fdN = sel.element().child("FileData");
            if (!fdN) fdN = sel.element().append_child("FileData");
            auto a = fdN.attribute("Path");
            if (!a) a = fdN.append_attribute("Path");
            a.set_value(buf);
            auto t = fdN.attribute("Type");
            if (!t) fdN.append_attribute("Type").set_value("Normal");
            if (editor.doc()) editor.doc()->dirty = true;
            editor.syncNodeCsdToAx(sel);
        }
        ImGui::TextWrapped(svg
            ? "Runtime rasterises via rsvg-convert. "
              "Re-rasterises on setTargetSize when the on-screen footprint changes."
            : "Runtime is a stub until rlottie wiring lands. "
              "Path is persisted + loaded; tick() ready to drive a real renderer.");
    }

    // LitSprite section — ambient + per-light editor + add/delete.
    // Edits flip dirty and call syncNodeCsdToAx so the canvas
    // updates the sprite's color modulation live.
    if (sel.ctype() == "LitSpriteObjectData")
    {
        ImGui::Separator();
        ImGui::TextDisabled("Lights");
        auto root = sel.element().child("OCSLights");
        if (!root)
            ImGui::TextWrapped(
                "<OCSLights> sub-element missing — press Add Light.");
        else
        {
            // Ambient ColorEdit.
            const std::string s(root.attribute("Ambient").as_string("60,60,80,255"));
            int amb[4] = {60,60,80,255};
            std::sscanf(s.c_str(), "%d,%d,%d,%d", amb, amb+1, amb+2, amb+3);
            float a[4] = { amb[0]/255.0f, amb[1]/255.0f,
                            amb[2]/255.0f, amb[3]/255.0f };
            if (ImGui::ColorEdit4("ambient", a,
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%d,%d,%d,%d",
                    (int)(a[0]*255),(int)(a[1]*255),
                    (int)(a[2]*255),(int)(a[3]*255));
                auto at = root.attribute("Ambient");
                if (!at) at = root.append_attribute("Ambient");
                at.set_value(buf);
                if (editor.doc()) editor.doc()->dirty = true;
                editor.syncNodeCsdToAx(sel);
            }

            int idx = 0;
            for (auto L = root.child("Light"); L; ++idx)
            {
                auto next = L.next_sibling("Light");
                ImGui::PushID(idx);
                float x = L.attribute("X").as_float(0);
                float y = L.attribute("Y").as_float(0);
                float r = L.attribute("Radius").as_float(100);
                float fo = L.attribute("Falloff").as_float(1.0f);
                const std::string cs(L.attribute("Color").as_string("255,255,255,255"));
                int c[4] = {255,255,255,255};
                std::sscanf(cs.c_str(), "%d,%d,%d,%d", c, c+1, c+2, c+3);
                float col[4] = { c[0]/255.0f, c[1]/255.0f, c[2]/255.0f, c[3]/255.0f };
                bool changed = false;
                ImGui::Text("Light %d", idx);
                ImGui::SameLine();
                if (ImGui::SmallButton("X##light"))
                {
                    root.remove_child(L);
                    if (editor.doc()) editor.doc()->dirty = true;
                    editor.syncNodeCsdToAx(sel);
                    L = next; ImGui::PopID(); continue;
                }
                if (ImGui::DragFloat("x", &x, 1.0f)) changed = true;
                if (ImGui::DragFloat("y", &y, 1.0f)) changed = true;
                if (ImGui::DragFloat("radius", &r, 1.0f, 0, 4096)) changed = true;
                if (ImGui::DragFloat("falloff", &fo, 0.05f, 0.05f, 8.0f)) changed = true;
                if (ImGui::ColorEdit4("color", col,
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
                    changed = true;
                // Direction vector + cone. Direction stored as
                // "dx,dy" string; cone in degrees with 360 = omni.
                float dir[2] = {0, 0};
                const std::string ds(L.attribute("Direction").as_string(""));
                std::sscanf(ds.c_str(), "%f,%f", &dir[0], &dir[1]);
                float coneDeg  = L.attribute("ConeDeg").as_float(360.0f);
                float coneSoft = L.attribute("ConeSoftness").as_float(0.2f);
                if (ImGui::DragFloat2("dir (dx,dy)", dir, 0.05f, -2.0f, 2.0f))
                    changed = true;
                if (ImGui::DragFloat("cone deg", &coneDeg, 1.0f, 1.0f, 360.0f))
                    changed = true;
                if (ImGui::DragFloat("cone soft", &coneSoft, 0.01f, 0.0f, 1.0f))
                    changed = true;
                if (changed)
                {
                    auto setF = [&](const char* k, float v) {
                        char buf[32]; std::snprintf(buf, sizeof(buf), "%.4f", v);
                        auto a = L.attribute(k);
                        if (!a) a = L.append_attribute(k);
                        a.set_value(buf);
                    };
                    setF("X", x); setF("Y", y);
                    setF("Radius", r); setF("Falloff", fo);
                    setF("ConeDeg", coneDeg);
                    setF("ConeSoftness", coneSoft);
                    char dbuf[64];
                    std::snprintf(dbuf, sizeof(dbuf), "%.4f,%.4f",
                                  dir[0], dir[1]);
                    auto da = L.attribute("Direction");
                    if (!da) da = L.append_attribute("Direction");
                    da.set_value(dbuf);
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%d,%d,%d,%d",
                        (int)(col[0]*255),(int)(col[1]*255),
                        (int)(col[2]*255),(int)(col[3]*255));
                    auto a = L.attribute("Color");
                    if (!a) a = L.append_attribute("Color");
                    a.set_value(buf);
                    if (editor.doc()) editor.doc()->dirty = true;
                    editor.syncNodeCsdToAx(sel);
                }
                ImGui::Separator();
                ImGui::PopID();
                L = next;
            }
        }
        if (ImGui::Button("Add Light"))
        {
            editor.pushUndo();
            auto el = sel.element();
            auto r = el.child("OCSLights");
            if (!r)
            { r = el.append_child("OCSLights");
              r.append_attribute("Ambient").set_value("60,60,80,255"); }
            auto L = r.append_child("Light");
            L.append_attribute("X").set_value("0");
            L.append_attribute("Y").set_value("0");
            L.append_attribute("Radius").set_value("200");
            L.append_attribute("Color").set_value("255,210,160,255");
            L.append_attribute("Falloff").set_value("1.5");
            if (editor.doc()) editor.doc()->dirty = true;
            editor.syncNodeCsdToAx(sel);
        }
    }

    // Bindings — declarative event-handler list. Stored under
    // <OCSBindings>/<Binding Event="…" Method="…" Params="…"/> on
    // the node. Exported to <basename>.bindings.json on csb export;
    // runtime walks the sidecar with ocs::ext::applyBindingsFromJson
    // and the caller dispatches to its own controller class.
    {
        ImGui::Separator();
        ImGui::TextDisabled("Bindings");
        static char sNewEvent[64]  = {0};
        static char sNewMethod[64] = {0};
        auto root = sel.element().child("OCSBindings");
        if (root)
        {
            int idx = 0;
            for (auto b = root.child("Binding"); b; ++idx)
            {
                auto next = b.next_sibling("Binding");
                ImGui::PushID(idx);
                const std::string ev(b.attribute("Event").as_string(""));
                const std::string mt(b.attribute("Method").as_string(""));
                ImGui::Text("%s → %s", ev.c_str(), mt.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("X"))
                {
                    root.remove_child(b);
                    if (editor.doc()) editor.doc()->dirty = true;
                    b = next; ImGui::PopID(); continue;
                }
                ImGui::PopID();
                b = next;
            }
        }
        ImGui::SetNextItemWidth(120);
        ImGui::InputTextWithHint(
            "##bEvent", "event (OnClick…)", sNewEvent, sizeof(sNewEvent));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        ImGui::InputTextWithHint(
            "##bMethod", "method name", sNewMethod, sizeof(sNewMethod));
        ImGui::SameLine();
        const bool canAdd = sNewEvent[0] && sNewMethod[0];
        // BeginDisabled so the button visibly greys out — the prior
        // version silently no-op'd when either field was empty, which
        // surfaced as "clicked Add but nothing happened" because the
        // user could not tell the field validation was the gate.
        if (!canAdd) ImGui::BeginDisabled();
        const bool clicked = ImGui::Button("Add##binding");
        if (!canAdd) ImGui::EndDisabled();
        if (clicked && canAdd)
        {
            editor.pushUndo();
            auto el = sel.element();
            auto r = el.child("OCSBindings");
            if (!r) r = el.append_child("OCSBindings");
            auto b = r.append_child("Binding");
            b.append_attribute("Event").set_value(sNewEvent);
            b.append_attribute("Method").set_value(sNewMethod);
            if (editor.doc()) editor.doc()->dirty = true;
            sNewEvent[0] = 0; sNewMethod[0] = 0;
        }
        if (!canAdd)
            ImGui::TextDisabled(
                "fill both event + method to enable Add");
    }

    // Text / Label type-specific properties. Cocos Studio's TextObjectData
    // (and adjacent Label variants) stores LabelText + font info as node
    // attrs, with OutlineColor/ShadowColor as sub-elements. This section
    // shows itself only when the ctype names a text widget so non-text
    // nodes keep a clean panel.
    {
        const auto ct = sel.ctype();
        const bool isText =
            ct.find("Text")  != std::string::npos ||
            ct.find("Label") != std::string::npos;
        if (isText)
        {
            ImGui::Separator();
            ImGui::TextDisabled("Text");
            bool dirty = false;

            // LabelText — multiline when the string already has newlines
            // or is long enough that one row would truncate.
            {
                auto cur = sel.attr("LabelText", "");
                char buf[1024];
                std::snprintf(buf, sizeof(buf), "%s", cur.c_str());
                const bool multi =
                    cur.find('\n') != std::string::npos || cur.size() > 40;
                ImGui::PushID("LabelText");
                bool changed = multi
                    ? ImGui::InputTextMultiline("Label", buf, sizeof(buf),
                        ImVec2(-1, 60))
                    : ImGui::InputText("Label", buf, sizeof(buf));
                if (changed)
                {
                    sel.setAttr("LabelText", buf);
                    dirty = true;
                }
                ImGui::PopID();
            }

            // FontSize — int attr.
            {
                int fs = 0;
                const auto a = sel.attr("FontSize", "");
                if (!a.empty()) fs = std::atoi(a.c_str());
                ImGui::PushID("FontSize");
                if (ImGui::InputInt("Font Size", &fs, 1, 5))
                {
                    char b[16];
                    std::snprintf(b, sizeof(b), "%d", fs);
                    sel.setAttr("FontSize", b);
                    dirty = true;
                }
                ImGui::PopID();
            }

            // Font path — <FontResource Type="Normal" Path="..." Plist=""/>.
            // Plain text field for now; a file picker can follow.
            {
                auto fr = sel.subAttrs("FontResource");
                std::string path, type = "Normal", plist;
                for (const auto& [k, v] : fr)
                {
                    if      (k == "Path")  path  = v;
                    else if (k == "Type")  type  = v;
                    else if (k == "Plist") plist = v;
                }
                char buf[512];
                std::snprintf(buf, sizeof(buf), "%s", path.c_str());
                ImGui::PushID("FontPath");
                if (ImGui::InputText("Font Path", buf, sizeof(buf)))
                {
                    sel.setSubAttrs("FontResource", {
                        {"Type",  type},
                        {"Path",  buf},
                        {"Plist", plist}
                    });
                    dirty = true;
                }
                ImGui::PopID();
            }

            // Alignment — Cocos stores as "HT_Center"/"HT_Left"/"HT_Right"
            // and "VT_Center"/"VT_Top"/"VT_Bottom". Use Combo for clarity.
            {
                const char* hItems[] = { "Left", "Center", "Right" };
                const char* hKeys [] = { "HT_Left", "HT_Center", "HT_Right" };
                const auto hCur = sel.attr("HorizontalAlignmentType",
                                           "HT_Left");
                int hIdx = 0;
                for (int i = 0; i < 3; ++i)
                    if (hCur == hKeys[i]) { hIdx = i; break; }
                ImGui::PushID("HAlign");
                if (ImGui::Combo("H Align", &hIdx, hItems, 3))
                {
                    sel.setAttr("HorizontalAlignmentType", hKeys[hIdx]);
                    dirty = true;
                }
                ImGui::PopID();

                const char* vItems[] = { "Top", "Center", "Bottom" };
                const char* vKeys [] = { "VT_Top", "VT_Center", "VT_Bottom" };
                const auto vCur = sel.attr("VerticalAlignmentType",
                                           "VT_Top");
                int vIdx = 0;
                for (int i = 0; i < 3; ++i)
                    if (vCur == vKeys[i]) { vIdx = i; break; }
                ImGui::PushID("VAlign");
                if (ImGui::Combo("V Align", &vIdx, vItems, 3))
                {
                    sel.setAttr("VerticalAlignmentType", vKeys[vIdx]);
                    dirty = true;
                }
                ImGui::PopID();
            }

            // Outline — enable flag + color + size. Writing sub-element
            // OutlineColor when the flag flips on seeds a visible default
            // so the change is immediately perceptible.
            {
                bool oEnabled = sel.attr("OutlineEnabled", "False") == "True";
                ImGui::PushID("OutlineEnabled");
                if (ImGui::Checkbox("Outline", &oEnabled))
                {
                    sel.setAttr("OutlineEnabled", oEnabled ? "True" : "False");
                    dirty = true;
                }
                ImGui::PopID();
                if (oEnabled)
                {
                    int oSize = 1;
                    const auto oS = sel.attr("OutlineSize", "");
                    if (!oS.empty()) oSize = std::atoi(oS.c_str());
                    ImGui::PushID("OutlineSize");
                    if (ImGui::InputInt("Outline Size", &oSize, 1, 2))
                    {
                        char b[16];
                        std::snprintf(b, sizeof(b), "%d", oSize);
                        sel.setAttr("OutlineSize", b);
                        dirty = true;
                    }
                    ImGui::PopID();

                    float oc[4] = { 0, 0, 0, 1 };
                    for (const auto& [k, v] : sel.subAttrs("OutlineColor"))
                    {
                        const int n = std::atoi(v.c_str());
                        if      (k == "R") oc[0] = n / 255.0f;
                        else if (k == "G") oc[1] = n / 255.0f;
                        else if (k == "B") oc[2] = n / 255.0f;
                        else if (k == "A") oc[3] = n / 255.0f;
                    }
                    ImGui::PushID("OutlineColor");
                    if (ImGui::ColorEdit4("Outline Color", oc,
                            ImGuiColorEditFlags_AlphaBar |
                            ImGuiColorEditFlags_NoInputs))
                    {
                        auto byte = [](float f) {
                            const int i = (int)std::lround(f * 255.0f);
                            return i < 0 ? 0 : (i > 255 ? 255 : i);
                        };
                        char br[8], bg[8], bb[8], ba[8];
                        std::snprintf(br, sizeof(br), "%d", byte(oc[0]));
                        std::snprintf(bg, sizeof(bg), "%d", byte(oc[1]));
                        std::snprintf(bb, sizeof(bb), "%d", byte(oc[2]));
                        std::snprintf(ba, sizeof(ba), "%d", byte(oc[3]));
                        sel.setSubAttrs("OutlineColor", {
                            {"A", ba}, {"R", br}, {"G", bg}, {"B", bb}
                        });
                        dirty = true;
                    }
                    ImGui::PopID();
                }
            }

            // Shadow — similar layout: enable + color + XY offset.
            {
                bool sEnabled = sel.attr("ShadowEnabled", "False") == "True";
                ImGui::PushID("ShadowEnabled");
                if (ImGui::Checkbox("Shadow", &sEnabled))
                {
                    sel.setAttr("ShadowEnabled", sEnabled ? "True" : "False");
                    dirty = true;
                }
                ImGui::PopID();
                if (sEnabled)
                {
                    float ox = std::strtof(
                        sel.attr("ShadowOffsetX", "2").c_str(), nullptr);
                    float oy = std::strtof(
                        sel.attr("ShadowOffsetY", "-2").c_str(), nullptr);
                    float offs[2] = { ox, oy };
                    ImGui::PushID("ShadowOffset");
                    if (ImGui::InputFloat2("Shadow Offset", offs, "%.4f"))
                    {
                        char bx[16], by[16];
                        std::snprintf(bx, sizeof(bx), "%.4f", offs[0]);
                        std::snprintf(by, sizeof(by), "%.4f", offs[1]);
                        sel.setAttr("ShadowOffsetX", bx);
                        sel.setAttr("ShadowOffsetY", by);
                        dirty = true;
                    }
                    ImGui::PopID();

                    float sc[4] = { 0, 0, 0, 1 };
                    for (const auto& [k, v] : sel.subAttrs("ShadowColor"))
                    {
                        const int n = std::atoi(v.c_str());
                        if      (k == "R") sc[0] = n / 255.0f;
                        else if (k == "G") sc[1] = n / 255.0f;
                        else if (k == "B") sc[2] = n / 255.0f;
                        else if (k == "A") sc[3] = n / 255.0f;
                    }
                    ImGui::PushID("ShadowColor");
                    if (ImGui::ColorEdit4("Shadow Color", sc,
                            ImGuiColorEditFlags_AlphaBar |
                            ImGuiColorEditFlags_NoInputs))
                    {
                        auto byte = [](float f) {
                            const int i = (int)std::lround(f * 255.0f);
                            return i < 0 ? 0 : (i > 255 ? 255 : i);
                        };
                        char br[8], bg[8], bb[8], ba[8];
                        std::snprintf(br, sizeof(br), "%d", byte(sc[0]));
                        std::snprintf(bg, sizeof(bg), "%d", byte(sc[1]));
                        std::snprintf(bb, sizeof(bb), "%d", byte(sc[2]));
                        std::snprintf(ba, sizeof(ba), "%d", byte(sc[3]));
                        sel.setSubAttrs("ShadowColor", {
                            {"A", ba}, {"R", br}, {"G", bg}, {"B", bb}
                        });
                        dirty = true;
                    }
                    ImGui::PopID();
                }
            }

            if (dirty) { if (auto* d = editor.doc()) d->dirty = true; }
        }
    }

    ImGui::Separator();

    // -- Image / FileData editor ------------------------------------------
    //
    // Cocos Studio widgets carry their texture paths in typed sub-elements
    // (FileData, NormalFileData, PressedFileData, etc.). This section
    // surfaces an editable row per sub-element: a text field for the Path
    // plus a Pick button that opens the native file dialog. The Type and
    // Plist attrs are preserved on write so PlistSubImage references
    // survive a path change.
    {
        // Same set the CsdModel's imageRefs walk knows about — kept in
        // sync so "view refs" and "edit refs" see the same tags.
        static constexpr const char* kFileTags[] = {
            "FileData",         "TextureFile",   "ImageFile",
            "ImageFileData",    "NormalFileData","PressedFileData",
            "DisabledFileData", "SelectedFileData",
            "BackGroundImageFileData", "LoadingBarFileData",
            "BallNormalFileData", "BallPressedFileData", "BallDisabledFileData",
            "FrameImage",       "ImageFileName", "FontResource",
        };
        auto setAx = [&]() {
            if (auto* d = editor.doc()) d->dirty = true;
            // No live preview yet — texture swap requires a CSLoader-
            // level refresh that's outside the scope of this widget. The
            // .csd persists immediately and the change applies after a
            // save + hot-reload via File → Open.
        };
        bool anyShown = false;
        for (const char* tag : kFileTags)
        {
            auto attrs = sel.subAttrs(tag);
            if (attrs.empty()) continue;
            if (!anyShown)
            {
                ImGui::Separator();
                ImGui::TextDisabled("Images");
                anyShown = true;
            }

            std::string path, type = "Normal", plist;
            for (const auto& [k, v] : attrs)
            {
                if      (k == "Path")  path  = v;
                else if (k == "Type")  type  = v;
                else if (k == "Plist") plist = v;
            }
            char buf[512];
            std::snprintf(buf, sizeof(buf), "%s", path.c_str());
            ImGui::PushID(tag);

            ImGui::TextUnformatted(tag);
            ImGui::SetNextItemWidth(-110.0f);
            const bool pathChanged =
                ImGui::InputText("##path", buf, sizeof(buf));
            snapshotOnActivation(editor);
            ImGui::SameLine();
            const bool pickClicked = ImGui::Button("Pick\xE2\x80\xA6");
            if (pickClicked) editor.pushUndo();  // snapshot before the write

            if (pathChanged || pickClicked)
            {
                std::string newPath = buf;
                if (pickClicked)
                {
                    const auto src = pickImage();
                    if (!src.empty())
                    {
                        // Copy into Resources so CSLoader can resolve the
                        // reference on its next load; use only the basename
                        // in the Path attribute (Cocos is Resources-relative).
                        namespace fs = std::filesystem;
                        std::error_code ec;
                        const auto resRoot = editor.assetsRoot();
                        if (!resRoot.empty())
                        {
                            auto dst = resRoot / fs::path(src).filename();
                            fs::copy_file(src, dst,
                                fs::copy_options::skip_existing, ec);
                            editor.rescanAssets();
                        }
                        newPath = fs::path(src).filename().string();
                    }
                    else
                    {
                        ImGui::PopID();
                        continue;
                    }
                }
                // Preserve the existing Type/Plist attributes so
                // PlistSubImage references don't silently fall back to
                // raw-file lookup.
                std::vector<std::pair<std::string, std::string>> next = {
                    {"Type", type}, {"Path", newPath}
                };
                if (!plist.empty() ||
                    type == "PlistSubImage" || type == "MarkedSubImage")
                    next.emplace_back("Plist", plist);
                sel.setSubAttrs(tag, next);
                setAx();
            }
            ImGui::PopID();
        }
    }

    // -- Image references --
    {
        auto refs = sel.imageRefs();
        if (!refs.empty())
        {
            ImGui::Separator();
            ImGui::Text("Images referenced");
            if (ImGui::BeginTable("##imgrefs", 3,
                                  ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("kind");
                ImGui::TableSetupColumn("path");
                ImGui::TableSetupColumn("plist");
                ImGui::TableHeadersRow();
                for (const auto& r : refs)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(r.kind.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(r.path.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(r.plist.c_str());
                }
                ImGui::EndTable();
            }

            // Inline preview: for each ref that lives inside an atlas
            // plist, look the frame up and render the atlas image cropped
            // to the frame rect via ImGui UV coords.
            auto* fu = ax::FileUtils::getInstance();
            for (const auto& r : refs)
            {
                if (r.plist.empty() || r.path.empty()) continue;

                const std::string plistAbs = fu->fullPathForFilename(r.plist);
                if (plistAbs.empty()) continue;

                // Frame is keyed by basename (e.g. "bt_play.png") in
                // Cocos2d plists — strip any folder prefix.
                std::string frameName = r.path;
                auto slash = frameName.find_last_of("/\\");
                if (slash != std::string::npos)
                    frameName = frameName.substr(slash + 1);

                auto frame = opencs::PlistAtlas::findFrame(
                    std::filesystem::path(plistAbs), frameName);
                if (!frame) continue;

                auto* tex = ax::Director::getInstance()
                                ->getTextureCache()
                                ->addImage(frame->texturePath.string());
                if (!tex) continue;

                const auto szPx = tex->getContentSizeInPixels();
                if (szPx.width <= 0 || szPx.height <= 0) continue;

                // UV coords for the sub-rect. Cocos atlas frames are in
                // atlas-pixel coords, origin top-left; ImTextureID
                // expects normalised (0..1) UVs.
                const float u0 = frame->x / szPx.width;
                const float v0 = frame->y / szPx.height;
                const float u1 = (frame->x + frame->w) / szPx.width;
                const float v1 = (frame->y + frame->h) / szPx.height;

                auto [texID, ref] =
                    ax::extension::ImGuiPresenter::getInstance()->useTexture(tex);
                (void)ref;

                ImGui::Spacing();
                ImGui::TextDisabled("%s (from %s%s)",
                                    frameName.c_str(), r.plist.c_str(),
                                    frame->rotated ? ", rotated" : "");

                const float maxW  = ImGui::GetContentRegionAvail().x;
                const float scale = std::min(1.0f, maxW / frame->w);
                const ImVec2 dispSz(frame->w * scale, frame->h * scale);

                if (frame->rotated)
                {
                    // Rotated frames are stored 90° CW. Feeding the four UVs
                    // in AddImageQuad lets us un-rotate on display without
                    // extra shader work.
                    auto* dl = ImGui::GetWindowDrawList();
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    const ImVec2 p00(p.x,            p.y);
                    const ImVec2 p10(p.x + dispSz.x, p.y);
                    const ImVec2 p11(p.x + dispSz.x, p.y + dispSz.y);
                    const ImVec2 p01(p.x,            p.y + dispSz.y);
                    // The rect in the atlas is w×h but the visual is h×w.
                    dl->AddImageQuad(texID,
                        p00, p10, p11, p01,
                        ImVec2(u0, v1),
                        ImVec2(u0, v0),
                        ImVec2(u1, v0),
                        ImVec2(u1, v1));
                    ImGui::Dummy(dispSz);
                }
                else
                {
                    ImGui::Image(texID, dispSz,
                                 ImVec2(u0, v0), ImVec2(u1, v1));
                }
            }
        }
    }

    ImGui::Separator();

    // -- Raw attribute table (read-only, for everything else) --
    if (ImGui::CollapsingHeader("Attributes (raw)"))
    {
        if (ImGui::BeginTable("##attrs", 2,
                              ImGuiTableFlags_Borders |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("key");
            ImGui::TableSetupColumn("value");
            ImGui::TableHeadersRow();
            for (auto a : sel.element().attributes())
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                std::string k(a.name());
                ImGui::TextUnformatted(k.c_str());
                ImGui::TableSetColumnIndex(1);
                std::string v(a.value());
                ImGui::TextUnformatted(v.c_str());
            }
            ImGui::EndTable();
        }
    }
}

}  // namespace opencs
