#include "UILayoutEditor/panels/Panels.h"

#include "UILayoutEditor/Editor.h"
#include "UILayoutEditor/CsdModel.h"
#include "UILayoutEditor/Timeline.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace opencs
{

namespace
{

/// Timeline panel layout constants. Tuned by eye against the
/// asset .csd files (60-90 frame durations, 5-15 timelines).
constexpr float kRowHeight        = 22.0f;   // per-track row
constexpr float kNodeHeaderHeight = 24.0f;   // per-node group header (above its tracks)
constexpr float kEventsLaneHeight = 26.0f;   // pinned events lane (a touch taller for labels)
constexpr float kHeaderHeight     = 28.0f;   // toolbar row
constexpr float kRulerHeight      = 22.0f;   // ruler row
constexpr float kLeftColumnWidth  = 240.0f;  // node + property labels
constexpr float kTrackIndent      = 18.0f;   // indent for track labels under their node header
constexpr float kMinPxPerFrame    = 4.0f;    // floor zoom
constexpr float kMaxPxPerFrame    = 40.0f;   // ceiling zoom
constexpr float kDefaultPxPerFrame = 8.0f;
constexpr float kKeyDiamondHalf   = 5.0f;    // keyframe marker half-size

/// One drawn row in the timeline body — either a per-node group
/// header (no animation data, just a label + "+" button) or a
/// per-property track row. We materialise the full row list ahead
/// of rendering so both columns walk the same structure with
/// matching y-offsets.
struct TLRow
{
    enum class Kind { NodeHeader, Track };
    Kind        kind;
    int         actionTag = 0;
    Node        node;            // valid iff Kind::NodeHeader
    TimelineRef tl;              // valid iff Kind::Track
    float       yOffset = 0.0f;  // from tracksTop
    float       height  = 0.0f;
};

/// Event keyframes belong to a single, scene-global Timeline at
/// ActionTag=0. We find-or-create it on demand so unedited docs
/// don't grow a stray empty <Timeline> just by opening them.
constexpr int kEventsTimelineActionTag = 0;

TimelineRef findEventsTimeline(const AnimationRef& anim)
{
    return anim.findTimeline(kEventsTimelineActionTag,
                             TimelineProperty::Event);
}

TimelineRef findOrCreateEventsTimeline(AnimationRef anim)
{
    auto t = findEventsTimeline(anim);
    if (t.valid()) return t;
    return anim.createTimeline(kEventsTimelineActionTag,
                               TimelineProperty::Event);
}

/// Default seed value for a freshly-created track on a node — used by
/// the "+ Track" popup to put a sensible first keyframe at the
/// current playhead. Reads the node's static csd attribute when
/// possible (Position, Scale, AnchorPoint, CColor, Alpha) so the
/// keyframe matches what the runtime would otherwise apply.
KeyValue defaultSeedFor(const Node& n, TimelineProperty p)
{
    auto readPair = [&](const char* tag, const char* kx, const char* ky,
                        float dx, float dy) -> std::pair<float, float>
    {
        float vx = dx, vy = dy;
        for (auto& [k, v] : n.subAttrs(tag))
        {
            if      (k == kx) vx = std::strtof(v.c_str(), nullptr);
            else if (k == ky) vy = std::strtof(v.c_str(), nullptr);
        }
        return {vx, vy};
    };

    KeyValue kv;
    switch (p)
    {
    case TimelineProperty::Position:
    {
        kv.kind = KeyValue::Kind::Vec2;
        auto v = readPair("Position", "X", "Y", 0.0f, 0.0f);
        kv.vx = v.first; kv.vy = v.second;
        break;
    }
    case TimelineProperty::Scale:
    {
        kv.kind = KeyValue::Kind::Vec2;
        auto v = readPair("Scale", "ScaleX", "ScaleY", 1.0f, 1.0f);
        kv.vx = v.first; kv.vy = v.second;
        break;
    }
    case TimelineProperty::AnchorPoint:
    {
        kv.kind = KeyValue::Kind::Vec2;
        auto v = readPair("AnchorPoint", "ScaleX", "ScaleY", 0.5f, 0.5f);
        kv.vx = v.first; kv.vy = v.second;
        break;
    }
    case TimelineProperty::RotationSkew:
    case TimelineProperty::Skew:
        kv.kind = KeyValue::Kind::Vec2;
        kv.vx = 0.0f; kv.vy = 0.0f;
        break;
    case TimelineProperty::CColor:
    {
        kv.kind = KeyValue::Kind::Color;
        kv.r = kv.g = kv.b = kv.a = 255;
        kv.alphaAttr = 255;
        for (auto& [k, v] : n.subAttrs("CColor"))
        {
            const int iv = std::atoi(v.c_str());
            if      (k == "A") kv.a = kv.alphaAttr = iv;
            else if (k == "R") kv.r = iv;
            else if (k == "G") kv.g = iv;
            else if (k == "B") kv.b = iv;
        }
        break;
    }
    case TimelineProperty::Alpha:
        kv.kind = KeyValue::Kind::Int;
        kv.intVal = 255;
        break;
    case TimelineProperty::ZOrder:
        kv.kind = KeyValue::Kind::Int;
        kv.intVal = std::atoi(n.attr("ZOrder", "0").c_str());
        break;
    case TimelineProperty::VisibleForFrame:
        kv.kind = KeyValue::Kind::Bool;
        kv.boolVal = n.visible();
        break;
    case TimelineProperty::Event:
        kv.kind = KeyValue::Kind::Event;
        break;
    case TimelineProperty::FileData:
    case TimelineProperty::InnerAction:
    case TimelineProperty::Unknown:
        kv.kind = KeyValue::Kind::None;
        break;
    }
    return kv;
}

/// Distill an event Value string into a one-line label drawn next to
/// the keyframe. "goto_stop:30" becomes "→■30", "goto_play:0" becomes
/// "→▶0", anything else is the raw event name.
std::string eventLabel(const std::string& value)
{
    auto starts = [&](std::string_view p) {
        return value.size() >= p.size() &&
               std::string_view(value).substr(0, p.size()) == p;
    };
    if (starts("goto_stop:")) return "\xE2\x86\x92" "\xE2\x96\xA0" + value.substr(10);
    if (starts("goto_play:")) return "\xE2\x86\x92" "\xE2\x96\xB6" + value.substr(10);
    return value;
}

/// Pretty label for a node — falls back to ctype if Name is empty
/// (cocostudio sometimes emits anonymous AbstractNodeData wrappers).
std::string nodeLabel(const Node& n)
{
    auto nm = n.name();
    if (!nm.empty()) return nm;
    auto ct = n.ctype();
    if (!ct.empty()) return "<" + ct + ">";
    return "<unnamed>";
}

/// Build a fast actionTag → Node map for the doc.  Used to label
/// timelines by their owner node.
std::unordered_map<int, Node> indexNodesByActionTag(const CsdDocument& doc)
{
    std::unordered_map<int, Node> map;
    for (auto& n : doc.allNodes())
    {
        if (auto t = n.actionTag())
            map.emplace(*t, n);
    }
    return map;
}

/// Format the toolbar's frame counter as "current / total".
std::string formatFrameCounter(int cur, int total)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d / %d", cur, total);
    return std::string(buf);
}

/// Stable color from a property — keeps the same hue across all
/// nodes' tracks for that property so the eye can pattern-match.
ImU32 colorForProperty(TimelineProperty p)
{
    switch (p)
    {
    case TimelineProperty::Position:        return IM_COL32(120, 200, 255, 255);
    case TimelineProperty::Scale:           return IM_COL32(120, 255, 160, 255);
    case TimelineProperty::RotationSkew:    return IM_COL32(255, 200, 100, 255);
    case TimelineProperty::Skew:            return IM_COL32(220, 180, 120, 255);
    case TimelineProperty::AnchorPoint:     return IM_COL32(180, 180, 220, 255);
    case TimelineProperty::CColor:          return IM_COL32(255, 140, 200, 255);
    case TimelineProperty::Alpha:           return IM_COL32(220, 120, 220, 255);
    case TimelineProperty::FileData:        return IM_COL32(255, 200, 80,  255);
    case TimelineProperty::VisibleForFrame: return IM_COL32(180, 220, 180, 255);
    case TimelineProperty::ZOrder:          return IM_COL32(140, 220, 220, 255);
    case TimelineProperty::InnerAction:     return IM_COL32(220, 220, 255, 255);
    case TimelineProperty::Event:           return IM_COL32(255, 255, 140, 255);
    case TimelineProperty::Unknown:         return IM_COL32(160, 160, 160, 255);
    }
    return IM_COL32(160, 160, 160, 255);
}

/// Centred diamond marker for a keyframe. `selected` and `tweened`
/// shift the visual treatment (selected = white outline; tweened
/// flag false = hollow centre, signalling "hold to next keyframe").
void drawKeyframeDiamond(ImDrawList* dl, ImVec2 c, ImU32 fill,
                         bool selected, bool tweened)
{
    const float h = kKeyDiamondHalf;
    const ImVec2 top   (c.x,     c.y - h);
    const ImVec2 right (c.x + h, c.y);
    const ImVec2 bot   (c.x,     c.y + h);
    const ImVec2 left  (c.x - h, c.y);
    if (tweened)
    {
        dl->AddQuadFilled(top, right, bot, left, fill);
    }
    else
    {
        // Hollow diamond — outline only, signals the keyframe holds
        // its value rather than tweens to the next.
        dl->AddQuad(top, right, bot, left, fill, 1.5f);
    }
    if (selected)
    {
        const ImVec2 hi(h + 1.5f, h + 1.5f);
        dl->AddQuad(
            ImVec2(c.x,         c.y - hi.y),
            ImVec2(c.x + hi.x,  c.y),
            ImVec2(c.x,         c.y + hi.y),
            ImVec2(c.x - hi.x,  c.y),
            IM_COL32_WHITE, 1.5f);
    }
}

}  // namespace


// Manual hit-test state. We render the right pane visuals normally
// (rectangles, lines, diamonds, text) but capture clicks via a
// SINGLE full-pane InvisibleButton at the end of submission. On
// click, we walk a priority-ordered list of bbox targets recorded
// during render and dispatch to the right handler. This sidesteps
// ImGui's overlap rules entirely.

// Drag in progress — only one at a time across the panel.
enum { sDragNone = 0, sDragKeyframe = 1, sDragEvent = 2,
       sDragEndHandle = 3, sDragRuler = 4 };
static int             sDragKind          = sDragNone;
static pugi::xml_node  sDragTlEl;        // owning timeline (Keyframe/Event)
static pugi::xml_node  sDragKfEl;        // dragged keyframe
static int             sDragStartFrame   = 0;
static int             sDragStartDuration = 0;
static float           sDragStartMouseX  = 0.0f;
static bool            sDragMoved        = false;
static bool            sDragUndoPushed   = false;

// Right-click context target — captured on right-click and consumed
// by global popups rendered after the pane InvisibleButton.
enum { sCtxNone = 0, sCtxKeyframe = 1, sCtxEvent = 2,
       sCtxTrackLane = 3 };
static int             sCtxKind = sCtxNone;
static pugi::xml_node  sCtxTlEl;
static pugi::xml_node  sCtxKfEl;
static int             sCtxFrame = 0;

void drawTimelineBody(Editor& editor)
{
    if (!editor.doc())
    {
        ImGui::TextDisabled("No document");
        return;
    }

    // Safety reset for drag state. If no ImGui item is active and
    // no mouse button is held, any leftover drag state is stale —
    // clear it so the next click starts fresh.
    if (!ImGui::IsAnyItemActive() &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        sDragKind = sDragNone;
        sDragMoved = false;
        sDragUndoPushed = false;
    }

    auto& doc = *editor.doc();
    auto anim = animationOf(doc);
    if (!anim.valid())
    {
        ImGui::TextDisabled(
            "No <Animation> element. (Round-trip preserves it; if you "
            "loaded a .csd with no timeline data, this is expected.)");
        return;
    }

    const int duration = std::max(0, anim.duration());
    auto animList = animationListOf(doc);
    const auto ranges = animList.valid() ? animList.ranges()
                                         : std::vector<AnimationRange>{};

    // Bound the playhead to the current animation's range. Empty
    // _tlAnimName means the "Full" range, 0 .. duration.
    int rangeStart = 0;
    int rangeEnd   = duration;
    if (!editor._tlAnimName.empty())
    {
        for (auto& r : ranges)
        {
            if (r.name == editor._tlAnimName)
            {
                rangeStart = r.startIndex;
                rangeEnd   = r.endIndex;
                break;
            }
        }
    }
    if (editor._tlFrame < rangeStart) editor._tlFrame = rangeStart;
    if (editor._tlFrame > rangeEnd)   editor._tlFrame = rangeEnd;

    // ---------------------------------------------------------------
    // Toolbar row: animation dropdown · play · pause · loop · counter
    // ---------------------------------------------------------------
    {
        ImGui::PushItemWidth(140.0f);
        const char* preview = editor._tlAnimName.empty()
            ? "Full"
            : editor._tlAnimName.c_str();
        if (ImGui::BeginCombo("##anim", preview))
        {
            if (ImGui::Selectable("Full", editor._tlAnimName.empty()))
                editor._tlAnimName.clear();
            for (auto& r : ranges)
            {
                const bool sel = (r.name == editor._tlAnimName);
                if (ImGui::Selectable(r.name.c_str(), sel))
                    editor._tlAnimName = r.name;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();

        // Play / pause toggles _tlPlaying. The actual evaluator
        // (apply timeline values to axmol nodes) lands in M2 — for
        // now this is a UI-only state flag.
        if (editor._tlPlaying)
        {
            if (ImGui::Button("Pause")) editor._tlPlaying = false;
        }
        else
        {
            if (ImGui::Button("Play"))  editor._tlPlaying = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop"))
        {
            editor._tlPlaying = false;
            editor._tlFrame   = rangeStart;
        }
        ImGui::SameLine();
        ImGui::Checkbox("Loop", &editor._tlLoop);
        ImGui::SameLine();

        ImGui::PushItemWidth(60.0f);
        int fps = editor._tlFps;
        if (ImGui::DragInt("fps", &fps, 1.0f, 1, 240, "%d"))
            editor._tlFps = std::clamp(fps, 1, 240);
        ImGui::PopItemWidth();
        ImGui::SameLine();

        // "+ Event @ playhead" — opens a popup that creates an
        // EventFrame on the scene-global event timeline at _tlFrame.
        if (ImGui::Button("+ Event"))
            ImGui::OpenPopup("##add-event-popup");

        // The popup body is shared between the toolbar button and the
        // events-lane right-click; both call OpenPopup with the same
        // ID. Building the event Value string is identical in both
        // entry points.
        if (ImGui::BeginPopup("##add-event-popup"))
        {
            static int  sKind = 2;        // 0=stop 1=play 2=send
            static int  sFrame = 0;
            static char sName[64] = "";
            ImGui::TextUnformatted("Add event keyframe");
            ImGui::Separator();
            ImGui::RadioButton("goto_stop", &sKind, 0); ImGui::SameLine();
            ImGui::RadioButton("goto_play", &sKind, 1); ImGui::SameLine();
            ImGui::RadioButton("send_event", &sKind, 2);
            if (sKind == 0 || sKind == 1)
                ImGui::InputInt("frame", &sFrame);
            else
                ImGui::InputText("name", sName, sizeof(sName));
            ImGui::Separator();
            if (ImGui::Button("Add"))
            {
                std::string value;
                if (sKind == 0) value = "goto_stop:" + std::to_string(sFrame);
                else if (sKind == 1) value = "goto_play:" + std::to_string(sFrame);
                else value = sName;
                if (!value.empty())
                {
                    editor.pushUndo();
                    auto evTl = findOrCreateEventsTimeline(anim);
                    KeyValue kv;
                    kv.kind = KeyValue::Kind::Event;
                    kv.strA = value;
                    evTl.appendFrame(editor._tlFrame, kv);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    ImGui::Separator();

    // ---------------------------------------------------------------
    // Build the per-row track list.
    // ---------------------------------------------------------------
    auto nodesByTag = indexNodesByActionTag(doc);
    auto allTimelines = anim.timelines();

    // Optional filter: only tracks for the selected node + its
    // descendants. Lets the user focus while editing.
    static bool sFilterToSelection = false;
    {
        // Right-aligned: frame readout + editable Duration + filter
        // checkbox + track count. Duration lives on the right end so
        // it reads naturally as "<current> / Length:[<total>]"
        // alongside the other readouts.
        ImGui::SameLine(ImGui::GetContentRegionAvail().x +
                        ImGui::GetCursorPosX() - 380.0f);
        char curBuf[20];
        std::snprintf(curBuf, sizeof(curBuf),
                      "Frame %d  ", editor._tlFrame);
        ImGui::TextUnformatted(curBuf);
        ImGui::SameLine();

        int dur = duration;
        ImGui::PushItemWidth(72.0f);
        if (ImGui::DragInt("Length", &dur, 0.5f, 0, 100000, "%d frames"))
        {
            if (dur != duration)
            {
                editor.pushUndo();
                anim.setDuration(std::max(0, dur));
            }
        }
        ImGui::PopItemWidth();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Total animation length in frames.\n"
                "Drag to change, or drag the last keyframe past the "
                "right edge to auto-grow.");
        ImGui::SameLine();

        ImGui::Checkbox("Selection only", &sFilterToSelection);
        ImGui::SameLine();
        ImGui::TextDisabled("(%d tracks)", (int)allTimelines.size());
    }

    std::set<int> selectionTags;
    if (sFilterToSelection && editor.selected().valid())
    {
        std::vector<Node> ds;
        editor.selected().collectDescendants(ds);
        for (auto& n : ds)
            if (auto t = n.actionTag()) selectionTags.insert(*t);
    }

    // Events render in a dedicated pinned lane at the top — separate
    // from the per-node tracks. So pull them out of the per-node
    // visible list and address them via `eventsTl` instead.
    auto eventsTl = findEventsTimeline(anim);

    std::vector<TimelineRef> visible;
    visible.reserve(allTimelines.size());
    for (auto& t : allTimelines)
    {
        if (t.property() == TimelineProperty::Event &&
            t.actionTag() == kEventsTimelineActionTag) continue;
        if (sFilterToSelection &&
            !selectionTags.count(t.actionTag())) continue;
        // Skip empty <Timeline> placeholders — only properties
        // that actually carry keyframes appear as tracks.
        if (t.frames().empty()) continue;
        visible.push_back(t);
    }

    // Hit-test targets — populated during render, consumed at the
    // end by the manual priority hit-test on a single full-pane
    // InvisibleButton.
    enum class HitKind {
        Keyframe, EventDiamond, EndHandle,
        TrackLane, EventsLane, Ruler
    };
    struct HitTarget {
        HitKind   kind;
        ImRect    bbox;
        pugi::xml_node tl;
        pugi::xml_node kf;
        int       frameIndex = 0;
    };
    std::vector<HitTarget> targets;

    // Group tracks by their owning node (ActionTag). Rendered as
    // node header → indented property rows. Document order is
    // preserved within each group so authored timelines come first.
    std::vector<TLRow> rows;
    {
        std::vector<int> orderedTags;
        std::unordered_map<int, std::vector<TimelineRef>> byTag;
        for (auto& t : visible)
        {
            const int tag = t.actionTag();
            if (!byTag.count(tag)) orderedTags.push_back(tag);
            byTag[tag].push_back(t);
        }
        float y = 0.0f;
        for (int tag : orderedTags)
        {
            TLRow header;
            header.kind = TLRow::Kind::NodeHeader;
            header.actionTag = tag;
            auto nIt = nodesByTag.find(tag);
            if (nIt != nodesByTag.end()) header.node = nIt->second;
            header.yOffset = y;
            header.height  = kNodeHeaderHeight;
            rows.push_back(std::move(header));
            y += kNodeHeaderHeight;
            for (auto& tl : byTag[tag])
            {
                TLRow tr;
                tr.kind = TLRow::Kind::Track;
                tr.actionTag = tag;
                tr.tl = tl;
                tr.yOffset = y;
                tr.height = kRowHeight;
                rows.push_back(std::move(tr));
                y += kRowHeight;
            }
        }
    }

    // Events lane only appears once the user has added at least one
    // event. The toolbar "+ Event" button is the entry point for
    // creating the first event; until then the lane stays hidden so
    // the timeline reads "what's animated" rather than "every kind of
    // thing that could be animated".
    const bool eventsLaneVisible =
        eventsTl.valid() && !eventsTl.frames().empty();
    const float laneH = eventsLaneVisible ? kEventsLaneHeight : 0.0f;

    // ---------------------------------------------------------------
    // Layout the ruler + tracks. The right pane is scrollable; left
    // column holds the labels and stays fixed.
    // ---------------------------------------------------------------
    static float sPxPerFrame = kDefaultPxPerFrame;

    const ImVec2 region = ImGui::GetContentRegionAvail();
    const float trackPaneWidth = std::max(100.0f, region.x - kLeftColumnWidth);

    // Auto-fit horizontal zoom on first display so a typical 60-frame
    // scene fills the panel. After the first frame, the user can
    // change zoom with Ctrl+wheel.
    if (duration > 0)
    {
        const float fit = (trackPaneWidth - 16.0f) / float(duration + 1);
        if (sPxPerFrame == kDefaultPxPerFrame && fit > 0.0f)
            sPxPerFrame = std::clamp(fit, kMinPxPerFrame, kMaxPxPerFrame);
    }

    // Two-column layout via Columns (simple, no Tables overhead).
    if (ImGui::BeginChild("##tl-container", region, false,
                          ImGuiWindowFlags_NoMove))
    {
        // Capture the child's visible-top once so we can pin the
        // ruler + events lane there regardless of vertical scroll.
        // GetWindowPos() returns the screen-top of this BeginChild,
        // which is fixed; GetCursorScreenPos() floats with scroll.
        const ImVec2 childWinPos = ImGui::GetWindowPos();
        const float  headerH     = kRulerHeight + laneH;

        // border=false removes the draggable column-resize splitter.
        // We draw a thin vertical separator with ImDrawList below
        // the per-row rendering so the visual cue is preserved
        // without the resize interaction.
        ImGui::Columns(2, "##tl-cols", false);
        ImGui::SetColumnWidth(0, kLeftColumnWidth);

        // ---- Left column: header spacer + per-track labels --------
        // The Events label on the left column gets pinned at the same
        // sticky-Y as the ruler/events lane on the right; we draw it
        // later as part of the overlay pass (see "Sticky overlay"
        // below). Reserve the header height here so per-node labels
        // start at the same Y as the per-node tracks on the right.
        ImGui::Dummy(ImVec2(0, headerH));

        // Track-deletion is captured here and applied AFTER both
        // column loops finish. Removing a <Timeline> mid-render
        // would invalidate the right column's iterator over `rows`
        // (since rows holds TimelineRef pointing at the deleted
        // element) and crash on the next access. Deferring the
        // remove_child until both loops are done keeps the structure
        // alive for one more pass; next frame rebuilds rows clean.
        pugi::xml_node deferredDeleteTimeline;

        // Walk the row list. NodeHeader rows show the node name
        // (once per group, not duplicated per property) and a "+"
        // button that opens a popup to add a new tweenable property.
        // Track rows are indented "Position" / "Scale" / etc. with
        // a small "x" affordance for delete (also reachable via
        // right-click).
        for (size_t ri = 0; ri < rows.size(); ++ri)
        {
            auto& r = rows[ri];
            if (r.kind == TLRow::Kind::NodeHeader)
            {
                std::string label = r.node.valid()
                    ? nodeLabel(r.node)
                    : ("actionTag=" + std::to_string(r.actionTag));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    IM_COL32(220, 220, 230, 255));
                ImGui::TextUnformatted(label.c_str());
                ImGui::PopStyleColor();

                // "+ Property" button right-aligned.
                ImGui::SameLine();
                const float btnW = 18.0f;
                ImGui::SameLine(kLeftColumnWidth - btnW - 6.0f);
                char addId[32];
                std::snprintf(addId, sizeof(addId),
                              "+##addprop-%d", r.actionTag);
                if (ImGui::SmallButton(addId))
                    ImGui::OpenPopup(addId);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Add tweenable property to %s",
                        label.c_str());

                // Add-Property popup. Shows only properties that
                // don't already have a track on this node.
                if (ImGui::BeginPopup(addId))
                {
                    ImGui::TextUnformatted("Add property");
                    ImGui::Separator();
                    static const TimelineProperty kProps[] = {
                        TimelineProperty::Position,
                        TimelineProperty::Scale,
                        TimelineProperty::RotationSkew,
                        TimelineProperty::Skew,
                        TimelineProperty::AnchorPoint,
                        TimelineProperty::CColor,
                        TimelineProperty::Alpha,
                        TimelineProperty::ZOrder,
                        TimelineProperty::VisibleForFrame,
                    };
                    for (auto p : kProps)
                    {
                        const bool exists =
                            anim.findTimeline(r.actionTag, p).valid();
                        if (exists) continue;
                        if (ImGui::MenuItem(
                                timelinePropertyDisplay(p)))
                        {
                            editor.pushUndo();
                            auto newTl = anim.createTimeline(
                                r.actionTag, p);
                            const KeyValue seed = r.node.valid()
                                ? defaultSeedFor(r.node, p)
                                : KeyValue{};
                            auto newKf = newTl.appendFrame(
                                editor._tlFrame, seed);
                            if (newKf.valid())
                            {
                                editor._tlSelectedKf.timelineEl =
                                    newTl.element();
                                editor._tlSelectedKf.frameEl =
                                    newKf.element();
                                editor._tlEvaluatorAppliedFrame = -2;
                            }
                        }
                    }
                    ImGui::EndPopup();
                }
            }
            else
            {
                // Track row — indented diamond + property name.
                // Context-menu actions (add keyframe / delete track)
                // are exposed via right-click on the track LANE in
                // the right column; keeping the label area as plain
                // text avoids a Selectable that would steal clicks
                // meant for keyframe diamonds.
                auto& t = r.tl;
                ImGui::Indent(kTrackIndent);
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      colorForProperty(t.property()));
                ImGui::TextUnformatted("\xE2\x97\x86 ");
                ImGui::PopStyleColor();
                ImGui::SameLine(0, 0);
                ImGui::TextUnformatted(
                    timelinePropertyDisplay(t.property()));
                ImGui::Unindent(kTrackIndent);
            }
        }
        if (rows.empty())
        {
            ImGui::TextDisabled(
                sFilterToSelection
                    ? "(select a node to see its tracks)"
                    : "(no animation tracks in this scene)");
        }

        // ---- Right column: per-node track lanes ------------------
        // Sticky ruler + events lane are drawn LAST as an overlay
        // (see below) so they sit on top of the scrolling tracks
        // and stay pinned at the visible top. The per-node tracks
        // below render at their natural content-space Y, then the
        // overlay re-draws ruler + events at child-window Y so they
        // visually float as the user scrolls.
        ImGui::NextColumn();

        const ImVec2 paneOrigin = ImGui::GetCursorScreenPos();
        const float  pxPerFrame = sPxPerFrame;
        const float  contentW   = std::max(
            trackPaneWidth, (duration + 1) * pxPerFrame + 16.0f);

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Reserve the header strip so per-node tracks lay out below
        // it in content space. The pixels reserved here will be
        // overpainted by the sticky overlay at the end.
        ImGui::Dummy(ImVec2(contentW, headerH));

        // ---- Per-node tracks (in content space, scrolls with body) -
        const float tracksTop = paneOrigin.y + headerH;

        // Compute total track-area height from the row list so the
        // bottom Dummy reserves the right amount of scroll content.
        const float bodyRowsH = rows.empty()
            ? kRowHeight
            : rows.back().yOffset + rows.back().height;

        // Lane backgrounds + content per row. NodeHeader rows show a
        // dim band; Track rows show alternating bands + diamonds +
        // a right-click lane for "Add keyframe at playhead".
        for (size_t ri = 0; ri < rows.size(); ++ri)
        {
            auto& r = rows[ri];
            const float y = tracksTop + r.yOffset;

            if (r.kind == TLRow::Kind::NodeHeader)
            {
                dl->AddRectFilled(
                    ImVec2(paneOrigin.x, y),
                    ImVec2(paneOrigin.x + contentW, y + r.height),
                    IM_COL32(34, 36, 42, 255));
                dl->AddLine(
                    ImVec2(paneOrigin.x, y + r.height - 0.5f),
                    ImVec2(paneOrigin.x + contentW, y + r.height - 0.5f),
                    IM_COL32(60, 60, 70, 255), 1.0f);
                continue;
            }

            // Track row.
            auto& t = r.tl;
            const ImU32 propCol = colorForProperty(t.property());

            // Lane background: subtle alternating bands.
            const ImU32 band = (ri & 1)
                ? IM_COL32(38, 40, 48, 255)
                : IM_COL32(46, 48, 56, 255);
            dl->AddRectFilled(
                ImVec2(paneOrigin.x, y),
                ImVec2(paneOrigin.x + contentW, y + kRowHeight),
                band);
            // Light horizontal centerline so the track reads as a row.
            dl->AddLine(
                ImVec2(paneOrigin.x + 8.0f,            y + kRowHeight * 0.5f),
                ImVec2(paneOrigin.x + 8.0f + (duration + 0.5f) * pxPerFrame,
                       y + kRowHeight * 0.5f),
                IM_COL32(80, 80, 90, 200), 1.0f);

            // Record the lane-bg as a hit-test target for right-click
            // "Add keyframe @ N" / "Delete Track". Left-click on the
            // lane (off-diamond) does nothing.
            {
                HitTarget tgt;
                tgt.kind = HitKind::TrackLane;
                tgt.bbox = ImRect(paneOrigin.x, y,
                                  paneOrigin.x + contentW,
                                  y + kRowHeight);
                tgt.tl = t.element();
                targets.push_back(tgt);
            }

            // Keyframes — visual + recorded as hit targets. Manual
            // hit-test below picks them up first (highest priority).
            for (auto kf : t.frames())
            {
                const int idx = kf.frameIndex();
                const float x = paneOrigin.x + 8.0f + idx * pxPerFrame;
                const bool isSelected = editor._tlSelectedKf.valid() &&
                    editor._tlSelectedKf.frameEl == kf.element();
                drawKeyframeDiamond(
                    dl,
                    ImVec2(x, y + kRowHeight * 0.5f),
                    propCol, isSelected, kf.tween());

                HitTarget tgt;
                tgt.kind = HitKind::Keyframe;
                tgt.bbox = ImRect(x - kKeyDiamondHalf - 1.0f,
                                  y + 2.0f,
                                  x + kKeyDiamondHalf + 1.0f,
                                  y + kRowHeight - 2.0f);
                tgt.tl = t.element();
                tgt.kf = kf.element();
                tgt.frameIndex = idx;
                targets.push_back(tgt);
            }
        }

        // Reserve enough vertical content space so the right pane
        // scrolls when there are many tracks. Use the precomputed
        // row-list total so node-headers + tracks both contribute.
        const float totalH = std::max(
            region.y - kHeaderHeight,
            headerH + bodyRowsH + 4.0f);
        ImGui::SetCursorScreenPos(paneOrigin);
        ImGui::Dummy(ImVec2(contentW, totalH));

        // ---- Sticky overlay: ruler + events lane + playhead -------
        // Drawn at the visible top of the BeginChild so it stays put
        // when the user scrolls vertically. Drawn AFTER per-node
        // tracks so it overpaints them at the top edge of the view.
        const float stickyRulerY  = childWinPos.y;
        const float stickyEventsY = stickyRulerY + kRulerHeight;
        {
            // -- Ruler band background + ticks --
            const ImVec2 rulerMin(paneOrigin.x, stickyRulerY);
            const ImVec2 rulerMax(paneOrigin.x + contentW,
                                  stickyRulerY + kRulerHeight);
            dl->AddRectFilled(rulerMin, rulerMax,
                              IM_COL32(40, 40, 50, 255));
            for (int f = 0; f <= duration; ++f)
            {
                const float x = paneOrigin.x + 8.0f + f * pxPerFrame;
                const bool major = (f % 10 == 0);
                const bool mid   = (f % 5  == 0);
                const float h = major ? 10.0f : (mid ? 6.0f : 3.0f);
                const ImU32 c = major ? IM_COL32(220, 220, 220, 255)
                                      : IM_COL32(140, 140, 150, 255);
                dl->AddLine(ImVec2(x, rulerMax.y - h),
                            ImVec2(x, rulerMax.y),
                            c, 1.0f);
                if (major)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "%d", f);
                    dl->AddText(ImVec2(x + 2.0f, rulerMin.y + 2.0f),
                                IM_COL32(220, 220, 220, 220), buf);
                }
            }
            // Record the ruler band as a hit-test target. Left-click
            // here scrubs the playhead. Lower priority than diamonds
            // and the green end handle.
            {
                HitTarget tgt;
                tgt.kind = HitKind::Ruler;
                tgt.bbox = ImRect(rulerMin.x, rulerMin.y,
                                  rulerMax.x, rulerMax.y);
                targets.push_back(tgt);
            }

            // -- Events lane (only when at least one event exists) --
            const ImU32 evCol = colorForProperty(TimelineProperty::Event);
            if (eventsLaneVisible)
            {
            dl->AddRectFilled(
                ImVec2(paneOrigin.x, stickyEventsY),
                ImVec2(paneOrigin.x + contentW,
                       stickyEventsY + kEventsLaneHeight),
                IM_COL32(28, 30, 36, 255));
            dl->AddLine(
                ImVec2(paneOrigin.x,
                       stickyEventsY + kEventsLaneHeight - 0.5f),
                ImVec2(paneOrigin.x + contentW,
                       stickyEventsY + kEventsLaneHeight - 0.5f),
                IM_COL32(60, 60, 70, 255), 1.0f);
            dl->AddLine(
                ImVec2(paneOrigin.x + 8.0f,
                       stickyEventsY + kEventsLaneHeight * 0.5f),
                ImVec2(paneOrigin.x + 8.0f +
                       (duration + 0.5f) * pxPerFrame,
                       stickyEventsY + kEventsLaneHeight * 0.5f),
                IM_COL32(80, 80, 90, 200), 1.0f);

            // Record events lane bg as a hit-test target — right-
            // click anywhere in the lane opens the add-event popup.
            {
                HitTarget tgt;
                tgt.kind = HitKind::EventsLane;
                tgt.bbox = ImRect(paneOrigin.x, stickyEventsY,
                                  paneOrigin.x + contentW,
                                  stickyEventsY + kEventsLaneHeight);
                targets.push_back(tgt);
            }

            if (eventsTl.valid())
            {
                for (auto kf : eventsTl.frames())
                {
                    const int idx = kf.frameIndex();
                    const float x = paneOrigin.x + 8.0f + idx * pxPerFrame;
                    const ImVec2 c(
                        x, stickyEventsY + kEventsLaneHeight * 0.5f);
                    drawKeyframeDiamond(dl, c, evCol,
                        /*selected=*/false, /*tweened=*/kf.tween());

                    const auto val = kf.read(TimelineProperty::Event);
                    const std::string lbl = eventLabel(val.strA);
                    dl->AddText(
                        ImVec2(c.x + kKeyDiamondHalf + 2.0f,
                               c.y - ImGui::GetTextLineHeight() * 0.5f),
                        IM_COL32(220, 220, 220, 220), lbl.c_str());

                    HitTarget tgt;
                    tgt.kind = HitKind::EventDiamond;
                    tgt.bbox = ImRect(
                        c.x - kKeyDiamondHalf - 2.0f,
                        stickyEventsY + 2.0f,
                        c.x + kKeyDiamondHalf + 2.0f,
                        stickyEventsY + kEventsLaneHeight - 2.0f);
                    tgt.tl = eventsTl.element();
                    tgt.kf = kf.element();
                    tgt.frameIndex = idx;
                    targets.push_back(tgt);
                }
            }

            // -- Sticky "Events" label on the LEFT column ----------
            // Drawn last so the rendered glyph + text sit on top of
            // any per-node label that scrolled into the same Y.
            {
                const float labelX = childWinPos.x + 6.0f;
                const float labelY = stickyEventsY + (kEventsLaneHeight -
                                          ImGui::GetTextLineHeight()) * 0.5f;
                // Bg block matching column width so scrolled labels
                // don't bleed through.
                dl->AddRectFilled(
                    ImVec2(childWinPos.x, stickyRulerY),
                    ImVec2(childWinPos.x + kLeftColumnWidth,
                           stickyEventsY + kEventsLaneHeight),
                    IM_COL32(28, 30, 36, 255));
                dl->AddText(
                    ImVec2(labelX, labelY),
                    colorForProperty(TimelineProperty::Event),
                    "\xE2\x97\x86");
                const int evCount = eventsTl.valid()
                    ? (int)eventsTl.frames().size() : 0;
                char ev[48];
                std::snprintf(ev, sizeof(ev), "  Events (%d)", evCount);
                dl->AddText(
                    ImVec2(labelX + 12.0f, labelY),
                    IM_COL32(220, 220, 220, 240), ev);
            }
            }   // end if (eventsLaneVisible)
        }

        // ---- Column separator (visual only, no resize) ----------
        // Replaces the built-in draggable Columns splitter so the
        // user can't accidentally resize the labels column. Drawn
        // after track lanes + sticky overlay so it sits on top of
        // them but stays below the playhead/end-handle.
        dl->AddLine(
            ImVec2(paneOrigin.x, childWinPos.y),
            ImVec2(paneOrigin.x,
                   childWinPos.y + ImGui::GetWindowSize().y),
            IM_COL32(70, 72, 80, 255), 1.0f);

        // ---- End-of-duration handle ------------------------------
        // Vertical bar at frame == duration, draggable horizontally
        // to grow or shrink the animation length. Same visual style
        // as the playhead but drawn in green so it's distinguishable
        // and pinned at the timeline's end. Dragging snaps to the
        // nearest integer frame.
        if (duration >= 0)
        {
            const float endX = paneOrigin.x + 8.0f
                             + duration * pxPerFrame;
            // Visual line spans the full panel height — gives the
            // user a reference column for the timeline's end. The
            // CLICK hit area is restricted to the ruler band only
            // (kRulerHeight tall) so it doesn't occlude keyframe
            // diamonds at frame == duration.
            const float endTop = stickyRulerY;
            const float endVisualBot = childWinPos.y +
                                       ImGui::GetWindowSize().y;
            dl->AddLine(ImVec2(endX, endTop),
                        ImVec2(endX, endVisualBot),
                        IM_COL32(80, 220, 120, 220), 1.5f);
            // Triangle handle at the top, pointing left so the
            // affordance reads as "the end edge of the timeline".
            const ImVec2 e0(endX,         endTop + 1.0f);
            const ImVec2 e1(endX - 8.0f,  endTop + 5.0f);
            const ImVec2 e2(endX,         endTop + 9.0f);
            dl->AddTriangleFilled(
                e0, e1, e2, IM_COL32(80, 220, 120, 230));

            // Record the green end handle as a hit-test target —
            // priority above ruler scrub, below diamonds. Hit zone
            // pinned to the ruler band only.
            HitTarget tgt;
            tgt.kind = HitKind::EndHandle;
            tgt.bbox = ImRect(endX - 7.0f, endTop,
                              endX + 7.0f,
                              endTop + kRulerHeight);
            targets.push_back(tgt);
        }

        // ---- Playhead (full-height vertical line + handle) -------
        // Drawn AFTER the sticky overlay so the triangle handle sits
        // on top of the ruler. Spans from the sticky ruler's top
        // down through the visible track area.
        if (duration > 0 || rangeEnd > rangeStart)
        {
            const float phx = paneOrigin.x + 8.0f
                            + editor._tlFrame * pxPerFrame;
            const float phyTop = stickyRulerY;
            const float phyBot = childWinPos.y +
                                 ImGui::GetWindowSize().y;
            dl->AddLine(ImVec2(phx, phyTop),
                        ImVec2(phx, phyBot),
                        IM_COL32(255, 100, 100, 240), 1.5f);
            const ImVec2 t0(phx, phyTop + 1.0f);
            const ImVec2 t1(phx - 5.0f, phyTop + 9.0f);
            const ImVec2 t2(phx + 5.0f, phyTop + 9.0f);
            dl->AddTriangleFilled(
                t0, t1, t2, IM_COL32(255, 100, 100, 240));
        }

        // Ctrl+wheel zooms; plain wheel scrolls the pane normally.
        if (ImGui::IsWindowHovered() &&
            ImGui::GetIO().KeyCtrl &&
            ImGui::GetIO().MouseWheel != 0.0f)
        {
            sPxPerFrame = std::clamp(
                sPxPerFrame * (1.0f + ImGui::GetIO().MouseWheel * 0.1f),
                kMinPxPerFrame, kMaxPxPerFrame);
        }

        // ---- Manual hit-test on a single full-pane button --------
        // Records of clickable items (recorded above as `targets`)
        // are tested in priority order: keyframe diamonds first
        // (so they always win where they overlap with lanes), then
        // event diamonds, then the green end handle, then the lane
        // backgrounds (right-click only), then the ruler scrub.
        // Bypassing ImGui's per-item hit-test gives us deterministic
        // behaviour without needing AllowOverlap stitching.
        ImGui::SetCursorScreenPos(paneOrigin);
        ImGui::InvisibleButton("##tl-pane",
            ImVec2(contentW, totalH),
            ImGuiButtonFlags_MouseButtonLeft |
            ImGuiButtonFlags_MouseButtonRight);

        const ImVec2 mp = ImGui::GetIO().MousePos;
        const bool   leftClicked  = ImGui::IsItemHovered() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        const bool   rightClicked = ImGui::IsItemHovered() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right);

        auto findHit =
            [&](std::initializer_list<HitKind> order) -> HitTarget*
        {
            for (HitKind k : order)
                for (auto& tgt : targets)
                    if (tgt.kind == k && tgt.bbox.Contains(mp))
                        return &tgt;
            return nullptr;
        };

        // Cursor feedback when hovering the green end handle.
        if (HitTarget* h = findHit(
                {HitKind::Keyframe, HitKind::EventDiamond,
                 HitKind::EndHandle}))
        {
            if (h->kind == HitKind::EndHandle)
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }

        // ---- Mouse-down: pick a target and arm a drag ------------
        if (leftClicked)
        {
            HitTarget* h = findHit({
                HitKind::Keyframe,
                HitKind::EventDiamond,
                HitKind::EndHandle,
                HitKind::Ruler,
            });
            if (h)
            {
                if (h->kind == HitKind::Keyframe ||
                    h->kind == HitKind::EventDiamond)
                {
                    sDragKind = (h->kind == HitKind::Keyframe)
                        ? sDragKeyframe : sDragEvent;
                    sDragTlEl = h->tl;
                    sDragKfEl = h->kf;
                    sDragStartFrame  = h->frameIndex;
                    sDragStartMouseX = mp.x;
                    sDragMoved       = false;
                    sDragUndoPushed  = false;
                }
                else if (h->kind == HitKind::EndHandle)
                {
                    sDragKind = sDragEndHandle;
                    sDragStartDuration = anim.duration();
                    sDragStartMouseX   = mp.x;
                    sDragMoved         = false;
                    sDragUndoPushed    = false;
                }
                else if (h->kind == HitKind::Ruler)
                {
                    sDragKind = sDragRuler;
                    // Scrub immediately on click.
                    const float relx = mp.x - paneOrigin.x - 8.0f;
                    editor._tlFrame = std::clamp(
                        int(relx / pxPerFrame + 0.5f),
                        rangeStart, rangeEnd);
                }
            }
        }

        // ---- While held: continue drag --------------------------
        if (ImGui::IsItemActive() &&
            ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
            sDragKind != sDragNone)
        {
            const float dx = mp.x - sDragStartMouseX;
            if (sDragKind == sDragKeyframe ||
                sDragKind == sDragEvent)
            {
                if (std::abs(dx) >= 4.0f && sDragKfEl)
                {
                    if (!sDragUndoPushed)
                    {
                        editor.pushUndo();
                        sDragUndoPushed = true;
                    }
                    sDragMoved = true;
                    int target = sDragStartFrame +
                        int(std::round(dx / pxPerFrame));
                    target = std::max(0, target);
                    if (target > anim.duration())
                        anim.setDuration(target);
                    KeyframeRef kf(sDragKfEl);
                    if (target != kf.frameIndex())
                        kf.setFrameIndex(target);
                }
            }
            else if (sDragKind == sDragEndHandle)
            {
                if (std::abs(dx) >= 4.0f)
                {
                    if (!sDragUndoPushed)
                    {
                        editor.pushUndo();
                        sDragUndoPushed = true;
                    }
                    int target = sDragStartDuration +
                        int(std::round(dx / pxPerFrame));
                    target = std::max(0, target);
                    if (target != anim.duration())
                        anim.setDuration(target);
                }
            }
            else if (sDragKind == sDragRuler)
            {
                const float relx = mp.x - paneOrigin.x - 8.0f;
                editor._tlFrame = std::clamp(
                    int(relx / pxPerFrame + 0.5f),
                    rangeStart, rangeEnd);
            }
        }

        // ---- On release: clean-click select + cleanup -----------
        if (ImGui::IsItemDeactivated())
        {
            if ((sDragKind == sDragKeyframe ||
                 sDragKind == sDragEvent) &&
                !sDragMoved && sDragKfEl)
            {
                // Plain click — select + seek.
                editor._tlSelectedKf.timelineEl = sDragTlEl;
                editor._tlSelectedKf.frameEl    = sDragKfEl;
                editor._tlFrame = std::clamp(
                    sDragStartFrame, rangeStart, rangeEnd);
            }
            sDragKind = sDragNone;
            sDragMoved = false;
            sDragUndoPushed = false;
        }

        // ---- Right-click: open a context popup ------------------
        if (rightClicked)
        {
            HitTarget* h = findHit({
                HitKind::Keyframe,
                HitKind::EventDiamond,
                HitKind::TrackLane,
                HitKind::EventsLane,
            });
            if (h)
            {
                if (h->kind == HitKind::Keyframe)
                {
                    sCtxKind = sCtxKeyframe;
                    sCtxTlEl = h->tl;
                    sCtxKfEl = h->kf;
                    ImGui::OpenPopup("##tl-ctx-kf");
                }
                else if (h->kind == HitKind::EventDiamond)
                {
                    sCtxKind = sCtxEvent;
                    sCtxTlEl = h->tl;
                    sCtxKfEl = h->kf;
                    ImGui::OpenPopup("##tl-ctx-ev");
                }
                else if (h->kind == HitKind::TrackLane)
                {
                    sCtxKind = sCtxTrackLane;
                    sCtxTlEl = h->tl;
                    const float relx = mp.x - paneOrigin.x - 8.0f;
                    sCtxFrame = std::clamp(
                        int(relx / pxPerFrame + 0.5f),
                        0, duration);
                    ImGui::OpenPopup("##tl-ctx-trk");
                }
                else if (h->kind == HitKind::EventsLane)
                {
                    ImGui::OpenPopup("##add-event-popup");
                }
            }
        }

        // ---- Global popups (rendered after the pane button) -----
        if (ImGui::BeginPopup("##tl-ctx-kf"))
        {
            KeyframeRef kf(sCtxKfEl);
            TimelineRef tl(sCtxTlEl);
            if (kf.valid() && tl.valid())
            {
                if (ImGui::MenuItem("Delete"))
                {
                    editor.pushUndo();
                    if (editor._tlSelectedKf.frameEl == sCtxKfEl)
                        editor.clearSelectedKeyframe();
                    tl.element().remove_child(sCtxKfEl);
                }
                else
                {
                    bool tw = kf.tween();
                    if (ImGui::MenuItem("Tween", nullptr, tw))
                    {
                        editor.pushUndo();
                        kf.setTween(!tw);
                    }
                    if (ImGui::BeginMenu("Easing"))
                    {
                        static const char* kEaseNames[] = {
                            "Linear",
                            "Sine in", "Sine out", "Sine in/out",
                            "Quad in", "Quad out", "Quad in/out",
                            "Cubic in", "Cubic out", "Cubic in/out",
                            "Quart in", "Quart out", "Quart in/out",
                            "Quint in", "Quint out", "Quint in/out",
                            "Expo in", "Expo out", "Expo in/out",
                            "Circ in", "Circ out", "Circ in/out",
                            "Elastic in", "Elastic out",
                            "Elastic in/out",
                            "Back in", "Back out", "Back in/out",
                            "Bounce in", "Bounce out", "Bounce in/out",
                        };
                        const int cur = kf.easingType();
                        for (int e = 0;
                             e < (int)IM_ARRAYSIZE(kEaseNames); ++e)
                        {
                            if (ImGui::MenuItem(kEaseNames[e],
                                                nullptr, cur == e))
                            {
                                editor.pushUndo();
                                kf.setEasingType(e);
                            }
                        }
                        ImGui::EndMenu();
                    }
                }
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("##tl-ctx-ev"))
        {
            KeyframeRef kf(sCtxKfEl);
            TimelineRef tl(sCtxTlEl);
            if (kf.valid() && tl.valid() &&
                ImGui::MenuItem("Delete"))
            {
                editor.pushUndo();
                if (editor._tlSelectedKf.frameEl == sCtxKfEl)
                    editor.clearSelectedKeyframe();
                tl.element().remove_child(sCtxKfEl);
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("##tl-ctx-trk"))
        {
            TimelineRef tl(sCtxTlEl);
            if (tl.valid())
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf),
                              "Add keyframe @ %d", sCtxFrame);
                if (ImGui::MenuItem(buf))
                {
                    editor.pushUndo();
                    KeyValue seed;
                    auto src = tl.frameAtOrBefore(sCtxFrame);
                    if (!src.valid())
                        src = tl.frameAtOrBefore(INT_MAX);
                    if (src.valid())
                        seed = src.read(tl.property());
                    auto newKf = tl.appendFrame(sCtxFrame, seed);
                    if (newKf.valid())
                    {
                        editor._tlSelectedKf.timelineEl =
                            tl.element();
                        editor._tlSelectedKf.frameEl =
                            newKf.element();
                        editor._tlEvaluatorAppliedFrame = -2;
                        editor._tlFrame = sCtxFrame;
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete Track"))
                {
                    editor.pushUndo();
                    if (editor._tlSelectedKf.timelineEl ==
                        sCtxTlEl)
                        editor.clearSelectedKeyframe();
                    deferredDeleteTimeline = sCtxTlEl;
                }
            }
            ImGui::EndPopup();
        }

        // Apply any deferred Delete Track. Safe here — both column
        // loops have finished iterating `rows`, so removing the
        // <Timeline> element doesn't invalidate any in-flight refs.
        if (deferredDeleteTimeline)
        {
            auto parent = deferredDeleteTimeline.parent();
            parent.remove_child(deferredDeleteTimeline);
        }

        ImGui::Columns(1);
    }
    ImGui::EndChild();
}

}  // namespace opencs
