// Editor-side binding between the OCS document model (.csd XML) and the
// OCSExtension runtime node `VectorMapNode`. CSLoader.txt-format doesn't
// know about `VectorMapObjectData` yet (no FlatBuffers schema entry), so
// the editor instantiates the runtime node manually as a child of
// whatever ax::Node CSLoader produced and populates its layer list from
// the XML sub-element `<VectorMapData>`.
//
// Schema written into .csd:
//   <AbstractNodeData Name="…" ctype="VectorMapObjectData">
//     <Size X="…" Y="…"/>  <Position X="…" Y="…"/>  …
//     <VectorMapData>
//       <Layer Name="bg" Visible="True">
//         <Polygon  Points="x,y x,y …" Fill="r,g,b,a"
//                   Stroke="r,g,b,a" StrokeWidth="2"/>
//         <Polyline Points="x,y x,y …" Stroke="r,g,b,a" StrokeWidth="1"/>
//         <Circle   X="…" Y="…" Radius="…" Fill="r,g,b,a"/>
//       </Layer>
//     </VectorMapData>
//   </AbstractNodeData>

#pragma once

#include "UILayoutEditor/CsdModel.h"
#include "OCSExtension/VectorMapNode.h"

namespace ax { class Node; }

namespace opencs
{

/// True when `n` is a VectorMap-ish ctype the editor should render via
/// the OCSExtension runtime path.
bool isVectorMapNode(const Node& n);

/// Parse the <VectorMapData> sub-element under `n`. Returns an empty
/// vector when the element is missing or malformed.
std::vector<ocs::ext::VectorLayer> parseVectorMapLayers(const Node& n);

/// Ensure `ax` has a `VectorMapNode` child (looked up by tag) and
/// re-populates its layers from the current csd XML. Safe to call every
/// sync — the existing VectorMapNode is reused, only its layer vector
/// is rebuilt. No-op when `ax` is null.
void syncVectorMapToAx(const Node& csd, ax::Node* ax);

}  // namespace opencs
