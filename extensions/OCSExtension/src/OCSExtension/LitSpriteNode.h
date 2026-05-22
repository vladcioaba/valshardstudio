// LitSpriteNode — 2D-lights sprite.
//
// Sprite subclass that accumulates N point lights into its color
// modulation each frame. Lights are simple radial falloff:
//   contribution = color * (1 - clamp(dist / radius, 0, 1))^falloff
// summed across every light + an ambient floor.
//
// Two consumption modes:
//   1. Live  — game code (or the editor) calls setLights() each
//              frame or on input. Cheap when there are few lights;
//              the per-frame CPU cost is N_lights * 1 lookup per
//              sprite (the per-pixel cost stays on the GPU via
//              `setColor` only — we currently approximate with a
//              single uniform color modulation per sprite based on
//              an average sampled at the sprite centre. Per-pixel
//              shader is a v2 upgrade).
//   2. Baked — editor's `bake-lights` RPC walks every LitSprite,
//              composites the lights into a new PNG, swaps the
//              FileData ref. Runtime sees a flat Sprite — zero
//              lighting cost.
//
// XML schema (under the AbstractNodeData):
//   <OCSLights Ambient="60,60,80,255">
//     <Light X="100" Y="80"  Radius="200" Color="255,210,160,255"
//            Falloff="1.5"/>
//   </OCSLights>

#pragma once

#include "2d/Sprite.h"
#include "cocostudio/WidgetReader/NodeReaderProtocol.h"
#include "cocostudio/WidgetReader/NodeReaderDefine.h"

#include <string>
#include <vector>

namespace ocs::ext
{

struct Light2D
{
    float       x = 0, y = 0;
    float       radius = 100.0f;
    ax::Color4F color { 1, 1, 1, 1 };
    float       falloff = 1.0f;   // exponent on the (1 - d/r) term

    // Direction + cone. dirX/dirY are a non-normalised vector
    // pointing FROM the light source INTO the cone axis. Zero
    // vector = omnidirectional (legacy behaviour). ConeDeg is the
    // FULL cone angle in degrees (e.g. 60 = ±30 around the axis).
    // Soft-edge fraction inside which the cone fade-out happens —
    // 0 = hard edge, 1 = entire cone is a falloff ramp.
    float       dirX    = 0.0f;
    float       dirY    = 0.0f;
    float       coneDeg = 360.0f;
    float       coneSoftness = 0.2f;
};

class LitSpriteNode : public ax::Sprite
{
public:
    static LitSpriteNode* create();

    void setLights(std::vector<Light2D> lights);
    const std::vector<Light2D>& lights() const { return _lights; }

    void setAmbient(ax::Color4F c) { _ambient = c; _dirty = true; }
    ax::Color4F ambient() const { return _ambient; }

protected:
    LitSpriteNode() = default;
    bool init() override;
    void recompose();

private:
    std::vector<Light2D> _lights;
    ax::Color4F          _ambient { 0.4f, 0.4f, 0.5f, 1.0f };
    bool                 _dirty   = true;
};

class LitSpriteReader : public ax::Object,
                       public cocostudio::NodeReaderProtocol
{
    DECLARE_CLASS_NODE_READER_INFO
public:
    static LitSpriteReader* getInstance();
    static void destroyInstance();
    ax::Node* createNodeWithFlatBuffers(
        const flatbuffers::Table* nodeOptions) override;
    void setPropsWithFlatBuffers(
        ax::Node* /*node*/,
        const flatbuffers::Table* /*nodeOptions*/) override {}
    flatbuffers::Offset<flatbuffers::Table> createOptionsWithFlatBuffers(
        pugi::xml_node /*objectData*/,
        flatbuffers::FlatBufferBuilder* /*builder*/) override
    { return flatbuffers::Offset<flatbuffers::Table>(); }

private:
    LitSpriteReader()  = default;
    ~LitSpriteReader() = default;
    static LitSpriteReader* _instance;
};

}  // namespace ocs::ext
