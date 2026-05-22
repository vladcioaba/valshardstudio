#include "OCSExtension/LitSpriteNode.h"

#include "axmol.h"

#include <algorithm>
#include <cmath>

namespace ocs::ext
{

LitSpriteNode* LitSpriteNode::create()
{
    auto* n = new LitSpriteNode();
    if (n->init()) { n->autorelease(); return n; }
    delete n;
    return nullptr;
}

bool LitSpriteNode::init()
{
    if (!ax::Sprite::init()) return false;
    // 1x1 white placeholder so the node is non-degenerate before
    // the texture is set. Game code calls setTexture afterwards.
    auto* img = new ax::Image();
    uint8_t white[4] = { 255, 255, 255, 255 };
    img->initWithRawData(white, 4, 1, 1, 8);
    auto* tex = new ax::Texture2D();
    tex->initWithImage(img);
    img->release();
    setTexture(tex);
    setTextureRect(ax::Rect(0, 0, 1, 1));
    tex->release();
    schedule([this](float) { if (_dirty) recompose(); }, "ocs_lit_tick");
    return true;
}

void LitSpriteNode::setLights(std::vector<Light2D> lights)
{
    _lights = std::move(lights);
    _dirty  = true;
}

void LitSpriteNode::recompose()
{
    // Per-sprite color modulation approximation: sample lighting at
    // the sprite centre (parent-local 0,0 → world). For a real
    // per-pixel effect, replace this with a fragment shader that
    // takes the light array as a uniform; this v1 keeps the API
    // identical so the shader upgrade is a body-only change.
    const ax::Vec2 worldC = convertToWorldSpace(
        ax::Vec2(getContentSize().width * 0.5f,
                 getContentSize().height * 0.5f));
    ax::Color4F acc = _ambient;
    for (const auto& L : _lights)
    {
        // Vector FROM light TO sample point.
        const float dx = worldC.x - L.x;
        const float dy = worldC.y - L.y;
        const float d  = std::sqrt(dx * dx + dy * dy);
        if (d >= L.radius) continue;

        // Radial falloff.
        const float radialT = 1.0f - (d / std::max(1.0f, L.radius));
        float w = std::pow(std::clamp(radialT, 0.0f, 1.0f),
                            std::max(0.05f, L.falloff));

        // Cone gate. Skip when the light is omnidirectional (cone
        // ≥ 360°) or when the direction vector is zero. Otherwise
        // compute the angle between the cone axis (L.dirX, L.dirY)
        // and the (dx, dy) vector; outside the cone → contribution 0.
        const float dlen = std::sqrt(L.dirX * L.dirX + L.dirY * L.dirY);
        if (L.coneDeg < 360.0f && dlen > 1e-4f && d > 1e-4f)
        {
            const float ax_ = L.dirX / dlen;
            const float ay_ = L.dirY / dlen;
            const float bx  = dx / d;
            const float by  = dy / d;
            const float cosA = std::clamp(ax_ * bx + ay_ * by, -1.0f, 1.0f);
            const float angle = std::acos(cosA);          // 0..π
            const float halfCone = (L.coneDeg * 0.5f) *
                                   (3.14159265358979323846f / 180.0f);
            if (angle >= halfCone) continue;
            // Optional soft edge inside the cone — fade from full
            // weight at the axis to zero at the edge across the
            // softness band.
            const float soft = std::clamp(L.coneSoftness, 0.0f, 1.0f);
            const float edge0 = halfCone * (1.0f - soft);
            if (soft > 0.0f && angle > edge0)
            {
                const float t = 1.0f - (angle - edge0) / (halfCone - edge0);
                w *= std::clamp(t, 0.0f, 1.0f);
            }
        }

        acc.r += L.color.r * w;
        acc.g += L.color.g * w;
        acc.b += L.color.b * w;
    }
    acc.r = std::min(1.0f, acc.r);
    acc.g = std::min(1.0f, acc.g);
    acc.b = std::min(1.0f, acc.b);
    setColor(ax::Color3B((uint8_t)(acc.r * 255),
                          (uint8_t)(acc.g * 255),
                          (uint8_t)(acc.b * 255)));
    _dirty = false;
}

// ---- Reader --------------------------------------------------------------

IMPLEMENT_CLASS_NODE_READER_INFO(LitSpriteReader)
LitSpriteReader* LitSpriteReader::_instance = nullptr;

LitSpriteReader* LitSpriteReader::getInstance()
{
    if (!_instance) _instance = new LitSpriteReader();
    return _instance;
}

void LitSpriteReader::destroyInstance()
{
    delete _instance; _instance = nullptr;
}

ax::Node* LitSpriteReader::createNodeWithFlatBuffers(
    const flatbuffers::Table* /*nodeOptions*/)
{
    return LitSpriteNode::create();
}

}  // namespace ocs::ext
