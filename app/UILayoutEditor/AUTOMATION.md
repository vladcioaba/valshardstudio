# OCS automation rule

Every editor-side feature **must** ship an RPC binding before it lands.

If a panel button exists, an equivalent `nc 127.0.0.1 9876` command MUST exist
for it. The agent surface (`handleInspectCommand` in `EditorInspect.cpp`) is
the source of truth — anything the user can do in the editor, an LLM or
shell script can also do, headless, deterministically, idempotently.

## What "RPC binding" means

For every new feature:

1. **Mutation cmds** — one verb per logical edit. Single-token args
   where possible; reuse `setattr <path> <SubTag> K=V K=V` for bulk
   sub-element attribute writes. Always:
   - parse args, error early with `ERR: ...`
   - `pushUndo()` before mutating
   - mark `_doc->dirty = true`
   - call `syncNodeCsdToAx(n)` so the canvas updates live
   - return `ok\n` or a stable single-line result
2. **Read cmds** — read-only counterpart (`listX`, `dumpX`,
   `getattr` extension). Auth-gated handlers go through the existing
   gate via the `kMutating` set in `EditorInspect.cpp`.
3. **Help text** — append the cmd shape under the matching section
   in the `help` body. Keep alphabetised within section.
4. **Sidecar export** — if the feature persists state that doesn't
   travel inside the cocostudio FlatBuffers schema, write a
   `<basename>.<feature>.json` next to the `.csb` from
   `Editor::exportCsb`. Mirror on the runtime side with an
   `applyXFromJson(scene, path, ...)` helper in `OCSExtension`.
5. **Properties UI** — every cmd should be triggerable from the
   Properties panel for the selected node. Panel = thin wrapper
   over the same XML mutation paths the RPC verbs use; never bypass
   them with direct pugi calls outside the panel section.
6. **Watch events** — when the operation changes user-visible state
   (selection, dirty, save, export, mode), emit a matching `EVENT`
   line via `_inspect.publishEvent` so subscribed agents see the
   transition.

## Audit of the current feature set

| Feature | Mutation RPC | Read RPC | Sidecar | Watch event | Properties UI |
|---|---|---|---|---|---|
| Generic attr | `setattr` | `getattr` | (in csb) | dirty | yes |
| Color (CColor / SingleColor / FirstColor) | `setcolor`, `setbgcolor` | `getattr` | (in csb) | dirty | yes |
| Visibility | `setvisible` | `getattr Visible` | (in csb) | dirty | yes |
| Sprite ref | `setfile`, `setpanelimage` | `getattr FileData.Path` | (in csb) | dirty | (Sprite Catalog) |
| Text label | `setlabel` | `getattr LabelText` | (in csb) | dirty | yes |
| Structural add/del | `addchild`, `delete` | `tree` | (in csb) | dirty + tree | yes |
| Move / size / rotate / scale | `setattr` Position/Size, `rotate`, `scale`, `size` | `getattr` | (in csb) | dirty | yes |
| Bindings | `addbinding`, `delbinding`, `listbindings` | `listbindings` | `.bindings.json` | dirty | yes |
| VectorMap | `vmap-add-layer`, `vmap-add-prim`, `vmap-add-keyframe`, `vmap-set-anim` | `getattr`, `dump` | `.vmaps.json` | dirty | yes |
| LitSprite (2D lights) | `litsprite-add-light` | `getattr OCSLights.*` | (TODO) | dirty | (TODO) |
| SvgImage / Lottie ctype | (TODO) | `describe SvgImageObjectData` | (TODO) | dirty | (TODO) |
| Mode (Edit/View/Side/Top) | `mode` | `mode` | n/a | mode | yes |
| Selection | `select` (name / path / chain) | `selected` | n/a | selection | yes |
| Bake / convert | `svg-rasterize`, `svg-bake` | n/a | n/a | n/a | (TODO) |
| Visual capture | `screenshot`, `screenshot-canvas`, `preview-visual` | n/a | n/a | n/a | n/a |
| Doc IO | `save`, `reload`, `export`, `script`, `apply-to-scenes` | n/a | sibling files | saved, opened, exported | menu |
| Activity | `tree-since`, `validate`, `events` | n/a | n/a | n/a | n/a |
| Catalog | `download`, sprite-search default provider | n/a | n/a | n/a | yes |
| Codegen | `gen-controller` | n/a | n/a | n/a | n/a |

## Items missing the rule

- `LitSpriteObjectData`: no Properties UI section yet. RPC + binding shipped.
  Sidecar export not wired (lights only persist inside the `.csd`; runtime
  game can read via `setLights` in code but no JSON yet).
- `SvgImageObjectData`: no `setattr <path> FileData Path=…` short-cut
  Properties UI; user has to use generic `setattr`. No `applySvgFromJson`
  sidecar.
- `LottieObjectData`: no Properties UI section; runtime is a stub (waiting
  on rlottie). `setjsonpath` RPC missing.
- `VectorMap`: per-primitive editor exists in Properties, but `vmap-add-
  layer` / `vmap-add-prim` / `vmap-add-keyframe` are RPC-only. No
  Sprite-Catalog tile for "Insert VectorMap".

These three node types should follow up with the same triple
(`RPC + sidecar + Properties section`) before being declared shipped.

## Quick checklist before merging a feature

```
[ ] RPC verb registered in EditorInspect dispatch
[ ] Added to kMutating if it mutates state
[ ] Help text updated, alphabetical inside section
[ ] pushUndo + syncNodeCsdToAx in the mutation path
[ ] Properties panel section for any non-numeric edit
[ ] Sidecar JSON writer added when state lives outside .csb
[ ] Matching applyXFromJson on the runtime side
[ ] Watch event emitted when state changes
[ ] At least one round-trip RPC test in the commit log
```
