// Timeline / keyframe wrappers over the existing pugi::xml document.
//
// Cocos Studio stores per-property animation tracks inside a single
// <Animation> element that is a sibling of the scene's root <ObjectData>.
// Each <Timeline ActionTag="<int>" Property="Position|Scale|...">
// contains zero or more typed frame children:
//
//   <Timeline ActionTag="123" Property="Position">
//     <PointFrame FrameIndex="0" X="100" Y="200">
//       <EasingData Type="0"/>
//     </PointFrame>
//     <PointFrame FrameIndex="30" Tween="False" X="200" Y="200"/>
//   </Timeline>
//
// Frames bind to scene nodes via ActionTag, which matches each
// <AbstractNodeData ActionTag="..."> in the tree.
//
// Mirroring CsdModel's design, the wrappers below are thin views over
// pugi::xml_node — mutations write straight to XML so save round-trip
// is automatic. No parallel parsed data structure to keep in sync.

#pragma once

#include <pugixml.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace opencs
{

class CsdDocument;

/// Properties cocos studio stores per-frame. The XML strings are
/// fixed by the format; a few have surprising mappings (RotationSkew
/// uses ScaleFrame entries with X/Y angles, not ScaleFrame value).
enum class TimelineProperty
{
    Unknown,
    Position,           // PointFrame  X Y
    Scale,              // ScaleFrame  X Y
    RotationSkew,       // ScaleFrame  X Y  (skewed rotation, degrees)
    Skew,               // ScaleFrame  X Y
    AnchorPoint,        // PointFrame  X Y
    CColor,             // ColorFrame  Alpha + <Color A R G B/>
    Alpha,              // IntFrame    Value (0..255)
    FileData,           // TextureFrame Path Plist Type
    VisibleForFrame,    // BoolFrame   Value
    ZOrder,             // IntFrame    Value
    InnerAction,        // InnerActionFrame
    Event,              // EventFrame  Value
};

/// XML name as it appears in the `Property` attribute (e.g. "Position").
const char* timelinePropertyXmlName(TimelineProperty);
TimelineProperty timelinePropertyFromXml(std::string_view);

/// Short human label for the Timeline panel ("Position", "Color", "Visible").
const char* timelinePropertyDisplay(TimelineProperty);

/// Tagged value carried by a single keyframe. Layout chosen so the
/// struct is cheap to copy / pass around without unique_ptr indirection.
struct KeyValue
{
    enum class Kind
    {
        None,
        Vec2,           // Position / Scale / RotationSkew / Skew / AnchorPoint
        Color,          // CColor (rgba + Alpha attribute on the frame)
        Int,            // Alpha / ZOrder
        Bool,           // VisibleForFrame
        File,           // FileData (texture frame)
        Inner,          // InnerActionFrame
        Event,          // EventFrame
    } kind = Kind::None;

    float vx = 0.0f, vy = 0.0f;            // Vec2
    int   r = 255, g = 255, b = 255, a = 255; // Color rgba
    int   alphaAttr = 255;                  // ColorFrame's Alpha="..." attribute
    int   intVal = 0;                       // Int / ZOrder
    bool  boolVal = true;                   // Bool

    // File: Type+Path+Plist; Event: Value in strA; Inner: action+ranges in strA/B/C
    std::string strA, strB, strC;
};

/// Single <PointFrame|ScaleFrame|...> element.
class KeyframeRef
{
public:
    KeyframeRef() = default;
    explicit KeyframeRef(pugi::xml_node el) : _el(el) {}

    pugi::xml_node element() const noexcept { return _el; }
    bool valid() const noexcept { return !_el.empty(); }

    int  frameIndex() const;
    void setFrameIndex(int);

    /// Cocos writes Tween="False" only on the last frame of a track
    /// (or any frame that should hold its value). Default is true.
    bool tween() const;
    void setTween(bool);

    /// EasingData/@Type — int enum matching ax::tweenfunc::TweenType
    /// (0 == Linear). -1 means CUSTOM_EASING with <Points> children;
    /// we read the type but leave Points untouched on round-trip.
    int  easingType() const;
    void setEasingType(int);

    /// Type-aware read/write. Caller passes the owning timeline's
    /// property so the wrapper knows which attributes to look at.
    KeyValue read(TimelineProperty p) const;
    void     write(TimelineProperty p, const KeyValue&);

    /// XML tag of the underlying frame element ("PointFrame", etc.).
    std::string tag() const;

private:
    pugi::xml_node _el;
};

/// Single <Timeline> element — one track for a (node, property) pair.
class TimelineRef
{
public:
    TimelineRef() = default;
    explicit TimelineRef(pugi::xml_node el) : _el(el) {}

    pugi::xml_node element() const noexcept { return _el; }
    bool valid() const noexcept { return !_el.empty(); }

    int actionTag() const;
    TimelineProperty property() const;
    std::string propertyName() const;     // raw XML string, may be unrecognised

    std::vector<KeyframeRef> frames() const;
    /// First keyframe at or before `frameIndex`, or invalid if none.
    KeyframeRef frameAtOrBefore(int frameIndex) const;
    /// First keyframe strictly after `frameIndex`, or invalid if none.
    KeyframeRef frameAfter(int frameIndex) const;

    /// Append a new frame element of the right type for this timeline's
    /// property, with FrameIndex=`idx` and the given value. Returns the
    /// new ref.
    KeyframeRef appendFrame(int idx, const KeyValue&);

    /// Remove a frame by element identity. No-op if the frame is not
    /// a child of this timeline.
    void removeFrame(KeyframeRef);

private:
    pugi::xml_node _el;
};

/// One <AnimationInfo> entry inside <AnimationList>. Cocos uses these
/// to publish named ranges of the master timeline ("idle", "run", ...).
struct AnimationRange
{
    std::string name;
    int startIndex = 0;
    int endIndex   = 0;
};

/// Top-level <Animation> wrapper.
class AnimationRef
{
public:
    AnimationRef() = default;
    explicit AnimationRef(pugi::xml_node el) : _el(el) {}

    pugi::xml_node element() const noexcept { return _el; }
    bool valid() const noexcept { return !_el.empty(); }

    int   duration() const;     // total frames
    void  setDuration(int);
    float speed() const;        // playback multiplier (default 1.0)
    void  setSpeed(float);

    std::vector<TimelineRef> timelines() const;
    TimelineRef findTimeline(int actionTag, TimelineProperty) const;

    /// Append a fresh <Timeline> for the given (actionTag, property).
    TimelineRef createTimeline(int actionTag, TimelineProperty);
    void removeTimeline(TimelineRef);

private:
    pugi::xml_node _el;
};

/// <AnimationList> wrapper — read-only for v1.
class AnimationListRef
{
public:
    AnimationListRef() = default;
    explicit AnimationListRef(pugi::xml_node el) : _el(el) {}

    pugi::xml_node element() const noexcept { return _el; }
    bool valid() const noexcept { return !_el.empty(); }

    std::vector<AnimationRange> ranges() const;

private:
    pugi::xml_node _el;
};

/// Locate <Animation> / <AnimationList> in the document. They live as
/// siblings of the root <ObjectData> under Content/Content. Returns
/// invalid refs if not present.
AnimationRef     animationOf(const CsdDocument&);
AnimationListRef animationListOf(const CsdDocument&);

}  // namespace opencs
