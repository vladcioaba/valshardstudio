// Minimal Cocos2d texture-atlas plist reader.
//
// Resolves a sprite-frame name (e.g. "bt_play_round_green.png") inside a
// Cocos2d-format .plist into the atlas's texture file + sub-rect, so the
// editor can render a cropped preview of the frame instead of the whole
// atlas.
//
// Apple plist is XML, which we already have a parser for (pugixml). Only
// the v2/v3 Cocos formats are supported — that's what Cocos Studio and
// modern TexturePacker emit. Rotated frames are reported but the caller
// must handle the 90° rotation itself.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace opencs
{

/// One sprite frame inside an atlas. Rect is in the atlas image's
/// pixel coordinates, origin top-left (matches Cocos2d convention).
struct AtlasFrame
{
    float x      = 0.0f;
    float y      = 0.0f;
    float w      = 0.0f;
    float h      = 0.0f;
    bool  rotated = false;                   ///< true → frame is 90° CW in the atlas
    std::filesystem::path texturePath;       ///< absolute path to the atlas .png
};

class PlistAtlas
{
public:
    /// Look up a frame inside a plist. Returns nullopt if the plist can't
    /// be parsed or the frame name isn't present. Loaded plists are cached
    /// per-process so repeat calls are cheap.
    static std::optional<AtlasFrame> findFrame(
        const std::filesystem::path& plistPath,
        const std::string&           frameName);

    /// Enumerate every frame in a plist, sorted by frame name. Returns
    /// empty on parse failure. Powers the tree view in the Assets panel;
    /// shares the per-process cache with findFrame so listing + later
    /// per-frame lookups don't re-parse.
    static std::vector<std::pair<std::string, AtlasFrame>> listFrames(
        const std::filesystem::path& plistPath);

    /// True iff the file at `plistPath` parses as a Cocos2d texture-atlas
    /// plist (v2 or v3). Used by the Assets-panel scanner to decide
    /// whether to pair a `.plist` with a same-basename `.png`.
    static bool isAtlasPlist(const std::filesystem::path& plistPath);

    /// Drop all cached plists — call after a TexturePacker run or a
    /// Resources rescan so stale data doesn't survive.
    static void clearCache();

    // Implementation detail — left public so the free-function cache
    // lookup in the .cpp can reference it without friending.
    struct Atlas
    {
        std::unordered_map<std::string, AtlasFrame> frames;
        std::filesystem::path                       texturePath;
    };

private:
    static const Atlas* load(const std::filesystem::path& plistPath);
};

}  // namespace opencs
