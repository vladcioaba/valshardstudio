#include "UILayoutEditor/VectorMapBinding.h"

#include "axmol.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace opencs
{

namespace
{
/// Tag identifying the editor-attached VectorMapNode child so we can
/// find + reuse it across re-syncs instead of re-attaching every
/// frame. Distinct from the per-node overlay tag.
constexpr int kVectorMapTag = 0x564D4150;  // 'VMAP'

ax::Color4F parseColor(const std::string& s, ax::Color4F dflt)
{
    if (s.empty()) return dflt;
    std::vector<int> parts;
    std::string tok;
    for (char c : s)
    {
        if (c == ',') { if (!tok.empty()) { parts.push_back(std::atoi(tok.c_str())); tok.clear(); } }
        else if (std::isdigit((unsigned char)c) || c == '-' || c == ' ') tok.push_back(c);
    }
    if (!tok.empty()) parts.push_back(std::atoi(tok.c_str()));
    if (parts.size() < 3) return dflt;
    const float a = parts.size() >= 4 ? parts[3] / 255.0f : 1.0f;
    return ax::Color4F(parts[0] / 255.0f, parts[1] / 255.0f,
                       parts[2] / 255.0f, a);
}

std::vector<ax::Vec2> parsePoints(const std::string& s)
{
    // "x,y x,y x,y" — comma separates components, whitespace separates
    // points. Tolerant of arbitrary whitespace.
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
        const float x = std::strtof(pair.substr(0, comma).c_str(), nullptr);
        const float y = std::strtof(pair.substr(comma + 1).c_str(), nullptr);
        out.emplace_back(x, y);
    }
    return out;
}
}  // namespace

bool isVectorMapNode(const Node& n)
{
    return n.valid() && n.ctype() == "VectorMapObjectData";
}

std::vector<ocs::ext::VectorLayer> parseVectorMapLayers(const Node& n)
{
    std::vector<ocs::ext::VectorLayer> out;
    if (!n.valid()) return out;
    auto root = n.element().child("VectorMapData");
    if (!root) return out;

    for (auto layerEl = root.child("Layer"); layerEl;
         layerEl = layerEl.next_sibling("Layer"))
    {
        ocs::ext::VectorLayer layer;
        layer.name    = std::string(layerEl.attribute("Name").as_string(""));
        const std::string vis(layerEl.attribute("Visible").as_string("True"));
        layer.visible = !(vis == "False" || vis == "false" || vis == "0");

        for (auto p = layerEl.first_child(); p; p = p.next_sibling())
        {
            const std::string tag(p.name());
            ocs::ext::VectorPrimitive prim;
            if (tag == "Polygon")  prim.kind = ocs::ext::VectorPrimitive::Kind::Polygon;
            else if (tag == "Polyline") prim.kind = ocs::ext::VectorPrimitive::Kind::Polyline;
            else if (tag == "Circle")   prim.kind = ocs::ext::VectorPrimitive::Kind::Circle;
            else continue;

            prim.fill   = parseColor(std::string(p.attribute("Fill"  ).as_string("")),
                                      ax::Color4F(1, 1, 1, 1));
            prim.stroke = parseColor(std::string(p.attribute("Stroke").as_string("")),
                                      ax::Color4F(0, 0, 0, 1));
            prim.strokeWidth = p.attribute("StrokeWidth").as_float(1.0f);

            if (prim.kind == ocs::ext::VectorPrimitive::Kind::Circle)
            {
                prim.points.emplace_back(
                    p.attribute("X").as_float(0.0f),
                    p.attribute("Y").as_float(0.0f));
                prim.radius = p.attribute("Radius").as_float(0.0f);
            }
            else
            {
                prim.points = parsePoints(
                    std::string(p.attribute("Points").as_string("")));
            }
            layer.primitives.push_back(std::move(prim));
        }
        // Optional <Animation Duration FPS Loop> block + flat
        // <Keyframe Frame PrimIdx Attr Value> children.
        if (auto animEl = layerEl.child("Animation"))
        {
            layer.animation.duration =
                animEl.attribute("Duration").as_int(60);
            layer.animation.fps =
                animEl.attribute("FPS").as_int(30);
            const std::string lp(animEl.attribute("Loop").as_string("True"));
            layer.animation.loop = !(lp == "False" || lp == "false" || lp == "0");
            for (auto kf = animEl.child("Keyframe"); kf;
                 kf = kf.next_sibling("Keyframe"))
            {
                ocs::ext::VectorKeyframe k;
                k.frame   = kf.attribute("Frame").as_int(0);
                k.primIdx = kf.attribute("PrimIdx").as_int(0);
                k.attr    = std::string(kf.attribute("Attr").as_string(""));
                k.value   = std::string(kf.attribute("Value").as_string(""));
                if (!k.attr.empty())
                    layer.animation.keyframes.push_back(std::move(k));
            }
        }
        out.push_back(std::move(layer));
    }
    return out;
}

void syncVectorMapToAx(const Node& csd, ax::Node* ax)
{
    if (!ax || !isVectorMapNode(csd)) return;
    auto* existing = dynamic_cast<ocs::ext::VectorMapNode*>(
        ax->getChildByTag(kVectorMapTag));
    if (!existing)
    {
        existing = ocs::ext::VectorMapNode::create();
        if (!existing) return;
        existing->setTag(kVectorMapTag);
        existing->setLocalZOrder(0);
        ax->addChild(existing);
    }
    existing->setLayers(parseVectorMapLayers(csd));
}

}  // namespace opencs
