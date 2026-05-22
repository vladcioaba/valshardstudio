// CSD model round-trip tests (C++ port of tests/test_csd_roundtrip.py).
//
// Each test loads a sample .csd from the SolitaireClassic resources, saves it
// back, and byte-compares against the original. Matches the Python test's
// strictest guarantee: untouched documents round-trip byte-identical.

// doctest in axmol is split (header + .cpp), so declarations live here and the
// implementation + main() are in a sibling doctest_main.cpp.
#include "doctest.h"

#include "UILayoutEditor/CsdModel.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static const fs::path kSampleDir =
    "/Users/Vlad/Work/GameDevBackup/solitaireclassic/"
    "ResourcesCocos/CocosProject/cocosstudio";


static std::vector<fs::path> gatherSamples()
{
    std::vector<fs::path> out;
    if (!fs::exists(kSampleDir)) return out;
    for (auto& e : fs::recursive_directory_iterator(kSampleDir))
    {
        if (e.is_regular_file() && e.path().extension() == ".csd")
            out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}


static std::string readAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Trim trailing whitespace (matches the Python test's `original.rstrip()`).
static std::string rtrim(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' '  || s.back() == '\t'))
        s.pop_back();
    return s;
}


TEST_CASE("load and save preserves bytes")
{
    auto samples = gatherSamples();
    REQUIRE(!samples.empty());

    auto tempDir = fs::temp_directory_path() / "opencs-csd-roundtrip";
    fs::create_directories(tempDir);

    int checked = 0;
    for (const auto& sample : samples)
    {
        auto doc = opencs::loadCsd(sample);
        auto out = tempDir / sample.filename();
        opencs::saveCsd(doc, &out);

        std::string orig = rtrim(readAll(sample));
        std::string saved = rtrim(readAll(out));
        CHECK_MESSAGE(orig == saved,
                      "round-trip mismatch for " << sample.filename().string());
        ++checked;
    }
    MESSAGE("round-tripped " << checked << " .csd files");
}


TEST_CASE("scene tree walk")
{
    auto sample = kSampleDir / "objects" / "Button.csd";
    REQUIRE(fs::exists(sample));

    auto doc = opencs::loadCsd(sample);
    REQUIRE(doc.rootNode.valid());
    CHECK(doc.rootNode.ctype() == "GameLayerObjectData");

    auto children = doc.rootNode.children();
    bool foundButton = false;
    for (auto& c : children)
        if (c.ctype() == "ButtonObjectData") foundButton = true;
    CHECK(foundButton);
}


TEST_CASE("set attr persists")
{
    auto sample = kSampleDir / "objects" / "Button.csd";
    auto doc = opencs::loadCsd(sample);

    // Find the "button" node by name.
    std::vector<opencs::Node> all;
    doc.rootNode.collectDescendants(all);
    opencs::Node btn;
    for (auto& n : all) if (n.name() == "button") btn = n;
    REQUIRE(btn.valid());

    btn.setAttr("Tag", "9999");

    auto out = fs::temp_directory_path() / "Button_edited.csd";
    opencs::saveCsd(doc, &out);

    auto reloaded = opencs::loadCsd(out);
    std::vector<opencs::Node> r;
    reloaded.rootNode.collectDescendants(r);
    for (auto& n : r)
        if (n.name() == "button")
            CHECK(n.attr("Tag") == "9999");
}


TEST_CASE("set sub attrs persists")
{
    auto sample = kSampleDir / "objects" / "Button.csd";
    auto doc = opencs::loadCsd(sample);

    std::vector<opencs::Node> all;
    doc.rootNode.collectDescendants(all);
    opencs::Node txt;
    for (auto& n : all) if (n.name() == "txtLabel") txt = n;
    REQUIRE(txt.valid());

    auto attrs = txt.subAttrs("Position");
    for (auto& p : attrs)
    {
        if (p.first == "X") p.second = "42.0000";
        if (p.first == "Y") p.second = "13.0000";
    }
    txt.setSubAttrs("Position", attrs);

    auto out = fs::temp_directory_path() / "Button_pos.csd";
    opencs::saveCsd(doc, &out);

    auto reloaded = opencs::loadCsd(out);
    std::vector<opencs::Node> r;
    reloaded.rootNode.collectDescendants(r);
    for (auto& n : r)
    {
        if (n.name() == "txtLabel")
        {
            auto pos = n.subAttrs("Position");
            std::string x, y;
            for (auto& p : pos) { if (p.first == "X") x = p.second; if (p.first == "Y") y = p.second; }
            CHECK(x == "42.0000");
            CHECK(y == "13.0000");
        }
    }
}
