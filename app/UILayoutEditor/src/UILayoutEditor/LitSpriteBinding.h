// Editor-side binding for LitSpriteObjectData (OCSExtension ctype).
// CSLoader still doesn't know our ctype, so the editor instantiates
// a LitSpriteNode child of whatever ax node CSLoader produced and
// re-populates its texture + light list from the XML on every sync.

#pragma once

#include "UILayoutEditor/CsdModel.h"
#include "OCSExtension/LitSpriteNode.h"

namespace ax { class Node; }

namespace opencs
{

bool isLitSpriteNode(const Node& n);
void syncLitSpriteToAx(const Node& csd, ax::Node* ax,
                       const std::filesystem::path& assetsRoot);

}  // namespace opencs
