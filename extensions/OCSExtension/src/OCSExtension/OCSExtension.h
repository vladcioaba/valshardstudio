// OCSExtension — runtime registration entry point.
//
// Game projects built with the OCS editor link this static library and
// call `ocs::ext::registerAll()` once at startup. That call registers
// every OCS-specific node type with axmol's ObjectFactory so CSLoader
// can instantiate them when it loads a .csb authored in OCS.
//
// Adding a new node type:
//   1. Drop SrcName.{h,cpp} into this same directory.
//   2. Declare a NodeReaderProtocol subclass with
//      DECLARE_CLASS_NODE_READER_INFO + IMPLEMENT_CLASS_NODE_READER_INFO.
//   3. Add `ocs::ext::registerType<MyReader>();` inside registerAll().
//   4. Editor-side: extend CsdModel + the Sprite Catalog / Properties
//      panel to surface the new ctype.

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ax { class Node; }

namespace ocs::ext
{

/// Register every OCS-extension node-reader with axmol's
/// ObjectFactory. Idempotent — repeated calls are no-ops. Safe to
/// invoke before Director::getInstance()->runWithScene().
void registerAll();

/// List the ctype names that this build of OCSExtension exposes. Used
/// by the editor's `describe` introspection (lets the agent discover
/// what extension types are available) and by integration tests to
/// confirm registerAll picked everything up.
const std::vector<std::string>& registeredTypes();

/// Walk a JSON sidecar (`<scene>.vmaps.json`) and attach a
/// VectorMapNode (populated from the JSON layer list) under every
/// node in `sceneRoot` whose `Name` matches a sidecar entry. Used
/// by game code after CSLoader::createNode succeeds:
///
///   auto* root = CSLoader::createNode("scenes/MyScene.csb");
///   ocs::ext::applyVectorMapsFromJson(
///       root, "scenes/MyScene.vmaps.json");
///
/// Idempotent — re-running on the same root replaces existing
/// children tagged as VectorMap. Returns the number of VectorMaps
/// attached (0 on missing file / parse error).
int applyVectorMapsFromJson(ax::Node* sceneRoot,
                            const std::string& jsonPath);

/// One declarative binding pulled from a `<scene>.bindings.json`
/// sidecar. The runtime helper below resolves `nodePath` to an
/// `ax::Node*`, then hands the binding to the caller-supplied
/// `dispatch` so game code wires it onto whatever delegate matches
/// the event ("OnClick" → ax::ui::Widget::addClickEventListener,
/// "OnDrag" → custom drag handler, etc).
struct SceneBinding
{
    std::string nodePath;   // slash-joined name path
    std::string event;      // OnClick, OnTouchBegan, OnTimer, …
    std::string method;     // controller method name (free-form)
    std::string params;     // optional raw payload (e.g. "Interval=0.5")
};

using SceneBindingDispatch =
    std::function<void(ax::Node* node, const SceneBinding& b)>;

/// Walk `<scene>.bindings.json` sidecar, resolve each entry to its
/// runtime ax::Node via slash-separated name path, call `dispatch`.
/// Caller is responsible for the actual delegate wiring — keeps this
/// helper free of dependencies on ui::Widget / specific event APIs.
int applyBindingsFromJson(ax::Node* sceneRoot,
                          const std::string& jsonPath,
                          const SceneBindingDispatch& dispatch);

/// Walk `<scene>.lights.json` and attach a LitSpriteNode child
/// (populated from the JSON light list + texture path) under every
/// matched node. Same name-path resolution as the other helpers.
/// Returns the count of LitSprites attached.
int applyLitSpritesFromJson(ax::Node* sceneRoot,
                            const std::string& jsonPath);

}  // namespace ocs::ext
