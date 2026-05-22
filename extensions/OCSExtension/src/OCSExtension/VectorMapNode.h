// VectorMapNode — resolution-independent vector renderer for stylized
// game maps (overworlds, level-select, country shapes, vector roads).
//
// Stores a list of `Layer`s. Each layer is a flat list of polygons +
// polylines with fill/stroke style. Rendering goes through axmol's
// DrawNode so the result stays crisp at any zoom level without any
// external SVG/NanoVG dependency.
//
// Data model is intentionally trivial — easy for the editor to
// serialise into a JSON sidecar OR into the CSD XML directly.

#pragma once

#include "2d/Node.h"
#include "2d/DrawNode.h"
#include "cocostudio/WidgetReader/NodeReaderProtocol.h"
#include "cocostudio/WidgetReader/NodeReaderDefine.h"

#include <string>
#include <vector>

namespace ocs::ext
{

/// Single drawing primitive inside a VectorMapNode layer.
struct VectorPrimitive
{
    enum class Kind { Polygon, Polyline, Circle };

    Kind                            kind = Kind::Polygon;
    std::vector<ax::Vec2>           points;     // local coords (design px)
    ax::Color4F                     fill   {1.f, 1.f, 1.f, 1.f};
    ax::Color4F                     stroke {0.f, 0.f, 0.f, 1.f};
    float                           strokeWidth = 1.0f;
    // Circle-specific: points[0] = centre, this = radius.
    float                           radius = 0.0f;
};

/// One keyframe row in a layer-level animation. `attr` names the
/// targeted primitive attribute (Fill / Stroke / StrokeWidth /
/// Radius / X / Y / Points). Values are stored as strings in the
/// same wire format as the static XML so the parser stays unified.
struct VectorKeyframe
{
    int         frame   = 0;
    int         primIdx = 0;   // index into VectorLayer::primitives
    std::string attr;
    std::string value;
};

struct VectorAnimation
{
    int  duration = 60;          // total frame count
    int  fps      = 30;          // playback rate
    bool loop     = true;
    std::vector<VectorKeyframe> keyframes;
};

struct VectorLayer
{
    std::string                     name;
    bool                            visible = true;
    std::vector<VectorPrimitive>    primitives;
    VectorAnimation                 animation;
};

class VectorMapNode : public ax::Node
{
public:
    static VectorMapNode* create();

    /// Replace the layer list and re-render. Cheap — DrawNode clears
    /// and re-emits its triangle batch.
    void setLayers(std::vector<VectorLayer> layers);
    const std::vector<VectorLayer>& layers() const { return _layers; }

    /// Load layers from a JSON sidecar at `path`. Returns true on
    /// success; on failure leaves the existing layer list intact and
    /// reports via the log.
    bool loadFromJsonFile(const std::string& path);

    /// Pause / resume layer animations + reset the playhead.
    void setPlaying(bool p) { _playing = p; }
    void setFrame(int f);
    int  frame() const { return _frameInt; }

protected:
    VectorMapNode();
    bool init() override;
    void rebuild();
    void renderLayers(const std::vector<VectorLayer>& layers);
    /// Advance the playhead by `dt` seconds and re-sample animated
    /// layers into the working primitives list before the next
    /// rebuild. Snapshots the static `_layers` data so successive
    /// animation ticks always interpolate from the authored
    /// keyframes — never from the last frame's mutated copy.
    void tick(float dt);

private:
    std::vector<VectorLayer>        _layers;
    ax::DrawNode*                   _draw = nullptr;
    // Animation state. Phase is the wall-clock playhead in seconds;
    // frame is the integer frame index used for keyframe sampling.
    bool   _playing  = true;
    float  _phase    = 0.0f;
    int    _frameInt = 0;
};

/// CSLoader-side reader. Registered by `ocs::ext::registerAll()`.
class VectorMapReader : public ax::Object,
                       public cocostudio::NodeReaderProtocol
{
    DECLARE_CLASS_NODE_READER_INFO
public:
    static VectorMapReader* getInstance();
    static void destroyInstance();

    // NodeReaderProtocol: build a ax::Node* from flatbuffers data.
    // For first pass we instantiate an empty VectorMapNode and expect
    // the data to come from a sidecar JSON file referenced via the
    // generic UserData / FileData on the parent node. Refined as the
    // flatbuffers schema gets a dedicated VectorMapOptions table.
    ax::Node* createNodeWithFlatBuffers(const flatbuffers::Table* nodeOptions) override;

    // No per-node properties to set yet — the empty VectorMapNode is
    // populated by game code post-create via setLayers(). Required
    // overrides to satisfy the abstract base.
    void setPropsWithFlatBuffers(ax::Node* /*node*/,
                                 const flatbuffers::Table* /*nodeOptions*/) override {}
    // Returns an empty flatbuffers offset — VectorMap-specific
    // schema isn't bolted onto the .csb format yet. Once the editor
    // can encode layer data into the flatbuffers stream, we'll
    // serialise it here and parse the matching table in
    // createNodeWithFlatBuffers.
    flatbuffers::Offset<flatbuffers::Table> createOptionsWithFlatBuffers(
        pugi::xml_node /*objectData*/,
        flatbuffers::FlatBufferBuilder* /*builder*/) override
    { return flatbuffers::Offset<flatbuffers::Table>(); }

private:
    VectorMapReader()  = default;
    ~VectorMapReader() = default;
    static VectorMapReader* _instance;
};

}  // namespace ocs::ext
