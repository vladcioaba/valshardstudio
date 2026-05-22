#include "AppDelegate.h"

#include "cocostudio/ActionTimeline/CSLoader.h"
#include "cocostudio/FlatBuffersSerialize.h"
#include "ImGui/ImGuiPresenter.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "platform/RenderViewImpl.h"
#include "UILayoutEditor/Editor.h"
#include "UILayoutEditor/CsdModel.h"
#include "UILayoutEditor/panels/Panels.h"
#include "OCSExtension/OCSExtension.h"
#include "rapidjson/document.h"
#include "UILayoutEditor/Log.h"
#include "NativeMenu.h"
#include "DragLabel.h"
#include "IpcBridge.h"
#include "PlatformSpawn.h"

#include "GLFW/glfw3.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <cstdio>
#include <memory>
#include <cstdlib>
#include <climits>

using namespace ax;
using namespace ax::extension;

std::string              AppDelegate::sCsdPath;
std::vector<std::string> AppDelegate::sCsdPaths;
std::string              AppDelegate::sPanelMode;
int                      AppDelegate::sPanelWindowW = 0;
int                      AppDelegate::sPanelWindowH = 0;

// Panels hidden locally because a peer registered them as detached.
// Shared between the render loop's singleton-sync block and the View
// menu's toggle handler — when the user toggles a panel that's
// currently detached, the toggle must erase the name from this set
// (so close-to-reattach doesn't auto-re-show the panel afterwards).
static std::set<std::string> gSuppressedByDetach;

static Size kDesignResolution = Size(1600, 900);


AppDelegate::AppDelegate() = default;
AppDelegate::~AppDelegate() = default;

void AppDelegate::initGfxContextAttrs()
{
    GfxContextAttrs attrs = {8, 8, 8, 8, 24, 8, 0};
    RenderView::setGfxContextAttrs(attrs);
}

namespace
{

/// Walk up from the .csd to find an asset root for FileUtils search paths.
/// Mirrors the Editor's heuristic in Editor.cpp:guessResourcesDir — accepts
/// any of {Resources, resources, assets, Assets} as a sibling, falls back
/// to ancestors literally NAMED resources/assets/cocos (case-insensitive)
/// for projects nested under e.g. `assets/cocos/scenes/`.
std::string guessResourcesDir(const std::string& csdPath)
{
    namespace fs = std::filesystem;
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    auto isAssetishName = [&](const fs::path& dir) {
        const std::string n = lower(dir.filename().string());
        return n == "resources" || n == "assets" || n == "cocos";
    };

    fs::path p(csdPath);
    std::error_code ec;
    // Pass 1: sibling named Resources / resources / assets / Assets.
    for (fs::path cur = p.parent_path();
         !cur.empty() && cur != cur.parent_path();
         cur = cur.parent_path())
    {
        for (const char* name : {"Resources", "resources", "assets", "Assets"})
        {
            auto candidate = cur / name;
            if (fs::is_directory(candidate, ec)) return candidate.string();
        }
    }
    // Pass 2: ancestor literally named one of the asset-ish tokens.
    for (fs::path cur = p.parent_path();
         !cur.empty() && cur != cur.parent_path();
         cur = cur.parent_path())
    {
        if (isAssetishName(cur) && fs::is_directory(cur, ec))
            return cur.string();
    }
    return {};
}

// Running-scene state hoisted here so File → Open can rebuild without
// restarting the app. `gScene` is the Director's root scene; we never
// replace it (ImGui lives on top of it via ImGuiPresenter), we just swap
// which tab's loaded root is visible under it. `gTopRoot` sits between
// the scene and the visible tab's loaded root — it carries the
// zoom/pan/rotation transforms, so the scene itself stays identity.
ax::Scene*                       gScene       = nullptr;
// Shared top-root for everything the Editor sees as "the canvas".
// Every tab's loadedRoot AND the editor's overlay DrawNode hang off
// this node. Only the active tab's loadedRoot is visible; inactive
// tabs stay in the tree but with setVisible(false). Tagged so the
// Editor can find it on the scene via getChildByTag.
ax::Node*                        gTopRoot     = nullptr;
// Snapshot of FileUtils search paths before we ever added a Resources/ or
// .csd-sibling path. Each tab activation resets to this and re-adds
// paths for the active project so asset resolution always matches the
// visible document.
std::vector<std::string>         gBaseSearchPaths;

// One entry per open document. `loadedRoot` is kept as a child of
// gTopRoot for the tab's entire lifetime — we just toggle visibility
// on switch instead of remove/re-add (CSLoader bakes textures at
// creation time, so reparenting isn't necessary and would churn
// driver-side state). The Editor owns per-tab pan/zoom/selection/
// undo state, so tab switches preserve it.
struct Tab
{
    std::string                      csdPath;
    ax::Node*                        loadedRoot = nullptr;
    std::unique_ptr<opencs::Editor>  editor;
    ax::Size                         designSize{1600, 900};
    std::vector<std::string>         searchPaths;   // snapshot for activation
    std::string                      label;         // short display name for tab
    bool                             requestClose = false;
};

std::vector<std::unique_ptr<Tab>> gTabs;   // unique_ptr keeps Tab addresses stable
int                               gActiveTab = -1;

// Drag-end detection shared between the EVENT_WINDOW_POSITIONED
// listener (writes this) and the render loop's monitor-DPI guard
// (reads it). Must be namespace-scope so both callbacks can see it.
std::atomic<int64_t>              gLastWindowMoveMs{0};

// (Historical) slot for the GLFW window-size handler axmol
// registered at startup. The trampoline path that used this was
// removed after it caused cascading freezes on repeated resizes.

opencs::Editor* activeEditor()
{
    if (gActiveTab < 0 || gActiveTab >= static_cast<int>(gTabs.size()))
        return nullptr;
    return gTabs[gActiveTab]->editor.get();
}

/// Derive a short unique tab label from the .csd file stem.
std::string labelFor(const std::string& csdPath)
{
    namespace fs = std::filesystem;
    auto stem = fs::path(csdPath).stem().string();
    if (stem.empty()) stem = "untitled";
    return stem;
}

/// Apply a tab's design resolution so the window framing matches the
/// visible document. Search paths are NOT swapped here: calling
/// setSearchPaths would stomp paths axmol adds lazily (shader cache,
/// etc.) — those additions are invisible to our snapshot taken at
/// startup. Each tab's project Resources/ dir is addSearchPath'd
/// once inside openTab() and stays in the global list for the app's
/// lifetime. Multi-project filename collisions are possible but
/// acceptable for now.
void applyTabEnvironment(const Tab& tab)
{
    if (auto* rv = Director::getInstance()->getRenderView())
        rv->setDesignResolutionSize(tab.designSize.width, tab.designSize.height,
                                    ResolutionPolicy::NO_BORDER);
    if (gTopRoot) gTopRoot->setContentSize(tab.designSize);
}

/// Hide the currently-active tab's loadedRoot and make `idx`'s visible.
/// No-op if `idx` already is active. Also clears the outgoing tab's
/// editor overlay so stale selection rectangles / layer projections
/// from the previous scene don't bleed through onto the new one.
void setActiveTab(int idx)
{
    if (idx == gActiveTab) return;
    if (gActiveTab >= 0 && gActiveTab < static_cast<int>(gTabs.size()))
    {
        auto& outgoing = *gTabs[gActiveTab];
        if (auto* r = outgoing.loadedRoot) r->setVisible(false);
        if (outgoing.editor) outgoing.editor->setCanvasActive(false);
    }
    gActiveTab = idx;
    if (idx >= 0 && idx < static_cast<int>(gTabs.size()))
    {
        auto& tab = *gTabs[idx];
        if (tab.loadedRoot) tab.loadedRoot->setVisible(true);
        if (tab.editor) tab.editor->setCanvasActive(true);
        applyTabEnvironment(tab);
        AppDelegate::sCsdPath = tab.csdPath;
    }
    else
    {
        AppDelegate::sCsdPath.clear();
    }
}

/// Parse a .csd for its authored Size. Returns (1600, 900) on any failure
/// so the caller never has to branch on exceptions.
ax::Size readDesignSize(const std::string& csdPath)
{
    ax::Size sz(1600, 900);
    try
    {
        auto csd   = opencs::loadCsd(csdPath);
        auto attrs = csd.rootNode.subAttrs("Size");
        float w = 0, h = 0;
        for (const auto& [k, v] : attrs)
        {
            if      (k == "X") w = std::strtof(v.c_str(), nullptr);
            else if (k == "Y") h = std::strtof(v.c_str(), nullptr);
        }
        if (w > 0 && h > 0) sz = ax::Size(w, h);
    }
    catch (const std::exception& e)
    {
        OCS_LOG_ERROR("app", "loadCsd failed: %s", e.what());
    }
    return sz;
}

/// Build the search-paths list for a tab: pristine base paths + a
/// sibling Resources/ dir (if one exists up the tree) + the .csd's own
/// parent dir. Same heuristic loadCsdIntoScene used before tabs.
std::vector<std::string> buildSearchPathsFor(const std::string& csdPath)
{
    namespace fs = std::filesystem;
    std::vector<std::string> paths = gBaseSearchPaths;
    if (auto res = guessResourcesDir(csdPath); !res.empty())
        paths.insert(paths.begin(), res);
    paths.push_back(fs::path(csdPath).parent_path().string());
    return paths;
}

/// Open a new document and return its tab index (or -1 on failure).
/// The tab starts HIDDEN — caller decides whether to activate it.
int openTab(const std::string& csdPath)
{
    namespace fs = std::filesystem;
    if (csdPath.empty()) return -1;

    // Duplicate detection: focus an already-open tab rather than
    // opening the same file twice.
    for (int i = 0; i < static_cast<int>(gTabs.size()); ++i)
        if (gTabs[i]->csdPath == csdPath) { setActiveTab(i); return i; }

    auto tab        = std::make_unique<Tab>();
    tab->csdPath    = csdPath;
    tab->label      = labelFor(csdPath);
    tab->designSize = readDesignSize(csdPath);
    tab->searchPaths = buildSearchPathsFor(csdPath);

    // Additive path registration: add the project's Resources/ and
    // the .csd's parent dir to FileUtils once, without ever clearing
    // the existing list. Setting search paths outright would wipe
    // anything axmol added lazily (shader cache dirs, etc.) and
    // trigger a crash later when those are re-queried.
    auto* fu = ax::FileUtils::getInstance();
    namespace fsns = std::filesystem;
    if (auto res = guessResourcesDir(csdPath); !res.empty())
        fu->addSearchPath(res, /*front=*/true);
    fu->addSearchPath(fsns::path(csdPath).parent_path().string());

    // Two lookup strategies in order:
    //   1. Sibling .csb next to the .csd (.csd → .csb in same dir).
    //   2. Project-relative form `<parent-dir-name>/<stem>.csb`, run
    //      through FileUtils so search paths (including the
    //      Resources/ we added in buildSearchPathsFor) take effect.
    //      Catches layouts like solitaireclassic where compiled
    //      assets live under `<proj>/Resources/scenes/` rather than
    //      next to the authoring `.csd`.
    fs::path csd(csdPath), csbSibling = csd;
    csbSibling.replace_extension(".csb");
    std::string csbResolved;
    if (fs::exists(csbSibling))
    {
        csbResolved = csbSibling.string();
    }
    else
    {
        const std::string rel =
            (csd.parent_path().filename() / csbSibling.filename()).string();
        std::string full =
            ax::FileUtils::getInstance()->fullPathForFilename(rel);
        if (!full.empty() && fs::exists(full))
            csbResolved = full;
    }

    // Auto-compile a sibling .csb when one isn't already on disk. The
    // editor needs a live axmol tree to render the canvas (and to
    // drive timeline playback against), and asking the user to
    // hand-export every scene before opening it is friction. Same
    // schema-aware codepath as Editor::exportCsb — sibling write,
    // empty error means success.
    if (csbResolved.empty())
    {
        auto* fbs = cocostudio::FlatBuffersSerialize::getInstance();
        const std::string err = fbs->serializeFlatBuffersWithXMLFile(
            csdPath, csbSibling.string());
        if (err.empty() && fs::exists(csbSibling))
        {
            csbResolved = csbSibling.string();
            OCS_LOG_INFO("app", "auto-compiled %s",
                         csbResolved.c_str());
        }
        else if (!err.empty())
        {
            OCS_LOG_ERROR("app", "auto-compile failed for %s: %s",
                          csdPath.c_str(), err.c_str());
        }
    }

    if (!csbResolved.empty())
    {
        if (auto* node = CSLoader::createNode(csbResolved))
        {
            node->setVisible(false);   // activation will flip this on
            gTopRoot->addChild(node);
            tab->loadedRoot = node;
            OCS_LOG_INFO("app", "loaded %s", csbResolved.c_str());
        }
        else
        {
            OCS_LOG_ERROR("app", "CSLoader failed for %s",
                          csbResolved.c_str());
        }
    }
    else
    {
        std::fprintf(stderr,
            "[ocs] no .csb found for %s (tried sibling + "
            "Resources/<scene-parent>/.csb) — rendering blank scene\n",
            csdPath.c_str());
    }

    tab->editor = std::make_unique<opencs::Editor>(csdPath);
    tab->editor->setAxmolRoot(tab->loadedRoot);

    // Default SpriteNlpHandler: posts the prompt to an HTTP endpoint
    // configured via OCS_NLP_URL (env), receives a JSON response with
    // either {"reply":"…"} or {"command":"<RPC cmd>"} which the editor
    // executes directly. Lets a user wire their own LLM gateway
    // without changing OCS code. Stays a noop when env var is unset.
    if (const char* nlpUrl = std::getenv("OCS_NLP_URL");
        nlpUrl && *nlpUrl)
    {
        const std::string endpoint = nlpUrl;
        tab->editor->setSpriteNlpHandler(
            [endpoint](const std::string& prompt, opencs::Editor& ed) -> std::string {
                if (prompt.empty()) return "(empty prompt)";
                // Minimal JSON escape for the prompt body. Wraps it
                // as {"prompt":"…","selection":"<pathStr>"} so the
                // remote can scope its answer to the current node.
                std::string esc;
                for (char c : prompt)
                {
                    if (c == '"')  esc += "\\\"";
                    else if (c == '\\') esc += "\\\\";
                    else if (c == '\n') esc += "\\n";
                    else if (c == '\r') {}
                    else esc.push_back(c);
                }
                std::string selPath;
                if (ed.selected().valid())
                {
                    auto p = ed.pathTo(ed.selected());
                    for (std::size_t i = 0; i < p.size(); ++i)
                    { if (i) selPath.push_back(','); selPath += std::to_string(p[i]); }
                }
                const std::string body =
                    "{\"prompt\":\"" + esc +
                    "\",\"selection\":\"" + selPath + "\"}";
                // popen with a HEREDOC-style body. Write a temp file
                // then curl -d @file to avoid shell-escaping the body.
                char tmp[L_tmpnam];
                std::tmpnam(tmp);
                {
                    std::ofstream o(tmp, std::ios::trunc);
                    o << body;
                }
                char cmd[2048];
                std::snprintf(cmd, sizeof(cmd),
                    "/usr/bin/curl -sL --max-time 20 "
                    "-H 'Content-Type: application/json' "
                    "--data @%s '%s' 2>/dev/null",
                    tmp, endpoint.c_str());
                FILE* f = popen(cmd, "r");
                std::string out;
                if (f)
                {
                    char buf[1024];
                    while (std::size_t n = std::fread(buf, 1, sizeof(buf), f))
                        out.append(buf, n);
                    pclose(f);
                }
                std::remove(tmp);
                if (out.empty()) return "no response from " + endpoint;

                rapidjson::Document doc;
                doc.Parse(out.c_str(), out.size());
                if (doc.HasParseError()) return out;  // raw echo
                if (doc.HasMember("command") && doc["command"].IsString())
                {
                    const std::string c = doc["command"].GetString();
                    return ed.handleInspectCommand(c);
                }
                if (doc.HasMember("reply") && doc["reply"].IsString())
                    return doc["reply"].GetString();
                return out;
            });
    }

    // Default SpriteSearchProvider: shell-out via /usr/bin/curl to an
    // endpoint configured by `OCS_SPRITE_SEARCH_URL`. The endpoint
    // appended with `?q=<query>` should return JSON of the shape:
    //   {"results":[{"label":"…","thumb":"…","download":"…","provider":"…"},…]}
    // Tolerates a flat array fallback. When the env var is unset the
    // catalog's Online tab keeps showing its "configure a provider"
    // placeholder; users can still wire a custom one via setSpriteSearchProvider.
    if (const char* base = std::getenv("OCS_SPRITE_SEARCH_URL");
        base && *base)
    {
        const std::string endpoint = base;
        tab->editor->setSpriteSearchProvider(
            [endpoint](const std::string& q) -> std::vector<opencs::SpriteResult> {
                std::vector<opencs::SpriteResult> out;
                if (q.empty()) return out;
                // URL-encode minimally — spaces -> %20.
                std::string enc;
                for (char c : q)
                { if (c == ' ') enc += "%20"; else enc.push_back(c); }
                const std::string url = endpoint +
                    (endpoint.find('?') == std::string::npos ? "?q=" : "&q=") + enc;
                char cmd[2048];
                std::snprintf(cmd, sizeof(cmd),
                    "/usr/bin/curl -sL --max-time 8 '%s' 2>/dev/null",
                    url.c_str());
                FILE* f = popen(cmd, "r");
                if (!f) return out;
                std::string body;
                char buf[1024];
                while (std::size_t n = std::fread(buf, 1, sizeof(buf), f))
                    body.append(buf, n);
                pclose(f);
                rapidjson::Document doc;
                doc.Parse(body.c_str(), body.size());
                if (doc.HasParseError()) return out;
                const rapidjson::Value* arr = nullptr;
                if (doc.IsArray()) arr = &doc;
                else if (doc.HasMember("results") &&
                         doc["results"].IsArray()) arr = &doc["results"];
                if (!arr) return out;
                for (auto& r : arr->GetArray())
                {
                    if (!r.IsObject()) continue;
                    opencs::SpriteResult sr;
                    if (r.HasMember("label") && r["label"].IsString())
                        sr.label = r["label"].GetString();
                    if (r.HasMember("thumb") && r["thumb"].IsString())
                        sr.thumbUrl = r["thumb"].GetString();
                    if (r.HasMember("download") && r["download"].IsString())
                        sr.downloadUrl = r["download"].GetString();
                    else if (r.HasMember("url") && r["url"].IsString())
                        sr.downloadUrl = r["url"].GetString();
                    if (r.HasMember("provider") && r["provider"].IsString())
                        sr.provider = r["provider"].GetString();
                    if (sr.label.empty()) sr.label = sr.downloadUrl;
                    out.push_back(std::move(sr));
                }
                return out;
            });
    }

    // Live ax-tree refresh: structural mutations (addChildNode,
    // deleteNode, moveNode, undo/redo of any structural op) ask us
    // to recompile the in-memory csd to a fresh ax::Node and swap it
    // under gTopRoot. Without this hook, the parallel csd↔ax walk in
    // rebuildAxMapsAndSync drifts whenever child counts diverge,
    // leaving orphan ax nodes (delete) or invisible ones (add) until
    // the user closes and reopens the tab.
    Tab* tabRaw = tab.get();
    tab->editor->setRefreshAxRootFn([tabRaw]() -> ax::Node* {
        opencs::Editor& ed = *tabRaw->editor;
        if (!ed.doc()) return nullptr;

        // Serialize the in-memory pugi doc to XML in memory.
        std::ostringstream ss;
        ed.doc()->doc.save(ss, "",
                           pugi::format_raw | pugi::format_no_declaration);
        std::string xml = ss.str();

        // Compile to a per-process temp .csb. FBS reads/writes via FileUtils
        // so the path needs to exist on disk; we drop it after CSLoader
        // consumes it. One path per pid is fine — refreshes are synchronous
        // on the render thread, never concurrent.
        namespace fs = std::filesystem;
        fs::path tmp = fs::temp_directory_path() /
            ("ocs-refresh-" + std::to_string(::getpid()) + ".csb");
        auto* fbs = cocostudio::FlatBuffersSerialize::getInstance();
        const std::string err =
            fbs->serializeFlatBuffersWithXMLBuffer(xml, tmp.string());
        if (!err.empty())
        {
            OCS_LOG_ERROR("app", "refresh: FBS failed: %s", err.c_str());
            std::error_code ec; fs::remove(tmp, ec);
            return nullptr;
        }
        ax::Node* fresh = CSLoader::createNode(tmp.string());
        std::error_code ec; fs::remove(tmp, ec);
        if (!fresh)
        {
            OCS_LOG_ERROR("app", "refresh: CSLoader returned null");
            return nullptr;
        }

        // Swap loadedRoot under gTopRoot. Preserve visibility so an
        // inactive tab's refresh stays hidden. retain across the
        // remove/add since CSLoader returns an autoreleased node and
        // we want it parented before any cleanup happens.
        const bool wasVisible = tabRaw->loadedRoot
            ? tabRaw->loadedRoot->isVisible() : false;
        if (tabRaw->loadedRoot)
        {
            tabRaw->loadedRoot->removeFromParent();   // /*cleanup=*/true
            tabRaw->loadedRoot = nullptr;
        }
        fresh->setVisible(wasVisible);
        if (gTopRoot) gTopRoot->addChild(fresh);
        tabRaw->loadedRoot = fresh;
        return fresh;
    });

    gTabs.push_back(std::move(tab));
    const int newIdx = static_cast<int>(gTabs.size()) - 1;

    // Additive search-path model means this open leaves FileUtils'
    // list in a valid state for every tab — nothing to restore.
    // Re-apply active design resolution so a background open didn't
    // visually change the currently-shown window framing.
    if (gActiveTab >= 0) applyTabEnvironment(*gTabs[gActiveTab]);

    return newIdx;
}

/// Reorder gTabs by moving the tab at `from` to insertion position
/// `to` (where `to` is interpreted as "insert BEFORE original index
/// `to`", with `to == size` meaning "insert at end"). Keeps
/// gActiveTab tracking the same logical tab across the move.
void reorderTab(int from, int to)
{
    const int n = static_cast<int>(gTabs.size());
    if (from < 0 || from >= n) return;
    if (to < 0)  to = 0;
    if (to > n)  to = n;
    // Both "same spot" and "immediately after self" collapse to no
    // movement — dropping just past the source tab's own position
    // means the user wants it to stay where it was.
    if (to == from || to == from + 1) return;
    auto movedPtr = std::move(gTabs[from]);
    gTabs.erase(gTabs.begin() + from);
    // After erase the target indices to the right of `from` have
    // shifted left by 1. So convert `to` (original-index space) into
    // post-erase insertion-index space.
    const int insertAt = (to > from) ? (to - 1) : to;
    gTabs.insert(gTabs.begin() + insertAt, std::move(movedPtr));
    // Recompute active index so the user's selected document stays
    // selected even though its position changed.
    if (gActiveTab == from)
        gActiveTab = insertAt;
    else if (from < gActiveTab && insertAt >= gActiveTab)
        gActiveTab--;
    else if (from > gActiveTab && insertAt <= gActiveTab)
        gActiveTab++;
}

/// Like openTab(path) but inserts the new tab at a specific position
/// instead of appending. Used when a peer process drops a tab onto
/// our bar at a specific insertion index.
int openTabAt(int insertIdx, const std::string& csdPath)
{
    const int newIdx = openTab(csdPath);
    if (newIdx < 0) return newIdx;
    const int n = static_cast<int>(gTabs.size());
    if (insertIdx < 0) insertIdx = n - 1;
    if (insertIdx > n - 1) insertIdx = n - 1;
    if (insertIdx != newIdx) reorderTab(newIdx, insertIdx);
    return insertIdx;
}

/// Close tab `idx`, detach its loadedRoot, and shift the active tab to
/// the nearest survivor (or -1 if none).
void closeTab(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(gTabs.size())) return;

    if (auto* r = gTabs[idx]->loadedRoot)
        gTopRoot->removeChild(r);

    const bool wasActive = (idx == gActiveTab);
    gTabs.erase(gTabs.begin() + idx);

    if (gTabs.empty())
    {
        gActiveTab = -1;
        AppDelegate::sCsdPath.clear();
        return;
    }
    if (wasActive)
    {
        const int newIdx = std::min(idx, static_cast<int>(gTabs.size()) - 1);
        gActiveTab = -1;          // force setActiveTab to treat it as a switch
        setActiveTab(newIdx);
    }
    else if (idx < gActiveTab)
    {
        gActiveTab--;             // indices shifted down after erase
    }
}

/// Fork off a fresh Walshard Studio process that opens `csdPath` as
/// its only tab. Used by the tab-bar tear-off: dragging a tab outside
/// the app window promotes it to its own window (Notepad++ style).
/// Goes through ocs::spawn::launchDetached so the same call works on
/// macOS (posix_spawn — must not raw-fork because Cocoa global state
/// isn't fork-safe), Linux (posix_spawn), and Windows (CreateProcess).
void spawnNewWindow(const std::string& csdPath)
{
    const std::string exePath = ocs::spawn::selfExecutablePath();
    if (exePath.empty())
    {
        OCS_LOG_ERROR("app", "tear-off: selfExecutablePath failed");
        return;
    }
    const int pid = ocs::spawn::launchDetached(exePath, {exePath, csdPath});
    if (pid > 0)
        OCS_LOG_INFO("app", "tear-off: spawned %s as pid=%d",
                     csdPath.c_str(), pid);
    else
        OCS_LOG_ERROR("app", "tear-off: spawn failed");
}

/// Launch a new Walshard Studio process in detached-panel mode —
/// `--panel=<name>` tells main.cpp / AppDelegate to skip the full
/// editor UI and render just that panel. The detached window writes
/// `/tmp/walshard/detached.<Name>` on launch (see IpcBridge)
/// so peer processes know to hide the panel from their own layout
/// — the singleton rule we agreed on for multi-window panels.
void spawnDetachedPanel(const std::string& panelName,
                        int widthPx  = 0,
                        int heightPx = 0)
{
    const std::string exePath = ocs::spawn::selfExecutablePath();
    if (exePath.empty())
    {
        OCS_LOG_ERROR("app", "detach: selfExecutablePath failed");
        return;
    }
    // Width / height = exactly what the caller measured (panel's
    // ImGui Size or ContentSize + chrome pad). Fall back to a
    // sensible default only when the measurement is missing.
    if (widthPx  <= 0) widthPx  = 360;
    if (heightPx <= 0) heightPx = 720;
    std::vector<std::string> argv = {
        exePath,
        "--panel=" + panelName,
    };
    if (widthPx > 0 && heightPx > 0)
        argv.push_back("--size=" + std::to_string(widthPx) + "x" +
                       std::to_string(heightPx));
    const int pid = ocs::spawn::launchDetached(exePath, argv);
    if (pid > 0)
        OCS_LOG_INFO("app",
            "detach: spawned panel=%s size=%dx%d as pid=%d",
            panelName.c_str(), widthPx, heightPx, pid);
    else
        OCS_LOG_ERROR("app", "detach: spawn failed");
}

/// Detached-panel render — this process was started via
/// `--panel=<Name>`, so fill the entire OS window with that panel's
/// UI. For phase 1 we show a placeholder body: the actual panel
/// content is wired to the focused peer's selection in a later pass
/// (phase 4 — IPC `.selection` / `.focus` files).  The goal here is
/// to prove the plumbing: the child process is alive, the window is
/// chrome-less, the panel name is visible, and its PID is registered
/// so peers hide their own copy.
// Classify a Properties field based on its CSD key (and value shape as
// a fallback) so the detached panel can pick a typed widget instead
// of a free-form InputText. Returns one of:
//   "bool"   — True/False; rendered as Checkbox.
//   "u8"     — 0..255 integer (CColor channels, Alpha); DragInt clamped.
//   "int"    — free integer (Tag, ActionTag, ZOrder); DragInt.
//   "float"  — free float (Position, Size, Scale, …); DragFloat.
//   "string" — anything else; InputText.
const char* classifyFieldType(const std::string& k, const std::string& v)
{
    // Exact-match well-known boolean attributes first.
    static const char* const kBools[] = {
        "attr.CanEdit", "attr.IsInteractive", "attr.ResourceAdapt",
        "attr.VisibleForFrame", "attr.SingleTouch",
        "attr.EnableBorder", "attr.CustomSizeEnabled",
        "attr.TouchEnable", "attr.UseMergedTexture",
        "attr.IgnoreSize",
    };
    for (const char* b : kBools) if (k == b) return "bool";
    // Value shape fallback for bools the table hasn't captured.
    if (v == "True" || v == "False") return "bool";
    // 0..255 channels.
    if (k == "attr.Alpha")                       return "u8";
    if (k.rfind("sub.CColor.",        0) == 0)   return "u8";
    if (k.rfind("sub.SingleColor.",   0) == 0)   return "u8";
    if (k.rfind("sub.FirstColor.",    0) == 0)   return "u8";
    if (k.rfind("sub.EndColor.",      0) == 0)   return "u8";
    // Positional / geometric sub-elements are free floats.
    if (k.rfind("sub.Position.",    0) == 0)   return "float";
    if (k.rfind("sub.PrePosition.", 0) == 0)   return "float";
    if (k.rfind("sub.Size.",        0) == 0)   return "float";
    if (k.rfind("sub.PreSize.",     0) == 0)   return "float";
    if (k.rfind("sub.Scale.",       0) == 0)   return "float";
    if (k.rfind("sub.AnchorPoint.", 0) == 0)   return "float";
    // Integer attrs.
    if (k == "attr.Tag" || k == "attr.ActionTag" ||
        k == "attr.ZOrder" || k == "attr.RenderingType") return "int";
    // Free floats (rotation / opacity-like attrs).
    if (k == "attr.RotationSkewX" || k == "attr.RotationSkewY" ||
        k == "attr.Rotation"      || k == "attr.RotationZ_") return "float";
    return "string";
}

/// Detached process holds its own opencs::Editor whose document is
/// kept in lock-step with the focused peer's via the docpath +
/// docxml mirrors. drawSceneTreeBody / drawPropertiesBody / etc.
/// run against this local editor exactly as in the host process —
/// every panel feature (lock icon, drag-drop, context menu) Just
/// Works without re-implementation.
opencs::Editor* getDetachedLocalEditor(const std::string& panelName)
{
    static std::unique_ptr<opencs::Editor> sEditor;
    static std::string sLocalPath;
    static std::string sLocalXmlSeen;
    const int focusedPid = ocs::ipc::mostRecentlyFocusedPeer();
    if (focusedPid <= 0) return sEditor.get();

    const std::string docPath =
        ocs::ipc::readMirror(focusedPid, "docpath");
    const std::string docXml =
        ocs::ipc::readMirror(focusedPid, "docxml");

    // First time we see a path → construct an Editor from disk so
    // asset scanning + initial state happens via the regular ctor.
    if (!sEditor || docPath != sLocalPath)
    {
        if (!docPath.empty())
        {
            try
            {
                sEditor = std::make_unique<opencs::Editor>(
                    std::filesystem::path(docPath));
                sLocalPath = docPath;
                sLocalXmlSeen.clear();
            }
            catch (...) { sEditor.reset(); }
        }
    }
    // Doc XML changed peer-side → re-import. Skip when our local
    // doc is the source of the change (sLocalXmlSeen tracks what we
    // last saw FROM the peer).
    if (sEditor && sEditor->doc() && !docXml.empty() &&
        docXml != sLocalXmlSeen)
    {
        try { sEditor->doc()->reloadFromString(docXml); }
        catch (...) {}
        // CRITICAL: reloadFromString invalidates every pugi node
        // handle that was held against the old document. _selected
        // AND _selection (the multi-select vector) would dereference
        // freed memory on the next panel render. Reset both to safe
        // defaults: setSelected(rootNode) handles _selected and also
        // clears + repopulates _selection with the root.
        sEditor->setSelected(sEditor->doc()->rootNode);
        sLocalXmlSeen = docXml;
    }
    return sEditor.get();
}

/// After each detached frame, compare local doc XML to what the
/// peer last published. If we mutated locally (panel UI changed
/// the doc), forward the full document to the peer via replacedoc
/// IPC — the peer applies and republishes, closing the loop.
void syncDetachedDocToPeer(opencs::Editor* localEd,
                           const std::string& panelName)
{
    if (!localEd || !localEd->doc()) return;
    const int focusedPid = ocs::ipc::mostRecentlyFocusedPeer();
    if (focusedPid <= 0) return;
    static std::string sLastSentXml;
    std::ostringstream oss;
    localEd->doc()->doc.save(oss, "  ");
    std::string xml = oss.str();
    const std::string peerXml =
        ocs::ipc::readMirror(focusedPid, "docxml");
    // Only forward when local diverges from BOTH the peer's last
    // publish AND our last sent — the second guard kills echo loops
    // (we already sent this body; wait for peer to publish back).
    if (xml == peerXml || xml == sLastSentXml) return;
    ocs::ipc::sendReplaceDoc(focusedPid, xml);
    sLastSentXml = std::move(xml);
}

void renderDetachedPanelWindow(const std::string& panelName)
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration     |
        ImGuiWindowFlags_NoMove           |
        ImGuiWindowFlags_NoResize         |
        ImGuiWindowFlags_NoSavedSettings  |
        ImGuiWindowFlags_NoDocking        |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const std::string title = panelName + "##detached";
    if (ImGui::Begin(title.c_str(), nullptr, kFlags))
    {
        // Pick the most-recently-focused main-app peer (by `.focus`
        // mtime) and read its mirror file for this panel. When there
        // is no peer yet (e.g. the user just launched into a cold
        // tree) we fall back to a short placeholder.
        const int focusedPid = ocs::ipc::mostRecentlyFocusedPeer();
        const std::string body = focusedPid > 0
            ? ocs::ipc::readMirror(focusedPid, panelName)
            : std::string{};

        ImGui::Text("%s  ·  mirror of pid %d",
                    panelName.c_str(),
                    focusedPid > 0 ? focusedPid : 0);
        ImGui::Separator();
        // Real-panel render path: load (or refresh) a local Editor
        // from the focused peer's docpath / docxml mirrors, then
        // call the actual panel body function so the detached UI
        // matches the docked one feature-for-feature (lock icon,
        // drag-drop, context menus, expand/collapse, …).
        opencs::Editor* localEd = nullptr;
        if (panelName == "Scene"      || panelName == "Properties" ||
            panelName == "Layout"     || panelName == "Assets"     ||
            panelName == "Messages"   || panelName == "Timeline")
        {
            localEd = getDetachedLocalEditor(panelName);
        }
        if (localEd && localEd->doc())
        {
            // Protective wrapper: any exception from a panel body
            // bubbles up here as text instead of taking the whole
            // detached process down. SIGSEGV obviously still kills
            // the process, but a stray throw won't.
            try
            {
                if      (panelName == "Scene")      opencs::drawSceneTreeBody(*localEd);
                else if (panelName == "Properties") opencs::drawPropertiesBody(*localEd);
                else if (panelName == "Assets")     opencs::drawAssetBrowserBody(*localEd);
                else if (panelName == "Layout")     opencs::drawLayoutBody(*localEd);
                else if (panelName == "Messages")   localEd->drawMessagesBody();
                else if (panelName == "Timeline")   opencs::drawTimelineBody(*localEd);
                syncDetachedDocToPeer(localEd, panelName);
            }
            catch (const std::exception& e)
            {
                ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1.0f),
                    "Panel render threw: %s", e.what());
                OCS_LOG_ERROR("app", "detached %s render: %s",
                    panelName.c_str(), e.what());
            }
        }
        else if (body.empty())
        {
            ImGui::TextDisabled(
                "No data yet. Focus a main Walshard Studio window "
                "and its %s panel will mirror here.",
                panelName.c_str());
        }
        else if (panelName == "Properties")
        {
            // Parse the `key=value` lines into an editable form.
            // Changes commit to the focused main-app via
            // `sendEdit`: the per-input buffer is seeded from the
            // mirror but the user-edited value is what's sent on
            // Enter or focus-loss. Reset-buffer on mirror-change
            // keeps remote updates visible without nuking an
            // in-progress edit on the same key.
            // Per-key edit state persists across frames.
            struct Field { std::string k, v; bool dirty = false; };
            static std::string sLastBody;
            static std::string sLastPath;
            static std::string sLastDisplay;
            static std::vector<Field> sFields;
            if (body != sLastBody)
            {
                // Re-parse only when the mirror actually changed so
                // user typing isn't stomped every frame.
                sLastBody = body;
                sFields.clear();
                std::string newPath, newDisplay;
                std::size_t i = 0;
                while (i < body.size())
                {
                    auto eol = body.find('\n', i);
                    if (eol == std::string::npos) eol = body.size();
                    const std::string line = body.substr(i, eol - i);
                    i = eol + 1;
                    const auto eq = line.find('=');
                    if (eq == std::string::npos) continue;
                    const std::string k = line.substr(0, eq);
                    const std::string v = line.substr(eq + 1);
                    if (k == "path")    { newPath    = v; continue; }
                    if (k == "display") { newDisplay = v; continue; }
                    sFields.push_back({k, v, false});
                }
                sLastPath    = newPath;
                sLastDisplay = newDisplay;
            }
            const std::string& nodepath = sLastPath;

            ImGui::TextDisabled("path: %s", nodepath.c_str());
            if (!sLastDisplay.empty())
                ImGui::Text("%s", sLastDisplay.c_str());
            ImGui::Separator();
            // Wrap the table in a child so its scroll position
            // doesn't jerk around when the mirror body changes
            // shape after an edit (e.g. one field switching widget
            // type or a value's formatted length growing).
            ImGui::BeginChild("##props-body", ImVec2(0, 0), false);
            if (ImGui::BeginTable("##props", 2,
                    ImGuiTableFlags_SizingStretchProp |
                    ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthStretch, 0.42f);
                ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
                for (std::size_t idx = 0; idx < sFields.size(); ++idx)
                {
                    auto& f = sFields[idx];
                    const char* type = classifyFieldType(f.k, f.v);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(f.k.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::PushID(static_cast<int>(idx));
                    ImGui::SetNextItemWidth(-FLT_MIN);

                    bool changed = false, commit = false;
                    // Checkbox for booleans: instant commit — no
                    // in-progress drag to wait for.
                    if (std::strcmp(type, "bool") == 0)
                    {
                        bool b = (f.v == "True" || f.v == "true" ||
                                  f.v == "1");
                        if (ImGui::Checkbox("##v", &b))
                        {
                            f.v = b ? "True" : "False";
                            changed = commit = true;
                        }
                    }
                    else if (std::strcmp(type, "u8") == 0)
                    {
                        int x = std::atoi(f.v.c_str());
                        x = std::clamp(x, 0, 255);
                        if (ImGui::DragInt("##v", &x, 1.0f, 0, 255))
                        {
                            f.v = std::to_string(x);
                            changed = true;
                        }
                        commit = ImGui::IsItemDeactivatedAfterEdit();
                    }
                    else if (std::strcmp(type, "int") == 0)
                    {
                        int x = std::atoi(f.v.c_str());
                        if (ImGui::DragInt("##v", &x, 1.0f))
                        {
                            f.v = std::to_string(x);
                            changed = true;
                        }
                        commit = ImGui::IsItemDeactivatedAfterEdit();
                    }
                    else if (std::strcmp(type, "float") == 0)
                    {
                        float x = std::strtof(f.v.c_str(), nullptr);
                        if (ImGui::DragFloat("##v", &x, 0.5f, 0.0f, 0.0f,
                                             "%.4f"))
                        {
                            char buf[32];
                            std::snprintf(buf, sizeof(buf), "%.4f", x);
                            f.v = buf;
                            changed = true;
                        }
                        commit = ImGui::IsItemDeactivatedAfterEdit();
                    }
                    else   // "string"
                    {
                        char buf[512];
                        std::snprintf(buf, sizeof(buf), "%s", f.v.c_str());
                        const bool pressed = ImGui::InputText(
                            "##v", buf, sizeof(buf),
                            ImGuiInputTextFlags_EnterReturnsTrue);
                        if (std::string(buf) != f.v)
                        {
                            f.v = buf;
                            changed = true;
                        }
                        commit = pressed ||
                                 (changed && ImGui::IsItemDeactivatedAfterEdit()) ||
                                 ImGui::IsItemDeactivatedAfterEdit();
                    }
                    if (changed) f.dirty = true;
                    if (commit && f.dirty &&
                        focusedPid > 0 && !nodepath.empty())
                    {
                        ocs::ipc::sendEdit(focusedPid, nodepath, f.k, f.v);
                        f.dirty = false;
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndChild();
        }
        else if (panelName == "Scene")
        {
            // Scene tree: each non-header line is
            // `<depth>\t<name>\t<ctype>\t<path>`.  Render as a flat
            // selectable list with manual indent (simpler and
            // fully-visible than ImGui TreeNode — no collapse state
            // to persist and the tree is shallow in practice).  The
            // `selected=...` header drives the row highlight.
            std::string selectedPath;
            if (body.rfind("selected=", 0) == 0)
            {
                auto eol = body.find('\n');
                selectedPath = body.substr(9, eol - 9);
            }
            if (ImGui::BeginChild("##scene-body", ImVec2(0, 0), false,
                                  ImGuiWindowFlags_HorizontalScrollbar))
            {
                std::size_t i = body.find('\n');
                if (i != std::string::npos) ++i;
                while (i < body.size())
                {
                    auto eol = body.find('\n', i);
                    if (eol == std::string::npos) eol = body.size();
                    const std::string line = body.substr(i, eol - i);
                    i = eol + 1;
                    // Split by tab.
                    int depth = 0;
                    std::string name, ctype, path;
                    std::size_t a = 0, b;
                    b = line.find('\t', a); if (b == std::string::npos) continue;
                    depth = std::atoi(line.substr(a, b - a).c_str());
                    a = b + 1;
                    b = line.find('\t', a); if (b == std::string::npos) continue;
                    name = line.substr(a, b - a);
                    a = b + 1;
                    b = line.find('\t', a); if (b == std::string::npos) continue;
                    ctype = line.substr(a, b - a);
                    path  = line.substr(b + 1);
                    const bool isSel = (path == selectedPath);
                    ImGui::PushID(path.c_str());
                    const float indent = std::max(0, depth) * 14.0f;
                    if (indent > 0.0f) ImGui::Indent(indent);
                    char row[256];
                    std::snprintf(row, sizeof(row), "%s  (%s)",
                        name.c_str(), ctype.c_str());
                    if (ImGui::Selectable(row, isSel) &&
                        focusedPid > 0)
                        ocs::ipc::sendSelect(focusedPid, path);
                    if (indent > 0.0f) ImGui::Unindent(indent);
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
        }
        else if (panelName == "Assets")
        {
            // Assets body: `selected=<path>` header, then one
            // `<kind>\t<displayName>\t<absPath>` per row. Render as a
            // selectable table; clicking a row sends `selectasset
            // <absPath>` to the focused main-app.
            std::string selectedPath;
            if (body.rfind("selected=", 0) == 0)
            {
                auto eol = body.find('\n');
                selectedPath = body.substr(9, eol - 9);
            }
            if (ImGui::BeginTable("##assets", 2,
                    ImGuiTableFlags_SizingStretchProp |
                    ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("name",
                    ImGuiTableColumnFlags_WidthStretch, 0.55f);
                ImGui::TableSetupColumn("kind",
                    ImGuiTableColumnFlags_WidthStretch, 0.45f);
                std::size_t i = body.find('\n');
                if (i != std::string::npos) ++i;
                while (i < body.size())
                {
                    auto eol = body.find('\n', i);
                    if (eol == std::string::npos) eol = body.size();
                    const std::string line = body.substr(i, eol - i);
                    i = eol + 1;
                    std::size_t a = 0, b;
                    b = line.find('\t', a);
                    if (b == std::string::npos) continue;
                    const std::string kind = line.substr(a, b - a);
                    a = b + 1;
                    b = line.find('\t', a);
                    if (b == std::string::npos) continue;
                    const std::string nameCol = line.substr(a, b - a);
                    const std::string absPath = line.substr(b + 1);
                    const bool isSel = (absPath == selectedPath);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushID(absPath.c_str());
                    if (ImGui::Selectable(nameCol.c_str(), isSel,
                            ImGuiSelectableFlags_SpanAllColumns) &&
                        focusedPid > 0)
                    {
                        ocs::ipc::sendSelectAsset(focusedPid, absPath);
                    }
                    ImGui::PopID();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextDisabled("%s", kind.c_str());
                }
                ImGui::EndTable();
            }
        }
        else if (panelName == "Layout")
        {
            // Layout body: `key=value` lines only. Render as a
            // two-column read-only table.
            if (ImGui::BeginTable("##layout", 2,
                    ImGuiTableFlags_SizingStretchProp |
                    ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_Borders))
            {
                ImGui::TableSetupColumn("key",
                    ImGuiTableColumnFlags_WidthStretch, 0.45f);
                ImGui::TableSetupColumn("value",
                    ImGuiTableColumnFlags_WidthStretch, 0.55f);
                std::size_t i = 0;
                while (i < body.size())
                {
                    auto eol = body.find('\n', i);
                    if (eol == std::string::npos) eol = body.size();
                    const std::string line = body.substr(i, eol - i);
                    i = eol + 1;
                    const auto eq = line.find('=');
                    if (eq == std::string::npos) continue;
                    const std::string k = line.substr(0, eq);
                    const std::string v = line.substr(eq + 1);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(k.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(v.c_str());
                }
                ImGui::EndTable();
            }
        }
        else
        {
            // Scrollable region so long click logs / selection dumps
            // don't push the content off the bottom of the OS window.
            if (ImGui::BeginChild("##mirror-body", ImVec2(0, 0), false,
                                  ImGuiWindowFlags_HorizontalScrollbar))
            {
                ImGui::TextUnformatted(body.c_str(),
                                       body.c_str() + body.size());
            }
            ImGui::EndChild();
        }
    }
    // Auto-shrink-to-content removed: it was making detached-window
    // sizes diverge per panel (Scene was wide because tree content
    // was tall, Layout was narrow because content was short). The
    // unified `parentW/6` width set in spawnDetachedPanel is the
    // single source of truth for the OS-window size now.
    ImGui::End();
    ImGui::PopStyleVar(2);
}

/// Render the tab bar pinned to the top of the viewport, below the
/// native macOS menu bar. We manually reserve `barHeight` pixels from
/// the viewport's WorkPos/WorkSize so DockSpaceOverViewport in
/// Editor::render fills only the region *below* the tab strip — no
/// floating, no overlap with the Mode panel.
bool renderTabBar()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float barHeight = ImGui::GetFrameHeight() + 8.0f;

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, barHeight));
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 2));
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration          |
        ImGuiWindowFlags_NoMove                |
        ImGuiWindowFlags_NoScrollbar           |
        ImGuiWindowFlags_NoSavedSettings       |
        ImGuiWindowFlags_NoDocking             |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    // Tear-off + reorder state. `dragSourceIdx` is set every frame
    // that a tab is being actively dragged via ImGui's drag-drop
    // source. We use BeginDragDropSource rather than raw
    // IsItemActive+IsMouseDragging because the latter was triggering
    // on spurious events during app launch — drag-drop only fires
    // once the user genuinely drags.
    static int   dragSourceIdx   = -1;
    static float ghostAlpha      = 0.0f;
    // Previous-frame tab rects — needed THIS frame to compute a
    // ghost's target insertion index before we've rendered. One-
    // frame lag is imperceptible.
    static std::vector<std::pair<int, int>> gLastTabRects;
    int dragThisFrame = -1;

    // Early peer-hover read so the render loop below can use it to
    // decide whether to insert a ghost tab.
    int         peerSourcePid = 0;
    int         peerCursorSX  = 0;
    std::string peerLabel;
    const bool  peerHovering = ocs::ipc::readHoverSignal(
                                   peerSourcePid, peerCursorSX, peerLabel);

    // Main-window screen origin + size, computed once per frame.
    // Needed for cursor→screen coord conversion + for publishing.
    int mainWX = 0, mainWY = 0, mainWW = 0, mainWH = 0;
    if (auto* rv = dynamic_cast<RenderViewImpl*>(
            ax::Director::getInstance()->getRenderView()))
    {
        if (GLFWwindow* mw = rv->getWindow())
        {
            glfwGetWindowPos(mw, &mainWX, &mainWY);
            glfwGetWindowSize(mw, &mainWW, &mainWH);
        }
    }
    const ImVec2 mousePos = ImGui::GetMousePos();
    const float  cursorSX = mainWX + (mousePos.x - viewport->Pos.x);
    const float  cursorSY = mainWY + (mousePos.y - viewport->Pos.y);

    // Bar's ImGui-local rect captured inside Begin() so we can hand
    // it to the IPC publisher + collect per-tab rects for peers.
    ImVec2 barMin(0, 0), barMax(0, 0);
    std::vector<std::pair<int, int>> thisFrameTabRects;

    int requestedActive = gActiveTab;
    if (ImGui::Begin("##scenes_bar", nullptr, kFlags))
    {
        barMin = ImGui::GetWindowPos();
        barMax = ImVec2(barMin.x + ImGui::GetWindowSize().x,
                        barMin.y + ImGui::GetWindowSize().y);

        // ImGui's built-in reorder is DISABLED — our drag handling
        // does reorders itself so local swaps and cross-process
        // merges share the same ghost/shift animation.
        constexpr ImGuiTabBarFlags kTabFlags =
            ImGuiTabBarFlags_FittingPolicyScroll |
            ImGuiTabBarFlags_AutoSelectNewTabs   |
            ImGuiTabBarFlags_TabListPopupButton;

        // Compute ghost target using LAST frame's tab rects. One-
        // frame lag is imperceptible, and dodges the chicken-and-
        // egg of needing rects before we've rendered.
        auto computeInsertIdx = [](int cx,
            const std::vector<std::pair<int,int>>& rects) -> int {
            const int n = static_cast<int>(rects.size());
            for (int i = 0; i < n; ++i)
            {
                const int mid = (rects[i].first + rects[i].second) / 2;
                if (cx < mid) return i;
            }
            return n;
        };
        // barMin/barMax are viewport-local (from GetWindowPos).
        // Compare against the ImGui mouse (also viewport-local) —
        // NOT the screen-coord cursorSX/SY, which would silently
        // fail whenever the app window isn't pinned at (0, 0).
        const ImVec2 mNow = ImGui::GetMousePos();
        const bool cursorInBarNow =
            mNow.x >= barMin.x && mNow.x < barMax.x &&
            mNow.y >= barMin.y && mNow.y < barMax.y;
        int         ghostIdx   = -1;
        std::string ghostLabel;
        if (dragSourceIdx >= 0 &&
            dragSourceIdx < static_cast<int>(gTabs.size()) &&
            cursorInBarNow)
        {
            ghostIdx   = computeInsertIdx(
                static_cast<int>(cursorSX), gLastTabRects);
            ghostLabel = gTabs[dragSourceIdx]->label;
        }
        else if (peerHovering)
        {
            ghostIdx   = computeInsertIdx(peerCursorSX, gLastTabRects);
            ghostLabel = peerLabel;
        }

        // Animate ghost alpha toward target. Fades in when ghost
        // appears, out when the drag leaves or ends.
        const float deltaT      = ImGui::GetIO().DeltaTime;
        const float targetAlpha = (ghostIdx >= 0) ? 0.55f : 0.0f;
        ghostAlpha += (targetAlpha - ghostAlpha) *
                      std::min(1.0f, deltaT * 14.0f);

        auto renderGhost = [&](const std::string& label, int idx) {
            // Ghost = a transparent placeholder tab that occupies
            // layout space, causing real tabs to shift around it.
            // Passing nullptr for p_open suppresses the close button
            // naturally, so no NoCloseButton flag is needed.
            const std::string id =
                label + "##__ghost_" + std::to_string(idx);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ghostAlpha);
            if (ImGui::BeginTabItem(id.c_str(), nullptr,
                    ImGuiTabItemFlags_NoReorder |
                    ImGuiTabItemFlags_NoTooltip))
            {
                ImGui::EndTabItem();
            }
            ImGui::PopStyleVar();
        };

        if (ImGui::BeginTabBar("##scene_tabs", kTabFlags))
        {
            const int n = static_cast<int>(gTabs.size());
            thisFrameTabRects.reserve(n);
            for (int i = 0; i < n; ++i)
            {
                // Ghost BEFORE tab i — existing tabs shift
                // automatically because the ghost occupies layout
                // space. Suppressed when the ghost would collide
                // with the home slot (that position is already
                // represented by the grayed placeholder below).
                if (ghostIdx == i && !ghostLabel.empty() &&
                    i != dragSourceIdx)
                    renderGhost(ghostLabel, i);

                auto& tab = *gTabs[i];

                // Home-slot of the tab currently being dragged —
                // swap its colours to a prominent blue so the user
                // gets immediate confirmation the drag registered.
                // Outline drawn on top of the tab rect makes it
                // unmissable against any tab-bar background.
                if (i == dragSourceIdx)
                {
                    constexpr ImU32 kDragBG     = IM_COL32(55, 115, 210, 220);
                    constexpr ImU32 kDragFill   = IM_COL32(95, 155, 235, 240);
                    constexpr ImU32 kDragOutline = IM_COL32(160, 205, 255, 255);
                    ImGui::PushStyleColor(ImGuiCol_Tab,         kDragBG);
                    ImGui::PushStyleColor(ImGuiCol_TabHovered,  kDragFill);
                    ImGui::PushStyleColor(ImGuiCol_TabSelected, kDragFill);
                    ImGui::PushID(tab.csdPath.c_str());
                    if (ImGui::BeginTabItem(
                            tab.label.c_str(), nullptr,
                            ImGuiTabItemFlags_NoReorder |
                            ImGuiTabItemFlags_NoTooltip))
                    {
                        ImGui::EndTabItem();
                    }
                    const ImVec2 tm = ImGui::GetItemRectMin();
                    const ImVec2 tx = ImGui::GetItemRectMax();
                    const int sx0 = mainWX + int(tm.x - viewport->Pos.x);
                    const int sx1 = mainWX + int(tx.x - viewport->Pos.x);
                    thisFrameTabRects.push_back({sx0, sx1});
                    // Draw a thick outline on top so it reads
                    // even against the tab bar's own border.
                    auto* dl = ImGui::GetForegroundDrawList();
                    dl->AddRect(tm, tx, kDragOutline, 4.0f, 0, 3.0f);
                    ImGui::PopID();
                    ImGui::PopStyleColor(3);
                    continue;
                }

                bool open = true;
                ImGui::PushID(tab.csdPath.c_str());
                const bool itemOpen = ImGui::BeginTabItem(
                    tab.label.c_str(), &open, ImGuiTabItemFlags_None);

                // Capture this tab's screen X extent so future frames
                // (and peer processes) can locate it.
                const ImVec2 tm = ImGui::GetItemRectMin();
                const ImVec2 tx = ImGui::GetItemRectMax();
                const int sx0 = mainWX + int(tm.x - viewport->Pos.x);
                const int sx1 = mainWX + int(tx.x - viewport->Pos.x);
                thisFrameTabRects.push_back({sx0, sx1});

                if (ImGui::BeginDragDropSource(
                        ImGuiDragDropFlags_SourceAllowNullID |
                        ImGuiDragDropFlags_SourceNoHoldToOpenOthers |
                        ImGuiDragDropFlags_SourceNoPreviewTooltip))
                {
                    ImGui::SetDragDropPayload(
                        "OCS_TAB", &i, sizeof(int));
                    dragThisFrame = i;
                    ImGui::EndDragDropSource();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", tab.csdPath.c_str());
                if (itemOpen)
                {
                    requestedActive = i;
                    ImGui::EndTabItem();
                }
                ImGui::PopID();
                if (!open) tab.requestClose = true;
            }
            // Ghost at past-end position — suppressed if the home
            // slot is the last tab (ghost there would duplicate the
            // grayed placeholder's position).
            if (ghostIdx == n && !ghostLabel.empty() &&
                dragSourceIdx != n - 1)
                renderGhost(ghostLabel, n);
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);

    // Stash this frame's rects for the NEXT frame's insert-idx calc
    // (and for publish() below so peers can target us precisely).
    gLastTabRects = std::move(thisFrameTabRects);

    auto mouseOutside = [&](float margin) {
        const ImVec2 m = ImGui::GetMousePos();
        return m.x < viewport->Pos.x - margin ||
               m.x > viewport->Pos.x + viewport->Size.x + margin ||
               m.y < viewport->Pos.y - margin ||
               m.y > viewport->Pos.y + viewport->Size.y + margin;
    };

    // Peer-under-cursor detection. Only look for merge targets when
    // a drag is in progress AND we've left our own window — staying
    // inside means the user is reordering locally. The peer's
    // published per-tab X ranges let us compute the exact insertion
    // index so the source side knows where the dropped tab will land.
    int hoveredPeerPid       = 0;
    int hoveredPeerInsertIdx = -1;
    if (dragSourceIdx >= 0 && mouseOutside(0.0f))
    {
        const auto peers = ocs::ipc::listPeers();
        for (const auto& p : peers)
        {
            if (cursorSX >= p.tabBarX0 && cursorSX < p.tabBarX1 &&
                cursorSY >= p.tabBarY0 && cursorSY < p.tabBarY1)
            {
                hoveredPeerPid = p.pid;
                const int cnt = static_cast<int>(p.tabRects.size());
                const int cx  = static_cast<int>(cursorSX);
                hoveredPeerInsertIdx = cnt;
                for (int k = 0; k < cnt; ++k)
                {
                    const int mid =
                        (p.tabRects[k].first + p.tabRects[k].second) / 2;
                    if (cx < mid) { hoveredPeerInsertIdx = k; break; }
                }
                break;
            }
        }
    }

    // While ImGui reports a live drag source this frame, remember
    // which tab is being dragged AND switch the cursor to a hand so
    // the user gets Sublime-style visual feedback from frame 1 of
    // the drag. Manage the follow-cursor label + cross-process
    // hover-signal in the same block.
    static bool labelVisible = false;
    static int  lastSignaledPeer = 0;
    // Promote dragThisFrame to the persistent dragSourceIdx. Stays
    // set even on frames where dragThisFrame is -1 (e.g., when we
    // skip rendering the dragged tab via `continue` inside the
    // loop). Cleared on mouse release below.
    if (dragThisFrame >= 0) dragSourceIdx = dragThisFrame;

    if (dragSourceIdx >= 0 &&
        dragSourceIdx < static_cast<int>(gTabs.size()) &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        const auto& tab = *gTabs[dragSourceIdx];
        const float sx = cursorSX + 16.0f;
        const float sy = cursorSY + 16.0f;
        // Swap the label text to telegraph "this will merge" when the
        // cursor is over a peer's tab bar, so the user sees the drop
        // intent before releasing.
        const std::string text = hoveredPeerPid
            ? ("-> merge into window: " + tab.label)
            :  tab.label;

        if (!labelVisible)
        {
            ocs::draglabel::show(text, sx, sy);
            labelVisible = true;
        }
        else
        {
            ocs::draglabel::moveTo(sx, sy);
            // moveTo doesn't change the text; if the target state
            // just flipped, reissue show() with the new text.
            static int lastShownPeer = -1;
            if (hoveredPeerPid != lastShownPeer)
            {
                ocs::draglabel::show(text, sx, sy);
                lastShownPeer = hoveredPeerPid;
            }
        }

        // Hover-signal lifecycle: write to the current target every
        // frame (so the target treats it as fresh), clear the
        // previous one if the target switched.
        if (lastSignaledPeer != 0 &&
            lastSignaledPeer != hoveredPeerPid)
        {
            ocs::ipc::clearHoverSignal(lastSignaledPeer);
        }
        if (hoveredPeerPid != 0)
        {
            ocs::ipc::writeHoverSignal(
                hoveredPeerPid, ::getpid(),
                static_cast<int>(cursorSX),
                gTabs[dragSourceIdx]->label);
        }
        lastSignaledPeer = hoveredPeerPid;
    }
    else if (labelVisible)
    {
        // Drag ended — hide the label once, then stop poking it.
        ocs::draglabel::hide();
        labelVisible = false;
        if (lastSignaledPeer != 0)
        {
            ocs::ipc::clearHoverSignal(lastSignaledPeer);
            lastSignaledPeer = 0;
        }
    }

    // On mouse release: four outcomes, in priority order.
    //   1. Cursor over a peer's tab bar → cross-process merge at
    //      the computed insertion index.
    //   2. Cursor inside our own bar → local reorder to the ghost's
    //      position.
    //   3. Cursor clearly outside viewport → tear-off (new process).
    //   4. Otherwise → cancel.
    if (dragSourceIdx >= 0 &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        constexpr float kReleaseMargin = 40.0f;
        if (dragSourceIdx < static_cast<int>(gTabs.size()))
        {
            const std::string path = gTabs[dragSourceIdx]->csdPath;
            // barMin/barMax come from ImGui::GetWindowPos, which
            // lives in viewport-local coords — so we compare against
            // the ImGui mouse position (same space), not against the
            // screen-coord cursorSX/SY (which would silently fail
            // when the app window isn't pinned at (0,0)).
            const ImVec2 mNow = ImGui::GetMousePos();
            const bool inOwnBar =
                mNow.x >= barMin.x && mNow.x < barMax.x &&
                mNow.y >= barMin.y && mNow.y < barMax.y;
            if (hoveredPeerPid != 0)
            {
                ocs::ipc::sendOpen(
                    hoveredPeerPid, path, hoveredPeerInsertIdx);
                OCS_LOG_INFO("app",
                    "merge: sent %s to pid=%d at idx=%d",
                    path.c_str(), hoveredPeerPid,
                    hoveredPeerInsertIdx);
                closeTab(dragSourceIdx);
                if (gTabs.empty())
                {
                    ocs::ipc::shutdown();
                    ax::Director::getInstance()->end();
                }
            }
            else if (inOwnBar)
            {
                // Local reorder to the ghost's position. Use the
                // freshly-updated tab rects (same as target this
                // frame) so the drop lands where the ghost sat.
                const int n = static_cast<int>(gLastTabRects.size());
                int targetIdx = n;
                const int cx = static_cast<int>(cursorSX);
                for (int k = 0; k < n; ++k)
                {
                    const int mid =
                        (gLastTabRects[k].first +
                         gLastTabRects[k].second) / 2;
                    if (cx < mid) { targetIdx = k; break; }
                }
                reorderTab(dragSourceIdx, targetIdx);
            }
            else if (mouseOutside(kReleaseMargin))
            {
                spawnNewWindow(path);
                closeTab(dragSourceIdx);
                // If the source had only that tab, the spawned
                // window now owns the document — exit the empty
                // source so the user isn't left with an "0-tab"
                // shell (welcome card / black canvas). Same shape
                // as the merge path above.
                if (gTabs.empty())
                {
                    ocs::ipc::shutdown();
                    ax::Director::getInstance()->end();
                }
            }
        }
        if (lastSignaledPeer != 0)
        {
            ocs::ipc::clearHoverSignal(lastSignaledPeer);
            lastSignaledPeer = 0;
        }
        dragSourceIdx = -1;
    }

    // Publish this window's identity to peers: window frame + tab
    // bar rect + per-tab X ranges, so dragging peers can compute
    // the exact insertion index targeting us. Written every frame so
    // peers always see a current layout — the file is tiny.
    {
        ocs::ipc::SelfInfo info{};
        info.frameX   = mainWX;
        info.frameY   = mainWY;
        info.frameW   = mainWW;
        info.frameH   = mainWH;
        info.tabBarX0 = mainWX + int(barMin.x - viewport->Pos.x);
        info.tabBarY0 = mainWY + int(barMin.y - viewport->Pos.y);
        info.tabBarX1 = mainWX + int(barMax.x - viewport->Pos.x);
        info.tabBarY1 = mainWY + int(barMax.y - viewport->Pos.y);
        info.tabCount = static_cast<int>(gTabs.size());
        info.tabRects = gLastTabRects;
        ocs::ipc::publish(info);
    }

    // Target-side bar highlight while a peer is hovering us. The
    // ghost tab (rendered inside the loop above) already shows the
    // insertion column + pushes neighbouring tabs aside; this frame
    // reinforces "drop zone" with an outline.
    if (peerHovering)
    {
        auto* dl = ImGui::GetForegroundDrawList();
        dl->AddRect(barMin, barMax,
                    IM_COL32(80, 170, 255, 220), 2.0f, 0, 3.0f);
    }

    // Shrink the viewport's work area so DockSpaceOverViewport (called
    // later this frame from Editor::render) doesn't overlap the strip.
    // This mirrors what BeginMainMenuBar does internally.
    viewport->WorkPos.y  += barHeight;
    viewport->WorkSize.y -= barHeight;

    // Defer close requests and activation changes until the tab bar is
    // fully iterated so we don't invalidate `tab` mid-loop.
    for (int i = static_cast<int>(gTabs.size()) - 1; i >= 0; --i)
        if (gTabs[i]->requestClose) closeTab(i);

    if (!gTabs.empty())
    {
        const int clamped = std::clamp(
            requestedActive, 0, static_cast<int>(gTabs.size()) - 1);
        if (clamped != gActiveTab) setActiveTab(clamped);
    }
    return !gTabs.empty();
}

}  // namespace

bool AppDelegate::applicationDidFinishLaunching()
{
    opencs::initDefaultLogging();
    // Register every OCS-extension node-reader with axmol's
    // ObjectFactory before any CSLoader call. Idempotent — game
    // projects built with OCS make this same call from their own
    // AppDelegate so editor + runtime instantiate identical classes
    // for the extension ctypes (VectorMapNode, future Lottie, ...).
    ocs::ext::registerAll();
    // Tag every log record this process emits so the Messages panel
    // shows which window emitted it. Main app says "main"; detached
    // panel processes use their panel name.
    opencs::setWindowLabel(sPanelMode.empty() ? "main" : sPanelMode);
    OCS_LOG_INFO("app", "Walshard Studio starting (pid=%d)", (int)::getpid());
    auto* director = Director::getInstance();
    auto* renderView = director->getRenderView();
    if (!renderView)
    {
        // Detached-panel override: when spawned via `--size=WxH`,
        // open the OS window at that exact size so the detached
        // window matches its previous docked footprint instead of
        // the full kDesignResolution default.
        const bool haveSizeHint =
            !sPanelMode.empty() && sPanelWindowW > 0 && sPanelWindowH > 0;
        const float initW = haveSizeHint ? (float)sPanelWindowW : kDesignResolution.width;
        const float initH = haveSizeHint ? (float)sPanelWindowH : kDesignResolution.height;
        const std::string title = sPanelMode.empty()
            ? std::string("Walshard Studio")
            : ("OCS · " + sPanelMode);
        renderView = RenderViewImpl::createWithRect(
            title,
            ax::Rect(0, 0, initW, initH),
            /*frameZoomFactor=*/1.0f, /*resizable=*/true);
        director->setRenderView(renderView);
    }
    // Default design resolution; loadCsdIntoScene refines this from the
    // .csd root Size once a document is loaded.
    renderView->setDesignResolutionSize(
        kDesignResolution.width, kDesignResolution.height,
        ResolutionPolicy::NO_BORDER);
    director->setAnimationInterval(1.0f / 60);

    // Snapshot the pristine FileUtils search paths once — every tab's
    // activation resets to this and re-adds paths for the active
    // project so asset resolution always matches the visible document.
    gBaseSearchPaths = FileUtils::getInstance()->getSearchPaths();

    // Scene is created once and held in `gScene`. Tab switches swap
    // visibility on the loaded-root children of gTopRoot rather than
    // replacing gScene, so ImGuiPresenter's render loop and the
    // window-resize listener stay attached across opens.
    gScene = Scene::create();
    gTopRoot = ax::Node::create();
    gTopRoot->setTag(0xCADCA12A);
    // Mirror Scene's default transform semantics so the Editor's
    // cachedSceneX/Y math keeps working unchanged:
    //   * anchor (0.5, 0.5) with contentSize = design resolution →
    //     setScale pivots from the visual centre, not the origin
    //   * ignoreAnchorPointForPosition = true → setPosition places
    //     local (0, 0) at the given point (not the anchor)
    // Refreshed per-tab in applyTabEnvironment() whenever a tab's
    // design size differs from the previously-active document.
    gTopRoot->setAnchorPoint(ax::Vec2(0.5f, 0.5f));
    gTopRoot->setContentSize(kDesignResolution);
    gTopRoot->setIgnoreAnchorPointForPosition(true);
    gScene->addChild(gTopRoot);

    auto* imgui = ImGuiPresenter::getInstance();
    imgui->setViewResolution(kDesignResolution.width, kDesignResolution.height);

    // Modern UI font — load macOS's San Francisco at 14px so the editor
    // doesn't fall back to ImGui's default ProggyClean (the single
    // biggest "looks like 2010" tell). SFNS.ttf has been on every
    // macOS install since 10.11; if it ever isn't there, addFont
    // fails silently via isFileExistInternal and we fall back.
    imgui->addFont("/System/Library/Fonts/SFNS.ttf", 14.0f);

    // Cross-process coordination: register this instance in
    // /tmp/walshard/ so peers can find us for drag-merge, and
    // so incoming tab-open requests from peers get picked up each
    // frame. See IpcBridge.h for the full file protocol.
    ocs::ipc::init();

    // Detached-panel mode (spawned via `--panel=<Name>`): publish a
    // registry file so peer main-app processes hide the panel from
    // their own dock layout. No scene / tabs are opened in this mode;
    // the render loop short-circuits to renderDetachedPanelWindow().
    if (!sPanelMode.empty())
    {
        ocs::ipc::registerDetachedPanel(sPanelMode);
        OCS_LOG_INFO("app", "detached-panel mode: %s",
                     sPanelMode.c_str());
    }

    // Seed list from main(): argv[1..N]. First path becomes active; the
    // rest open as background tabs. Skipped in detached-panel mode —
    // that process hosts just the one panel and has no scene.
    for (const auto& p : sCsdPaths)
        if (sPanelMode.empty()) openTab(p);
    // Back-compat: if a caller only set sCsdPath (older entry points or
    // tests), treat it as a single-element seed.
    if (sCsdPaths.empty() && !sCsdPath.empty())
        openTab(sCsdPath);
    if (!gTabs.empty())
    {
        gActiveTab = -1;   // force setActiveTab to apply env
        setActiveTab(0);
    }

    // macOS native File menu (Open / Save / Save As) — shows in the system
    // menu bar at the top of the screen. All callbacks act on the active
    // tab's editor; they no-op when there are no tabs open.
    ocs::MacMenuCallbacks cb;
    cb.onSave = [] { if (auto* e = activeEditor()) e->saveToCurrent(); };
    cb.onSaveAs = [](const std::string& p) {
        if (auto* e = activeEditor()) e->saveAs(p);
    };
    cb.onExportCsb = [] { if (auto* e = activeEditor()) e->exportCsb(); };
    cb.onTogglePanel = [](int panelId) {
        auto* e = activeEditor();
        if (!e) return;
        const char* name = nullptr;
        bool* flag = nullptr;
        switch (panelId)
        {
            case 0: name = "Scene";          flag = &e->_showScenePanel;        break;
            case 1: name = "Assets";         flag = &e->_showAssetsPanel;       break;
            case 2: name = "Properties";     flag = &e->_showPropertiesPanel;   break;
            case 3: name = "Layout";         flag = &e->_showLayoutPanel;       break;
            case 4: name = "Mode";           flag = &e->_showModePanel;         break;
            case 5: name = "Messages";       flag = &e->_showMessagesPanel;     break;
            case 6: name = "Timeline";       flag = &e->_showTimelinePanel;     break;
            case 7: name = "Sprite Editor";  flag = &e->_showSpriteEditor;      break;
            case 8: name = "Sprite Catalog"; flag = &e->_showSpriteCatalog;     break;
        }
        if (!name || !flag) return;
        // If the panel is currently detached, toggle means "make it
        // disappear" — SIGTERM the child process and drop the name
        // from gSuppressedByDetach so close-to-reattach doesn't
        // auto-re-show it. The panel stays hidden. A second toggle
        // click later (on the now-hidden panel) re-shows it at its
        // last known docked position, because ImGui remembers the
        // dock state across hide/show as long as we don't rebuild.
        const auto detached = ocs::ipc::listDetachedPanels();
        const bool isDetached =
            std::find(detached.begin(), detached.end(), name) !=
            detached.end();
        if (isDetached)
        {
            // Toggle on a detached panel = "bring it home": close
            // the detached window and let the bidirectional sync
            // flip _show back to true on the next frame (the name
            // stays in gSuppressedByDetach, so the sync's "was
            // suppressed, no longer detached" branch reattaches
            // the panel at its last docked position).
            ocs::ipc::terminateDetachedPanel(name);
            return;
        }
        *flag = !*flag;
    };
    cb.isPanelVisible = [](int panelId) -> bool {
        auto* e = activeEditor();
        if (!e) return true;
        // A panel counts as "visible" for menu-checkmark purposes
        // whenever it exists somewhere — either docked here or
        // detached in its own OS window. That way toggling an item
        // whose panel is detached correctly presents the user with
        // a checked state to uncheck (which closes the detached
        // window via onTogglePanel's detach-branch).
        const char* name = nullptr;
        bool localShown = false;
        switch (panelId)
        {
            case 0: name = "Scene";          localShown = e->_showScenePanel;        break;
            case 1: name = "Assets";         localShown = e->_showAssetsPanel;       break;
            case 2: name = "Properties";     localShown = e->_showPropertiesPanel;   break;
            case 3: name = "Layout";         localShown = e->_showLayoutPanel;       break;
            case 4: name = "Mode";           localShown = e->_showModePanel;         break;
            case 5: name = "Messages";       localShown = e->_showMessagesPanel;     break;
            case 6: name = "Timeline";       localShown = e->_showTimelinePanel;     break;
            case 7: name = "Sprite Editor";  localShown = e->_showSpriteEditor;      break;
            case 8: name = "Sprite Catalog"; localShown = e->_showSpriteCatalog;     break;
        }
        if (localShown) return true;
        if (!name) return false;
        const auto detached = ocs::ipc::listDetachedPanels();
        return std::find(detached.begin(), detached.end(), name) !=
               detached.end();
    };
    cb.onResetLayout = [] {
        // Full reset: every panel re-appears at its default docked
        // position in this window, and any detached panel processes
        // are asked to exit so their panels return home too.
        // terminateDetachedPanels() removes the registry files
        // synchronously, so the singleton-sync block won't re-hide
        // the panels we just un-hid on the next frame.
        ocs::ipc::terminateDetachedPanels();
        gSuppressedByDetach.clear();
        if (auto* e = activeEditor())
        {
            e->_showScenePanel      = true;
            e->_showAssetsPanel     = true;
            e->_showPropertiesPanel = true;
            e->_showLayoutPanel     = true;
            e->_showModePanel       = true;
            e->_showMessagesPanel   = true;
            e->_showTimelinePanel   = true;
            e->requestLayoutRebuild();
        }
        OCS_LOG_INFO("app", "view: reset layout");
    };
    cb.onDetachPanel = [](int panelId) {
        // Match the same ordering used by onTogglePanel / isPanelVisible.
        const char* name = nullptr;
        switch (panelId)
        {
            case 0: name = "Scene";          break;
            case 1: name = "Assets";         break;
            case 2: name = "Properties";     break;
            case 3: name = "Layout";         break;
            case 4: name = "Mode";           break;
            case 5: name = "Messages";       break;
            case 6: name = "Timeline";       break;
            case 7: name = "Sprite Editor";  break;
            case 8: name = "Sprite Catalog"; break;
        }
        if (!name) return;
        // Measure the panel's actual ImGui Size — open the OS
        // window at exactly that footprint. No clamping, no
        // ContentSize transformation, no per-panel floor: the user
        // expectation is "the new window matches the panel's size".
        int w = 0, h = 0;
        if (auto* win = ImGui::FindWindowByName(name))
        {
            w = (int)win->Size.x;
            h = (int)win->Size.y;
            if (auto* e = activeEditor())
                e->appendInfoLog("detach-measure",
                    std::string(name) + ": Size=" +
                    std::to_string(w) + "x" + std::to_string(h));
        }
        // Hide the local docked copy now — peer sync (via the IPC
        // `detached.<Name>` file the spawned process writes at startup)
        // takes effect once that process publishes its registration,
        // but the local window shouldn't wait for a round-trip.
        if (auto* e = activeEditor())
        {
            switch (panelId)
            {
                case 0: e->_showScenePanel      = false; break;
                case 1: e->_showAssetsPanel     = false; break;
                case 2: e->_showPropertiesPanel = false; break;
                case 3: e->_showLayoutPanel     = false; break;
                case 4: e->_showModePanel       = false; break;
                case 5: e->_showMessagesPanel   = false; break;
                case 6: e->_showTimelinePanel   = false; break;
                case 7: e->_showSpriteEditor    = false; break;
                case 8: e->_showSpriteCatalog   = false; break;
            }
        }
        spawnDetachedPanel(name, w, h);
    };
    cb.onOpenPreferences = [] {
        if (auto* e = activeEditor()) e->_showPreferences = true;
    };
    cb.onOpen = [](const std::string& p) {
        const int idx = openTab(p);
        if (idx >= 0) setActiveTab(idx);
    };
    cb.onNewProject = [](const std::string& rootDir) {
        // Scaffold a minimal Cocos-Studio-compatible project layout:
        //   <rootDir>/Resources/
        //   <rootDir>/cocosstudio/scenes/Main.csd
        // and print the scene path so the user can relaunch with it.
        namespace fs = std::filesystem;
        try
        {
            const fs::path root(rootDir);
            fs::create_directories(root / "Resources");
            const auto scenesDir = root / "cocosstudio" / "scenes";
            fs::create_directories(scenesDir);
            const auto csd = scenesDir / "Main.csd";

            static constexpr const char* kTemplate =
"<GameFile>\n"
"  <PropertyGroup Name=\"Main\" Type=\"Layer\" ID=\"new-project\" Version=\"3.10.0.0\" />\n"
"  <Content ctype=\"GameProjectContent\">\n"
"    <Content>\n"
"      <Animation Duration=\"0\" Speed=\"1.0000\" />\n"
"      <ObjectData Name=\"Layer\" CanEdit=\"True\" FrameEvent=\"\" ctype=\"GameLayerObjectData\">\n"
"        <Size X=\"1600.0000\" Y=\"900.0000\" />\n"
"        <Children />\n"
"      </ObjectData>\n"
"    </Content>\n"
"  </Content>\n"
"</GameFile>\n";
            std::ofstream out(csd, std::ios::binary);
            out << kTemplate;
            std::printf("[ocs] New project scaffolded:\n"
                        "         %s\n"
                        "       Relaunch with:\n"
                        "         Walshard Studio %s\n",
                        rootDir.c_str(), csd.string().c_str());
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "[ocs] new project failed: %s\n", e.what());
        }
    };
    // Detached-panel mode installs a stripped-down menu with just
    // View → Reattach to Parent Window — no panel-toggle list, no
    // Detach submenu, no Reset Layout. The reattach handler calls
    // Director::end(); the process's shutdown path (std::atexit
    // hook in IpcBridge) removes the `detached.<Name>` registry,
    // so the main-app's close-to-reattach sync brings the panel
    // home at its last docked position.
    if (sPanelMode.empty())
    {
        ocs::installMacMenu(cb);
    }
    else
    {
        cb.onReattachToParent = [] {
            OCS_LOG_INFO("app", "detached-panel: reattach via menu");
            ax::Director::getInstance()->end();
        };
        ocs::installMacMenuForDetachedPanel(cb);
    }

    // Let extension-side panels (asset Add, future Open…) use the native
    // file dialog without reaching into host code.
    opencs::setImagePicker([] { return ocs::showOpenImageDialog(); });
    // Per-panel "↗ Detach" button uses the same flow as the View →
    // Detach Panel menu — hide the docked copy in this window, then
    // spawn a child process with `--panel=<name> --size=<w>x<h>`.
    opencs::setDetachPanelFn([](const std::string& name, int w, int h) {
        if (auto* e = activeEditor())
        {
            if (name == "Scene")      e->_showScenePanel      = false;
            else if (name == "Assets")     e->_showAssetsPanel     = false;
            else if (name == "Properties") e->_showPropertiesPanel = false;
            else if (name == "Layout")     e->_showLayoutPanel     = false;
            else if (name == "Mode")       e->_showModePanel       = false;
            else if (name == "Messages")   e->_showMessagesPanel   = false;
        }
        spawnDetachedPanel(name, w, h);
    });
    imgui->addRenderLoop("#opencs", [] {
        // Pick up any cross-process log-level change so a Trace
        // toggle in any Messages panel propagates to every OCS
        // process — otherwise the dropdown only affects its own
        // process and you'd miss events from peer windows.
        opencs::pollSharedLogLevel();

        // Detached-panel mode (child process spawned via `--panel=<N>`):
        // render only the named panel full-window. No dockspace, no
        // tabs, no peer scanning — the process exists just to host one
        // panel in its own OS window.
        if (!AppDelegate::sPanelMode.empty())
        {
            renderDetachedPanelWindow(AppDelegate::sPanelMode);

            // Detached-process event log → publish so the main app's
            // Messages panel can aggregate "what's happening in every
            // OCS process" in one view. Tracks OS-window resize +
            // an initial "spawn" line on first frame.
            {
                static std::deque<std::string> sEvents;
                static int sLastW = -1, sLastH = -1;
                static bool sLogged = false;
                int wx = 0, wy = 0;
                if (auto* rv = dynamic_cast<RenderViewImpl*>(
                        ax::Director::getInstance()->getRenderView()))
                {
                    if (auto* gw = rv->getWindow())
                        glfwGetWindowSize(gw, &wx, &wy);
                }
                auto push = [&](const std::string& line) {
                    sEvents.push_back(line);
                    while (sEvents.size() > 32) sEvents.pop_front();
                };
                char buf[160];
                if (!sLogged && wx > 0 && wy > 0)
                {
                    std::snprintf(buf, sizeof(buf),
                        "%7.2fs spawn pid=%d panel=%s size=%dx%d",
                        ImGui::GetTime(), (int)::getpid(),
                        AppDelegate::sPanelMode.c_str(), wx, wy);
                    push(buf);
                    sLogged = true;
                }
                if (sLastW > 0 && (wx != sLastW || wy != sLastH))
                {
                    std::snprintf(buf, sizeof(buf),
                        "%7.2fs resize pid=%d panel=%s %dx%d (was %dx%d)",
                        ImGui::GetTime(), (int)::getpid(),
                        AppDelegate::sPanelMode.c_str(),
                        wx, wy, sLastW, sLastH);
                    push(buf);
                }
                sLastW = wx; sLastH = wy;
                std::string body;
                for (auto& l : sEvents) body += l + "\n";
                ocs::ipc::publishMirror("events", body);
            }

            // Child-process lifecycle: a detached panel exists only to
            // accompany a main-app (canvas-bearing) OCS window. When no
            // such peer remains, the panel has nothing useful to show,
            // so it terminates itself. Tiny startup grace period so a
            // race between this child's first render and the parent's
            // first .window publish doesn't trigger a premature exit.
            static double sLonelySince = 0.0;
            if (ocs::ipc::listPeers().empty())
            {
                const double t = ImGui::GetTime();
                if (sLonelySince == 0.0) sLonelySince = t;
                if (t - sLonelySince > 1.5)
                {
                    OCS_LOG_INFO("app",
                        "detached-panel: no main-app peers, exiting");
                    ax::Director::getInstance()->end();
                }
            }
            else
            {
                sLonelySince = 0.0;
            }
            return;
        }

        // --- Drag-out detection: docked panel → detached OS window ---
        // Watch g.MovingWindow each frame. When the user drags one of
        // our panels by its title or tab, track whether the cursor
        // ever leaves the app window's viewport. On mouse release, if
        // it did, hide the docked copy here and spawn a detached
        // process at the panel's current footprint. Mirrors the "drag
        // tab out = new window" Notepad++ pattern, but for panels.
        {
            auto* g = ImGui::GetCurrentContext();
            static ImGuiID sMovingPanelId = 0;
            static std::string sMovingPanelName;
            static bool sEverOutside = false;
            static int sCapturedW = 0, sCapturedH = 0;
            static const char* const kPanels[] = {
                "Scene", "Assets", "Properties", "Layout", "Messages"
            };
            if (g->MovingWindow)
            {
                // Which of our panels (if any) owns the drag?
                if (sMovingPanelName.empty())
                {
                    for (const char* name : kPanels)
                    {
                        auto* win = ImGui::FindWindowByName(name);
                        if (!win) continue;
                        if (g->MovingWindow == win ||
                            g->MovingWindow->RootWindow == win->RootWindow ||
                            g->MovingWindow->RootWindowDockTree ==
                                win->RootWindowDockTree)
                        {
                            sMovingPanelId   = win->ID;
                            sMovingPanelName = name;
                            // ImGui has switched the panel into floating
                            // mode by the time MovingWindow flips, so
                            // win->Size is the standalone-window footprint
                            // — exactly what we want for the spawned OS
                            // window. Earlier we tried snapshotting the
                            // pre-drag dock-slot size, but that included
                            // the slot's full chrome and felt too big.
                            sCapturedW = (int)win->Size.x;
                            sCapturedH = (int)win->Size.y;
                            break;
                        }
                    }
                }
                // Cursor outside the app window's viewport yet?
                if (!sMovingPanelName.empty())
                {
                    const auto* vp = ImGui::GetMainViewport();
                    const ImVec2 m = ImGui::GetMousePos();
                    if (m.x < vp->Pos.x ||
                        m.x >= vp->Pos.x + vp->Size.x ||
                        m.y < vp->Pos.y ||
                        m.y >= vp->Pos.y + vp->Size.y)
                    {
                        sEverOutside = true;
                    }
                }
            }
            else if (sMovingPanelId != 0)
            {
                // Drag ended. If cursor went outside at any point, detach.
                if (sEverOutside && !sMovingPanelName.empty())
                {
                    if (auto* e = activeEditor())
                    {
                        const std::string& n = sMovingPanelName;
                        if      (n == "Scene")      e->_showScenePanel      = false;
                        else if (n == "Assets")     e->_showAssetsPanel     = false;
                        else if (n == "Properties") e->_showPropertiesPanel = false;
                        else if (n == "Layout")     e->_showLayoutPanel     = false;
                        else if (n == "Messages")   e->_showMessagesPanel   = false;
                    }
                    const int w = sCapturedW > 0 ? sCapturedW : 360;
                    const int h = sCapturedH > 0 ? sCapturedH : 420;
                    OCS_LOG_INFO("app",
                        "drag-out: detaching panel=%s (%dx%d)",
                        sMovingPanelName.c_str(), w, h);
                    spawnDetachedPanel(sMovingPanelName, w, h);
                }
                sMovingPanelId = 0;
                sMovingPanelName.clear();
                sEverOutside = false;
                sCapturedW = sCapturedH = 0;
            }
        }

        // Cross-process event aggregation: each detached panel
        // process publishes a `<pid>.events.mirror` with its own
        // resize / spawn events. Each frame we diff against last
        // scan and append the new lines into our own _eventLog so
        // the docked Messages panel shows what's happening across
        // all OCS windows in real time.
        {
            static std::map<int, std::string> sLastPeerEvents;
            const auto peers = ocs::ipc::listPeerEventMirrors();
            std::set<int> alivePids;
            for (auto& [pid, body] : peers)
            {
                alivePids.insert(pid);
                auto& last = sLastPeerEvents[pid];
                if (body == last) continue;
                // Append only the suffix that's new since last scan.
                // Round common-prefix down to the previous newline so
                // we don't paste a partial line into the log.
                std::size_t common = 0;
                while (common < body.size() && common < last.size() &&
                       body[common] == last[common])
                    ++common;
                while (common > 0 && body[common - 1] != '\n') --common;
                const std::string suffix = body.substr(common);
                last = body;
                if (auto* e = activeEditor())
                {
                    std::size_t i = 0;
                    while (i < suffix.size())
                    {
                        auto eol = suffix.find('\n', i);
                        if (eol == std::string::npos) eol = suffix.size();
                        const std::string line = suffix.substr(i, eol - i);
                        i = eol + 1;
                        if (line.empty()) continue;
                        e->appendInfoLog("peer",
                            "pid=" + std::to_string(pid) + " " + line);
                    }
                }
            }
            // GC entries for peers that exited so a relaunched pid
            // sees its events as fresh (not "already seen").
            for (auto it = sLastPeerEvents.begin();
                 it != sLastPeerEvents.end(); )
                if (alivePids.count(it->first) == 0)
                    it = sLastPeerEvents.erase(it);
                else
                    ++it;
        }

        // Window-resize event → Messages log. Poll glfwGetWindowSize
        // each frame and log when it changes (axmol's RenderViewImpl
        // already owns the GLFW size callback, so we can't override
        // it without breaking internal resize handling — polling is
        // the path of least resistance).
        {
            static int sPrevWW = -1, sPrevWH = -1;
            int wx = 0, wy = 0;
            if (auto* rv = dynamic_cast<RenderViewImpl*>(
                    ax::Director::getInstance()->getRenderView()))
            {
                if (auto* gw = rv->getWindow())
                    glfwGetWindowSize(gw, &wx, &wy);
            }
            if ((wx != sPrevWW || wy != sPrevWH) &&
                sPrevWW > 0 && sPrevWH > 0)
            {
                if (auto* e = activeEditor())
                    e->appendInfoLog("resize",
                        std::to_string(wx) + "x" + std::to_string(wy) +
                        " (was " + std::to_string(sPrevWW) + "x" +
                        std::to_string(sPrevWH) + ")");
            }
            sPrevWW = wx;
            sPrevWH = wy;
        }

        // Publish our focus mtime whenever this window is key, so
        // detached-panel processes can pick the most-recently-focused
        // main-app as their mirror source. `focused` flips via GLFW's
        // windowFocusCallback — we read the attribute each frame so
        // we don't need a separate callback subscription.
        {
            bool focused = true;
            if (auto* rv = dynamic_cast<RenderViewImpl*>(
                    ax::Director::getInstance()->getRenderView()))
            {
                if (GLFWwindow* w = rv->getWindow())
                    focused = glfwGetWindowAttrib(w, GLFW_FOCUSED) != 0;
            }
            if (focused) ocs::ipc::publishFocus();
        }

        // Publish the Messages panel's serialized state so detached
        // panel processes can mirror it. Only re-write the file when
        // the text actually changes (cheap string compare) so we
        // don't stat-thrash /tmp for peers polling mtime. Extend to
        // Properties / Scene / ... as later panels get serializers.
        if (auto* e = activeEditor())
        {
            auto mirror = [&](const char* name, std::string& last) {
                std::string body = e->serializePanelMirror(name);
                if (body != last)
                {
                    ocs::ipc::publishMirror(name, body);
                    last = std::move(body);
                }
            };
            static std::string sLastMessagesMirror;
            static std::string sLastPropertiesMirror;
            static std::string sLastSceneMirror;
            static std::string sLastAssetsMirror;
            static std::string sLastLayoutMirror;
            mirror("Messages",   sLastMessagesMirror);
            mirror("Properties", sLastPropertiesMirror);
            mirror("Scene",      sLastSceneMirror);
            mirror("Assets",     sLastAssetsMirror);
            mirror("Layout",     sLastLayoutMirror);
            // docpath mirror lets a detached process load the same
            // .csd locally and render the real panel UI, so it gets
            // every panel feature (lock icon, drag-drop, expand/
            // collapse, context menus) without us re-implementing
            // each one in the simplified mirror renderer.
            static std::string sLastDocPath;
            static std::string sLastDocXml;
            if (auto* d = e->doc())
            {
                std::string body = d->path.string();
                if (body != sLastDocPath)
                {
                    ocs::ipc::publishMirror("docpath", body);
                    sLastDocPath = std::move(body);
                }
                // Full XML snapshot — published only when changed
                // (cheap string compare; the doc's dirty flag isn't
                // perfectly synced with every mutation, so just
                // serialize and diff each frame).
                std::ostringstream oss;
                d->doc.save(oss, "  ");
                std::string xml = oss.str();
                if (xml != sLastDocXml)
                {
                    ocs::ipc::publishMirror("docxml", xml);
                    sLastDocXml = std::move(xml);
                }
            }
        }

        // Singleton-panel sync (bidirectional). When a peer registers
        // a detached panel we hide our docked copy; when the peer
        // later exits (OS ✕ or terminateDetachedPanels) the registry
        // file is GC'd, the name drops out of the list, and we flip
        // _show back to true — ImGui's Begin will restore the window
        // at its last-known docked position so the panel re-appears
        // exactly where the user dragged it out from.
        //
        // sSuppressedByDetach tracks ONLY the panels we hid due to a
        // detach, so a panel the user closes via View → toggle or
        // titlebar-✕ never gets auto-re-shown here.
        if (auto* e = activeEditor())
        {
            const auto detached = ocs::ipc::listDetachedPanels();
            std::set<std::string> det(detached.begin(), detached.end());
            auto apply = [&](const char* name, bool& flag) {
                const bool isDetached = det.count(name) > 0;
                const bool wasSuppressed =
                    gSuppressedByDetach.count(name) > 0;
                if (isDetached)
                {
                    flag = false;
                    gSuppressedByDetach.insert(name);
                }
                else if (wasSuppressed)
                {
                    flag = true;
                    gSuppressedByDetach.erase(name);
                    // Reattach before this frame's Editor::render so
                    // the panel lands in its previous dock slot (or
                    // triggers a layout rebuild when that slot was
                    // collapsed during the detach).
                    e->reattachPanel(name);
                }
            };
            apply("Scene",      e->_showScenePanel);
            apply("Assets",     e->_showAssetsPanel);
            apply("Properties", e->_showPropertiesPanel);
            apply("Layout",     e->_showLayoutPanel);
            apply("Mode",       e->_showModePanel);
            apply("Messages",   e->_showMessagesPanel);
        }

        // [DISABLED 2026-04-27] Drag-end DPI-cross handling.
        //
        // Multiple iterations tried (full layout rebuild, dock
        // resnap, design-resolution re-pump, deferred window restore
        // via the scheduler) — each had its own failure mode (crash
        // mid-frame, panels shrinking 1 s after settle, canvas
        // shrinking on the new screen, app freeze after multiple
        // swaps). No DPI-cross handler runs at all until we have a
        // clean strategy; the issue is tracked in BUGS.md.
        // Pick up any cross-process "open this tab" requests from
        // peers that dragged a tab onto our tab bar. Handled BEFORE
        // the tab bar render so a dropped tab becomes active this
        // same frame.
        ocs::ipc::drainInbox(
            [](const std::string& path, int insertIdx) {
                const int placedAt = (insertIdx >= 0)
                    ? openTabAt(insertIdx, path)
                    : openTab(path);
                if (placedAt >= 0) setActiveTab(placedAt);
            },
            // Reattach request from a detached panel process — just
            // flip the matching visibility flag back on so the panel
            // re-appears in this window's dock layout at its saved
            // position. The detached process exits after sending, and
            // the peer-GC in listDetachedPanels() removes the stale
            // `detached.<Name>` file on the next scan.
            [](const std::string& panelName) {
                auto* e = activeEditor();
                if (!e) return;
                if      (panelName == "Scene")      e->_showScenePanel      = true;
                else if (panelName == "Assets")     e->_showAssetsPanel     = true;
                else if (panelName == "Properties") e->_showPropertiesPanel = true;
                else if (panelName == "Layout")     e->_showLayoutPanel     = true;
                else if (panelName == "Mode")       e->_showModePanel       = true;
                else if (panelName == "Messages")   e->_showMessagesPanel   = true;
                OCS_LOG_INFO("app",
                    "dock-back: re-attached %s", panelName.c_str());
            },
            // Edit request from a detached Properties panel — apply
            // it to the currently-active editor's document. If the
            // node path doesn't resolve (e.g. the user switched tabs
            // mid-send), the call is a silent no-op.
            [](const std::string& nodepath,
               const std::string& key,
               const std::string& value) {
                if (auto* e = activeEditor())
                    e->applyMirrorEdit(nodepath, key, value);
            },
            // Select request from a detached Scene-tree click.
            [](const std::string& nodepath) {
                if (auto* e = activeEditor()) e->selectByPath(nodepath);
            },
            // Asset-select request from a detached Assets click.
            [](const std::string& absPath) {
                if (auto* e = activeEditor())
                    e->setSelectedAsset(absPath);
            },
            // Full-document replace from a detached panel that
            // mutated its local copy (e.g., toggled a lock, dragged
            // a node). We push undo before the replace so Cmd+Z
            // reverts the remote-originated change just like a
            // local one.
            [](const std::string& xmlBody) {
                auto* e = activeEditor();
                if (!e || !e->doc()) return;
                e->pushUndo();
                try { e->doc()->reloadFromString(xmlBody); }
                catch (...) { return; }
                e->rebuildAxMapsAndSync();
                e->doc()->dirty = true;
                OCS_LOG_INFO("app",
                    "replaced doc from peer (%zu bytes)", xmlBody.size());
            });
        // Tab bar runs BEFORE the active Editor's render() so any tab
        // switch or close this frame takes effect immediately — the
        // Editor then draws for whichever tab is current.
        const bool anyTabs = renderTabBar();
        if (auto* e = activeEditor()) e->render();
        else if (!anyTabs)
        {
            // Center a compact welcome card on the viewport. Fixed
            // minimum size + AlwaysAutoResize keeps the text on one
            // readable line regardless of what ImGui's layout
            // heuristics picked before.
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            constexpr float kW = 480.0f;
            const ImVec2 center(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                vp->WorkPos.y + vp->WorkSize.y * 0.5f);
            ImGui::SetNextWindowPos(center, ImGuiCond_Always,
                                    ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(kW, 0), ImGuiCond_Always);
            constexpr ImGuiWindowFlags kFlags =
                ImGuiWindowFlags_NoResize    |
                ImGuiWindowFlags_NoCollapse  |
                ImGuiWindowFlags_NoDocking   |
                ImGuiWindowFlags_NoSavedSettings;
            if (ImGui::Begin("Walshard Studio", nullptr, kFlags))
            {
                ImGui::TextWrapped(
                    "No scene loaded.\n"
                    "Pass one or more .csd paths as argv, or use File → Open.");
            }
            ImGui::End();
        }
    }, nullptr);

    // macOS live window resize takes over the Cocoa runloop (NSEventTracking
    // mode) and starves axmol's main loop. GLFW still dispatches the size
    // callback during the drag — we piggy-back on it to force one extra
    // render per resize event so the panels + canvas reflow live instead of
    // snapping at drag end. Also update the scene's centering offset BEFORE
    // the forced draw, otherwise the scene would render at the previous
    // frame's cached offset and visibly lag the panels.
    director->getEventDispatcher()->addCustomEventListener(
        RenderViewImpl::EVENT_WINDOW_RESIZED,
        [](EventCustom* ev) {
            if (auto* sz = static_cast<ax::Size*>(ev->getUserData()))
            {
                OCS_LOG_TRACE("window",
                    "EVENT_WINDOW_RESIZED %.0fx%.0f (fbs=%.2f)",
                    sz->width, sz->height,
                    ImGui::GetIO().DisplayFramebufferScale.x);
                if (auto* e = activeEditor())
                    e->onWindowResized(sz->width, sz->height);
            }
            // The mid-listener `Director::drawScene()` call was added
            // to keep the canvas redrawing during a live edge-drag,
            // but on a multi-monitor screen swap GLFW emits multiple
            // size + framebuffer-scale events back-to-back; each
            // listener call drained pending events that re-fired the
            // listener, and even with the 16 ms rate-limit + atomic
            // re-entrance flag the main loop ended up starved
            // (user-reported: "after multiple screen swaps the app
            // becomes unresponsive"). The natural per-frame redraw
            // already covers the canvas; only the live edge-drag
            // case loses smoothness, which is an acceptable trade.
        });

    // Monitor-DPI refresh — simulate a windowSize callback with
    // the current (w, h) unchanged.
    //
    // Chain GLFW's size callback: grab axmol's registered handler,
    // replace with a trampoline that forwards to it (so normal
    // edge-nudge resizes keep working), and save the function
    // pointer so we can invoke it directly on drag-end with the
    // CURRENT window size. axmol's handler re-reads
    // glfwGetFramebufferSize, flips `_retinaFactor`, resizes the
    // Metal attachment texture, and dispatches EVENT_WINDOW_RESIZED
    // — exactly what a monitor-swap needs, with no actual window
    // state change.
    director->getEventDispatcher()->addCustomEventListener(
        RenderViewImpl::EVENT_WINDOW_POSITIONED,
        [](EventCustom* ev) {
            using clk = std::chrono::steady_clock;
            const int64_t nowMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    clk::now().time_since_epoch()).count();
            gLastWindowMoveMs.store(nowMs);
            // Throttle the trace output — GLFW can emit dozens of
            // position events per second during a drag, and at Trace
            // level each one locks the ring sink mutex and serialises
            // through every other sink. 100 ms gives a usable
            // breadcrumb without flooding.
            static std::atomic<int64_t> sLastLogMs{0};
            if (nowMs - sLastLogMs.load() >= 100)
            {
                sLastLogMs.store(nowMs);
                if (auto* p = static_cast<ax::Vec2*>(ev->getUserData()))
                    OCS_LOG_TRACE("window",
                        "EVENT_WINDOW_POSITIONED (%.0f,%.0f)",
                        p->x, p->y);
            }
        });
    // (Previously: chained GLFW size-callback trampoline for a
    // replay-on-drag-end refresh. Removed — the replay cascaded
    // into Metal/NSWindow state and froze after repeated resizes.)

    // Trace-log every GLFW window callback so the Messages panel
    // shows the full signal chain during a screen swap / resize.
    // axmol installs its own handlers for some of these — chain ours
    // in front and forward to the existing handler so the editor's
    // resize / centre logic keeps working. Uses GLFW_TRUE that the
    // setter returns the previously-registered callback.
    if (auto* rv = dynamic_cast<RenderViewImpl*>(
            director->getRenderView()))
    {
        if (GLFWwindow* gw = rv->getWindow())
        {
            // Position events fire continuously during drag — throttle
            // the trace so the ring sink isn't flooded.
            static GLFWwindowposfun  sPrevPos =
                glfwSetWindowPosCallback(gw,
                    [](GLFWwindow* w, int x, int y) {
                        using clk = std::chrono::steady_clock;
                        static std::atomic<int64_t> sLast{0};
                        const int64_t now =
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                clk::now().time_since_epoch()).count();
                        if (now - sLast.load() >= 100) {
                            sLast.store(now);
                            OCS_LOG_TRACE("glfw",
                                "windowPos (%d,%d)", x, y);
                        }
                        if (sPrevPos) sPrevPos(w, x, y);
                    });
            static GLFWwindowsizefun sPrevSize =
                glfwSetWindowSizeCallback(gw,
                    [](GLFWwindow* w, int wx, int hy) {
                        OCS_LOG_TRACE("glfw",
                            "windowSize %dx%d", wx, hy);
                        if (sPrevSize) sPrevSize(w, wx, hy);
                    });
            // framebufferSize: trace-only. The earlier "paired with
            // windowContentScale → onWindowResized + layout rebuild"
            // path didn't actually move panels (window points hadn't
            // changed yet, so ImGui dock had nothing to recompute) —
            // user-confirmed broken, removed.
            //
            // windowContentScale IS the DPI-commit signal: when the
            // ratio changes, request a panel scale by the inverse so
            // panels keep their physical pixel area on the new
            // display while the canvas (design-resolution-managed)
            // absorbs the rest.
            static std::atomic<float> sLastContentScale{0.0f};
            // Seed with the current scale so the FIRST DPI cross
            // has a valid `prev` value. Without this the callback's
            // `prev <= 0.001f → return` guard silently swallowed
            // the first cross.
            {
                float xs0 = 1.0f, ys0 = 1.0f;
                glfwGetWindowContentScale(gw, &xs0, &ys0);
                if (xs0 > 0.001f) sLastContentScale.store(xs0);
                OCS_LOG_INFO("dpi",
                    "initial windowContentScale=%.2f", xs0);
            }
            glfwSetFramebufferSizeCallback(gw,
                [](GLFWwindow*, int wpx, int hpx) {
                    OCS_LOG_TRACE("glfw",
                        "framebufferSize %dx%d", wpx, hpx);
                });
            glfwSetWindowContentScaleCallback(gw,
                [](GLFWwindow* gw, float xs, float ys) {
                    OCS_LOG_TRACE("glfw",
                        "windowContentScale %.2f,%.2f", xs, ys);
                    const float prev = sLastContentScale.load();
                    sLastContentScale.store(xs);
                    if (prev <= 0.001f || xs <= 0.001f) return;
                    if (std::abs(prev - xs) < 0.01f) return;
                    // Empirical fix for the Retina ↔ non-Retina
                    // panel-size bug. DisplaySize stays in points
                    // across the swap (only FbScale flips), so
                    // ImGui's per-frame DockNodeUpdate has no
                    // reason to recompute. A real 1px window
                    // resize kicks the recompute, which is what
                    // the user observed manually.
                    //
                    // We can't call glfwSetWindowSize from this
                    // GLFW callback (re-enters ImGui::NewFrame and
                    // asserts), so we defer via performFunction-
                    // InCocosThread: the H+1 nudge runs at the
                    // start of the next frame, then a second
                    // deferred call (queued from inside the first)
                    // restores H one frame later. Two-frame gap
                    // ensures ImGui actually sees the H+1 size and
                    // runs its layout pass before we revert.
                    OCS_LOG_INFO("dpi",
                        "contentScale %.2f → %.2f, "
                        "scheduling 2-frame GLFW resize nudge",
                        prev, xs);
                    int w = 0, h = 0;
                    glfwGetWindowSize(gw, &w, &h);
                    GLFWwindow* gwCap = gw;
                    auto* sched = ax::Director::getInstance()
                                      ->getScheduler();
                    sched->runOnAxmolThread([gwCap, w, h]() {
                        OCS_LOG_TRACE("dpi",
                            "nudge frame 1: glfwSetWindowSize "
                            "%dx%d", w, h + 1);
                        glfwSetWindowSize(gwCap, w, h + 1);
                        // Queue the restore for the *following*
                        // frame so ImGui sees the H+1 size for
                        // one full layout pass before we revert.
                        ax::Director::getInstance()->getScheduler()
                            ->runOnAxmolThread(
                                [gwCap, w, h]() {
                                OCS_LOG_TRACE("dpi",
                                    "nudge frame 2: glfwSet"
                                    "WindowSize %dx%d", w, h);
                                glfwSetWindowSize(gwCap, w, h);
                            });
                    });
                });
            static GLFWwindowfocusfun sPrevFocus =
                glfwSetWindowFocusCallback(gw,
                    [](GLFWwindow* w, int focused) {
                        OCS_LOG_TRACE("glfw",
                            "windowFocus %s",
                            focused ? "gained" : "lost");
                        if (sPrevFocus) sPrevFocus(w, focused);
                    });
            static GLFWwindowiconifyfun sPrevIconify =
                glfwSetWindowIconifyCallback(gw,
                    [](GLFWwindow* w, int iconified) {
                        OCS_LOG_TRACE("glfw",
                            "windowIconify %s",
                            iconified ? "minimised" : "restored");
                        if (sPrevIconify) sPrevIconify(w, iconified);
                    });
            glfwSetWindowMaximizeCallback(gw,
                [](GLFWwindow*, int maximized) {
                    OCS_LOG_TRACE("glfw",
                        "windowMaximize %s",
                        maximized ? "yes" : "no");
                });
            glfwSetWindowRefreshCallback(gw,
                [](GLFWwindow*) {
                    OCS_LOG_TRACE("glfw", "windowRefresh");
                });
            static GLFWwindowclosefun sPrevClose =
                glfwSetWindowCloseCallback(gw,
                    [](GLFWwindow* w) {
                        OCS_LOG_TRACE("glfw", "windowClose");
                        if (sPrevClose) sPrevClose(w);
                    });
        }
    }


    director->runWithScene(gScene);
    return true;
}

void AppDelegate::applicationDidEnterBackground()
{
    Director::getInstance()->stopAnimation();
}

void AppDelegate::applicationWillEnterForeground()
{
    Director::getInstance()->startAnimation();
}
