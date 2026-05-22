// Tests for the CsdDocument-level operations the Editor orchestrates:
// snapshot/restore (undo backbone), Z-order read/write, DFS path addressing,
// delete/duplicate/add via pugi, and attribute-only edits.
//
// These bypass the Editor class itself (which depends on axmol for its
// ax::Node maps and overlay rendering), so the tests stay in the existing
// axmol-free harness. The Editor's structural-mutation methods are thin
// wrappers around these same pugi operations — a regression in them would
// surface here first.

#include "doctest.h"

#include "UILayoutEditor/CsdModel.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static const fs::path kSampleDir =
    "/Users/Vlad/Work/GameDevBackup/solitaireclassic/"
    "ResourcesCocos/CocosProject/cocosstudio";


static std::string readAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}


/// Helper — serialize a pugi doc to an in-memory string. Matches what the
/// Editor's pushUndo stores on the undo stack.
static std::string serialize(const pugi::xml_document& d)
{
    std::ostringstream out;
    d.save(out, "  ", pugi::format_default, pugi::encoding_utf8);
    return out.str();
}


/// DFS child-index path from root to `n` — mirrors Editor::pathTo.
static std::vector<std::size_t> pathTo(const opencs::Node& root,
                                       const opencs::Node& target)
{
    std::vector<std::size_t> out;
    if (!target.valid() || !root.valid()) return out;
    if (target.element() == root.element()) return out;

    // Walk up from target collecting child indices, then reverse.
    auto el = target.element();
    while (el && el != root.element())
    {
        auto parent = el.parent();
        if (!parent) break;
        std::size_t idx = 0;
        for (auto s = parent.first_child(); s; s = s.next_sibling(), ++idx)
            if (s == el) { out.push_back(idx); break; }
        el = parent;
    }
    std::reverse(out.begin(), out.end());
    return out;
}


/// Inverse — walk indices from root — mirrors Editor::nodeAtPath.
static opencs::Node nodeAtPath(const opencs::Node& root,
                               const std::vector<std::size_t>& path)
{
    pugi::xml_node cur = root.element();
    for (auto idx : path)
    {
        std::size_t i = 0;
        pugi::xml_node c = cur.first_child();
        for (; c && i < idx; c = c.next_sibling(), ++i) {}
        if (!c) return opencs::Node{};
        cur = c;
    }
    return opencs::Node(cur);
}


TEST_CASE("CsdDocument snapshot round-trip preserves serialization")
{
    // The undo stack stores full-document XML snapshots. Restoring a
    // snapshot and reserializing must produce the exact same bytes —
    // otherwise undo would drift after a few operations.
    auto sample = kSampleDir / "objects" / "Button.csd";
    REQUIRE(fs::exists(sample));
    auto doc = opencs::loadCsd(sample);

    std::string snap = serialize(doc.doc);
    doc.reloadFromString(snap);
    std::string after = serialize(doc.doc);

    CHECK(snap == after);
    CHECK(doc.rootNode.valid());
}


TEST_CASE("reloadFromString restores full scene tree")
{
    auto sample = kSampleDir / "objects" / "Button.csd";
    auto doc    = opencs::loadCsd(sample);

    // Capture snapshot before mutating.
    std::string snap = serialize(doc.doc);

    // Mutate: rename button node and add a bogus attribute.
    std::vector<opencs::Node> all;
    doc.rootNode.collectDescendants(all);
    for (auto& n : all)
        if (n.name() == "button") n.setAttr("Tag", "1234567");

    // Restore snapshot — bogus attr should be gone.
    doc.reloadFromString(snap);
    std::vector<opencs::Node> after;
    doc.rootNode.collectDescendants(after);
    for (auto& n : after)
        if (n.name() == "button")
            CHECK(n.attr("Tag") != "1234567");
}


TEST_CASE("Z-order attribute persists and round-trips")
{
    // Editor::setZOrder writes the ZOrder attribute on the AbstractNodeData
    // element. Read-back and reload-from-string must both preserve it.
    auto sample = kSampleDir / "objects" / "Button.csd";
    auto doc    = opencs::loadCsd(sample);

    std::vector<opencs::Node> all;
    doc.rootNode.collectDescendants(all);
    opencs::Node target;
    for (auto& n : all) if (n.name() == "button") target = n;
    REQUIRE(target.valid());

    target.setAttr("ZOrder", "42");
    CHECK(target.attr("ZOrder") == "42");

    // Round-trip through serialize/reload and check the attr survives.
    std::string snap = serialize(doc.doc);
    doc.reloadFromString(snap);
    std::vector<opencs::Node> r;
    doc.rootNode.collectDescendants(r);
    bool found = false;
    for (auto& n : r)
        if (n.name() == "button")
        {
            CHECK(n.attr("ZOrder") == "42");
            found = true;
        }
    CHECK(found);
}


TEST_CASE("DFS path addressing is a bijection")
{
    // Editor uses pathTo/nodeAtPath to preserve selection across undo/redo.
    // The two must be inverses for every node in the tree.
    auto sample = kSampleDir / "objects" / "Button.csd";
    auto doc    = opencs::loadCsd(sample);

    std::vector<opencs::Node> all;
    doc.rootNode.collectDescendants(all);
    REQUIRE(all.size() > 1);

    int roundTripped = 0;
    for (auto& n : all)
    {
        auto path    = pathTo(doc.rootNode, n);
        auto resolve = nodeAtPath(doc.rootNode, path);
        REQUIRE(resolve.valid());
        CHECK(resolve.element() == n.element());
        ++roundTripped;
    }
    CHECK(roundTripped == (int)all.size());
}


TEST_CASE("deleteNode-equivalent removes from Children")
{
    // Editor::deleteNode is a pugi remove_child on the parent's <Children>.
    // After deletion the node must no longer appear in a fresh DFS walk.
    auto sample = kSampleDir / "objects" / "Button.csd";
    auto doc    = opencs::loadCsd(sample);

    std::vector<opencs::Node> all;
    doc.rootNode.collectDescendants(all);
    opencs::Node btn;
    for (auto& n : all) if (n.name() == "button") btn = n;
    REQUIRE(btn.valid());

    auto el        = btn.element();
    auto parentCh  = el.parent();
    REQUIRE(std::string(parentCh.name()) == "Children");
    parentCh.remove_child(el);

    std::vector<opencs::Node> after;
    doc.rootNode.collectDescendants(after);
    for (auto& n : after)
        CHECK(n.name() != "button");
}


TEST_CASE("duplicateNode-equivalent deep-copies and inserts as sibling")
{
    // Editor::duplicateNode uses pugi deep-copy + append under the same
    // <Children>. Verify the copy is an independent subtree (mutating the
    // copy doesn't affect the original) and is picked up by DFS.
    auto sample = kSampleDir / "objects" / "Button.csd";
    auto doc    = opencs::loadCsd(sample);

    std::vector<opencs::Node> all;
    doc.rootNode.collectDescendants(all);
    opencs::Node btn;
    for (auto& n : all) if (n.name() == "button") btn = n;
    REQUIRE(btn.valid());

    auto el       = btn.element();
    auto parentCh = el.parent();
    auto copy     = parentCh.append_copy(el);
    REQUIRE(copy);

    // Touching the copy must not bleed back into the original.
    copy.attribute("Name").set_value("button_dup");
    CHECK(std::string(el.attribute("Name").as_string()) == "button");
    CHECK(std::string(copy.attribute("Name").as_string()) == "button_dup");

    std::vector<opencs::Node> after;
    doc.rootNode.collectDescendants(after);
    int namedOrig = 0, namedDup = 0;
    for (auto& n : after)
    {
        if (n.name() == "button")     ++namedOrig;
        if (n.name() == "button_dup") ++namedDup;
    }
    CHECK(namedOrig == 1);
    CHECK(namedDup  == 1);
}


TEST_CASE("writePair formats floats with four-decimal precision")
{
    // The editor uses Node::writePair for every Position/Size/Scale/Anchor
    // edit. Format drift here would corrupt every property-panel commit.
    auto sample = kSampleDir / "objects" / "Button.csd";
    auto doc    = opencs::loadCsd(sample);

    std::vector<opencs::Node> all;
    doc.rootNode.collectDescendants(all);
    opencs::Node btn;
    for (auto& n : all) if (n.name() == "button") btn = n;
    REQUIRE(btn.valid());

    btn.writePair("Position", "X", 123.5f, "Y", -7.25f);
    auto pos = btn.subAttrs("Position");
    std::string x, y;
    for (auto& p : pos)
    {
        if (p.first == "X") x = p.second;
        if (p.first == "Y") y = p.second;
    }
    CHECK(x == "123.5000");
    CHECK(y == "-7.2500");
}


TEST_CASE("addChildNode-equivalent appends AbstractNodeData under Children")
{
    // Editor::addChildNode creates <Children> if missing and appends an
    // <AbstractNodeData ctype="..."> element. Verify the shape is what
    // CSLoader expects (sub-element under <Children>, ctype attribute set).
    auto sample = kSampleDir / "objects" / "Button.csd";
    auto doc    = opencs::loadCsd(sample);

    auto rootEl = doc.rootNode.element();
    auto children = rootEl.child("Children");
    if (!children) children = rootEl.append_child("Children");

    auto fresh = children.append_child("AbstractNodeData");
    fresh.append_attribute("Name").set_value("new_node");
    fresh.append_attribute("ctype").set_value("NodeObjectData");

    std::vector<opencs::Node> after;
    doc.rootNode.collectDescendants(after);
    bool found = false;
    for (auto& n : after)
        if (n.name() == "new_node")
        {
            CHECK(n.ctype() == "NodeObjectData");
            CHECK(n.tag() == "AbstractNodeData");
            found = true;
        }
    CHECK(found);
}


TEST_CASE("subAttrs round-trip empty map clears sub-element")
{
    // setSubAttrs with empty attrs deletes the sub-element — used to
    // clear per-node layout constraints from the Layout panel.
    auto sample = kSampleDir / "objects" / "Button.csd";
    auto doc    = opencs::loadCsd(sample);

    std::vector<opencs::Node> all;
    doc.rootNode.collectDescendants(all);
    opencs::Node btn;
    for (auto& n : all) if (n.name() == "button") btn = n;
    REQUIRE(btn.valid());

    // Make sure Position exists first, then wipe it.
    REQUIRE(!btn.subAttrs("Position").empty());
    btn.setSubAttrs("Position", {});
    CHECK(btn.subAttrs("Position").empty());
}


TEST_CASE("undo stack deep state after two mutations")
{
    // Three-way assertion: snapshot-A, mutate, snapshot-B, mutate again,
    // restore snapshot-B, state matches the post-first-mutation state.
    auto sample = kSampleDir / "objects" / "Button.csd";
    auto doc    = opencs::loadCsd(sample);

    std::string snapA = serialize(doc.doc);

    std::vector<opencs::Node> all;
    doc.rootNode.collectDescendants(all);
    opencs::Node btn;
    for (auto& n : all) if (n.name() == "button") btn = n;
    REQUIRE(btn.valid());

    btn.setAttr("ZOrder", "5");
    std::string snapB = serialize(doc.doc);
    btn.setAttr("ZOrder", "99");

    // Restore B — ZOrder should be 5 again.
    doc.reloadFromString(snapB);
    std::vector<opencs::Node> r;
    doc.rootNode.collectDescendants(r);
    for (auto& n : r)
        if (n.name() == "button") CHECK(n.attr("ZOrder") == "5");

    // Restore A — ZOrder should be absent or not "5".
    doc.reloadFromString(snapA);
    std::vector<opencs::Node> r2;
    doc.rootNode.collectDescendants(r2);
    for (auto& n : r2)
        if (n.name() == "button") CHECK(n.attr("ZOrder") != "5");
}
