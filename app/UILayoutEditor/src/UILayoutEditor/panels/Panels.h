// ImGui panel entry points. Kept as free functions that take the Editor by
// reference — the state lives on Editor, the panels are just views.

#pragma once

namespace opencs
{
class Editor;

// Panel body functions — assume the caller has already opened an ImGui
// window with ImGui::Begin and will close it with ImGui::End. Keeps window
// sizing/locking logic centralized in Editor::render.
void drawSceneTreeBody(Editor& editor);
void drawPropertiesBody(Editor& editor);
void drawAssetBrowserBody(Editor& editor);
void drawLayoutBody(Editor& editor);
void drawTimelineBody(Editor& editor);
void drawSpriteEditorBody(Editor& editor);
void drawSpriteCatalogBody(Editor& editor);

// Cross-panel drag-drop. The Asset browser publishes an AssetDragPayload
// for each File / AtlasFrame leaf; the Scene tree accepts it and inserts
// an ImageView child of the dropped-on node. Fixed-size POD so ImGui
// can copy it through its internal payload buffer without owning a
// std::string. Paths longer than the buffer get truncated — fine for
// any practical project layout.
constexpr const char* kAssetDragPayloadId = "OCS_ASSET";
struct AssetDragPayload
{
    enum class Kind { File, AtlasFrame } kind;
    char absPath[1024];        // .png on disk (atlas image for AtlasFrame)
    char atlasPlistPath[1024]; // empty for File; the atlas .plist otherwise
    char frameName[256];       // empty for File; the sprite-frame key
};

}  // namespace opencs
