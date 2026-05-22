#include "Timeline.h"
#include "CsdModel.h"

#include <fmt/format.h>

#include <limits>

#include <algorithm>
#include <cstring>

namespace opencs
{

namespace
{

constexpr const char* kAnimationTag     = "Animation";
constexpr const char* kAnimationListTag = "AnimationList";
constexpr const char* kAnimInfoTag      = "AnimationInfo";
constexpr const char* kTimelineTag      = "Timeline";

/// The inner <Content> sibling that hosts <Animation>, <ObjectData>,
/// <AnimationList>. Mirrors findRootObjectData but returns the
/// container so we can look up siblings.
pugi::xml_node animationContainer(const CsdDocument& doc)
{
    auto gameFile = doc.doc.child("GameFile");
    if (!gameFile) return {};
    auto outer = gameFile.child("Content");
    if (!outer) return {};
    auto inner = outer.child("Content");
    return inner ? inner : outer;
}

/// "%.4f" formatting that matches Cocos Studio's editor output —
/// trailing zeros preserved so diffs stay tight against round-trips
/// from the upstream editor.
std::string fmt4(float v)
{
    return fmt::format("{:.4f}", v);
}

/// Frame XML tag for a given property. Cocos uses ScaleFrame for
/// RotationSkew/Skew (X = X-axis angle, Y = Y-axis angle), so the
/// mapping is value-shaped, not name-shaped.
const char* frameTagFor(TimelineProperty p)
{
    switch (p)
    {
    case TimelineProperty::Position:        return "PointFrame";
    case TimelineProperty::AnchorPoint:     return "PointFrame";
    case TimelineProperty::Scale:           return "ScaleFrame";
    case TimelineProperty::RotationSkew:    return "ScaleFrame";
    case TimelineProperty::Skew:            return "ScaleFrame";
    case TimelineProperty::CColor:          return "ColorFrame";
    case TimelineProperty::Alpha:           return "IntFrame";
    case TimelineProperty::ZOrder:          return "IntFrame";
    case TimelineProperty::VisibleForFrame: return "BoolFrame";
    case TimelineProperty::FileData:        return "TextureFrame";
    case TimelineProperty::InnerAction:     return "InnerActionFrame";
    case TimelineProperty::Event:           return "EventFrame";
    case TimelineProperty::Unknown:         return nullptr;
    }
    return nullptr;
}

}  // namespace


// =====================================================================
// Property string conversion

const char* timelinePropertyXmlName(TimelineProperty p)
{
    switch (p)
    {
    case TimelineProperty::Position:        return "Position";
    case TimelineProperty::Scale:           return "Scale";
    case TimelineProperty::RotationSkew:    return "RotationSkew";
    case TimelineProperty::Skew:            return "Skew";
    case TimelineProperty::AnchorPoint:     return "AnchorPoint";
    case TimelineProperty::CColor:          return "CColor";
    case TimelineProperty::Alpha:           return "Alpha";
    case TimelineProperty::FileData:        return "FileData";
    case TimelineProperty::VisibleForFrame: return "VisibleForFrame";
    case TimelineProperty::ZOrder:          return "ZOrder";
    case TimelineProperty::InnerAction:     return "InnerAction";
    case TimelineProperty::Event:           return "Event";
    case TimelineProperty::Unknown:         return "";
    }
    return "";
}

TimelineProperty timelinePropertyFromXml(std::string_view s)
{
    if (s == "Position")        return TimelineProperty::Position;
    if (s == "Scale")           return TimelineProperty::Scale;
    if (s == "RotationSkew")    return TimelineProperty::RotationSkew;
    if (s == "Skew")            return TimelineProperty::Skew;
    if (s == "AnchorPoint")     return TimelineProperty::AnchorPoint;
    if (s == "CColor")          return TimelineProperty::CColor;
    if (s == "Alpha")           return TimelineProperty::Alpha;
    if (s == "FileData")        return TimelineProperty::FileData;
    if (s == "VisibleForFrame") return TimelineProperty::VisibleForFrame;
    if (s == "ZOrder")          return TimelineProperty::ZOrder;
    if (s == "InnerAction")     return TimelineProperty::InnerAction;
    if (s == "Event")           return TimelineProperty::Event;
    return TimelineProperty::Unknown;
}

const char* timelinePropertyDisplay(TimelineProperty p)
{
    switch (p)
    {
    case TimelineProperty::Position:        return "Position";
    case TimelineProperty::Scale:           return "Scale";
    case TimelineProperty::RotationSkew:    return "Rotation";
    case TimelineProperty::Skew:            return "Skew";
    case TimelineProperty::AnchorPoint:     return "Anchor";
    case TimelineProperty::CColor:          return "Color";
    case TimelineProperty::Alpha:           return "Alpha";
    case TimelineProperty::FileData:        return "Texture";
    case TimelineProperty::VisibleForFrame: return "Visible";
    case TimelineProperty::ZOrder:          return "ZOrder";
    case TimelineProperty::InnerAction:     return "Inner";
    case TimelineProperty::Event:           return "Event";
    case TimelineProperty::Unknown:         return "?";
    }
    return "?";
}


// =====================================================================
// KeyframeRef

int KeyframeRef::frameIndex() const
{
    return _el.attribute("FrameIndex").as_int(0);
}

void KeyframeRef::setFrameIndex(int idx)
{
    auto a = _el.attribute("FrameIndex");
    if (!a) a = _el.append_attribute("FrameIndex");
    a.set_value(idx);
}

bool KeyframeRef::tween() const
{
    auto a = _el.attribute("Tween");
    if (!a) return true;  // default
    const std::string s(a.as_string(""));
    return !(s == "False" || s == "false" || s == "0");
}

void KeyframeRef::setTween(bool v)
{
    if (v)
    {
        // Default — drop the attribute so output matches cocos style.
        _el.remove_attribute("Tween");
        return;
    }
    auto a = _el.attribute("Tween");
    if (!a) a = _el.append_attribute("Tween");
    a.set_value("False");
}

int KeyframeRef::easingType() const
{
    auto e = _el.child("EasingData");
    if (!e) return 0;  // implicit Linear
    return e.attribute("Type").as_int(0);
}

void KeyframeRef::setEasingType(int t)
{
    auto e = _el.child("EasingData");
    if (!e) e = _el.append_child("EasingData");
    auto a = e.attribute("Type");
    if (!a) a = e.append_attribute("Type");
    a.set_value(t);
}

std::string KeyframeRef::tag() const
{
    return std::string(_el.name());
}

KeyValue KeyframeRef::read(TimelineProperty p) const
{
    KeyValue kv;
    switch (p)
    {
    case TimelineProperty::Position:
    case TimelineProperty::AnchorPoint:
    case TimelineProperty::Scale:
    case TimelineProperty::RotationSkew:
    case TimelineProperty::Skew:
        kv.kind = KeyValue::Kind::Vec2;
        kv.vx = _el.attribute("X").as_float(0.0f);
        kv.vy = _el.attribute("Y").as_float(0.0f);
        break;
    case TimelineProperty::CColor:
    {
        kv.kind = KeyValue::Kind::Color;
        kv.alphaAttr = _el.attribute("Alpha").as_int(255);
        if (auto c = _el.child("Color"))
        {
            kv.a = c.attribute("A").as_int(255);
            kv.r = c.attribute("R").as_int(255);
            kv.g = c.attribute("G").as_int(255);
            kv.b = c.attribute("B").as_int(255);
        }
        break;
    }
    case TimelineProperty::Alpha:
    case TimelineProperty::ZOrder:
        kv.kind = KeyValue::Kind::Int;
        kv.intVal = _el.attribute("Value").as_int(0);
        break;
    case TimelineProperty::VisibleForFrame:
    {
        kv.kind = KeyValue::Kind::Bool;
        const std::string s(_el.attribute("Value").as_string("True"));
        kv.boolVal = !(s == "False" || s == "false" || s == "0");
        break;
    }
    case TimelineProperty::FileData:
        kv.kind = KeyValue::Kind::File;
        kv.strA = _el.attribute("Path").as_string("");
        kv.strB = _el.attribute("Plist").as_string("");
        kv.strC = _el.attribute("Type").as_string("");
        break;
    case TimelineProperty::InnerAction:
        kv.kind = KeyValue::Kind::Inner;
        kv.strA = _el.attribute("InnerActionType").as_string("");
        kv.intVal = _el.attribute("StartFrameIndex").as_int(0);
        kv.vx = _el.attribute("Speed").as_float(1.0f);
        break;
    case TimelineProperty::Event:
        kv.kind = KeyValue::Kind::Event;
        kv.strA = _el.attribute("Value").as_string("");
        break;
    case TimelineProperty::Unknown:
        break;
    }
    return kv;
}

void KeyframeRef::write(TimelineProperty p, const KeyValue& kv)
{
    auto setAttr = [&](const char* name, std::string_view value) {
        auto a = _el.attribute(name);
        if (!a) a = _el.append_attribute(name);
        a.set_value(std::string(value).c_str());
    };
    auto setAttrInt = [&](const char* name, int v) {
        auto a = _el.attribute(name);
        if (!a) a = _el.append_attribute(name);
        a.set_value(v);
    };

    switch (p)
    {
    case TimelineProperty::Position:
    case TimelineProperty::AnchorPoint:
    case TimelineProperty::Scale:
    case TimelineProperty::RotationSkew:
    case TimelineProperty::Skew:
        setAttr("X", fmt4(kv.vx));
        setAttr("Y", fmt4(kv.vy));
        break;
    case TimelineProperty::CColor:
    {
        setAttrInt("Alpha", kv.alphaAttr);
        auto c = _el.child("Color");
        if (!c) c = _el.append_child("Color");
        auto upsert = [&](const char* name, int v) {
            auto a = c.attribute(name);
            if (!a) a = c.append_attribute(name);
            a.set_value(v);
        };
        upsert("A", kv.a);
        upsert("R", kv.r);
        upsert("G", kv.g);
        upsert("B", kv.b);
        break;
    }
    case TimelineProperty::Alpha:
    case TimelineProperty::ZOrder:
        setAttrInt("Value", kv.intVal);
        break;
    case TimelineProperty::VisibleForFrame:
        setAttr("Value", kv.boolVal ? "True" : "False");
        break;
    case TimelineProperty::FileData:
        setAttr("Path",  kv.strA);
        setAttr("Plist", kv.strB);
        setAttr("Type",  kv.strC);
        break;
    case TimelineProperty::InnerAction:
        setAttr("InnerActionType", kv.strA);
        setAttrInt("StartFrameIndex", kv.intVal);
        setAttr("Speed", fmt4(kv.vx));
        break;
    case TimelineProperty::Event:
        setAttr("Value", kv.strA);
        break;
    case TimelineProperty::Unknown:
        break;
    }
}


// =====================================================================
// TimelineRef

int TimelineRef::actionTag() const
{
    return _el.attribute("ActionTag").as_int(0);
}

TimelineProperty TimelineRef::property() const
{
    return timelinePropertyFromXml(_el.attribute("Property").as_string(""));
}

std::string TimelineRef::propertyName() const
{
    return std::string(_el.attribute("Property").as_string(""));
}

std::vector<KeyframeRef> TimelineRef::frames() const
{
    std::vector<KeyframeRef> out;
    for (auto c = _el.first_child(); c; c = c.next_sibling())
    {
        if (c.type() != pugi::node_element) continue;
        // Frames are typed elements; skip anything that's not one
        // (e.g. comments, stray text). The set of valid frame tags
        // ends in "Frame".
        const std::string_view n(c.name());
        if (n.size() < 5) continue;
        if (n.substr(n.size() - 5) != "Frame") continue;
        out.emplace_back(c);
    }
    return out;
}

KeyframeRef TimelineRef::frameAtOrBefore(int frameIndex) const
{
    KeyframeRef best;
    int bestIdx = -1;
    for (auto kf : frames())
    {
        const int idx = kf.frameIndex();
        if (idx <= frameIndex && idx > bestIdx)
        {
            best = kf;
            bestIdx = idx;
        }
    }
    return best;
}

KeyframeRef TimelineRef::frameAfter(int frameIndex) const
{
    KeyframeRef best;
    int bestIdx = std::numeric_limits<int>::max();
    for (auto kf : frames())
    {
        const int idx = kf.frameIndex();
        if (idx > frameIndex && idx < bestIdx)
        {
            best = kf;
            bestIdx = idx;
        }
    }
    return best;
}

KeyframeRef TimelineRef::appendFrame(int idx, const KeyValue& v)
{
    const char* tag = frameTagFor(property());
    if (!tag) return KeyframeRef();
    auto el = _el.append_child(tag);
    KeyframeRef kf(el);
    kf.setFrameIndex(idx);
    kf.write(property(), v);
    // Match cocos's editor format: tweened frames get an explicit
    // <EasingData Type="0" /> child. Non-Event frames default to
    // tweened — Event frames don't carry easing.
    if (property() != TimelineProperty::Event)
        kf.setEasingType(0);
    return kf;
}

void TimelineRef::removeFrame(KeyframeRef kf)
{
    if (!kf.valid()) return;
    _el.remove_child(kf.element());
}


// =====================================================================
// AnimationRef

int AnimationRef::duration() const
{
    return _el.attribute("Duration").as_int(0);
}

void AnimationRef::setDuration(int d)
{
    auto a = _el.attribute("Duration");
    if (!a) a = _el.append_attribute("Duration");
    a.set_value(d);
}

float AnimationRef::speed() const
{
    return _el.attribute("Speed").as_float(1.0f);
}

void AnimationRef::setSpeed(float s)
{
    auto a = _el.attribute("Speed");
    if (!a) a = _el.append_attribute("Speed");
    a.set_value(fmt4(s).c_str());
}

std::vector<TimelineRef> AnimationRef::timelines() const
{
    std::vector<TimelineRef> out;
    for (auto c = _el.child(kTimelineTag); c;
         c = c.next_sibling(kTimelineTag))
    {
        out.emplace_back(c);
    }
    return out;
}

TimelineRef AnimationRef::findTimeline(int tag, TimelineProperty p) const
{
    const std::string_view propStr = timelinePropertyXmlName(p);
    for (auto t : timelines())
    {
        if (t.actionTag() != tag) continue;
        if (std::string_view(
                t.element().attribute("Property").as_string("")) == propStr)
            return t;
    }
    return TimelineRef();
}

TimelineRef AnimationRef::createTimeline(int tag, TimelineProperty p)
{
    auto el = _el.append_child(kTimelineTag);
    el.append_attribute("ActionTag").set_value(tag);
    el.append_attribute("Property").set_value(timelinePropertyXmlName(p));
    return TimelineRef(el);
}

void AnimationRef::removeTimeline(TimelineRef t)
{
    if (!t.valid()) return;
    _el.remove_child(t.element());
}


// =====================================================================
// AnimationListRef

std::vector<AnimationRange> AnimationListRef::ranges() const
{
    std::vector<AnimationRange> out;
    for (auto c = _el.child(kAnimInfoTag); c;
         c = c.next_sibling(kAnimInfoTag))
    {
        AnimationRange r;
        r.name       = c.attribute("Name").as_string("");
        r.startIndex = c.attribute("StartIndex").as_int(0);
        r.endIndex   = c.attribute("EndIndex").as_int(0);
        out.push_back(std::move(r));
    }
    return out;
}


// =====================================================================
// Doc lookup

AnimationRef animationOf(const CsdDocument& doc)
{
    auto c = animationContainer(doc);
    if (!c) return AnimationRef();
    return AnimationRef(c.child(kAnimationTag));
}

AnimationListRef animationListOf(const CsdDocument& doc)
{
    auto c = animationContainer(doc);
    if (!c) return AnimationListRef();
    return AnimationListRef(c.child(kAnimationListTag));
}

}  // namespace opencs
