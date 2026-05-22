#include "UILayoutEditor/LitSpriteBinding.h"

#include "axmol.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace opencs
{

namespace
{
constexpr int kLitSpriteTag = 0x4C495433;  // 'LIT3'

ax::Color4F parseColor4(const std::string& s, ax::Color4F dflt)
{
    int r=0,g=0,b=0,a=255;
    if (std::sscanf(s.c_str(), "%d,%d,%d,%d", &r,&g,&b,&a) < 3) return dflt;
    return ax::Color4F(r/255.0f, g/255.0f, b/255.0f, a/255.0f);
}
}  // namespace

bool isLitSpriteNode(const Node& n)
{
    return n.valid() && n.ctype() == "LitSpriteObjectData";
}

void syncLitSpriteToAx(const Node& csd, ax::Node* ax,
                       const std::filesystem::path& assetsRoot)
{
    if (!ax || !isLitSpriteNode(csd)) return;
    auto* existing = dynamic_cast<ocs::ext::LitSpriteNode*>(
        ax->getChildByTag(kLitSpriteTag));
    if (!existing)
    {
        existing = ocs::ext::LitSpriteNode::create();
        if (!existing) return;
        existing->setTag(kLitSpriteTag);
        existing->setAnchorPoint(::ax::Vec2(0.5f, 0.5f));
        ax->addChild(existing);
    }

    // Texture from FileData/Path. Resolves under the editor's
    // assetsRoot. Skipped silently when missing — node falls back
    // to its 1x1 white placeholder so the user sees something.
    namespace fs = std::filesystem;
    auto fd = csd.element().child("FileData");
    if (fd)
    {
        const std::string rel(fd.attribute("Path").as_string(""));
        if (!rel.empty())
        {
            fs::path full = rel;
            if (full.is_relative() && !assetsRoot.empty())
                full = assetsRoot / rel;
            if (fs::exists(full))
            {
                auto* tex = ::ax::Director::getInstance()
                    ->getTextureCache()->addImage(full.string());
                if (tex)
                {
                    existing->setTexture(tex);
                    existing->setTextureRect(::ax::Rect(
                        0, 0, tex->getContentSize().width,
                              tex->getContentSize().height));
                }
            }
        }
    }

    // Lights block. Optional Ambient attr + <Light X Y Radius Color
    // Falloff/> children.
    std::vector<ocs::ext::Light2D> lights;
    auto lroot = csd.element().child("OCSLights");
    if (lroot)
    {
        const std::string amb(lroot.attribute("Ambient").as_string(""));
        existing->setAmbient(parseColor4(amb,
            ::ax::Color4F(0.4f, 0.4f, 0.5f, 1.0f)));
        for (auto L = lroot.child("Light"); L; L = L.next_sibling("Light"))
        {
            ocs::ext::Light2D lt;
            lt.x       = L.attribute("X").as_float(0);
            lt.y       = L.attribute("Y").as_float(0);
            lt.radius  = L.attribute("Radius").as_float(100);
            lt.color   = parseColor4(
                std::string(L.attribute("Color").as_string("")),
                ::ax::Color4F(1, 1, 1, 1));
            lt.falloff = L.attribute("Falloff").as_float(1.0f);
            // Direction is "dx,dy" (any magnitude — normalised at
            // sample time). Cone in degrees, soft as 0..1.
            const std::string ds(L.attribute("Direction").as_string(""));
            if (!ds.empty())
                std::sscanf(ds.c_str(), "%f,%f", &lt.dirX, &lt.dirY);
            lt.coneDeg      = L.attribute("ConeDeg").as_float(360.0f);
            lt.coneSoftness = L.attribute("ConeSoftness").as_float(0.2f);
            lights.push_back(lt);
        }
    }
    existing->setLights(std::move(lights));
}

}  // namespace opencs
