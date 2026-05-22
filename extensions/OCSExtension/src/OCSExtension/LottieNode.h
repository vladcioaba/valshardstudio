// LottieNode — placeholder Lottie animation node.
//
// Authored in OCS as `LottieObjectData`. Carries a `.json` path
// attribute (the After-Effects → Bodymovin export). The runtime
// shell here keeps the asset hooked into the CSLoader flow + the
// editor canvas (it just renders an empty Node + a debug label
// for now); plugging an actual renderer (e.g. Samsung's rlottie)
// is a one-day port that drops into `tick()` below to rasterise
// each frame into a Texture2D.
//
// Why ship the stub today: it lets designers reference Lottie
// JSONs in scenes + binds the ctype to a stable runtime class
// so binding tables ride alongside without per-asset code edits.
// rlottie integration ships as a follow-up that swaps the body
// of `tick()` without changing the public surface here.

#pragma once

#include "2d/Node.h"
#include "cocostudio/WidgetReader/NodeReaderProtocol.h"
#include "cocostudio/WidgetReader/NodeReaderDefine.h"

#include <string>

namespace ocs::ext
{

class LottieNode : public ax::Node
{
public:
    static LottieNode* create();

    /// Set the source .json path. Path is resolved through axmol
    /// FileUtils so packaged resources and filesystem paths both
    /// work. Currently stores the path; no animation runs until
    /// rlottie is wired into `tick()` below.
    void setJsonPath(const std::string& path);
    const std::string& jsonPath() const { return _jsonPath; }

    /// Playback loop control. Honoured once a renderer is wired.
    void setLoop(bool loop) { _loop = loop; }
    void setFps (int  fps)  { _fps  = fps;  }

protected:
    LottieNode() = default;
    bool init();

    /// Per-frame tick. Stub: increments the playhead. Replace the
    /// body with rlottie::Animation::renderSync into an upload
    /// path that swaps the Texture2D when wiring real playback.
    void tick(float dt);

private:
    std::string _jsonPath;
    bool        _loop = true;
    int         _fps  = 60;
    float       _phase = 0.0f;  // seconds since loop start
};

class LottieReader : public ax::Object,
                    public cocostudio::NodeReaderProtocol
{
    DECLARE_CLASS_NODE_READER_INFO
public:
    static LottieReader* getInstance();
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
    LottieReader()  = default;
    ~LottieReader() = default;
    static LottieReader* _instance;
};

}  // namespace ocs::ext
