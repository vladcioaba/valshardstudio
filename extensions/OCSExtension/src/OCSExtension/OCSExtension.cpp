#include "OCSExtension/OCSExtension.h"
#include "OCSExtension/VectorMapNode.h"
#include "OCSExtension/SvgImageNode.h"
#include "OCSExtension/LottieNode.h"
#include "OCSExtension/LitSpriteNode.h"

#include "axmol.h"
#include "base/ObjectFactory.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"

#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>

namespace ocs::ext
{

namespace
{
std::vector<std::string>& mutableTypes()
{
    static std::vector<std::string> v;
    return v;
}

template <class ReaderT>
void registerOne()
{
    auto* f = ax::ObjectFactory::getInstance();
    // Use the class's own __Type info so the registered name matches
    // what cocostudio's CSLoader looks up by string.
    f->registerType(ReaderT::__Type);
    mutableTypes().push_back(ReaderT::__Type._class);
}
}  // namespace

void registerAll()
{
    static std::once_flag once;
    std::call_once(once, [] {
        // VectorMapNode + reader. Renders a list of polygon/path layers
        // via DrawNode — resolution-independent, no external lib.
        registerOne<VectorMapReader>();
        registerOne<SvgImageReader>();
        registerOne<LottieReader>();
        registerOne<LitSpriteReader>();
    });
}

const std::vector<std::string>& registeredTypes()
{
    return mutableTypes();
}

// ---- JSON sidecar loader -------------------------------------------------

namespace
{

ax::Color4F readColor(const rapidjson::Value& v, ax::Color4F dflt)
{
    if (!v.IsArray() || v.Size() < 3) return dflt;
    const float a = v.Size() >= 4 ? v[3].GetFloat() / 255.0f : 1.0f;
    return ax::Color4F(v[0].GetFloat() / 255.0f,
                       v[1].GetFloat() / 255.0f,
                       v[2].GetFloat() / 255.0f, a);
}

std::vector<ax::Vec2> readPoints(const rapidjson::Value& v)
{
    std::vector<ax::Vec2> out;
    if (!v.IsArray()) return out;
    for (auto& p : v.GetArray())
    {
        if (!p.IsArray() || p.Size() < 2) continue;
        out.emplace_back(p[0].GetFloat(), p[1].GetFloat());
    }
    return out;
}

VectorLayer parseLayer(const rapidjson::Value& v)
{
    VectorLayer layer;
    if (v.HasMember("name") && v["name"].IsString())
        layer.name = v["name"].GetString();
    if (v.HasMember("visible") && v["visible"].IsBool())
        layer.visible = v["visible"].GetBool();
    if (!v.HasMember("primitives") || !v["primitives"].IsArray())
        return layer;
    for (auto& p : v["primitives"].GetArray())
    {
        VectorPrimitive prim;
        const std::string kind = p.HasMember("kind") && p["kind"].IsString()
            ? p["kind"].GetString() : "Polygon";
        if      (kind == "Polyline") prim.kind = VectorPrimitive::Kind::Polyline;
        else if (kind == "Circle")   prim.kind = VectorPrimitive::Kind::Circle;
        else                          prim.kind = VectorPrimitive::Kind::Polygon;
        if (p.HasMember("fill"))    prim.fill   = readColor(p["fill"], ax::Color4F(1,1,1,1));
        if (p.HasMember("stroke"))  prim.stroke = readColor(p["stroke"], ax::Color4F(0,0,0,1));
        if (p.HasMember("strokeWidth") && p["strokeWidth"].IsNumber())
            prim.strokeWidth = p["strokeWidth"].GetFloat();
        if (prim.kind == VectorPrimitive::Kind::Circle)
        {
            float cx = p.HasMember("x") ? p["x"].GetFloat() : 0.0f;
            float cy = p.HasMember("y") ? p["y"].GetFloat() : 0.0f;
            prim.points.emplace_back(cx, cy);
            prim.radius = p.HasMember("radius") ? p["radius"].GetFloat() : 0.0f;
        }
        else if (p.HasMember("points"))
        {
            prim.points = readPoints(p["points"]);
        }
        layer.primitives.push_back(std::move(prim));
    }
    // Optional anim block. Each entry: {frame, primIdx, attr, value}.
    if (v.HasMember("anim") && v["anim"].IsObject())
    {
        const auto& a = v["anim"];
        if (a.HasMember("duration") && a["duration"].IsInt())
            layer.animation.duration = a["duration"].GetInt();
        if (a.HasMember("fps") && a["fps"].IsInt())
            layer.animation.fps = a["fps"].GetInt();
        if (a.HasMember("loop") && a["loop"].IsBool())
            layer.animation.loop = a["loop"].GetBool();
        if (a.HasMember("keyframes") && a["keyframes"].IsArray())
            for (auto& k : a["keyframes"].GetArray())
            {
                VectorKeyframe kf;
                if (k.HasMember("frame")   && k["frame"].IsInt())
                    kf.frame   = k["frame"].GetInt();
                if (k.HasMember("primIdx") && k["primIdx"].IsInt())
                    kf.primIdx = k["primIdx"].GetInt();
                if (k.HasMember("attr")    && k["attr"].IsString())
                    kf.attr    = k["attr"].GetString();
                if (k.HasMember("value")   && k["value"].IsString())
                    kf.value   = k["value"].GetString();
                if (!kf.attr.empty())
                    layer.animation.keyframes.push_back(std::move(kf));
            }
    }
    return layer;
}

ax::Node* findByNamePath(ax::Node* root, const std::string& path)
{
    if (!root) return nullptr;
    if (path.empty() || path == "/") return root;
    // Segments separated by '/'. First segment matches if root name OR
    // an immediate child. Subsequent segments must be direct children.
    std::vector<std::string> segs;
    std::string cur;
    for (char c : path)
    {
        if (c == '/') { if (!cur.empty()) { segs.push_back(cur); cur.clear(); } }
        else cur.push_back(c);
    }
    if (!cur.empty()) segs.push_back(cur);
    if (segs.empty()) return root;

    // Head: match root by name OR DFS first match.
    ax::Node* head = nullptr;
    if (root->getName() == segs[0]) head = root;
    else
    {
        std::function<ax::Node*(ax::Node*)> dfs = [&](ax::Node* n) -> ax::Node* {
            if (!n) return nullptr;
            if (n->getName() == segs[0]) return n;
            for (auto* c : n->getChildren())
                if (auto* r = dfs(c)) return r;
            return nullptr;
        };
        head = dfs(root);
    }
    for (std::size_t i = 1; i < segs.size() && head; ++i)
        head = head->getChildByName(segs[i]);
    return head;
}

constexpr int kVectorMapApplyTag = 0x564D4150;  // 'VMAP' — same as editor

}  // namespace

int applyLitSpritesFromJson(ax::Node* sceneRoot,
                            const std::string& jsonPath)
{
    if (!sceneRoot) return 0;
    std::ifstream f(jsonPath);
    if (!f) return 0;
    std::stringstream ss; ss << f.rdbuf();
    const std::string body = ss.str();
    rapidjson::Document doc;
    doc.Parse(body.c_str(), body.size());
    if (doc.HasParseError() || !doc.HasMember("lits") ||
        !doc["lits"].IsArray()) return 0;
    constexpr int kTag = 0x4C495433;  // 'LIT3' — match editor binding
    int applied = 0;
    for (auto& e : doc["lits"].GetArray())
    {
        const std::string path = e.HasMember("path") && e["path"].IsString()
            ? e["path"].GetString() : "";
        ax::Node* target = findByNamePath(sceneRoot, path);
        if (!target) continue;
        auto* lit = dynamic_cast<LitSpriteNode*>(target->getChildByTag(kTag));
        if (!lit)
        {
            lit = LitSpriteNode::create();
            if (!lit) continue;
            lit->setTag(kTag);
            lit->setAnchorPoint(ax::Vec2(0.5f, 0.5f));
            target->addChild(lit);
        }
        if (e.HasMember("texture") && e["texture"].IsString())
        {
            auto* tex = ax::Director::getInstance()->getTextureCache()
                ->addImage(e["texture"].GetString());
            if (tex)
            {
                lit->setTexture(tex);
                lit->setTextureRect(ax::Rect(0, 0,
                    tex->getContentSize().width,
                    tex->getContentSize().height));
            }
        }
        if (e.HasMember("ambient") && e["ambient"].IsArray() &&
            e["ambient"].Size() >= 3)
            lit->setAmbient(readColor(e["ambient"], ax::Color4F(0.4f, 0.4f, 0.5f, 1)));
        std::vector<Light2D> lts;
        if (e.HasMember("lights") && e["lights"].IsArray())
            for (auto& L : e["lights"].GetArray())
            {
                Light2D lt;
                if (L.HasMember("x")) lt.x = L["x"].GetFloat();
                if (L.HasMember("y")) lt.y = L["y"].GetFloat();
                if (L.HasMember("r")) lt.radius = L["r"].GetFloat();
                if (L.HasMember("color"))
                    lt.color = readColor(L["color"], ax::Color4F(1,1,1,1));
                if (L.HasMember("falloff")) lt.falloff = L["falloff"].GetFloat();
                if (L.HasMember("dir") && L["dir"].IsArray() &&
                    L["dir"].Size() >= 2)
                {
                    lt.dirX = L["dir"][0].GetFloat();
                    lt.dirY = L["dir"][1].GetFloat();
                }
                if (L.HasMember("cone")) lt.coneDeg      = L["cone"].GetFloat();
                if (L.HasMember("soft")) lt.coneSoftness = L["soft"].GetFloat();
                lts.push_back(lt);
            }
        lit->setLights(std::move(lts));
        ++applied;
    }
    return applied;
}

int applyBindingsFromJson(ax::Node* sceneRoot,
                          const std::string& jsonPath,
                          const SceneBindingDispatch& dispatch)
{
    if (!sceneRoot || !dispatch) return 0;
    std::ifstream f(jsonPath);
    if (!f) return 0;
    std::stringstream ss; ss << f.rdbuf();
    const std::string body = ss.str();
    rapidjson::Document doc;
    doc.Parse(body.c_str(), body.size());
    if (doc.HasParseError() || !doc.HasMember("bindings") ||
        !doc["bindings"].IsArray()) return 0;
    int applied = 0;
    for (auto& e : doc["bindings"].GetArray())
    {
        SceneBinding b;
        if (e.HasMember("path")   && e["path"].IsString())    b.nodePath = e["path"].GetString();
        if (e.HasMember("event")  && e["event"].IsString())   b.event    = e["event"].GetString();
        if (e.HasMember("method") && e["method"].IsString())  b.method   = e["method"].GetString();
        if (e.HasMember("params") && e["params"].IsString())  b.params   = e["params"].GetString();
        ax::Node* n = findByNamePath(sceneRoot, b.nodePath);
        if (!n) continue;
        dispatch(n, b);
        ++applied;
    }
    return applied;
}

int applyVectorMapsFromJson(ax::Node* sceneRoot,
                            const std::string& jsonPath)
{
    if (!sceneRoot) return 0;
    std::ifstream f(jsonPath);
    if (!f) return 0;
    std::stringstream ss; ss << f.rdbuf();
    const std::string body = ss.str();

    rapidjson::Document doc;
    doc.Parse(body.c_str(), body.size());
    if (doc.HasParseError())
    {
        AXLOGW("[ocs::ext] vmaps JSON parse error: {}",
               rapidjson::GetParseError_En(doc.GetParseError()));
        return 0;
    }
    if (!doc.HasMember("vmaps") || !doc["vmaps"].IsArray()) return 0;

    int applied = 0;
    for (auto& entry : doc["vmaps"].GetArray())
    {
        const std::string path = entry.HasMember("path") && entry["path"].IsString()
            ? entry["path"].GetString() : "";
        const std::string name = entry.HasMember("name") && entry["name"].IsString()
            ? entry["name"].GetString() : "";
        ax::Node* target =
            !path.empty() ? findByNamePath(sceneRoot, path)
                          : findByNamePath(sceneRoot, name);
        if (!target) continue;

        std::vector<VectorLayer> layers;
        if (entry.HasMember("layers") && entry["layers"].IsArray())
            for (auto& L : entry["layers"].GetArray())
                layers.push_back(parseLayer(L));

        // Attach (or reuse) a VectorMapNode child under the target.
        auto* existing = dynamic_cast<VectorMapNode*>(
            target->getChildByTag(kVectorMapApplyTag));
        if (!existing)
        {
            existing = VectorMapNode::create();
            if (!existing) continue;
            existing->setTag(kVectorMapApplyTag);
            target->addChild(existing);
        }
        existing->setLayers(std::move(layers));
        ++applied;
    }
    return applied;
}

}  // namespace ocs::ext
