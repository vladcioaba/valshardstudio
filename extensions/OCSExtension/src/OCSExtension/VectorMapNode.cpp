#include "OCSExtension/VectorMapNode.h"

#include "axmol.h"

#include <fstream>
#include <sstream>

namespace ocs::ext
{

// ---- VectorMapNode -------------------------------------------------------

VectorMapNode::VectorMapNode() = default;

VectorMapNode* VectorMapNode::create()
{
    auto* n = new VectorMapNode();
    if (n->init()) { n->autorelease(); return n; }
    delete n;
    return nullptr;
}

bool VectorMapNode::init()
{
    if (!ax::Node::init()) return false;
    _draw = ax::DrawNode::create();
    addChild(_draw);
    schedule([this](float dt) { tick(dt); }, "ocs_vmap_anim");
    return true;
}

namespace
{
ax::Color4F parseColor4(const std::string& s, ax::Color4F dflt)
{
    int r = 0, g = 0, b = 0, a = 255;
    if (std::sscanf(s.c_str(), "%d,%d,%d,%d", &r, &g, &b, &a) < 3) return dflt;
    return ax::Color4F(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

std::vector<ax::Vec2> parsePoints2(const std::string& s)
{
    std::vector<ax::Vec2> out;
    std::size_t i = 0;
    while (i < s.size())
    {
        while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
        if (i >= s.size()) break;
        const std::size_t start = i;
        while (i < s.size() && !std::isspace((unsigned char)s[i])) ++i;
        const std::string pair = s.substr(start, i - start);
        const auto comma = pair.find(',');
        if (comma == std::string::npos) continue;
        out.emplace_back(
            std::strtof(pair.substr(0, comma).c_str(), nullptr),
            std::strtof(pair.substr(comma + 1).c_str(), nullptr));
    }
    return out;
}

float lerpF(float a, float b, float t) { return a + (b - a) * t; }
ax::Color4F lerpC(const ax::Color4F& a, const ax::Color4F& b, float t)
{
    return { lerpF(a.r, b.r, t), lerpF(a.g, b.g, t),
             lerpF(a.b, b.b, t), lerpF(a.a, b.a, t) };
}
}  // namespace

void VectorMapNode::setFrame(int f)
{
    _frameInt = f;
    _phase = 0.0f;
}

void VectorMapNode::tick(float dt)
{
    if (!_playing) return;
    if (_layers.empty()) return;
    // Cheap: only re-sample / rebuild when at least one layer has
    // animation data. Static layers stay untouched by the tick path.
    bool any = false;
    for (const auto& L : _layers)
        if (!L.animation.keyframes.empty()) { any = true; break; }
    if (!any) return;

    // Work on a copy so authored static data never gets clobbered.
    auto working = _layers;
    for (auto& L : working)
    {
        const auto& anim = L.animation;
        if (anim.keyframes.empty() || anim.duration <= 0) continue;
        // Advance the playhead with the layer's own duration since
        // anims can have different lengths per layer. Per-instance
        // _phase is shared; treat it as the "global" cursor and wrap
        // per-layer.
        const float dur = (float)anim.duration / std::max(1, anim.fps);
        if (anim.loop)
            _phase = std::fmod(_phase + dt, dur);
        else
            _phase = std::min(_phase + dt, dur);
        _frameInt = (int)std::floor(_phase * anim.fps);
        const int F = _frameInt;
        // Walk per-primitive + per-attr. Find bracket keyframes for
        // each (primIdx, attr) tuple. Sparse data is the norm so
        // O(N²) is fine.
        struct Pair { const VectorKeyframe* a = nullptr; const VectorKeyframe* b = nullptr; };
        std::map<std::pair<int, std::string>, Pair> brackets;
        for (const auto& kf : anim.keyframes)
        {
            auto& pr = brackets[{kf.primIdx, kf.attr}];
            if (kf.frame <= F && (!pr.a || kf.frame > pr.a->frame)) pr.a = &kf;
            if (kf.frame >  F && (!pr.b || kf.frame < pr.b->frame)) pr.b = &kf;
        }
        for (auto& [key, pr] : brackets)
        {
            if (!pr.a && !pr.b) continue;
            const VectorKeyframe* k1 = pr.a ? pr.a : pr.b;
            const VectorKeyframe* k2 = pr.b ? pr.b : pr.a;
            const int span = std::max(1, k2->frame - k1->frame);
            const float t = (k1 == k2) ? 0.0f
                : std::clamp((float)(F - k1->frame) / (float)span, 0.0f, 1.0f);
            if ((std::size_t)key.first >= L.primitives.size()) continue;
            auto& prim = L.primitives[key.first];
            const std::string& attr = key.second;
            if (attr == "Fill")
                prim.fill = lerpC(parseColor4(k1->value, prim.fill),
                                   parseColor4(k2->value, prim.fill), t);
            else if (attr == "Stroke")
                prim.stroke = lerpC(parseColor4(k1->value, prim.stroke),
                                     parseColor4(k2->value, prim.stroke), t);
            else if (attr == "StrokeWidth")
                prim.strokeWidth = lerpF(
                    std::strtof(k1->value.c_str(), nullptr),
                    std::strtof(k2->value.c_str(), nullptr), t);
            else if (attr == "Radius")
                prim.radius = lerpF(
                    std::strtof(k1->value.c_str(), nullptr),
                    std::strtof(k2->value.c_str(), nullptr), t);
            else if (attr == "X" && !prim.points.empty())
                prim.points[0].x = lerpF(
                    std::strtof(k1->value.c_str(), nullptr),
                    std::strtof(k2->value.c_str(), nullptr), t);
            else if (attr == "Y" && !prim.points.empty())
                prim.points[0].y = lerpF(
                    std::strtof(k1->value.c_str(), nullptr),
                    std::strtof(k2->value.c_str(), nullptr), t);
            else if (attr == "Points")
            {
                // Per-vertex lerp only when both keyframes share the
                // same vertex count. Mismatched lengths fall back to
                // the earlier keyframe (no morph) — a real path-morph
                // implementation comes later with resampling.
                const auto a = parsePoints2(k1->value);
                const auto b = parsePoints2(k2->value);
                if (a.size() == b.size() && !a.empty())
                {
                    prim.points.resize(a.size());
                    for (std::size_t i = 0; i < a.size(); ++i)
                        prim.points[i] = {
                            lerpF(a[i].x, b[i].x, t),
                            lerpF(a[i].y, b[i].y, t)
                        };
                }
                else prim.points = a;
            }
        }
    }
    // Push the interpolated working copy through the explicit
    // render path so `_layers` (authored static data) stays intact.
    renderLayers(working);
}

void VectorMapNode::setLayers(std::vector<VectorLayer> layers)
{
    _layers = std::move(layers);
    rebuild();
}

void VectorMapNode::rebuild()
{
    renderLayers(_layers);
}

void VectorMapNode::renderLayers(const std::vector<VectorLayer>& layers)
{
    if (!_draw) return;
    _draw->clear();
    for (const auto& layer : layers)
    {
        if (!layer.visible) continue;
        for (const auto& p : layer.primitives)
        {
            switch (p.kind)
            {
                case VectorPrimitive::Kind::Polygon:
                {
                    if (p.points.size() < 3) break;
                    _draw->drawPolygon(
                        const_cast<ax::Vec2*>(p.points.data()),
                        static_cast<int>(p.points.size()),
                        p.fill, p.strokeWidth, p.stroke);
                    break;
                }
                case VectorPrimitive::Kind::Polyline:
                {
                    if (p.points.size() < 2) break;
                    for (std::size_t i = 1; i < p.points.size(); ++i)
                        _draw->drawSegment(p.points[i-1], p.points[i],
                                           p.strokeWidth * 0.5f,
                                           p.stroke);
                    break;
                }
                case VectorPrimitive::Kind::Circle:
                {
                    if (p.points.empty() || p.radius <= 0.0f) break;
                    _draw->drawSolidCircle(p.points[0], p.radius,
                                            0.0f, 64, p.fill);
                    break;
                }
            }
        }
    }
}

bool VectorMapNode::loadFromJsonFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f) { AXLOGW("[ocs::ext] VectorMapNode: cannot open {}", path); return false; }
    // JSON parser intentionally NOT bundled here — game projects can
    // wire their own (axmol has rapidjson available). For now leave
    // the hook open and document it via the panel-side authoring
    // flow that hand-builds the layer vector and calls setLayers().
    AXLOGW("[ocs::ext] VectorMapNode::loadFromJsonFile not yet implemented "
           "(path={}) — call setLayers() directly", path);
    return false;
}

// ---- VectorMapReader -----------------------------------------------------

IMPLEMENT_CLASS_NODE_READER_INFO(VectorMapReader)
VectorMapReader* VectorMapReader::_instance = nullptr;

VectorMapReader* VectorMapReader::getInstance()
{
    if (!_instance) _instance = new VectorMapReader();
    return _instance;
}

void VectorMapReader::destroyInstance()
{
    delete _instance; _instance = nullptr;
}

ax::Node* VectorMapReader::createNodeWithFlatBuffers(
    const flatbuffers::Table* /*nodeOptions*/)
{
    // First-pass implementation: produce an empty VectorMapNode. The
    // editor side hand-fills layers via runtime code OR a JSON
    // sidecar referenced by path through a generic UserData attr.
    // Once we extend the FlatBuffers schema with a VectorMapOptions
    // table, parse `nodeOptions` here and call setLayers().
    return VectorMapNode::create();
}

}  // namespace ocs::ext
