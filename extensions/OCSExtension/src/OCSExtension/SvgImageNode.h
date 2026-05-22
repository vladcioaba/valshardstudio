// SvgImageNode — runtime SVG rendering via rsvg-convert (system CLI).
//
// The runtime stays SVG-blind otherwise; this node owns a path to a
// `.svg` and rasterises to a sibling `.png` on demand. Re-rasterises
// when `setTargetSize` is called (e.g. when the parent scales the
// content), so the texture stays crisp at whatever resolution the
// scene currently needs.
//
// This is a runtime fallback for cases where the editor's bake-time
// `svg-bake` is not enough — typically infinite-zoom UIs or maps
// where the on-screen size is only known at runtime. Most game UIs
// should still prefer the bake-time path (smaller binary, no fork-
// exec at load).
//
// Dependency: `/usr/bin/rsvg-convert` must be on PATH at runtime.
// Falls back to a magenta error tile when the CLI is missing or the
// raster fails, so misconfigured installs surface visibly instead
// of silently rendering nothing.

#pragma once

#include "2d/Sprite.h"
#include "cocostudio/WidgetReader/NodeReaderProtocol.h"
#include "cocostudio/WidgetReader/NodeReaderDefine.h"

#include <string>

namespace ocs::ext
{

class SvgImageNode : public ax::Sprite
{
public:
    static SvgImageNode* create();

    /// Set the source `.svg` path (absolute or relative to the
    /// FileUtils search paths). Rasterises immediately at the
    /// current `_targetW/H` (defaults to the SVG viewBox size).
    void setSvgPath(const std::string& path);
    const std::string& svgPath() const { return _svgPath; }

    /// Re-rasterise at a new target size. No-op when neither
    /// dimension changes by more than 1 px (avoids thrash on
    /// fractional scale updates).
    void setTargetSize(int w, int h);

protected:
    SvgImageNode() = default;
    bool rasterize();
    bool initWithErrorTile();

private:
    std::string _svgPath;
    int         _targetW = 0;
    int         _targetH = 0;
};

class SvgImageReader : public ax::Object,
                      public cocostudio::NodeReaderProtocol
{
    DECLARE_CLASS_NODE_READER_INFO
public:
    static SvgImageReader* getInstance();
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
    SvgImageReader()  = default;
    ~SvgImageReader() = default;
    static SvgImageReader* _instance;
};

}  // namespace ocs::ext
