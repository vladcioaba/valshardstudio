#include "OCSExtension/LottieNode.h"

#include "axmol.h"
#include "platform/FileUtils.h"

namespace ocs::ext
{

LottieNode* LottieNode::create()
{
    auto* n = new LottieNode();
    if (n->init()) { n->autorelease(); return n; }
    delete n;
    return nullptr;
}

bool LottieNode::init()
{
    if (!ax::Node::init()) return false;
    setContentSize(ax::Size(64, 64));
    schedule([this](float dt) { tick(dt); }, "ocs_lottie_tick");
    return true;
}

void LottieNode::setJsonPath(const std::string& path)
{
    _jsonPath = path;
    _phase = 0.0f;
    // The real renderer (rlottie) would parse the JSON here. The
    // stub just confirms the file resolves so misconfigured assets
    // are caught at load instead of on first frame.
    const std::string full =
        ax::FileUtils::getInstance()->fullPathForFilename(path);
    if (full.empty())
        AXLOGW("[ocs::ext] LottieNode: file not found: {}", path);
    else
        AXLOGI("[ocs::ext] LottieNode stub: loaded {} (renderer TODO)",
               full);
}

void LottieNode::tick(float dt)
{
    if (_jsonPath.empty()) return;
    _phase += dt;
    // TODO: rlottie::Animation::renderSync(frame, surface) + upload
    // to a Texture2D + setTexture(...). For now, only the playhead
    // advances so swap-in of a real renderer is a body-only change.
}

// ---- Reader --------------------------------------------------------------

IMPLEMENT_CLASS_NODE_READER_INFO(LottieReader)
LottieReader* LottieReader::_instance = nullptr;

LottieReader* LottieReader::getInstance()
{
    if (!_instance) _instance = new LottieReader();
    return _instance;
}

void LottieReader::destroyInstance()
{
    delete _instance; _instance = nullptr;
}

ax::Node* LottieReader::createNodeWithFlatBuffers(
    const flatbuffers::Table* /*nodeOptions*/)
{
    return LottieNode::create();
}

}  // namespace ocs::ext
