#include "CsdModel.h"

#include <fmt/format.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace opencs
{

// --------------------------------------------------------------------------
// Node

std::vector<Node> Node::children() const
{
    std::vector<Node> out;
    auto childrenEl = _el.child(tags::kChildren);
    if (!childrenEl) return out;
    for (auto c = childrenEl.first_child(); c; c = c.next_sibling())
    {
        auto nameSv = c.name();  // pugi string_view_t
        // Use strcmp-style comparison against our tag constants.
        if (nameSv == pugi::string_view_t(tags::kAbstractNode) ||
            nameSv == pugi::string_view_t(tags::kObjectData))
        {
            out.emplace_back(c);
        }
    }
    return out;
}

void Node::collectDescendants(std::vector<Node>& out) const
{
    out.push_back(*this);
    for (auto& c : children()) c.collectDescendants(out);
}

void Node::setAttr(std::string_view key, std::string_view value)
{
    std::string k(key);
    auto a = _el.attribute(k.c_str());
    if (a)
        a.set_value(std::string(value).c_str());
    else
        _el.append_attribute(k.c_str()).set_value(std::string(value).c_str());
}

void Node::removeAttr(std::string_view key)
{
    _el.remove_attribute(std::string(key).c_str());
}

std::vector<std::pair<std::string, std::string>> Node::subAttrs(
    std::string_view tag) const
{
    std::vector<std::pair<std::string, std::string>> out;
    auto sub = _el.child(std::string(tag).c_str());
    if (!sub) return out;
    for (auto a = sub.first_attribute(); a; a = a.next_attribute())
    {
        out.emplace_back(std::string(a.name()), std::string(a.as_string()));
    }
    return out;
}

void Node::setSubAttrs(
    std::string_view tag,
    const std::vector<std::pair<std::string, std::string>>& attrs)
{
    std::string t(tag);
    auto sub = _el.child(t.c_str());
    if (attrs.empty())
    {
        if (sub) _el.remove_child(sub);
        return;
    }
    if (!sub) sub = _el.append_child(t.c_str());

    // Remove attributes that are no longer present, preserve order for the rest.
    std::vector<std::string> toRemove;
    for (auto a = sub.first_attribute(); a; a = a.next_attribute())
    {
        std::string aname(a.name());
        auto found = std::find_if(
            attrs.begin(), attrs.end(),
            [&](const auto& p) { return p.first == aname; });
        if (found == attrs.end()) toRemove.push_back(aname);
    }
    for (const auto& k : toRemove) sub.remove_attribute(k.c_str());

    for (const auto& [k, v] : attrs)
    {
        auto existing = sub.attribute(k.c_str());
        if (existing)
            existing.set_value(v.c_str());
        else
            sub.append_attribute(k.c_str()).set_value(v.c_str());
    }
}

void Node::writePair(
    std::string_view tag,
    std::string_view xKey, float xValue,
    std::string_view yKey, float yValue)
{
    // Format values to the same "%.4f" convention Cocos Studio uses, so the
    // saved XML matches what CS originally wrote and minimises diff noise.
    char xBuf[32], yBuf[32];
    std::snprintf(xBuf, sizeof(xBuf), "%.4f", xValue);
    std::snprintf(yBuf, sizeof(yBuf), "%.4f", yValue);

    // Start from whatever attrs are already on <tag>. We preserve the order
    // of existing attrs and only overwrite xKey/yKey — other fields (e.g.
    // Type="PlistSubImage" on a FileData element) are left alone.
    auto current = subAttrs(tag);
    std::vector<std::pair<std::string, std::string>> updated;
    updated.reserve(current.size() + 2);

    bool wroteX = false;
    bool wroteY = false;
    for (const auto& [key, value] : current)
    {
        if (key == xKey)      { updated.emplace_back(key, xBuf); wroteX = true; }
        else if (key == yKey) { updated.emplace_back(key, yBuf); wroteY = true; }
        else                    updated.emplace_back(key, value);
    }
    if (!wroteX) updated.emplace_back(std::string(xKey), xBuf);
    if (!wroteY) updated.emplace_back(std::string(yKey), yBuf);

    setSubAttrs(tag, updated);
}

std::vector<Node::ImageRef> Node::imageRefs() const
{
    // Known Cocos Studio sub-element tag names that carry a Path attribute
    // pointing at an image / font / plist asset.
    static constexpr const char* kAssetTags[] = {
        "FileData",
        "TextureFile",
        "ImageFile",
        "ImageFileData",
        "NormalFileData",
        "PressedFileData",
        "DisabledFileData",
        "SelectedFileData",
        "BackGroundImageFileData",
        "LoadingBarFileData",
        "BallNormalFileData",
        "BallPressedFileData",
        "BallDisabledFileData",
        "FrameImage",
        "ImageFileName",
        "FontResource",
    };

    std::vector<ImageRef> out;
    for (auto c = _el.first_child(); c; c = c.next_sibling())
    {
        std::string tag(c.name());
        bool known = false;
        for (const char* k : kAssetTags)
        {
            if (tag == k) { known = true; break; }
        }
        if (!known) continue;

        ImageRef ref;
        ref.kind  = std::move(tag);
        ref.path  = std::string(c.attribute("Path").as_string(""));
        ref.plist = std::string(c.attribute("Plist").as_string(""));
        ref.type  = std::string(c.attribute("Type").as_string(""));
        if (ref.path.empty() && ref.plist.empty()) continue;
        out.push_back(std::move(ref));
    }
    return out;
}

std::string Node::displayLabel() const
{
    auto n = name();
    auto c = ctype();
    auto base = !n.empty() ? n : (!c.empty() ? c : std::string(_el.name()));
    if (!c.empty()) return fmt::format("{}  ·  {}", base, c);
    return base;
}

// --------------------------------------------------------------------------
// CsdDocument

std::string CsdDocument::fileType() const
{
    return std::string(doc.child(tags::kGameFile)
                           .child(tags::kPropGroup)
                           .attribute("Type")
                           .as_string(""));
}

std::string CsdDocument::version() const
{
    return std::string(doc.child(tags::kGameFile)
                           .child(tags::kPropGroup)
                           .attribute("Version")
                           .as_string(""));
}

std::vector<Node> CsdDocument::allNodes() const
{
    std::vector<Node> out;
    if (rootNode.valid()) rootNode.collectDescendants(out);
    return out;
}

// --------------------------------------------------------------------------
// Load / save

namespace
{

/// Locate the top-level <ObjectData> under <Content>/<Content>.
pugi::xml_node findRootObjectData(pugi::xml_node gameFile)
{
    auto outer = gameFile.child(tags::kContent);
    if (!outer) return {};
    auto inner = outer.child(tags::kContent);
    auto content = inner ? inner : outer;
    return content.child(tags::kObjectData);
}


/// Rewrite decimal character references (&#10;) → hex (&#xA;). Cocos Studio
/// uses hex notation for control characters inside attribute values (newlines
/// in LabelText); pugixml emits decimal. Same post-processing the Python
/// version applies.
void rewriteCharRefsToHex(std::string& xml)
{
    auto replaceAll = [&](std::string_view from, std::string_view to) {
        size_t pos = 0;
        while ((pos = xml.find(from, pos)) != std::string::npos)
        {
            xml.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll("&#10;", "&#xA;");
    replaceAll("&#13;", "&#xD;");
    replaceAll("&#9;", "&#x9;");
}

/// Ensure self-closing tags have a space before `/>` (Cocos Studio style:
/// `<Foo X="1" />` not `<Foo X="1"/>`). Critical for clean git diffs.
void addSpaceBeforeSelfClosing(std::string& xml)
{
    std::string out;
    out.reserve(xml.size() + 256);
    for (size_t i = 0; i < xml.size(); ++i)
    {
        char b = xml[i];
        if (b == '/' && i + 1 < xml.size() && xml[i + 1] == '>')
        {
            char prev = out.empty() ? '\0' : out.back();
            if (prev != ' ' && prev != '\t' && prev != '\n' && prev != '\r')
                out.push_back(' ');
        }
        out.push_back(b);
    }
    xml = std::move(out);
}

}  // namespace

CsdDocument loadCsd(const std::filesystem::path& p)
{
    CsdDocument doc;
    doc.path = p;
    auto result = doc.doc.load_file(
        p.string().c_str(),
        pugi::parse_default | pugi::parse_ws_pcdata,
        pugi::encoding_auto);
    if (!result)
        throw std::runtime_error(fmt::format(
            "{}: {}", p.string(), result.description()));
    auto root = doc.doc.child(tags::kGameFile);
    if (!root)
        throw std::runtime_error(fmt::format(
            "{}: expected <{}> root", p.string(), tags::kGameFile));
    auto rootEl = findRootObjectData(root);
    doc.rootNode = Node(rootEl);
    return doc;
}

void CsdDocument::reloadFromString(const std::string& xml)
{
    pugi::xml_document fresh;
    auto result = fresh.load_string(
        xml.c_str(), pugi::parse_default | pugi::parse_ws_pcdata);
    if (!result)
        throw std::runtime_error(fmt::format(
            "reloadFromString: {}", result.description()));
    doc.reset(fresh);
    rootNode = Node(findRootObjectData(doc.child(tags::kGameFile)));
    dirty = true;
}

std::filesystem::path saveCsd(
    CsdDocument& doc, const std::filesystem::path* path)
{
    std::filesystem::path target = path ? *path : doc.path;

    std::ostringstream ss;
    // format_raw: no pretty-print changes, no indentation transformation.
    // Cocos Studio's own indentation style is preserved as-is from parse.
    doc.doc.save(ss, "", pugi::format_raw | pugi::format_no_declaration);

    std::string xml = ss.str();
    addSpaceBeforeSelfClosing(xml);
    rewriteCharRefsToHex(xml);

    std::ofstream out(target, std::ios::binary);
    if (!out) throw std::runtime_error(
        fmt::format("cannot open {} for writing", target.string()));
    out.write(xml.data(), static_cast<std::streamsize>(xml.size()));

    doc.dirty = false;
    return target;
}

}  // namespace opencs
