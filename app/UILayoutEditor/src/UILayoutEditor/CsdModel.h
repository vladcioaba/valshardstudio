// Lossless CSD (.csd XML) model + round-trip serialization.
//
// Mirrors the Python walshard/model/csd.py module — same guarantees:
// * load_csd / save_csd round-trips byte-identical when no edits are made.
// * Saving a single attribute change moves only those bytes in the output.
//
// The file format:
//   <GameFile>
//     <PropertyGroup Name="…" Type="…" ID="…" Version="…" />
//     <Content ctype="GameProjectContent">
//       <Content>
//         <Animation Duration="…" Speed="…" />
//         <ObjectData Name="…" ctype="GameLayerObjectData">
//           ...<AnchorPoint/>...<Position/>...
//           <Children>
//             <AbstractNodeData Name="…" ctype="ButtonObjectData">
//               ...
//             </AbstractNodeData>
//             …
//           </Children>
//         </ObjectData>
//       </Content>
//     </Content>
//   </GameFile>

#pragma once

#include <pugixml.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace opencs
{

class Node;

/// Element tag names used by Cocos Studio.
namespace tags
{
inline constexpr const char* kGameFile    = "GameFile";
inline constexpr const char* kPropGroup   = "PropertyGroup";
inline constexpr const char* kContent     = "Content";
inline constexpr const char* kObjectData  = "ObjectData";
inline constexpr const char* kAbstractNode = "AbstractNodeData";
inline constexpr const char* kChildren    = "Children";
}  // namespace tags


/// Wrapper around a single <ObjectData>/<AbstractNodeData> element.
/// All mutations write directly into the underlying pugi::xml_node so
/// serialization is just `doc.save_file(path)` — no separate model state
/// to reconcile.
class Node
{
public:
    Node() = default;
    explicit Node(pugi::xml_node el) : _el(el) {}

    pugi::xml_node element() const noexcept { return _el; }
    bool valid() const noexcept { return !_el.empty(); }

    // -- identity ---------------------------------------------------------

    std::string name() const
    {
        return std::string(_el.attribute("Name").as_string(""));
    }
    void setName(std::string_view v)
    {
        auto a = _el.attribute("Name");
        if (a) a.set_value(std::string(v).c_str());
        else _el.append_attribute("Name").set_value(std::string(v).c_str());
    }

    std::string ctype() const
    {
        return std::string(_el.attribute("ctype").as_string(""));
    }
    std::string tag() const { return std::string(_el.name()); }

    std::optional<int> nodeTag() const
    {
        auto a = _el.attribute("Tag");
        if (a.empty()) return std::nullopt;
        return a.as_int();
    }
    std::optional<int> actionTag() const
    {
        auto a = _el.attribute("ActionTag");
        if (a.empty()) return std::nullopt;
        return a.as_int();
    }

    // -- children ---------------------------------------------------------

    std::vector<Node> children() const;
    /// Flat depth-first enumeration including this node.
    void collectDescendants(std::vector<Node>& out) const;

    // -- attribute access -------------------------------------------------

    std::string attr(std::string_view key, std::string_view dflt = "") const
    {
        auto a = _el.attribute(std::string(key).c_str());
        if (a) return std::string(a.as_string());
        return std::string(dflt);
    }

    void setAttr(std::string_view key, std::string_view value);
    void removeAttr(std::string_view key);

    // Editor-only "locked" flag stored as a custom OCS attribute on
    // the element so it round-trips through save/load.
    //   None         — unlocked (attribute absent)
    //   Self         — only this node is locked; descendants stay
    //                   selectable. Stored as OCSLocked="Self".
    //   Propagating  — this node + every descendant are locked.
    //                   Stored as OCSLocked="True".
    // Editor::isEffectivelyLocked treats Self as "blocks self only"
    // and Propagating as "blocks self + all descendants".
    enum class LockMode { None, Self, Propagating };
    LockMode lockMode() const
    {
        const std::string v(_el.attribute("OCSLocked").as_string(""));
        if (v == "True") return LockMode::Propagating;
        if (v == "Self") return LockMode::Self;
        return LockMode::None;
    }
    void setLockMode(LockMode m)
    {
        if (m == LockMode::None)
        {
            _el.remove_attribute("OCSLocked");
            return;
        }
        const char* val = (m == LockMode::Propagating) ? "True" : "Self";
        auto a = _el.attribute("OCSLocked");
        if (a) a.set_value(val);
        else _el.append_attribute("OCSLocked").set_value(val);
    }
    // Back-compat shim: any lock mode counts as locked.
    bool locked() const { return lockMode() != LockMode::None; }
    void setLocked(bool v)
    {
        setLockMode(v ? LockMode::Propagating : LockMode::None);
    }

    // Visibility flag — Cocos Studio uses TWO attributes that both
    // hide a node at runtime:
    //   `Visible="False"`         — the static visibility flag
    //   `VisibleForFrame="False"` — the animation-timeline frame-0
    //                                initial visibility (CSLoader maps
    //                                this onto `setVisible(false)` at
    //                                load time too).
    // We treat the node as hidden if EITHER says false, so the eye
    // icon matches what the canvas actually renders.
    bool visible() const
    {
        auto isFalse = [&](const char* name) {
            auto a = _el.attribute(name);
            if (!a) return false;
            const std::string s(a.as_string(""));
            return s == "False" || s == "false" || s == "0";
        };
        return !(isFalse("Visible") || isFalse("VisibleForFrame"));
    }
    void setVisible(bool v)
    {
        // Symmetric toggle: showing a node clears BOTH attrs (so a
        // VisibleForFrame-hidden node can be brought back without
        // hand-editing the timeline). Hiding writes the static
        // `Visible="False"` and leaves any timeline override alone.
        if (v)
        {
            _el.remove_attribute("Visible");
            _el.remove_attribute("VisibleForFrame");
            return;
        }
        auto a = _el.attribute("Visible");
        if (a) a.set_value("False");
        else _el.append_attribute("Visible").set_value("False");
    }

    /// Read a sub-element's attributes (e.g. <Position X="…" Y="…"/>).
    /// Returns empty map if the sub-element doesn't exist.
    std::vector<std::pair<std::string, std::string>> subAttrs(
        std::string_view tag) const;

    /// Upsert a sub-element with the given attribute set. Empty map deletes
    /// the sub-element. Preserves attribute order on existing sub-elements.
    void setSubAttrs(
        std::string_view tag,
        const std::vector<std::pair<std::string, std::string>>& attrs);

    /// Write two named floats (e.g. X/Y or ScaleX/ScaleY) into `<Tag>`
    /// as "%.4f" strings, preserving any other attributes on that sub-
    /// element. Centralises a pattern the editor uses for Position / Size
    /// / Scale / AnchorPoint / PrePosition / PreSize updates.
    void writePair(
        std::string_view tag,
        std::string_view xKey, float xValue,
        std::string_view yKey, float yValue);

    // -- Image references -------------------------------------------------

    struct ImageRef
    {
        std::string kind;   ///< tag name (FileData, BackGroundImageFileData…)
        std::string path;   ///< Path attribute value
        std::string plist;  ///< Plist attribute (may be empty)
        std::string type;   ///< Type attribute (Normal / PlistSubImage / …)
    };

    /// Extract asset references from this node's immediate children (does not
    /// recurse into sub-trees). Cocos Studio stores an image reference in
    /// a typed sub-element like <FileData Type="PlistSubImage" Path="…"/>.
    std::vector<ImageRef> imageRefs() const;

    // -- UI display -------------------------------------------------------

    std::string displayLabel() const;

private:
    pugi::xml_node _el;
};


/// Top-level .csd document wrapper.
class CsdDocument
{
public:
    std::filesystem::path path;
    pugi::xml_document doc;
    Node rootNode;          ///< The <ObjectData> under Content/Content
    bool dirty = false;

    std::string fileType() const;    ///< e.g. "Layer", "Node", "Scene"
    std::string version() const;     ///< e.g. "3.10.0.0"

    /// Depth-first iteration including root.
    std::vector<Node> allNodes() const;

    /// Replace the in-memory document with the given XML string and refresh
    /// rootNode. Throws std::runtime_error on parse failure. All prior Node
    /// handles into this document become invalid.
    void reloadFromString(const std::string& xml);
};


/// Load a .csd from disk. Throws std::runtime_error on failure.
CsdDocument loadCsd(const std::filesystem::path& p);

/// Save a document. If `path` is null, writes to `doc.path`. Returns the
/// path written to.
std::filesystem::path saveCsd(
    CsdDocument& doc,
    const std::filesystem::path* path = nullptr);

}  // namespace opencs
