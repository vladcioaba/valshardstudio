// Timeline playback: per-frame tick that advances _tlFrame, plus the
// evaluator that drives axmol-node properties from the active doc's
// <Timeline> data. Kept in its own translation unit to keep
// Editor.cpp from growing — same pattern as EditorDocument.cpp /
// EditorInspect.cpp.

#include "UILayoutEditor/Editor.h"
#include "UILayoutEditor/CsdModel.h"
#include "UILayoutEditor/Timeline.h"
#include "UILayoutEditor/Log.h"

#include "axmol.h"
#include "2d/TweenFunction.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace opencs
{

namespace
{

/// Lerp two KeyValues by t ∈ [0,1]. Vec2 / Color / Int interpolate
/// linearly; Bool / File / Inner / Event snap to the "after" sample
/// at t≥1, "before" sample otherwise (no meaningful blend).
KeyValue lerpKeyValue(TimelineProperty p, const KeyValue& a,
                      const KeyValue& b, float t)
{
    KeyValue r = a;
    auto fl = [](int x, int y, float k) {
        const float v = x + (y - x) * k;
        return std::clamp(int(std::round(v)), 0, 255);
    };
    switch (p)
    {
    case TimelineProperty::Position:
    case TimelineProperty::AnchorPoint:
    case TimelineProperty::Scale:
    case TimelineProperty::RotationSkew:
    case TimelineProperty::Skew:
        r.kind = KeyValue::Kind::Vec2;
        r.vx = a.vx + (b.vx - a.vx) * t;
        r.vy = a.vy + (b.vy - a.vy) * t;
        break;
    case TimelineProperty::CColor:
        r.kind = KeyValue::Kind::Color;
        r.r = fl(a.r, b.r, t);
        r.g = fl(a.g, b.g, t);
        r.b = fl(a.b, b.b, t);
        r.a = fl(a.a, b.a, t);
        r.alphaAttr = fl(a.alphaAttr, b.alphaAttr, t);
        break;
    case TimelineProperty::Alpha:
    case TimelineProperty::ZOrder:
        r.kind = KeyValue::Kind::Int;
        r.intVal = (int)std::round(a.intVal + (b.intVal - a.intVal) * t);
        break;
    case TimelineProperty::VisibleForFrame:
        r.kind = KeyValue::Kind::Bool;
        r.boolVal = (t >= 1.0f) ? b.boolVal : a.boolVal;
        break;
    case TimelineProperty::FileData:
    case TimelineProperty::InnerAction:
    case TimelineProperty::Event:
    case TimelineProperty::Unknown:
        r = (t >= 1.0f) ? b : a;
        break;
    }
    return r;
}

/// Push a single property's value onto the live axmol node. Visual
/// properties only — File / Inner / Event are not driven from here
/// (they need separate handling: texture swap is M5+, events fire
/// in fireEventKeyframesIfCrossed).
void applyToAxNode(TimelineProperty p, const KeyValue& v, ax::Node* n)
{
    if (!n) return;
    switch (p)
    {
    case TimelineProperty::Position:
        n->setPosition(v.vx, v.vy);
        break;
    case TimelineProperty::AnchorPoint:
        n->setAnchorPoint(ax::Vec2(v.vx, v.vy));
        break;
    case TimelineProperty::Scale:
        n->setScaleX(v.vx);
        n->setScaleY(v.vy);
        break;
    case TimelineProperty::RotationSkew:
        // Cocos's RotationSkew is two independent angles applied as
        // X / Y rotations (degrees). axmol exposes them via
        // setRotationSkewX/Y.
        n->setRotationSkewX(v.vx);
        n->setRotationSkewY(v.vy);
        break;
    case TimelineProperty::Skew:
        n->setSkewX(v.vx);
        n->setSkewY(v.vy);
        break;
    case TimelineProperty::CColor:
        n->setColor(ax::Color3B(
            (uint8_t)v.r, (uint8_t)v.g, (uint8_t)v.b));
        n->setOpacity((uint8_t)v.alphaAttr);
        break;
    case TimelineProperty::Alpha:
        n->setOpacity((uint8_t)std::clamp(v.intVal, 0, 255));
        break;
    case TimelineProperty::ZOrder:
        n->setLocalZOrder(v.intVal);
        break;
    case TimelineProperty::VisibleForFrame:
        n->setVisible(v.boolVal);
        break;
    case TimelineProperty::FileData:
    case TimelineProperty::InnerAction:
    case TimelineProperty::Event:
    case TimelineProperty::Unknown:
        // Not handled here. FileData (texture swap) is M5; events
        // fire via fireEventKeyframesIfCrossed.
        break;
    }
}

}  // namespace


// =====================================================================
// Range bounds

int Editor::timelineRangeStart() const
{
    if (!_doc) return 0;
    if (_tlAnimName.empty()) return 0;
    auto list = animationListOf(*_doc);
    if (!list.valid()) return 0;
    for (auto& r : list.ranges())
        if (r.name == _tlAnimName) return r.startIndex;
    return 0;
}

int Editor::timelineRangeEnd() const
{
    if (!_doc) return 0;
    auto anim = animationOf(*_doc);
    if (!anim.valid()) return 0;
    if (_tlAnimName.empty()) return anim.duration();
    auto list = animationListOf(*_doc);
    if (list.valid())
    {
        for (auto& r : list.ranges())
            if (r.name == _tlAnimName) return r.endIndex;
    }
    return anim.duration();
}


// =====================================================================
// Per-frame tick

void Editor::tickTimelinePlayback()
{
    if (!_tlPlaying)
    {
        // Reset the time accumulator so a fresh play() doesn't jump
        // forward by however long the editor has been idle.
        _tlLastTickTime = 0.0;
        _tlAccumFrames  = 0.0;
        return;
    }

    const int rangeStart = timelineRangeStart();
    const int rangeEnd   = timelineRangeEnd();
    if (rangeEnd <= rangeStart)
    {
        _tlPlaying = false;
        return;
    }

    const double now = ax::utils::gettime();
    if (_tlLastTickTime > 0.0)
    {
        const double dt = std::max(0.0, now - _tlLastTickTime);
        _tlAccumFrames += dt * std::max(1, _tlFps);
        const int whole = (int)_tlAccumFrames;
        if (whole > 0)
        {
            _tlAccumFrames -= whole;
            int target = _tlFrame + whole;
            if (target > rangeEnd)
            {
                if (_tlLoop)
                {
                    const int span = rangeEnd - rangeStart + 1;
                    target = rangeStart + ((target - rangeStart) % span);
                }
                else
                {
                    target = rangeEnd;
                    _tlPlaying = false;
                }
            }
            _tlFrame = target;
        }
    }
    _tlLastTickTime = now;
}


// =====================================================================
// Evaluator — apply timeline values to axmol nodes

void Editor::evaluateTimelineAtCurrentFrame()
{
    if (!_doc) return;
    auto anim = animationOf(*_doc);
    if (!anim.valid()) return;

    // Idle gate: when nothing has changed since the last apply, skip
    // the per-frame stomp on the live ax tree so the user's static
    // Properties-panel edits actually stick in Edit mode. The
    // evaluator still runs every render while playing, and runs once
    // when the playhead moves (scrub or click-keyframe).
    if (!_tlPlaying && _tlFrame == _tlEvaluatorAppliedFrame) return;
    _tlEvaluatorAppliedFrame = _tlFrame;

    // Fast actionTag → ax::Node* lookup for this evaluation pass.
    // The ax↔csd map is rebuilt by rebuildAxMapsAndSync so it stays
    // in sync with the live tree; we just project ActionTag onto it.
    std::unordered_map<int, ax::Node*> axByTag;
    axByTag.reserve(_axToCsd.size());
    for (auto& kv : _axToCsd)
    {
        if (auto t = kv.second.actionTag())
            axByTag[*t] = kv.first;
    }

    // Pre-pass: a node loaded with VisibleForFrame="False" arrives
    // setVisible(false) from CSLoader, the cocos convention being
    // "off until a Timeline says otherwise". When the user has
    // authored animation tracks for that node but no explicit
    // VisibleForFrame timeline, the editor preview should reveal
    // it — otherwise their work is invisible. So: any actionTag
    // touched by a non-Visible animation track gets force-shown,
    // unless a VisibleForFrame timeline exists (in which case its
    // own per-frame value wins later in the per-track loop).
    std::unordered_set<int> animatedTags;
    std::unordered_set<int> hasVisibleTimeline;
    for (auto tl : anim.timelines())
    {
        const TimelineProperty p = tl.property();
        if (p == TimelineProperty::Event ||
            p == TimelineProperty::Unknown) continue;
        animatedTags.insert(tl.actionTag());
        if (p == TimelineProperty::VisibleForFrame)
            hasVisibleTimeline.insert(tl.actionTag());
    }
    for (int tag : animatedTags)
    {
        if (hasVisibleTimeline.count(tag)) continue;
        auto it = axByTag.find(tag);
        if (it != axByTag.end()) it->second->setVisible(true);
    }

    for (auto tl : anim.timelines())
    {
        const TimelineProperty prop = tl.property();
        // Event timelines fire instead of drive — handled elsewhere.
        if (prop == TimelineProperty::Event ||
            prop == TimelineProperty::Unknown)
            continue;

        auto axIt = axByTag.find(tl.actionTag());
        if (axIt == axByTag.end()) continue;
        ax::Node* axNode = axIt->second;

        auto kfBefore = tl.frameAtOrBefore(_tlFrame);
        if (!kfBefore.valid()) continue;
        auto kfAfter  = tl.frameAfter(_tlFrame);

        KeyValue value;
        if (!kfAfter.valid() || !kfBefore.tween() ||
            kfBefore.frameIndex() == _tlFrame)
        {
            value = kfBefore.read(prop);
        }
        else
        {
            const int idxA = kfBefore.frameIndex();
            const int idxB = kfAfter.frameIndex();
            const float linT = (idxB > idxA)
                ? float(_tlFrame - idxA) / float(idxB - idxA)
                : 0.0f;
            const int easeId = kfBefore.easingType();
            const auto easeType =
                (easeId >= 0 && easeId < ax::tweenfunc::TWEEN_EASING_MAX)
                ? static_cast<ax::tweenfunc::TweenType>(easeId)
                : ax::tweenfunc::Linear;
            const float t = ax::tweenfunc::tweenTo(
                std::clamp(linT, 0.0f, 1.0f), easeType, nullptr);
            const auto a = kfBefore.read(prop);
            const auto b = kfAfter.read(prop);
            value = lerpKeyValue(prop, a, b, t);
        }

        applyToAxNode(prop, value, axNode);
    }
}


// =====================================================================
// Event firing — once per forward crossing

void Editor::fireEventKeyframesIfCrossed()
{
    if (!_doc) return;
    auto anim = animationOf(*_doc);
    if (!anim.valid()) return;

    // Backward seek: reset the marker so future forward play picks up
    // events again, but don't fire on the seek itself (would surprise
    // users scrubbing through with the mouse).
    if (_tlLastEvaluatedFrame > _tlFrame)
    {
        _tlLastEvaluatedFrame = _tlFrame;
        return;
    }
    const int from = _tlLastEvaluatedFrame;
    const int to   = _tlFrame;
    _tlLastEvaluatedFrame = _tlFrame;
    if (from == to) return;

    // A goto_stop / goto_play handler may rewrite _tlFrame mid-pass;
    // we capture the events to fire first, then process them. Avoids
    // an event re-firing itself when goto jumps the playhead back
    // across its own frame.
    struct Pending { int frame; std::string value; };
    std::vector<Pending> pending;

    for (auto tl : anim.timelines())
    {
        if (tl.property() != TimelineProperty::Event) continue;
        for (auto kf : tl.frames())
        {
            const int idx = kf.frameIndex();
            if (idx > from && idx <= to)
            {
                Pending p;
                p.frame = idx;
                p.value = kf.read(TimelineProperty::Event).strA;
                pending.push_back(std::move(p));
            }
        }
    }
    std::sort(pending.begin(), pending.end(),
              [](const Pending& x, const Pending& y) {
                  return x.frame < y.frame;
              });

    auto parsePrefixedInt = [](const std::string& s, std::string_view prefix,
                               int& out) -> bool {
        if (s.size() < prefix.size()) return false;
        if (std::string_view(s).substr(0, prefix.size()) != prefix) return false;
        const char* p = s.c_str() + prefix.size();
        char* endp = nullptr;
        const long v = std::strtol(p, &endp, 10);
        if (endp == p) return false;
        out = (int)v;
        return true;
    };

    for (const auto& ev : pending)
    {
        int targetFrame = 0;
        if (parsePrefixedInt(ev.value, "goto_stop:", targetFrame))
        {
            _tlFrame   = targetFrame;
            _tlPlaying = false;
            _tlLastEvaluatedFrame = targetFrame;  // suppress re-fire on the seek
            OCS_LOG_INFO("timeline",
                "frame %d: goto_stop %d", ev.frame, targetFrame);
            // Stop processing further pending events — we just paused.
            return;
        }
        if (parsePrefixedInt(ev.value, "goto_play:", targetFrame))
        {
            _tlFrame   = targetFrame;
            _tlPlaying = true;
            _tlLastEvaluatedFrame = targetFrame;
            OCS_LOG_INFO("timeline",
                "frame %d: goto_play %d", ev.frame, targetFrame);
            return;
        }
        // Plain event — log to Messages panel.
        OCS_LOG_INFO("timeline",
            "frame %d: event %s", ev.frame, ev.value.c_str());
    }
}

}  // namespace opencs
