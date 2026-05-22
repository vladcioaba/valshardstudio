#include "OCSExtension/SvgImageNode.h"

#include "axmol.h"
#include "platform/FileUtils.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace ocs::ext
{

namespace
{
std::string findRsvg()
{
    namespace fs = std::filesystem;
    for (const char* p : {"/usr/local/bin/rsvg-convert",
                          "/opt/homebrew/bin/rsvg-convert",
                          "/usr/bin/rsvg-convert"})
    {
        std::error_code ec;
        if (fs::exists(p, ec)) return p;
    }
    return {};
}
}  // namespace

SvgImageNode* SvgImageNode::create()
{
    auto* n = new SvgImageNode();
    if (n->initWithErrorTile()) { n->autorelease(); return n; }
    delete n;
    return nullptr;
}

bool SvgImageNode::initWithErrorTile()
{
    // 16x16 magenta tile so a broken raster surfaces immediately
    // rather than rendering nothing. Replaced by `rasterize()` once
    // a real SVG path is set.
    const int W = 16, H = 16;
    std::vector<uint8_t> px(W * H * 4);
    for (int i = 0; i < W * H; ++i)
    {
        px[i * 4 + 0] = 255;
        px[i * 4 + 1] = 0;
        px[i * 4 + 2] = 255;
        px[i * 4 + 3] = 255;
    }
    auto* img = new ax::Image();
    img->initWithRawData(px.data(), px.size(), W, H, 8);
    auto* tex = new ax::Texture2D();
    tex->initWithImage(img);
    img->release();
    const bool ok = ax::Sprite::initWithTexture(tex);
    tex->release();
    return ok;
}

void SvgImageNode::setSvgPath(const std::string& path)
{
    _svgPath = path;
    rasterize();
}

void SvgImageNode::setTargetSize(int w, int h)
{
    if (std::abs(w - _targetW) <= 1 && std::abs(h - _targetH) <= 1) return;
    _targetW = w; _targetH = h;
    rasterize();
}

bool SvgImageNode::rasterize()
{
    if (_svgPath.empty()) return false;
    namespace fs = std::filesystem;
    const std::string tool = findRsvg();
    if (tool.empty())
    {
        AXLOGW("[ocs::ext] SvgImageNode: rsvg-convert missing; "
               "install via `brew install librsvg`");
        return false;
    }
    // Resolve `_svgPath` through axmol FileUtils so apk / app-bundle
    // resources work the same as filesystem paths.
    const std::string src =
        ax::FileUtils::getInstance()->fullPathForFilename(_svgPath);
    if (src.empty()) { AXLOGW("[ocs::ext] svg not found: {}", _svgPath); return false; }

    // Stage to a temp PNG; load + delete after.
    char tmp[L_tmpnam];
    std::tmpnam(tmp);
    const std::string dst = std::string(tmp) + ".png";
    char cmd[2048];
    if (_targetW > 0 && _targetH > 0)
        std::snprintf(cmd, sizeof(cmd),
            "'%s' -w %d -h %d '%s' -o '%s' 2>/dev/null",
            tool.c_str(), _targetW, _targetH,
            src.c_str(), dst.c_str());
    else
        std::snprintf(cmd, sizeof(cmd),
            "'%s' '%s' -o '%s' 2>/dev/null",
            tool.c_str(), src.c_str(), dst.c_str());
    const int rc = std::system(cmd);
    if (rc != 0 || !fs::exists(dst))
    {
        AXLOGW("[ocs::ext] rsvg-convert failed rc={} src={}", rc, src);
        return false;
    }

    auto* tex = ax::Director::getInstance()
                    ->getTextureCache()->addImage(dst);
    std::error_code ec; fs::remove(dst, ec);
    if (!tex) return false;
    setTexture(tex);
    setTextureRect(ax::Rect(0, 0,
                            tex->getContentSize().width,
                            tex->getContentSize().height));
    return true;
}

// ---- Reader --------------------------------------------------------------

IMPLEMENT_CLASS_NODE_READER_INFO(SvgImageReader)
SvgImageReader* SvgImageReader::_instance = nullptr;

SvgImageReader* SvgImageReader::getInstance()
{
    if (!_instance) _instance = new SvgImageReader();
    return _instance;
}

void SvgImageReader::destroyInstance()
{
    delete _instance; _instance = nullptr;
}

ax::Node* SvgImageReader::createNodeWithFlatBuffers(
    const flatbuffers::Table* /*nodeOptions*/)
{
    // First-pass: empty SvgImageNode. Game code calls setSvgPath
    // after creation, or the editor extends the FlatBuffers schema
    // later so the path travels in the .csb.
    return SvgImageNode::create();
}

}  // namespace ocs::ext
