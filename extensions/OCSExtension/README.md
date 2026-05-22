# OCSExtension

Runtime library shipped to game projects built with the Walshard Studio editor.

Walshard Studio authors content (`.csd` → `.csb`) for cocos2d-x and
axmol games. When the designer uses a node type that the stock
cocos2d-x scene loader doesn't implement — `VectorMapObjectData`,
`LottieObjectData`, etc. — the game's runtime has to know how to
instantiate that type. This library is that bridge.

## Architecture

```
+----------------------+        +----------------------+        +----------------------+
| OCS Editor process   |        | OCSExtension (this)  |        | Game runtime         |
| (UILayoutEditor)     |◄──────►| static lib           |◄──────►| AppDelegate +        |
|                      |        |                      |        | scene CSLoader       |
| - canvas preview     | links  | - VectorMapNode      | links  | - registerAll()      |
| - properties panels  |        | - VectorMapReader    |        | - CSLoader loads .csb|
| - export to .csb     |        | - (future LottieNode)|        |   → factory makes    |
|                      |        |                      |        |   custom nodes       |
+----------------------+        +----------------------+        +----------------------+
```

Single source of truth for node implementations. The editor renders
exactly the same node class the runtime renders → no drift between
preview and ship.

## Game-project integration

```cmake
# In your game's CMakeLists.txt
add_subdirectory(${OCS_ROOT}/extensions/OCSExtension OCSExtension)
target_link_libraries(${APP_NAME} PRIVATE OCSExtension)
```

```cpp
// In AppDelegate::applicationDidFinishLaunching, before any CSLoader call:
#include "OCSExtension/OCSExtension.h"
ocs::ext::registerAll();
```

That's the entire wiring. `registerAll()` is idempotent and `std::call_once`-guarded.

## Adding a new node type

1. Drop `MyNode.{h,cpp}` into `src/OCSExtension/`.
2. Declare a `NodeReaderProtocol` subclass (singleton, with
   `DECLARE_CLASS_NODE_READER_INFO` + `IMPLEMENT_CLASS_NODE_READER_INFO`).
3. Add `registerOne<MyReader>()` inside `registerAll()` in
   `OCSExtension.cpp`.
4. Editor-side: extend `CsdModel` ctype lookup and the Sprite Catalog
   / Properties panel so the user can place + configure your node.

## Currently registered types

| ctype | Class | Renders via |
|---|---|---|
| `VectorMapObjectData` | `VectorMapNode` | `ax::DrawNode` (polygon + polyline + circle) — crisp at any zoom |

Future:
- `LottieObjectData` (rlottie-backed animations)
- `SvgImageObjectData` (runtime lunasvg for true zoom-independent images)

## Runtime helpers (non-ctype)

In addition to the `registerType`-based node factory, OCSExtension exposes
plain free functions that operate on a CSLoader-built scene tree:

| Function | What it does |
|---|---|
| `applyVectorMapsFromJson(scene, "<basename>.vmaps.json")` | Walks the editor-generated VectorMap sidecar, attaches a `VectorMapNode` populated with the parsed layers under every node it can resolve by slash-separated name path. Idempotent; safe to re-run. |
| `applyBindingsFromJson(scene, "<basename>.bindings.json", dispatch)` | Walks the editor-generated binding sidecar. For each entry, resolves the path and calls `dispatch(node, binding)`. Caller wires the actual delegate (e.g. `Widget::addClickEventListener`). |

Both helpers parse JSON via the rapidjson copy bundled with axmol — no
extra dependency.

## Workflow with the OCS editor

```
Designer authors in OCS:
  • Drops VectorMap nodes (per-primitive editor in Properties panel).
  • Attaches event bindings to Buttons / nodes via the "Bindings"
    section of the Properties panel.

OCS export step writes:
  Resources/MyScene.csb              ← standard cocostudio binary
  Resources/MyScene.vmaps.json       ← only when VectorMaps exist
  Resources/MyScene.bindings.json    ← only when bindings exist

Game startup (in AppDelegate or wherever scenes load):
  ocs::ext::registerAll();   // once, before any CSLoader call

  auto* root = CSLoader::createNode("Resources/MyScene.csb");
  ocs::ext::applyVectorMapsFromJson(root, "Resources/MyScene.vmaps.json");
  MyController::wire(root, "Resources/MyScene.bindings.json");
  scene->addChild(root);
```

`MyController` is generated from the editor via the `gen-controller`
RPC. It emits a paired `.h/.cpp` with one method-stub per unique
binding `Method` plus a `wire()` static that calls
`applyBindingsFromJson` and dispatches to the right method by name.
Re-running `gen-controller` is incremental — only the
`wire()` block (between sentinel comments) is regenerated; existing
method bodies are preserved and new methods are appended as empty
stubs.

## Build flags

| CMake var | Default | Meaning |
|---|---|---|
| `OCSEXT_AXMOL_TARGET` | auto-detected (`axmol` or `axmol::axmol`) | Override when your project re-exports axmol under a different name |
