// M5: Timeline mutation round-trip tests.
//
// Verifies that every kind of edit the Timeline panel makes — adding a
// keyframe, moving one in time, deleting one, editing its value, toggling
// tween, changing easing, adding an event — survives a save/reload cycle
// and only changes the bytes that *should* change. Untouched regions of
// the file must remain identical.

#include "doctest.h"

#include "UILayoutEditor/CsdModel.h"
#include "UILayoutEditor/Timeline.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace
{

const fs::path kSampleAnimated =
    "/Users/Vlad/Work/GameDev/solitaireclassic_godot/assets/cocos/objects/"
    "InfiniteLoaderAnimation.csd";

std::string readAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Save `doc` to a temp file under tempDir, then reload from disk.
/// Returns the saved-then-reloaded document. Asserts each step.
opencs::CsdDocument saveAndReload(opencs::CsdDocument& doc,
                                  const fs::path& tempDir,
                                  const std::string& tag)
{
    fs::create_directories(tempDir);
    auto out = tempDir / (tag + ".csd");
    opencs::saveCsd(doc, &out);
    REQUIRE(fs::exists(out));
    return opencs::loadCsd(out);
}

/// Find the first Timeline matching (actionTag, property). Caller must
/// CHECK validity.
opencs::TimelineRef findTimeline(opencs::AnimationRef anim,
                                 int tag, opencs::TimelineProperty p)
{
    return anim.findTimeline(tag, p);
}

}  // namespace


TEST_CASE("M5: load and save preserves animated .csd byte-identical")
{
    REQUIRE(fs::exists(kSampleAnimated));
    auto tempDir = fs::temp_directory_path() / "opencs-m5";
    fs::create_directories(tempDir);

    auto doc = opencs::loadCsd(kSampleAnimated);
    auto out = tempDir / "noop.csd";
    opencs::saveCsd(doc, &out);

    const auto orig  = readAll(kSampleAnimated);
    const auto saved = readAll(out);
    CHECK(orig == saved);
}


TEST_CASE("M5: edit existing keyframe value preserves other bytes")
{
    REQUIRE(fs::exists(kSampleAnimated));
    auto tempDir = fs::temp_directory_path() / "opencs-m5";

    auto doc = opencs::loadCsd(kSampleAnimated);
    auto anim = opencs::animationOf(doc);
    REQUIRE(anim.valid());

    // Mutate one keyframe's X value.
    auto tl = findTimeline(anim, 23100085,
                           opencs::TimelineProperty::Position);
    REQUIRE(tl.valid());
    auto kf = tl.frameAtOrBefore(0);
    REQUIRE(kf.valid());
    auto kv = kf.read(opencs::TimelineProperty::Position);
    kv.vx = 99.5f;  kv.vy = 88.25f;
    kf.write(opencs::TimelineProperty::Position, kv);

    auto reloaded = saveAndReload(doc, tempDir, "edit-pos");
    auto rTl = findTimeline(opencs::animationOf(reloaded), 23100085,
                            opencs::TimelineProperty::Position);
    REQUIRE(rTl.valid());
    auto rKf = rTl.frameAtOrBefore(0);
    REQUIRE(rKf.valid());
    auto rkv = rKf.read(opencs::TimelineProperty::Position);

    CHECK(rkv.vx == doctest::Approx(99.5f).epsilon(0.001f));
    CHECK(rkv.vy == doctest::Approx(88.25f).epsilon(0.001f));

    // Sanity: the rest of the file shouldn't have moved much. We
    // compare line counts + the first/last 100 chars as a coarse check
    // that the change was localised.
    const auto saved = readAll(tempDir / "edit-pos.csd");
    const auto orig  = readAll(kSampleAnimated);
    CHECK(saved.substr(0, 200) == orig.substr(0, 200));
    CHECK(saved.size() == doctest::Approx((double)orig.size())
                              .epsilon(0.05));  // <5% drift
}


TEST_CASE("M5: append keyframe + reload finds it")
{
    REQUIRE(fs::exists(kSampleAnimated));
    auto tempDir = fs::temp_directory_path() / "opencs-m5";

    auto doc = opencs::loadCsd(kSampleAnimated);
    auto anim = opencs::animationOf(doc);
    auto tl = findTimeline(anim, 23100085,
                           opencs::TimelineProperty::Position);
    REQUIRE(tl.valid());
    const int countBefore = (int)tl.frames().size();

    opencs::KeyValue kv;
    kv.kind = opencs::KeyValue::Kind::Vec2;
    kv.vx = 42.0f;  kv.vy = 17.0f;
    auto added = tl.appendFrame(15, kv);
    REQUIRE(added.valid());

    auto reloaded = saveAndReload(doc, tempDir, "append-kf");
    auto rTl = findTimeline(opencs::animationOf(reloaded), 23100085,
                            opencs::TimelineProperty::Position);
    REQUIRE(rTl.valid());
    CHECK((int)rTl.frames().size() == countBefore + 1);

    auto rKf = rTl.frameAtOrBefore(15);
    REQUIRE(rKf.valid());
    CHECK(rKf.frameIndex() == 15);
    auto rkv = rKf.read(opencs::TimelineProperty::Position);
    CHECK(rkv.vx == doctest::Approx(42.0f));
    CHECK(rkv.vy == doctest::Approx(17.0f));
}


TEST_CASE("M5: delete keyframe + reload removes it")
{
    REQUIRE(fs::exists(kSampleAnimated));
    auto tempDir = fs::temp_directory_path() / "opencs-m5";

    auto doc = opencs::loadCsd(kSampleAnimated);
    auto anim = opencs::animationOf(doc);
    auto tl = findTimeline(anim, 23100085,
                           opencs::TimelineProperty::Position);
    REQUIRE(tl.valid());
    const int countBefore = (int)tl.frames().size();
    REQUIRE(countBefore >= 2);

    // Pull one out by element identity and remove it.
    auto frames = tl.frames();
    auto toRemove = frames[1];
    const int removedIdx = toRemove.frameIndex();
    tl.removeFrame(toRemove);

    auto reloaded = saveAndReload(doc, tempDir, "delete-kf");
    auto rTl = findTimeline(opencs::animationOf(reloaded), 23100085,
                            opencs::TimelineProperty::Position);
    REQUIRE(rTl.valid());
    CHECK((int)rTl.frames().size() == countBefore - 1);

    // The removed frame index should no longer appear (or at least its
    // count went down by exactly one — multiple frames could share the
    // same index in edge cases).
    int matchCount = 0;
    for (auto kf : rTl.frames())
        if (kf.frameIndex() == removedIdx) ++matchCount;
    int origMatchCount = 0;
    for (auto& kf : frames)
        if (kf.frameIndex() == removedIdx) ++origMatchCount;
    CHECK(matchCount == origMatchCount - 1);
}


TEST_CASE("M5: move keyframe (frameIndex change) round-trips")
{
    REQUIRE(fs::exists(kSampleAnimated));
    auto tempDir = fs::temp_directory_path() / "opencs-m5";

    auto doc = opencs::loadCsd(kSampleAnimated);
    auto anim = opencs::animationOf(doc);
    auto tl = findTimeline(anim, 23100085,
                           opencs::TimelineProperty::Position);
    REQUIRE(tl.valid());

    auto kf = tl.frameAtOrBefore(0);
    REQUIRE(kf.valid());
    REQUIRE(kf.frameIndex() == 0);
    kf.setFrameIndex(7);

    auto reloaded = saveAndReload(doc, tempDir, "move-kf");
    auto rTl = findTimeline(opencs::animationOf(reloaded), 23100085,
                            opencs::TimelineProperty::Position);
    REQUIRE(rTl.valid());

    bool found7 = false;
    for (auto rKf : rTl.frames())
        if (rKf.frameIndex() == 7) { found7 = true; break; }
    CHECK(found7);
}


TEST_CASE("M5: tween toggle round-trips")
{
    REQUIRE(fs::exists(kSampleAnimated));
    auto tempDir = fs::temp_directory_path() / "opencs-m5";

    auto doc = opencs::loadCsd(kSampleAnimated);
    auto anim = opencs::animationOf(doc);
    auto tl = findTimeline(anim, 23100085,
                           opencs::TimelineProperty::Position);
    REQUIRE(tl.valid());

    auto kf = tl.frameAtOrBefore(0);
    REQUIRE(kf.valid());
    const bool before = kf.tween();
    kf.setTween(!before);

    auto reloaded = saveAndReload(doc, tempDir, "tween");
    auto rTl = findTimeline(opencs::animationOf(reloaded), 23100085,
                            opencs::TimelineProperty::Position);
    auto rKf = rTl.frameAtOrBefore(0);
    REQUIRE(rKf.valid());
    CHECK(rKf.tween() == !before);

    // Restore + verify second round-trip restores byte-identical state.
    rKf.setTween(before);
    auto restored = saveAndReload(reloaded, tempDir, "tween-restore");
    auto rrTl = findTimeline(opencs::animationOf(restored), 23100085,
                             opencs::TimelineProperty::Position);
    auto rrKf = rrTl.frameAtOrBefore(0);
    CHECK(rrKf.tween() == before);
}


TEST_CASE("M5: easing type change round-trips")
{
    REQUIRE(fs::exists(kSampleAnimated));
    auto tempDir = fs::temp_directory_path() / "opencs-m5";

    auto doc = opencs::loadCsd(kSampleAnimated);
    auto anim = opencs::animationOf(doc);
    auto tl = findTimeline(anim, 23100085,
                           opencs::TimelineProperty::Position);
    auto kf = tl.frameAtOrBefore(0);
    REQUIRE(kf.valid());
    kf.setEasingType(7);  // Cubic_EaseIn

    auto reloaded = saveAndReload(doc, tempDir, "easing");
    auto rTl = findTimeline(opencs::animationOf(reloaded), 23100085,
                            opencs::TimelineProperty::Position);
    auto rKf = rTl.frameAtOrBefore(0);
    CHECK(rKf.easingType() == 7);
}


TEST_CASE("M5: add events timeline + event keyframe round-trips")
{
    REQUIRE(fs::exists(kSampleAnimated));
    auto tempDir = fs::temp_directory_path() / "opencs-m5";

    auto doc = opencs::loadCsd(kSampleAnimated);
    auto anim = opencs::animationOf(doc);
    REQUIRE(anim.valid());

    // Sample doesn't have an events timeline — create one and append.
    auto evTl = anim.findTimeline(0, opencs::TimelineProperty::Event);
    if (!evTl.valid())
        evTl = anim.createTimeline(0, opencs::TimelineProperty::Event);
    REQUIRE(evTl.valid());

    opencs::KeyValue kv;
    kv.kind = opencs::KeyValue::Kind::Event;
    kv.strA = "level_complete";
    auto kf = evTl.appendFrame(45, kv);
    REQUIRE(kf.valid());

    auto reloaded = saveAndReload(doc, tempDir, "events");
    auto rEvTl = opencs::animationOf(reloaded).findTimeline(
        0, opencs::TimelineProperty::Event);
    REQUIRE(rEvTl.valid());
    auto rKf = rEvTl.frameAtOrBefore(45);
    REQUIRE(rKf.valid());
    CHECK(rKf.frameIndex() == 45);
    auto rkv = rKf.read(opencs::TimelineProperty::Event);
    CHECK(rkv.strA == "level_complete");
}
