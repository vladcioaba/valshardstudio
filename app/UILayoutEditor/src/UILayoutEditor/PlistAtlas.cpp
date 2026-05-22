#include "UILayoutEditor/PlistAtlas.h"

#include <pugixml.hpp>

#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>

namespace opencs
{

namespace
{

namespace fs = std::filesystem;

/// Parse a Cocos rect string `"{{x,y},{w,h}}"` into four floats.
/// Returns true on success. Caller passes a const-ref std::string so we
/// always get a null-terminated buffer for sscanf.
bool parseRectString(const std::string& s,
                     float& x, float& y, float& w, float& h)
{
    return std::sscanf(s.c_str(), "{{%f,%f},{%f,%f}}",
                       &x, &y, &w, &h) == 4;
}

/// In a pugi Apple-plist `<dict>`, key/value come as sibling elements:
/// `<key>name</key><value-tag/>`. Return the value element paired with
/// `key`, or an empty node if not found. Uses a string_view compare so it
/// works with pugi's `PUGIXML_HAS_STRING_VIEW` build.
pugi::xml_node valueForKey(pugi::xml_node dict, std::string_view key)
{
    for (auto k = dict.child("key"); k; k = k.next_sibling("key"))
    {
        if (std::string_view(k.child_value()) == key)
            return k.next_sibling();
    }
    return {};
}

/// Element-name comparison helper — pugi's `.name()` is a string_view.
bool nameIs(pugi::xml_node n, std::string_view name)
{
    return n && std::string_view(n.name()) == name;
}

using Atlas = PlistAtlas::Atlas;
std::unordered_map<std::string, Atlas> gCache;
std::mutex gCacheMutex;

}  // namespace

const PlistAtlas::Atlas* PlistAtlas::load(const fs::path& plistPath)
{
    const std::string key = plistPath.string();
    {
        std::lock_guard<std::mutex> lk(gCacheMutex);
        auto it = gCache.find(key);
        if (it != gCache.end()) return &it->second;
    }

    pugi::xml_document doc;
    if (!doc.load_file(plistPath.string().c_str()))
        return nullptr;

    auto plist = doc.child("plist");
    if (!plist) return nullptr;
    auto root = plist.child("dict");
    if (!root) return nullptr;

    Atlas atlas;

    // metadata.textureFileName → atlas image path (relative to the plist).
    // Fall back to "<plist basename>.png" if that key is missing.
    auto metadata = valueForKey(root, "metadata");
    std::string textureRel;
    if (nameIs(metadata, "dict"))
    {
        auto texNode = valueForKey(metadata, "textureFileName");
        if (nameIs(texNode, "string"))
            textureRel = std::string(texNode.child_value());
    }
    if (textureRel.empty())
        textureRel = plistPath.stem().string() + ".png";
    atlas.texturePath = plistPath.parent_path() / textureRel;

    // frames dict: <key>name.png</key><dict>…</dict> per frame.
    auto frames = valueForKey(root, "frames");
    if (nameIs(frames, "dict"))
    {
        for (auto k = frames.child("key"); k; k = k.next_sibling("key"))
        {
            const std::string frameName(k.child_value());
            auto frameDict = k.next_sibling();
            if (!nameIs(frameDict, "dict")) continue;

            AtlasFrame f;
            f.texturePath = atlas.texturePath;

            // v3 Cocos: <key>frame</key><string>{{x,y},{w,h}}</string>.
            // v2 fallback: textureRect has the same shape.
            auto rectNode = valueForKey(frameDict, "frame");
            if (!rectNode) rectNode = valueForKey(frameDict, "textureRect");
            if (nameIs(rectNode, "string"))
                parseRectString(std::string(rectNode.child_value()),
                                f.x, f.y, f.w, f.h);

            auto rotNode = valueForKey(frameDict, "rotated");
            if (!rotNode) rotNode = valueForKey(frameDict, "textureRotated");
            if (rotNode) f.rotated = nameIs(rotNode, "true");

            atlas.frames.emplace(frameName, f);
        }
    }

    std::lock_guard<std::mutex> lk(gCacheMutex);
    auto [it, _] = gCache.emplace(key, std::move(atlas));
    return &it->second;
}

std::optional<AtlasFrame> PlistAtlas::findFrame(
    const fs::path& plistPath, const std::string& frameName)
{
    const Atlas* a = load(plistPath);
    if (!a) return std::nullopt;
    auto it = a->frames.find(frameName);
    if (it == a->frames.end()) return std::nullopt;
    return it->second;
}

std::vector<std::pair<std::string, AtlasFrame>> PlistAtlas::listFrames(
    const fs::path& plistPath)
{
    std::vector<std::pair<std::string, AtlasFrame>> out;
    const Atlas* a = load(plistPath);
    if (!a) return out;
    out.reserve(a->frames.size());
    for (const auto& [name, f] : a->frames) out.emplace_back(name, f);
    std::sort(out.begin(), out.end(),
              [](const auto& x, const auto& y) { return x.first < y.first; });
    return out;
}

bool PlistAtlas::isAtlasPlist(const fs::path& plistPath)
{
    // Cheap structural test: root must be <plist>/<dict> and the dict
    // must contain a "frames" key whose sibling is a <dict>. Apple plists
    // used for other purposes (NSArray-root docs, preferences, scene
    // manifests) won't match. load() caches on hit so this is ~free for
    // the later listFrames/findFrame calls.
    return load(plistPath) != nullptr &&
           !load(plistPath)->frames.empty();
}

void PlistAtlas::clearCache()
{
    std::lock_guard<std::mutex> lk(gCacheMutex);
    gCache.clear();
}

}  // namespace opencs
