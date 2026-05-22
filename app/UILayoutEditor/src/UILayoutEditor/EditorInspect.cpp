// Inspect-server command dispatch, extracted from Editor.cpp.
//
// The dispatch is a string-keyed switch with side effects on the Editor's
// public surface (setMode, setSelected, exportCsb, undo/redo, etc.). It's
// self-contained enough to live in its own translation unit — the only
// shared pieces are the small tokenize / path-format helpers and the
// writePair pattern for numeric sub-element updates, both trivial.

#include "UILayoutEditor/Editor.h"
#include "UILayoutEditor/Log.h"

#include "imgui/imgui.h"

#include "axmol.h"
#include "ui/UILayout.h"
#include "ui/UIWidget.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace opencs
{

namespace
{

/// Tokenize by whitespace.
std::vector<std::string> split(const std::string& s)
{
    std::vector<std::string> out;
    std::string tok;
    for (char c : s)
    {
        if (std::isspace(static_cast<unsigned char>(c)))
        { if (!tok.empty()) { out.push_back(std::move(tok)); tok.clear(); } }
        else tok.push_back(c);
    }
    if (!tok.empty()) out.push_back(std::move(tok));
    return out;
}

/// Parse a comma-separated index path like "0,2,1". Accepts the special
/// sentinel "(root)" or an empty string and returns an empty path
/// (== rootNode). Throws std::invalid_argument on non-numeric segments
/// so callers can return an ERR reply instead of crashing.
std::vector<std::size_t> parsePath(const std::string& s)
{
    std::vector<std::size_t> out;
    if (s.empty() || s == "(root)") return out;
    std::string tok;
    auto pushTok = [&]() {
        if (tok.empty()) throw std::invalid_argument("empty path segment");
        std::size_t pos = 0;
        const unsigned long v = std::stoul(tok, &pos);
        if (pos != tok.size())
            throw std::invalid_argument("non-numeric path segment: " + tok);
        out.push_back(static_cast<std::size_t>(v));
        tok.clear();
    };
    for (char c : s)
    {
        if (c == ',') { pushTok(); }
        else tok.push_back(c);
    }
    pushTok();
    return out;
}

/// Parse a uint8 colour channel in [0,255]. Returns -1 on parse error.
int parseChannel(const std::string& s)
{
    if (s.empty()) return -1;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return -1;
    const long v = std::strtol(s.c_str(), nullptr, 10);
    if (v < 0 || v > 255) return -1;
    return static_cast<int>(v);
}

/// Upsert one attribute on a sub-element, preserving order + other attrs.
void upsertSubAttr(opencs::Node& n,
                   const char* tag,
                   const std::string& key,
                   const std::string& value)
{
    auto cur = n.subAttrs(tag);
    std::vector<std::pair<std::string, std::string>> next;
    next.reserve(cur.size() + 1);
    bool wrote = false;
    for (const auto& [k, v] : cur)
    {
        if (k == key) { next.emplace_back(k, value); wrote = true; }
        else            next.emplace_back(k, v);
    }
    if (!wrote) next.emplace_back(key, value);
    n.setSubAttrs(tag, next);
}

/// Write R/G/B (and optional A) into a colour sub-element, preserving any
/// other attrs already present (e.g. Type / Name on the SingleColor element).
void writeColorSub(opencs::Node& n,
                   const char* tag,
                   int R, int G, int B, int A /* -1 = leave alone */)
{
    auto cur = n.subAttrs(tag);
    std::vector<std::pair<std::string, std::string>> next;
    next.reserve(cur.size() + 4);
    bool wR = false, wG = false, wB = false, wA = false;
    auto s = [](int v) { return std::to_string(v); };
    for (const auto& [k, v] : cur)
    {
        if      (k == "R") { next.emplace_back(k, s(R)); wR = true; }
        else if (k == "G") { next.emplace_back(k, s(G)); wG = true; }
        else if (k == "B") { next.emplace_back(k, s(B)); wB = true; }
        else if (k == "A" && A >= 0) { next.emplace_back(k, s(A)); wA = true; }
        else next.emplace_back(k, v);
    }
    if (!wR) next.emplace_back("R", s(R));
    if (!wG) next.emplace_back("G", s(G));
    if (!wB) next.emplace_back("B", s(B));
    if (A >= 0 && !wA) next.emplace_back("A", s(A));
    n.setSubAttrs(tag, next);
}

std::string pathToString(const std::vector<std::size_t>& p)
{
    std::string s;
    for (size_t i = 0; i < p.size(); ++i)
    {
        if (i) s.push_back(',');
        s += std::to_string(p[i]);
    }
    return s.empty() ? std::string("(root)") : s;
}

/// True when every character in `s` is a digit or comma — i.e. the string
/// can be parsed as a numeric index path. Empty string and "(root)" also
/// count as numeric for resolveByPathArg's purposes.
bool looksNumericPath(const std::string& s)
{
    if (s.empty() || s == "(root)") return true;
    for (char c : s)
    {
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != ',')
            return false;
    }
    return true;
}

/// DFS, return the first descendant of `root` whose Name matches `name`.
/// Includes `root` itself in the search.
opencs::Node findNodeByName(const opencs::Node& root, std::string_view name)
{
    if (!root.valid()) return opencs::Node();
    if (root.name() == name) return root;
    for (auto& c : root.children())
    {
        auto r = findNodeByName(c, name);
        if (r.valid()) return r;
    }
    return opencs::Node();
}

/// Direct-child lookup by Name (no recursion).
opencs::Node findChildByName(const opencs::Node& parent, std::string_view name)
{
    if (!parent.valid()) return opencs::Node();
    for (auto& c : parent.children())
    {
        if (c.name() == name) return c;
    }
    return opencs::Node();
}

/// Shell-out HTTP GET via /usr/bin/curl. Returns body string on
/// success, empty on any failure. Caller checks for empty to know.
std::string httpGet(const std::string& url, int timeoutSec = 10)
{
    char cmd[2048];
    std::snprintf(cmd, sizeof(cmd),
        "/usr/bin/curl -sL --max-time %d '%s' 2>/dev/null",
        timeoutSec, url.c_str());
    FILE* f = popen(cmd, "r");
    if (!f) return {};
    std::string out;
    char buf[1024];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), f)) out.append(buf, n);
    int rc = pclose(f);
    if (rc != 0) return {};
    return out;
}

/// Shell-out HTTP download to a file. Returns empty on success,
/// otherwise human-readable error.
std::string httpDownload(const std::string& url,
                         const std::string& dst,
                         int timeoutSec = 30)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(dst).parent_path(), ec);
    char cmd[4096];
    std::snprintf(cmd, sizeof(cmd),
        "/usr/bin/curl -sL --max-time %d --fail '%s' -o '%s' 2>&1",
        timeoutSec, url.c_str(), dst.c_str());
    FILE* f = popen(cmd, "r");
    if (!f) return "popen failed";
    std::string err;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), f)) err += buf;
    int rc = pclose(f);
    if (rc != 0)
    {
        if (!err.empty() && err.back() == '\n') err.pop_back();
        return "curl rc=" + std::to_string(rc) + " " + err;
    }
    if (!fs::exists(dst, ec)) return "curl succeeded but output missing";
    return {};
}

/// Locate an SVG → PNG converter CLI. Order: $OCS_SVG_CONVERTER →
/// rsvg-convert (brew) → Inkscape. Returns empty when none present.
std::string findSvgConverter()
{
    namespace fs = std::filesystem;
    if (const char* env = std::getenv("OCS_SVG_CONVERTER"))
        if (*env && fs::exists(env)) return env;
    for (const char* p : {"/usr/local/bin/rsvg-convert",
                          "/opt/homebrew/bin/rsvg-convert",
                          "/usr/bin/rsvg-convert"})
    {
        std::error_code ec;
        if (fs::exists(p, ec)) return p;
    }
    for (const char* p : {"/usr/local/bin/inkscape",
                          "/opt/homebrew/bin/inkscape",
                          "/Applications/Inkscape.app/Contents/MacOS/inkscape"})
    {
        std::error_code ec;
        if (fs::exists(p, ec)) return p;
    }
    return {};
}

/// Rasterize `src` to `dst` at the requested pixel size (W/H = 0 → leave
/// to the converter to derive from the SVG viewBox). Returns empty
/// string on success, otherwise a human-readable error including the
/// CLI's stderr.
std::string rasterizeSvg(const std::filesystem::path& src,
                         const std::filesystem::path& dst,
                         int W, int H)
{
    namespace fs = std::filesystem;
    std::string tool = findSvgConverter();
    if (tool.empty())
        return "no SVG converter (install rsvg-convert via `brew install librsvg`)";
    if (!fs::exists(src))
        return "source missing: " + src.string();

    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);

    char cmd[4096];
    if (tool.find("rsvg-convert") != std::string::npos)
    {
        if (W > 0 && H > 0)
            std::snprintf(cmd, sizeof(cmd),
                "'%s' -w %d -h %d '%s' -o '%s' 2>&1",
                tool.c_str(), W, H,
                src.string().c_str(), dst.string().c_str());
        else
            std::snprintf(cmd, sizeof(cmd),
                "'%s' '%s' -o '%s' 2>&1",
                tool.c_str(),
                src.string().c_str(), dst.string().c_str());
    }
    else  // Inkscape
    {
        if (W > 0 && H > 0)
            std::snprintf(cmd, sizeof(cmd),
                "'%s' --export-type=png --export-filename='%s' "
                "--export-width=%d --export-height=%d '%s' 2>&1",
                tool.c_str(), dst.string().c_str(), W, H,
                src.string().c_str());
        else
            std::snprintf(cmd, sizeof(cmd),
                "'%s' --export-type=png --export-filename='%s' '%s' 2>&1",
                tool.c_str(),
                dst.string().c_str(), src.string().c_str());
    }

    FILE* f = popen(cmd, "r");
    if (!f) return "popen failed";
    std::string out;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), f)) out += buf;
    int rc = pclose(f);
    if (rc != 0)
    {
        if (!out.empty() && out.back() == '\n') out.pop_back();
        return "converter rc=" + std::to_string(rc) + " " + out;
    }
    if (!fs::exists(dst))
        return "converter succeeded but output missing";
    return {};
}

/// Split a name path like "btnPlay>txtLabel" (separator: '>') into its
/// segments, trimming whitespace.
std::vector<std::string> splitNamePath(const std::string& s)
{
    std::vector<std::string> out;
    std::string tok;
    auto trim = [](std::string& v) {
        while (!v.empty() && std::isspace((unsigned char)v.front())) v.erase(v.begin());
        while (!v.empty() && std::isspace((unsigned char)v.back()))  v.pop_back();
    };
    for (char c : s)
    {
        if (c == '>') { trim(tok); if (!tok.empty()) out.push_back(tok); tok.clear(); }
        else tok.push_back(c);
    }
    trim(tok);
    if (!tok.empty()) out.push_back(std::move(tok));
    return out;
}

}  // namespace

std::string Editor::handleInspectCommand(const std::string& line,
                                         InspectReply* reply)
{
    (void)reply;  // unused until a handler decides to defer
    const auto toks = split(line);
    if (toks.empty()) return "ERR: empty";

    const std::string& cmd = toks[0];

    // -- auth gate --------------------------------------------------------
    // If OCS_INSPECT_TOKEN is set in the environment, mutation /
    // destructive commands require a prior `auth <token>` line on the
    // RPC channel. Read-only inspect (graph/tree/dump/help/etc.) stays
    // open so external tools can sanity-check without first authing.
    if (cmd == "auth")
    {
        const char* env = std::getenv("OCS_INSPECT_TOKEN");
        if (!env || !*env) { _rpcAuthed = true; return "ok (no token configured)\n"; }
        if (toks.size() < 2) return "ERR: auth <token>\n";
        _rpcAuthed = (toks[1] == env);
        return _rpcAuthed ? "ok\n" : "ERR: bad token\n";
    }
    {
        // List of cmds whose effect mutates the doc OR triggers a
        // file/system action. Anything not in this list is treated as
        // read-only inspect.
        static const std::set<std::string> kMutating = {
            "setattr", "setcolor", "setbgcolor", "setfile",
            "setpanelimage", "setvisible", "size", "scale", "rotate",
            "delete", "addchild", "undo", "redo", "save", "reload",
            "export", "script", "apply-to-scenes", "preview",
            "preview-visual", "screenshot", "screenshot-canvas", "setlabel",
            "svg-rasterize", "svg-bake",
            "addbinding", "delbinding",
            "download",
            "vmap-add-layer", "vmap-add-prim",
            "vmap-add-keyframe", "vmap-set-anim",
            "litsprite-add-light",
            "gen-controller",
        };
        const char* env = std::getenv("OCS_INSPECT_TOKEN");
        if (env && *env && !_rpcAuthed && kMutating.count(cmd))
            return "ERR: auth required; send 'auth <token>'\n";
    }

    if (cmd == "help")
    {
        return
            "inspection:\n"
            "  graph                   dump shadow hierarchy (idx,path,name,ctype,bounds)\n"
            "  graph2 [bounds|visual|full]   dump ax scene graph via logger\n"
            "  diff-graph              side-by-side world/imgui/camera projections\n"
            "  overlay-graph           overlay+world AABBs per node\n"
            "  rotdump                 per-node world bl/tr + overlay positions\n"
            "  tree                    dump csd tree (path,name,ctype)\n"
            "  dump                    full .csd xml\n"
            "  selected                current selection path,name,ctype\n"
            "  scene                   running scene transform + viewport info\n"
            "  canvas                  canvas-tagged node transform + size\n"
            "  roots                   scene + loadedRoot world-corner AABBs\n"
            "  state                   editor counters + click/event totals\n"
            "  display                 ImGui display + framebuffer scale\n"
            "  mouse                   current mouse pos + click counter\n"
            "  events                  in-editor click event log\n"
            "  assets                  list scanned asset entries\n"
            "  refs <basename>         csd nodes that reference the asset\n"
            "  log [N | level X | clear]    tail or configure the ring log\n"
            "  list-textures           every asset reference deduped + counted\n"
            "  used-by <relPath>       csd nodes that reference the given path\n"
            "selection / interaction:\n"
            "  select <path|name|a>b>>  index path, first-match by Name, or\n"
            "                          'btnPlay>txtLabel' chain (head DFS, tail child)\n"
            "  hit <x> <y>             hit-test screen coords, print hit node\n"
            "  click <x> <y>           inject click at screen coords\n"
            "  hover <x> <y>           inject hover at screen coords (View mode)\n"
            "  mode [edit|view|side|top]    get or set editor mode\n"
            "  pan [x y] / zoom [v]    get/set pan offset or zoom\n"
            "mutation (push undo + sync to canvas; <path> accepts name or index):\n"
            "  setattr <path> <attr> <value>           top-level or 'Sub.Key' attr\n"
            "  setattr <path> <SubTag> K=V K=V ...     bulk-set one sub-element\n"
            "                                          (works for Size/Position/Scale/AnchorPoint/\n"
            "                                           CColor/SingleColor/FileData/etc.)\n"
            "  setlabel <path> <text...>               raw rest-of-line → LabelText\n"
            "  getattr <path> [attr|Sub.Key]           read one or all attrs\n"
            "  setcolor <path> R G B [A]               set CColor sub-element\n"
            "  setbgcolor <path> [single|first|end] R G B [A]   panel bg color\n"
            "  setfile <path> <relPath> [plist]        ImageView FileData; blank=clear\n"
            "  setpanelimage <path> <relPath> [plist]  Panel bg image; blank=clear\n"
            "  setvisible <path> 0|1 [self|cascade]    toggle visibility (cascade hides children)\n"
            "  size <w> [h]                            set selected Size\n"
            "  scale <sx> [sy]                         set selected Scale\n"
            "  rotate <deg>                            set selected Rotation\n"
            "  delete <path>                           remove node from doc\n"
            "  addchild <parentPath> [ctype] [name]    append empty child\n"
            "  undo / redo                             navigate undo stack\n"
            "document:\n"
            "  save                    save .csd to current path\n"
            "  reload [path] [force]   re-read csd from disk (force drops unsaved)\n"
            "  export [outPath]        compile to .csb (default sibling)\n"
            "  script <file>           run mutation cmds line-by-line (# = comment)\n"
            "  apply-to-scenes <glob|dir> <script>   reload+script+save+export per .csd\n"
            "agentic:\n"
            "  auth <token>            unlock mutation cmds when OCS_INSPECT_TOKEN set\n"
            "  describe [ctype]        attrs + sub-elements for a known ctype\n"
            "  validate                flag broken asset refs in current doc\n"
            "  preview <cmd...>        dry-run: emit XML diff, restore state (no commit)\n"
            "  preview-visual <out.png> <cmd...>\n"
            "                          apply, capture PNG, rollback (live flash ~2 frames)\n"
            "  screenshot <out.png>    schedule full-window PNG capture\n"
            "  screenshot-canvas <out.png> [W H]\n"
            "                          canvas-only via RenderTexture (no editor chrome)\n"
            "  watch on|off            subscribe this connection to EVENT lines\n"
            "                          (emits: selection, dirty, mode, opened,\n"
            "                           saved, exported)\n"
            "vector maps:\n"
            "  vmap-add-layer <path> <layerName>\n"
            "                          append empty Layer under VectorMapData\n"
            "  vmap-add-prim <path> <layer> <kind> K=V K=V ...\n"
            "                          kind = Polygon|Polyline|Circle;\n"
            "                          common K=V: Points / X Y Radius / Fill\n"
            "                          Stroke StrokeWidth\n"
            "  vmap-add-keyframe <path> <layer> <primIdx> <attr> <frame> <value>\n"
            "                          attr = Fill|Stroke|StrokeWidth|Radius|X|Y|Points\n"
            "  vmap-set-anim <path> <layer> Duration=N FPS=N Loop=True|False\n"
            "bindings (declarative event hookup):\n"
            "  addbinding <path> <event> <method> [params...]\n"
            "                          OCSBindings sub-element on node\n"
            "  delbinding <path> <event> [method]\n"
            "                          drop first match\n"
            "  listbindings [path]     dump bindings (whole doc or one node)\n"
            "  gen-controller <outDir> [className]\n"
            "                          emit starter .h/.cpp controller with\n"
            "                          method stubs + wire(scene, sidecar)\n"
            "io:\n"
            "  download <url> <dst>    GET <url> via curl, save to <dst>\n"
            "                          (relative <dst> resolves under assets root)\n"
            "svg pipeline:\n"
            "  svg-rasterize <src> <dst> [W H]\n"
            "                          bake one SVG → PNG via rsvg-convert\n"
            "  svg-bake [rewrite] [w=N] [h=N]\n"
            "                          walk doc, bake every .svg FileData ref to\n"
            "                          sibling .png (mtime-cached); `rewrite`\n"
            "                          flips Path attrs to the .png\n";
    }

    if (cmd == "events")
    {
        // Dump the click event log + the gate state at the time of the
        // last bail. Used to diagnose "canvas clicks don't work" reports
        // when GUI access isn't available — we get the same data the
        // Messages panel shows, plus a one-line gate snapshot.
        std::string out;
        char line[320];
        std::snprintf(line, sizeof(line),
            "gate: hovered=%s active=%s lastBail=%s overlayRuns=%u overlayBails=%u\n",
            _canvasHovered ? "yes" : "no",
            _canvasActive  ? "yes" : "no",
            _editLastBail ? _editLastBail : "(none)",
            _editOverlayRuns, _editOverlayBails);
        out += line;
        out += "events (oldest first):\n";
        for (const auto& ev : _eventLog)
        {
            std::snprintf(line, sizeof(line),
                "%7.2fs (%5.0f,%5.0f) kind=%-22s win=%s hovered=%s "
                "anyItem=%s gated=%s hovId=%u%s%s%s%s\n",
                ev.time, ev.x, ev.y, ev.kind ? ev.kind : "?",
                ev.hoverWinName.empty() ? "(null)" : ev.hoverWinName.c_str(),
                ev.inCentral ? "y" : "n",
                ev.anyItemHovered ? "y" : "n",
                ev.passedGate ? "y" : "n",
                ev.hoveredId,
                ev.hitName.empty()  ? "" : "  hit=",
                ev.hitName.c_str(),
                ev.hitCtype.empty() ? "" : " (",
                ev.hitCtype.empty() ? "" : ev.hitCtype.c_str());
            out += line;
            if (!ev.hitCtype.empty()) out += ")";
        }
        return out;
    }

    if (cmd == "mode")
    {
        if (toks.size() == 1)
        {
            switch (_mode) {
                case Mode::Edit:      return "edit\n";
                case Mode::View:      return "view\n";
                case Mode::LayerSide: return "side\n";
                case Mode::LayerTop:  return "top\n";
            }
        }
        if (toks[1] == "edit") { setMode(Mode::Edit); return "ok edit\n"; }
        if (toks[1] == "view" || toks[1] == "run")
        { setMode(Mode::View); return "ok view\n"; }
        if (toks[1] == "side") { setMode(Mode::LayerSide); return "ok side\n"; }
        if (toks[1] == "top")  { setMode(Mode::LayerTop);  return "ok top\n"; }
        return "ERR: mode must be edit|view|side|top\n";
    }

    if (cmd == "graph")
    {
        if (_axOrderDfs.empty()) return "(empty) axmol root not set\n";
        std::string out;
        out.reserve(2048);
        int idx = 0;
        for (ax::Node* n : _axOrderDfs)
        {
            auto it = _axToCsd.find(n);
            Node csd = (it != _axToCsd.end()) ? it->second : Node();
            auto path = pathTo(csd);
            const auto cs = n->getContentSize();
            const auto w0 = n->convertToWorldSpace(ax::Vec2(0, 0));
            const auto w1 = n->convertToWorldSpace(ax::Vec2(cs.width, cs.height));
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "[%d] path=%s name=%s ctype=%s size=%.1fx%.1f world=(%.1f,%.1f)-(%.1f,%.1f)\n",
                idx++, pathToString(path).c_str(),
                csd.name().c_str(), csd.ctype().c_str(),
                cs.width, cs.height, w0.x, w0.y, w1.x, w1.y);
            out += buf;
        }
        return out;
    }

    if (cmd == "rotdump")
    {
        // Diagnostic for the "overlay rotates faster than scene"
        // issue: prints per-node world positions (bottom-left +
        // top-right), anchor, positionZ, and — if an overlay
        // DrawNode was attached (tag 0x4C594F56) — its world
        // position. If the two worlds differ under rotation, we
        // know the rotation pivot or Z handling diverges.
        if (_axOrderDfs.empty()) return "(empty) axmol root not set\n";
        std::string out;
        out.reserve(4096);
        int idx = 0;
        constexpr int kLayerTag = 0x4C594F56;   // 'LYOV'
        for (ax::Node* n : _axOrderDfs)
        {
            auto it = _axToCsd.find(n);
            Node csd = (it != _axToCsd.end()) ? it->second : Node();
            const auto cs   = n->getContentSize();
            const auto bl   = n->convertToWorldSpace(ax::Vec2(0, 0));
            const auto tr   = n->convertToWorldSpace(ax::Vec2(cs.width, cs.height));
            const auto anc  = n->getAnchorPoint();
            const float pz  = n->getPositionZ();
            const auto rot3 = n->getRotation3D();

            const ax::Node* over = n->getChildByTag(kLayerTag);
            char overStr[128] = "no-overlay";
            if (over)
            {
                const auto obl = over->convertToWorldSpace(ax::Vec2(0, 0));
                const auto otr = over->convertToWorldSpace(ax::Vec2(cs.width, cs.height));
                const float opz = over->getPositionZ();
                std::snprintf(overStr, sizeof(overStr),
                    "ovr_bl=(%.1f,%.1f) ovr_tr=(%.1f,%.1f) ovr_z=%.2f",
                    obl.x, obl.y, otr.x, otr.y, opz);
            }

            char buf[640];
            std::snprintf(buf, sizeof(buf),
                "[%d] %s world_bl=(%.1f,%.1f) world_tr=(%.1f,%.1f) "
                "anc=(%.2f,%.2f) pz=%.2f rot3=(%.1f,%.1f,%.1f) %s\n",
                idx++, csd.name().c_str(),
                bl.x, bl.y, tr.x, tr.y,
                anc.x, anc.y, pz,
                rot3.x, rot3.y, rot3.z, overStr);
            out += buf;
        }
        return out;
    }

    if (cmd == "tree-since")
    {
        // tree-since <unix-seconds>  — list nodes whose OCSModified
        // attribute is strictly greater than the given timestamp.
        // The editor stamps OCSModified on every node passed through
        // pushUndo (so this captures every mutation made via the
        // editor's RPC + panel surfaces). Useful for incremental
        // sync-back to an agent that knows what state it last saw.
        if (toks.size() < 2) return "ERR: tree-since <unix-seconds>\n";
        if (!_doc) return "ERR: no doc\n";
        const long long since = std::atoll(toks[1].c_str());
        std::string out;
        std::function<void(const Node&, std::vector<std::size_t>&)> walk =
            [&](const Node& n, std::vector<std::size_t>& path)
        {
            const std::string ms(n.element().attribute("OCSModified")
                                  .as_string(""));
            if (!ms.empty() && std::atoll(ms.c_str()) > since)
            {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s\t%s\t%s\t%s\n",
                    pathToString(path).c_str(),
                    n.name().c_str(), n.ctype().c_str(), ms.c_str());
                out += buf;
            }
            auto kids = n.children();
            for (std::size_t i = 0; i < kids.size(); ++i)
            {
                path.push_back(i);
                walk(kids[i], path);
                path.pop_back();
            }
        };
        std::vector<std::size_t> p;
        walk(_doc->rootNode, p);
        return out.empty() ? "(no changes)\n" : out;
    }

    if (cmd == "tree")
    {
        if (!_doc) return "ERR: no doc\n";
        std::string out;
        std::function<void(const Node&, std::vector<std::size_t>&)> walk =
            [&](const Node& n, std::vector<std::size_t>& path)
        {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s\t%s\t%s\n",
                pathToString(path).c_str(),
                n.name().c_str(), n.ctype().c_str());
            out += buf;
            auto kids = n.children();
            for (std::size_t i = 0; i < kids.size(); ++i)
            {
                path.push_back(i);
                walk(kids[i], path);
                path.pop_back();
            }
        };
        std::vector<std::size_t> path;
        walk(_doc->rootNode, path);
        return out;
    }

    if (cmd == "selected")
    {
        if (!_selected.valid()) return "(none)\n";
        auto path = pathTo(_selected);
        return pathToString(path) + "\t" + _selected.name() + "\t" +
               _selected.ctype() + "\n";
    }

    if (cmd == "select")
    {
        if (toks.size() < 2) return "ERR: select <path|name|a>b>c>\n";
        // Accept three forms:
        //   select 0,1,3        — numeric index path (existing)
        //   select panelOpponents — first-match DFS by Name
        //   select btnPlay > txtLabel — sequential name chain (first
        //                                match for the head, direct
        //                                children for subsequent)
        // Reassemble the rest of the line so the user can use '>'
        // with or without surrounding whitespace.
        std::string arg = toks[1];
        for (std::size_t i = 2; i < toks.size(); ++i)
        { arg.push_back(' '); arg += toks[i]; }

        Node n;
        if (looksNumericPath(arg))
        {
            try { n = nodeAtPath(parsePath(arg)); }
            catch (const std::exception&) { return "ERR: bad path\n"; }
        }
        else
        {
            auto segs = splitNamePath(arg);
            if (segs.empty() || !_doc) return "ERR: bad name\n";
            // Head: first DFS match anywhere in the tree.
            n = findNodeByName(_doc->rootNode, segs[0]);
            // Tail: each subsequent segment must be a direct child.
            for (std::size_t i = 1; i < segs.size() && n.valid(); ++i)
                n = findChildByName(n, segs[i]);
        }
        if (!n.valid()) return "ERR: not found\n";
        setSelected(n);
        return pathToString(pathTo(n)) + "\t" + n.name() + "\t" +
               n.ctype() + "\n";
    }

    if (cmd == "hit" || cmd == "click" || cmd == "hover")
    {
        if (toks.size() < 3) return "ERR: " + cmd + " <x> <y>\n";
        const float x = std::strtof(toks[1].c_str(), nullptr);
        const float y = std::strtof(toks[2].c_str(), nullptr);
        // hit and hover just set the sim mouse for the next frame; click
        // additionally sets _simClick which the render path converts into an
        // ImGui::IsMouseClicked-equivalent action on the editor.
        _simMouse  = true;
        _simMouseX = x;
        _simMouseY = y;
        if (cmd == "click") _simClick = true;
        // Immediate hit-test reply using current scene state.
        ax::Node* hit = nullptr;
        if (!_axOrderDfs.empty())
        {
            auto* view = ax::Director::getInstance()->getRenderView();
            const float sx = view->getScaleX();
            const float sy = view->getScaleY();
            const auto vp = view->getViewPortRect();
            const auto fb = view->getFrameSize();
            if (sx > 0 && sy > 0)
            {
                // View's scale + viewport map scene-world to screen
                // points directly. No DisplayFramebufferScale division
                // (axmol reports frame numbers in points already on
                // macOS) and no gTopRoot transform composition (the
                // view's affine includes the full ancestor chain when
                // axmol renders, so feeding it world coords gives the
                // correct screen point). Matches the drawEditOverlay
                // screenToWorld fix.
                const float fbX = x;
                const float fbY = fb.height - y;
                const ax::Vec2 world((fbX - vp.origin.x) / sx,
                                      (fbY - vp.origin.y) / sy);
                for (auto it = _axOrderDfs.rbegin();
                     it != _axOrderDfs.rend(); ++it)
                {
                    ax::Node* n = *it;
                    if (!n->isVisible()) continue;
                    const auto cs = n->getContentSize();
                    if (cs.width <= 0 || cs.height <= 0) continue;
                    const auto local = n->convertToNodeSpace(world);
                    if (local.x >= 0 && local.x < cs.width &&
                        local.y >= 0 && local.y < cs.height) { hit = n; break; }
                }
            }
        }
        if (!hit) return "(miss)\n";
        auto it = _axToCsd.find(hit);
        if (it == _axToCsd.end()) return "(hit unmapped)\n";
        auto& csd = it->second;

        // Apply the effect of a real click so the UI reflects it.
        // In View mode we stay completely hands-off: no editor-side effect.
        if (cmd == "click" && _mode == Mode::Edit) setSelected(csd);

        return pathToString(pathTo(csd)) + "\t" + csd.name() + "\t" +
               csd.ctype() + "\n";
    }

    if (cmd == "dump")
    {
        if (!_doc) return "ERR: no doc\n";
        std::ostringstream ss;
        _doc->doc.save(ss, "", pugi::format_raw | pugi::format_no_declaration);
        return ss.str();
    }

    if (cmd == "save")
    {
        saveToCurrent();
        return _statusMsg + "\n";
    }

    if (cmd == "export")
    {
        // Optional argument = destination .csb path. Default: sibling of csd.
        std::filesystem::path outPath;
        if (toks.size() >= 2) outPath = toks[1];
        const std::string err = exportCsb(outPath);
        return err.empty()
            ? (_statusMsg + "\n")
            : (std::string("ERR: ") + err + "\n");
    }

    if (cmd == "undo") { undo(); return _statusMsg + "\n"; }
    if (cmd == "redo") { redo(); return _statusMsg + "\n"; }

    if (cmd == "size")
    {
        if (toks.size() < 2) return "ERR: size <w> [h]\n";
        if (!_selected.valid()) return "ERR: no selection\n";
        const float w = std::strtof(toks[1].c_str(), nullptr);
        const float h = toks.size() >= 3
            ? std::strtof(toks[2].c_str(), nullptr) : w;
        pushUndo();
        _selected.writePair("Size", "X", w, "Y", h);
        syncNodeCsdToAx(_selected);
        if (_doc) _doc->dirty = true;
        char out[64]; std::snprintf(out, sizeof(out), "sized %.4f,%.4f\n", w, h);
        return out;
    }

    if (cmd == "scale")
    {
        if (toks.size() < 2) return "ERR: scale <sx> [sy]\n";
        if (!_selected.valid()) return "ERR: no selection\n";
        const float sx = std::strtof(toks[1].c_str(), nullptr);
        const float sy = toks.size() >= 3
            ? std::strtof(toks[2].c_str(), nullptr) : sx;
        pushUndo();
        char bx[32], by[32];
        std::snprintf(bx, sizeof(bx), "%.4f", sx);
        std::snprintf(by, sizeof(by), "%.4f", sy);
        auto prev = _selected.subAttrs("Scale");
        std::vector<std::pair<std::string, std::string>> next;
        bool wX = false, wY = false;
        for (const auto& [k, v] : prev) {
            if      (k == "ScaleX") { next.emplace_back(k, bx); wX = true; }
            else if (k == "ScaleY") { next.emplace_back(k, by); wY = true; }
            else                      next.emplace_back(k, v);
        }
        if (!wX) next.emplace_back("ScaleX", bx);
        if (!wY) next.emplace_back("ScaleY", by);
        _selected.setSubAttrs("Scale", next);
        syncNodeCsdToAx(_selected);
        if (_doc) _doc->dirty = true;
        char out[64]; std::snprintf(out, sizeof(out), "scaled %.4f,%.4f\n", sx, sy);
        return out;
    }

    if (cmd == "canvas")
    {
        auto* scene = ax::Director::getInstance()->getRunningScene();
        if (!scene) return "ERR: no scene\n";
        auto* canvas = scene->getChildByTag(0xCADCA12A);
        if (!canvas) return "ERR: no canvas tag\n";
        const auto p = canvas->getPosition();
        const auto cs = canvas->getContentSize();
        const auto ap = canvas->getAnchorPoint();
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "canvas pos=(%.2f,%.2f) scale=(%.3f,%.3f) anchor=(%.3f,%.3f) "
            "size=(%.1fx%.1f) ignoreAnchor=%d rot3d=(%.2f,%.2f,%.2f) "
            "children=%d\n",
            p.x, p.y, canvas->getScaleX(), canvas->getScaleY(),
            ap.x, ap.y, cs.width, cs.height,
            canvas->isIgnoreAnchorPointForPosition() ? 1 : 0,
            canvas->getRotation3D().x, canvas->getRotation3D().y,
            canvas->getRotation3D().z,
            (int)canvas->getChildrenCount());
        return buf;
    }

    if (cmd == "scene")
    {
        auto* scene = ax::Director::getInstance()->getRunningScene();
        if (!scene) return "ERR: no scene\n";
        const auto p = scene->getPosition();
        const auto cs = scene->getContentSize();
        const auto ap = scene->getAnchorPoint();
        auto* rv = ax::Director::getInstance()->getRenderView();
        const auto vp = rv->getViewPortRect();
        const auto fb = rv->getFrameSize();
        ImGuiIO& io = ImGui::GetIO();
        char buf[1024];
        std::snprintf(buf, sizeof(buf),
            "scene pos=(%.2f,%.2f) scale=(%.3f,%.3f) anchor=(%.3f,%.3f) "
            "size=(%.1fx%.1f) ignoreAnchor=%d rot3d=(%.2f,%.2f,%.2f) "
            "zoom=%.3f cached=(%.2f,%.2f) panOff=(%.2f,%.2f)\n"
            "rv scale=(%.3f,%.3f) vp=(%.1f,%.1f %.1fx%.1f) "
            "fb=(%.0fx%.0f) fbs=%.2f displaySize=(%.0fx%.0f)\n",
            p.x, p.y, scene->getScaleX(), scene->getScaleY(),
            ap.x, ap.y, cs.width, cs.height,
            scene->isIgnoreAnchorPointForPosition() ? 1 : 0,
            scene->getRotation3D().x, scene->getRotation3D().y,
            scene->getRotation3D().z,
            _sceneZoom,
            _cachedSceneX, _cachedSceneY, _panOffsetX, _panOffsetY,
            rv->getScaleX(), rv->getScaleY(),
            vp.origin.x, vp.origin.y, vp.size.width, vp.size.height,
            fb.width, fb.height, io.DisplayFramebufferScale.x,
            io.DisplaySize.x, io.DisplaySize.y);
        return buf;
    }

    if (cmd == "log")
    {
        // `log`        → tail last N records from the ring sink.
        // `log N`      → tail last N (N = number).
        // `log level X`→ set global level (trace|debug|info|warn|error).
        // `log clear`  → clear the ring sink.
        if (toks.size() >= 2 && toks[1] == "level")
        {
            if (toks.size() < 3) return "ERR: log level <trace|debug|info|warn|error>\n";
            const auto& s = toks[2];
            auto& L = Log::instance();
            if      (s == "trace") L.setLevel(LogLevel::Trace);
            else if (s == "debug") L.setLevel(LogLevel::Debug);
            else if (s == "info")  L.setLevel(LogLevel::Info);
            else if (s == "warn")  L.setLevel(LogLevel::Warn);
            else if (s == "error") L.setLevel(LogLevel::Error);
            else                   return "ERR: bad level\n";
            return "ok\n";
        }
        if (toks.size() >= 2 && toks[1] == "clear")
        {
            if (auto r = defaultRingSink()) r->clear();
            return "ok\n";
        }
        std::size_t want = 50;
        if (toks.size() >= 2) want = (std::size_t)std::atoi(toks[1].c_str());
        auto ring = defaultRingSink();
        if (!ring) return "ERR: no ring sink\n";
        auto recs = ring->snapshot();
        if (recs.size() > want) recs.erase(recs.begin(), recs.end() - want);
        std::string out;
        for (const auto& r : recs)
        {
            char hdr[64];
            std::snprintf(hdr, sizeof(hdr),
                "[t=%.3f %s %s] ", r.time, levelName(r.level), r.category);
            out += hdr; out += r.message; out += '\n';
        }
        if (out.empty()) out = "(no log records)\n";
        return out;
    }

    if (cmd == "graph2")
    {
        // `graph2 [bounds|visual|full]` → dump axmol scene graph via the
        // logger at the requested verbosity. The legacy `graph` command
        // stays for structured per-node text the test harness uses.
        if (!_axmolRoot) return "ERR: no axmolRoot\n";
        GraphDumpVerbosity v = GraphDumpVerbosity::Bounds;
        if (toks.size() >= 2)
        {
            if      (toks[1] == "bounds") v = GraphDumpVerbosity::Bounds;
            else if (toks[1] == "visual") v = GraphDumpVerbosity::Visual;
            else if (toks[1] == "full")   v = GraphDumpVerbosity::Full;
            else return "ERR: graph2 <bounds|visual|full>\n";
        }
        dumpSceneGraph(_axmolRoot, LogLevel::Info, v);
        // Also tail the records emitted above so the caller sees them
        // without a separate `log` call.
        auto ring = defaultRingSink();
        if (!ring) return "ok\n";
        auto recs = ring->snapshot();
        std::string out;
        // Walk backwards to collect the most recent "graph" category
        // records (the dump we just emitted).
        std::size_t first = recs.size();
        for (std::size_t i = recs.size(); i-- > 0;)
        {
            if (std::string(recs[i].category) == "graph") first = i;
            else if (first != recs.size()) break;
        }
        for (std::size_t i = first; i < recs.size(); ++i)
        {
            out += recs[i].message; out += '\n';
        }
        return out;
    }

    if (cmd == "diff-graph" || cmd == "side-by-side")
    {
        // Three-column side-by-side table per tracked node:
        //   WORLD  — convertToWorldSpace through the parent chain
        //   IMGUI  — what worldToScreen produces (linear viewScale math)
        //   CAMERA — what ax::Camera::project produces (actual render
        //            projection; ground truth for where pixels land)
        //
        // `delta` reports CAMERA-minus-IMGUI in centre coords. A
        // non-zero delta pins the exact per-node offset between what
        // the overlay draws and what the renderer actually paints —
        // the quantity we need to apply as a correction to align them.
        if (_axOrderDfs.empty()) return "(empty) axmol root not set\n";
        auto* view  = ax::Director::getInstance()->getRenderView();
        auto* scene = ax::Director::getInstance()->getRunningScene();
        auto* cam   = scene ? scene->getDefaultCamera() : nullptr;
        const float sx  = view->getScaleX();
        const float sy  = view->getScaleY();
        const auto  vp  = view->getViewPortRect();
        const auto  fb  = view->getFrameSize();
        ImGuiIO& io     = ImGui::GetIO();
        const float fbs = io.DisplayFramebufferScale.x;
        auto w2s = [&](float wx, float wy) -> std::pair<float,float> {
            const float fbX = wx * sx + vp.origin.x;
            const float fbY = wy * sy + vp.origin.y;
            return { fbX / fbs, (fb.height - fbY) / fbs };
        };
        auto camProj = [&](float wx, float wy) -> std::pair<float,float> {
            if (!cam) return { 0.0f, 0.0f };
            const auto p = cam->project(ax::Vec3(wx, wy, 0.0f));
            return { p.x, p.y };
        };
        // Director::convertToUI walks the full pipeline: camera view +
        // projection + viewport + Y-flip to UI (top-left origin).
        // If the renderer and this agree, this is the ground-truth
        // screen position of the world point.
        auto dirUI = [&](float wx, float wy) -> std::pair<float,float> {
            const auto p = ax::Director::getInstance()->convertToUI(
                ax::Vec2(wx, wy));
            return { p.x, p.y };
        };

        constexpr int kNameW = 26;
        constexpr int kBoxW  = 25;
        std::string out;
        out.reserve(8192);
        char hdr[384];
        std::snprintf(hdr, sizeof(hdr),
            "%-*s  %-*s  %-*s  %-*s  %-*s\n",
            kNameW, "node",
            kBoxW, "WORLD",
            kBoxW, "IMGUI (linear)",
            kBoxW, "CAMERA::project",
            kBoxW, "DIRECTOR::convertToUI");
        out += hdr;
        out += std::string(kNameW + kBoxW*4 + 8, '-');
        out += '\n';

        for (ax::Node* n : _axOrderDfs)
        {
            if (!n) continue;
            const auto cs = n->getContentSize();
            const auto w0 = n->convertToWorldSpace(ax::Vec2(0,         0));
            const auto w1 = n->convertToWorldSpace(ax::Vec2(cs.width,  0));
            const auto w2 = n->convertToWorldSpace(ax::Vec2(cs.width,  cs.height));
            const auto w3 = n->convertToWorldSpace(ax::Vec2(0,         cs.height));
            const float wxMin = std::min({w0.x, w1.x, w2.x, w3.x});
            const float wyMin = std::min({w0.y, w1.y, w2.y, w3.y});
            const float wxMax = std::max({w0.x, w1.x, w2.x, w3.x});
            const float wyMax = std::max({w0.y, w1.y, w2.y, w3.y});

            // worldToScreen — linear view scale + viewport offset.
            const auto ia = w2s(wxMin, wyMin);
            const auto ib = w2s(wxMax, wyMax);
            const float ixMin = std::min(ia.first,  ib.first);
            const float iyMin = std::min(ia.second, ib.second);
            const float ixMax = std::max(ia.first,  ib.first);
            const float iyMax = std::max(ia.second, ib.second);

            // Camera::project — the actual camera viewProjection path.
            const auto ca = camProj(wxMin, wyMin);
            const auto cb = camProj(wxMax, wyMax);
            const float cxMin = std::min(ca.first,  cb.first);
            const float cyMin = std::min(ca.second, cb.second);
            const float cxMax = std::max(ca.first,  cb.first);
            const float cyMax = std::max(ca.second, cb.second);

            // Director::convertToUI — full pipeline including any
            // projection matrix quirks the camera applies.
            const auto da = dirUI(wxMin, wyMin);
            const auto db = dirUI(wxMax, wyMax);
            const float dxMin = std::min(da.first,  db.first);
            const float dyMin = std::min(da.second, db.second);
            const float dxMax = std::max(da.first,  db.first);
            const float dyMax = std::max(da.second, db.second);

            std::string name;
            int depth = 0;
            auto it = _axToCsd.find(n);
            if (it != _axToCsd.end() && it->second.valid())
            {
                name  = it->second.name();
                depth = (int)pathTo(it->second).size();
            }
            if (name.empty()) name = "<unnamed>";
            const int spaces = std::min(depth * 2, 14);
            std::string label = std::string(spaces, ' ') + "- " + name;
            if ((int)label.size() > kNameW)
                label.resize(kNameW);

            char wbox[64], ibox[64], cbox[64], dbox[64];
            std::snprintf(wbox, sizeof(wbox),
                "(%.0f,%.0f)-(%.0f,%.0f)", wxMin, wyMin, wxMax, wyMax);
            std::snprintf(ibox, sizeof(ibox),
                "(%.0f,%.0f)-(%.0f,%.0f)", ixMin, iyMin, ixMax, iyMax);
            std::snprintf(cbox, sizeof(cbox),
                "(%.0f,%.0f)-(%.0f,%.0f)", cxMin, cyMin, cxMax, cyMax);
            std::snprintf(dbox, sizeof(dbox),
                "(%.0f,%.0f)-(%.0f,%.0f)", dxMin, dyMin, dxMax, dyMax);

            char line[512];
            std::snprintf(line, sizeof(line),
                "%-*s  %-*s  %-*s  %-*s  %-*s\n",
                kNameW, label.c_str(),
                kBoxW, wbox,
                kBoxW, ibox,
                kBoxW, cbox,
                kBoxW, dbox);
            out += line;
        }
        return out;
    }

    if (cmd == "overlay-graph" || cmd == "controls")
    {
        // Walk the editor's tracked DFS and for each node emit:
        //   - node name + world AABB (what convertToWorldSpace reports)
        //   - the ImGui screen-space AABB the overlay would draw at,
        //     using the same worldToScreen formula drawEditOverlay uses.
        // Side-by-side they reveal exactly where overlay math diverges
        // from the renderer, node by node.
        if (_axOrderDfs.empty()) return "(empty) axmol root not set\n";
        auto* view = ax::Director::getInstance()->getRenderView();
        const float sx  = view->getScaleX();
        const float sy  = view->getScaleY();
        const auto  vp  = view->getViewPortRect();
        const auto  fb  = view->getFrameSize();
        ImGuiIO& io     = ImGui::GetIO();
        const float fbs = io.DisplayFramebufferScale.x;

        auto w2s = [&](float wx, float wy) -> std::pair<float,float> {
            const float fbX = wx * sx + vp.origin.x;
            const float fbY = wy * sy + vp.origin.y;
            return { fbX / fbs, (fb.height - fbY) / fbs };
        };

        std::string out;
        out.reserve(4096);
        // Map each ax::Node* to its csd path so indentation matches the
        // scene structure (pathTo gives a walk-order index that also
        // doubles as a depth hint).
        for (ax::Node* n : _axOrderDfs)
        {
            if (!n) continue;
            const auto cs = n->getContentSize();
            const auto w0 = n->convertToWorldSpace(ax::Vec2(0,         0));
            const auto w1 = n->convertToWorldSpace(ax::Vec2(cs.width,  0));
            const auto w2 = n->convertToWorldSpace(ax::Vec2(cs.width,  cs.height));
            const auto w3 = n->convertToWorldSpace(ax::Vec2(0,         cs.height));
            const float wMinX = std::min({w0.x, w1.x, w2.x, w3.x});
            const float wMinY = std::min({w0.y, w1.y, w2.y, w3.y});
            const float wMaxX = std::max({w0.x, w1.x, w2.x, w3.x});
            const float wMaxY = std::max({w0.y, w1.y, w2.y, w3.y});
            const auto s0 = w2s(wMinX, wMinY);
            const auto s1 = w2s(wMaxX, wMaxY);

            // csd name + depth from pathTo.
            std::string name, ctype;
            int depth = 0;
            auto it = _axToCsd.find(n);
            if (it != _axToCsd.end() && it->second.valid())
            {
                name  = it->second.name();
                ctype = it->second.ctype();
                depth = (int)pathTo(it->second).size();
            }
            const int spaces = std::min(depth * 2, 60);
            std::string indent(spaces, ' ');
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "%s- %s (%s) cs=(%.0fx%.0f)\n"
                "%s    world: (%.1f,%.1f)-(%.1f,%.1f)\n"
                "%s    imgui: (%.1f,%.1f)-(%.1f,%.1f)\n",
                indent.c_str(),
                name.empty() ? "<unnamed>" : name.c_str(),
                ctype.c_str(), cs.width, cs.height,
                indent.c_str(), wMinX, wMinY, wMaxX, wMaxY,
                indent.c_str(),
                std::min(s0.first,  s1.first),
                std::min(s0.second, s1.second),
                std::max(s0.first,  s1.first),
                std::max(s0.second, s1.second));
            out += buf;
        }
        return out;
    }

    if (cmd == "roots")
    {
        // Print the world-space bounding box of each top-level graph
        // root: the running Scene and the .csb-loaded root. Corners are
        // computed via convertToWorldSpace so any ancestor transform
        // (scene's own + loadedRoot's own) is applied.
        auto* scene = ax::Director::getInstance()->getRunningScene();
        if (!scene) return "ERR: no scene\n";

        auto corners = [](ax::Node* n, ax::Vec2 out[4]) {
            if (!n) { for (int i = 0; i < 4; ++i) out[i] = ax::Vec2::ZERO; return; }
            const auto cs = n->getContentSize();
            out[0] = n->convertToWorldSpace(ax::Vec2(0,         0));
            out[1] = n->convertToWorldSpace(ax::Vec2(cs.width,  0));
            out[2] = n->convertToWorldSpace(ax::Vec2(cs.width,  cs.height));
            out[3] = n->convertToWorldSpace(ax::Vec2(0,         cs.height));
        };
        ax::Vec2 scC[4], rtC[4];
        corners(scene, scC);
        corners(_axmolRoot, rtC);

        const auto scenePos = scene->getPosition();
        const auto sceneAP  = scene->getAnchorPoint();
        const auto sceneSize = scene->getContentSize();
        char buf[1500];
        if (!_axmolRoot)
        {
            std::snprintf(buf, sizeof(buf),
                "SCENE      pos=(%.1f,%.1f) scale=%.3f anchor=(%.2f,%.2f) "
                "size=(%.0fx%.0f) ignore=%d\n"
                "  world corners BL=(%.1f,%.1f) BR=(%.1f,%.1f) "
                "TR=(%.1f,%.1f) TL=(%.1f,%.1f)\n"
                "loadedRoot: null\n",
                scenePos.x, scenePos.y, scene->getScaleX(),
                sceneAP.x, sceneAP.y, sceneSize.width, sceneSize.height,
                scene->isIgnoreAnchorPointForPosition() ? 1 : 0,
                scC[0].x, scC[0].y, scC[1].x, scC[1].y,
                scC[2].x, scC[2].y, scC[3].x, scC[3].y);
            return buf;
        }
        const auto rootPos = _axmolRoot->getPosition();
        const auto rootAP  = _axmolRoot->getAnchorPoint();
        const auto rootSize = _axmolRoot->getContentSize();
        std::snprintf(buf, sizeof(buf),
            "SCENE       pos=(%.1f,%.1f) scale=%.3f anchor=(%.2f,%.2f) "
            "size=(%.0fx%.0f) ignore=%d\n"
            "  BL=(%.1f,%.1f) BR=(%.1f,%.1f) TR=(%.1f,%.1f) TL=(%.1f,%.1f)\n"
            "loadedRoot  pos=(%.1f,%.1f) scale=%.3f anchor=(%.2f,%.2f) "
            "size=(%.0fx%.0f) ignore=%d\n"
            "  BL=(%.1f,%.1f) BR=(%.1f,%.1f) TR=(%.1f,%.1f) TL=(%.1f,%.1f)\n",
            scenePos.x, scenePos.y, scene->getScaleX(),
            sceneAP.x, sceneAP.y, sceneSize.width, sceneSize.height,
            scene->isIgnoreAnchorPointForPosition() ? 1 : 0,
            scC[0].x, scC[0].y, scC[1].x, scC[1].y,
            scC[2].x, scC[2].y, scC[3].x, scC[3].y,
            rootPos.x, rootPos.y, _axmolRoot->getScaleX(),
            rootAP.x, rootAP.y, rootSize.width, rootSize.height,
            _axmolRoot->isIgnoreAnchorPointForPosition() ? 1 : 0,
            rtC[0].x, rtC[0].y, rtC[1].x, rtC[1].y,
            rtC[2].x, rtC[2].y, rtC[3].x, rtC[3].y);
        return buf;
    }

    if (cmd == "pan")
    {
        if (toks.size() < 3)
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.1f %.1f\n",
                _panOffsetX, _panOffsetY);
            return buf;
        }
        _panOffsetX = std::strtof(toks[1].c_str(), nullptr);
        _panOffsetY = std::strtof(toks[2].c_str(), nullptr);
        return "ok\n";
    }

    if (cmd == "zoom")
    {
        if (toks.size() < 2)
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.3f\n", _sceneZoom);
            return buf;
        }
        _sceneZoom = std::clamp(std::strtof(toks[1].c_str(), nullptr),
                                 0.05f, 20.0f);
        char buf[64]; std::snprintf(buf, sizeof(buf), "zoom=%.3f\n", _sceneZoom);
        return buf;
    }

    if (cmd == "rotate")
    {
        if (toks.size() < 2) return "ERR: rotate <deg>\n";
        if (!_selected.valid()) return "ERR: no selection\n";
        float deg = std::strtof(toks[1].c_str(), nullptr);
        pushUndo();
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.4f", deg);
        _selected.setAttr("Rotation", buf);
        syncNodeCsdToAx(_selected);
        if (_doc) _doc->dirty = true;
        return std::string("rotated to ") + buf + "\n";
    }

    if (cmd == "assets")
    {
        std::string out;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%zu assets scanned\n", _assets.size());
        out += buf;
        for (const auto& a : _assets)
        {
            out += "  " + a.displayName + "\t" + a.absPath.string() + "\n";
            if (out.size() > 4000) { out += "  …(truncated)\n"; break; }
        }
        return out;
    }

    if (cmd == "refs")
    {
        if (toks.size() < 2) return "ERR: refs <basename>\n";
        auto list = findAssetReferences(toks[1]);
        std::string out = std::to_string(list.size()) + " references\n";
        for (const auto& n : list)
            out += "  " + n.name() + "\t" + n.ctype() + "\n";
        return out;
    }

    if (cmd == "state")
    {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "mode=%s axmolRoot=%s dfs=%zu axToCsd=%zu "
            "editOverlayRuns=%u editBails=%u lastBail=%s totalClicks=%u events=%zu\n",
            _mode == Mode::Edit ? "edit" : "view",
            _axmolRoot ? "set" : "null",
            _axOrderDfs.size(), _axToCsd.size(),
            _editOverlayRuns, _editOverlayBails,
            _editLastBail, _totalClicks, _eventLog.size());
        return buf;
    }

    if (cmd == "mouse")
    {
        ImGuiIO& io = ImGui::GetIO();
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "pos=(%.0f,%.0f) down=%d totalClicks=%u events=%zu\n",
            io.MousePos.x, io.MousePos.y,
            ImGui::IsMouseDown(0) ? 1 : 0,
            _totalClicks, _eventLog.size());
        return buf;
    }

    if (cmd == "events")
    {
        if (_eventLog.empty()) return "(no clicks observed)\n";
        std::string out;
        for (const auto& e : _eventLog)
        {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "t=%.2f  (%.0f,%.0f)  inCentral=%d panel=%d anyItem=%d  kind=%s  hit=%s (%s)\n",
                e.time, e.x, e.y,
                e.inCentral ? 1 : 0,
                e.hoverIsPanel ? 1 : 0,
                e.anyItemHovered ? 1 : 0,
                e.kind,
                e.hitPath.c_str(),
                e.hitName.c_str());
            out += buf;
        }
        return out;
    }

    if (cmd == "display")
    {
        auto* view = ax::Director::getInstance()->getRenderView();
        ImGuiIO& io = ImGui::GetIO();
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "display=%.0fx%.0f framebuffer=%.0fx%.0f scale=%.2fx%.2f fbs=%.2f\n",
            io.DisplaySize.x, io.DisplaySize.y,
            view->getFrameSize().width, view->getFrameSize().height,
            view->getScaleX(), view->getScaleY(),
            io.DisplayFramebufferScale.x);
        return buf;
    }

    // ---- mutation commands -------------------------------------------------
    //
    // Each mutation: parse → resolve node → pushUndo → mutate XML →
    // syncNodeCsdToAx (so the live canvas reflects the change) → mark dirty.
    // Structural commands (delete / addchild) delegate to Editor methods
    // that handle the ax-tree rebuild via the host-installed refresh hook.

    auto resolveByPathArg = [&](const std::string& s) -> Node {
        if (!_doc) return Node();
        // Numeric (or "(root)"/empty) → index path. Otherwise: name
        // chain via `>` separator (head DFS, tail child-of), or a
        // plain single name (DFS first match). Lets every mutation
        // command accept the same address forms as `select`.
        if (looksNumericPath(s))
        {
            try { return nodeAtPath(parsePath(s)); }
            catch (const std::exception&) { return Node(); }
        }
        if (s.find('>') != std::string::npos)
        {
            auto segs = splitNamePath(s);
            if (segs.empty()) return Node();
            Node n = findNodeByName(_doc->rootNode, segs[0]);
            for (std::size_t i = 1; i < segs.size() && n.valid(); ++i)
                n = findChildByName(n, segs[i]);
            return n;
        }
        return findNodeByName(_doc->rootNode, s);
    };

    if (cmd == "reload")
    {
        // Re-read the current .csd (or `toks[1]` if specified) from disk.
        // Rejects dirty state unless `force` is passed.
        if (!_doc) return "ERR: no doc\n";

        std::filesystem::path target = _csdPath;
        bool force = false;
        for (std::size_t i = 1; i < toks.size(); ++i)
        {
            if (toks[i] == "force") force = true;
            else target = toks[i];
        }
        if (target.empty()) return "ERR: no path\n";
        if (_doc->dirty && !force)
            return "ERR: dirty, save or discard (use 'reload force')\n";

        try
        {
            auto fresh = loadCsd(target);
            _doc = std::make_unique<CsdDocument>(std::move(fresh));
        }
        catch (const std::exception& e)
        {
            return std::string("ERR: ") + e.what() + "\n";
        }
        _csdPath = target;
        _selected = _doc->rootNode;
        _selection.clear();
        _undoStack.clear();
        _redoStack.clear();
        _selectionChanged = true;
        removeAutosave();
        // Rebuild the ax tree from the fresh document so the canvas reflects
        // disk state. Falls back to a best-effort map-resync when no host
        // refresh hook is installed.
        refreshAxFromCsd();
        return std::string("reloaded ") + target.filename().string() + "\n";
    }

    if (cmd == "setattr")
    {
        if (toks.size() < 4)
            return "ERR: setattr <path> <attr|Sub.Key> <value>\n"
                   "     setattr <path> <SubTag> K=V K=V ...\n";
        Node n = resolveByPathArg(toks[1]);
        if (!n.valid()) return "ERR: no such path\n";
        const std::string& key = toks[2];

        // Detect typed-sub form: tag plus one-or-more K=V pairs as
        // separate tokens. Triggers when key has no dot AND every
        // remaining token contains '='.
        bool typedSub = key.find('.') == std::string::npos;
        if (typedSub)
        {
            for (std::size_t i = 3; i < toks.size(); ++i)
            {
                if (toks[i].find('=') == std::string::npos)
                { typedSub = false; break; }
            }
        }

        pushUndo();
        if (typedSub)
        {
            // Bulk-update a sub-element's attributes. Each K=V pair
            // upserts one attribute; pre-existing attrs on the same
            // sub-element are kept unless overwritten.
            for (std::size_t i = 3; i < toks.size(); ++i)
            {
                const auto& pair = toks[i];
                const auto eq = pair.find('=');
                const std::string k = pair.substr(0, eq);
                const std::string v = pair.substr(eq + 1);
                if (k.empty())
                {
                    _undoStack.pop_back();
                    return "ERR: bad K=V pair\n";
                }
                upsertSubAttr(n, key.c_str(), k, v);
            }
        }
        else
        {
            // Rejoin value tokens so strings with spaces (e.g.
            // LabelText) work in the scalar form.
            std::string value = toks[3];
            for (std::size_t i = 4; i < toks.size(); ++i)
            { value.push_back(' '); value += toks[i]; }

            const auto dot = key.find('.');
            if (dot == std::string::npos)
            {
                n.setAttr(key, value);
            }
            else
            {
                const std::string sub = key.substr(0, dot);
                const std::string subKey = key.substr(dot + 1);
                if (sub.empty() || subKey.empty())
                {
                    _undoStack.pop_back();
                    return "ERR: bad Sub.Key form\n";
                }
                upsertSubAttr(n, sub.c_str(), subKey, value);
            }
        }
        syncNodeCsdToAx(n);
        if (_doc) _doc->dirty = true;
        return "ok\n";
    }

    if (cmd == "getattr")
    {
        if (toks.size() < 2) return "ERR: getattr <path> [attr|Sub.Key]\n";
        Node n = resolveByPathArg(toks[1]);
        if (!n.valid()) return "ERR: no such path\n";
        if (toks.size() == 2)
        {
            // Whole-node dump: top-level attrs first, then every
            // sub-element with its attrs in `Sub.Key=Val` form.
            std::string out;
            for (auto a = n.element().first_attribute(); a;
                 a = a.next_attribute())
            {
                out += a.name();
                out.push_back('=');
                out += a.as_string("");
                out.push_back('\n');
            }
            for (auto c = n.element().first_child(); c;
                 c = c.next_sibling())
            {
                const std::string tag(c.name());
                if (tag == "Children") continue;
                for (auto a = c.first_attribute(); a; a = a.next_attribute())
                {
                    out += tag;
                    out.push_back('.');
                    out += a.name();
                    out.push_back('=');
                    out += a.as_string("");
                    out.push_back('\n');
                }
            }
            return out.empty() ? "(none)\n" : out;
        }
        const std::string& key = toks[2];
        const auto dot = key.find('.');
        if (dot == std::string::npos)
        {
            auto a = n.element().attribute(key.c_str());
            if (!a) return "(missing)\n";
            return std::string(a.as_string("")) + "\n";
        }
        const std::string sub    = key.substr(0, dot);
        const std::string subKey = key.substr(dot + 1);
        auto subs = n.subAttrs(sub.c_str());
        for (const auto& [k, v] : subs)
            if (k == subKey) return v + "\n";
        return "(missing)\n";
    }

    if (cmd == "setcolor")
    {
        if (toks.size() < 5)
            return "ERR: setcolor <path> R G B [A]\n";
        Node n = resolveByPathArg(toks[1]);
        if (!n.valid()) return "ERR: no such path\n";
        const int R = parseChannel(toks[2]);
        const int G = parseChannel(toks[3]);
        const int B = parseChannel(toks[4]);
        int A = -1;
        if (toks.size() >= 6) A = parseChannel(toks[5]);
        if (R < 0 || G < 0 || B < 0 || (toks.size() >= 6 && A < 0))
            return "ERR: channels must be 0-255\n";
        pushUndo();
        writeColorSub(n, "CColor", R, G, B, A);
        // CColor maps to ax::Node::setColor on every node; Alpha is
        // governed by the top-level Alpha attr instead, which we leave
        // alone unless caller passed an explicit A (they probably want
        // it on the colour node and don't realise it lives separately).
        if (A >= 0) n.setAttr("Alpha", std::to_string(A));
        syncNodeCsdToAx(n);
        if (_doc) _doc->dirty = true;
        return "ok\n";
    }

    if (cmd == "setbgcolor")
    {
        // Optional channel selector — first arg after path is "single",
        // "first", or "end" to target the corresponding sub-element. If
        // omitted we default to SingleColor (matches solid-fill panels).
        if (toks.size() < 5)
            return "ERR: setbgcolor <path> [single|first|end] R G B [A]\n";
        Node n = resolveByPathArg(toks[1]);
        if (!n.valid()) return "ERR: no such path\n";

        std::size_t off = 2;
        const char* channelTag = "SingleColor";
        if (toks.size() >= 6 &&
            (toks[2] == "single" || toks[2] == "first" || toks[2] == "end"))
        {
            if      (toks[2] == "single") channelTag = "SingleColor";
            else if (toks[2] == "first")  channelTag = "FirstColor";
            else                          channelTag = "EndColor";
            off = 3;
        }
        if (off + 3 > toks.size())
            return "ERR: setbgcolor <path> [channel] R G B [A]\n";
        const int R = parseChannel(toks[off + 0]);
        const int G = parseChannel(toks[off + 1]);
        const int B = parseChannel(toks[off + 2]);
        int A = -1;
        if (toks.size() > off + 3) A = parseChannel(toks[off + 3]);
        if (R < 0 || G < 0 || B < 0 ||
            (toks.size() > off + 3 && A < 0))
            return "ERR: channels must be 0-255\n";
        pushUndo();
        writeColorSub(n, channelTag, R, G, B, A);
        // BackColorAlpha is the layout's opacity knob — if alpha was
        // given we mirror it there too so the change is visible in
        // both solid and gradient panel modes.
        if (A >= 0) n.setAttr("BackColorAlpha", std::to_string(A));
        syncNodeCsdToAx(n);
        if (_doc) _doc->dirty = true;
        return std::string("ok ") + channelTag + "\n";
    }

    if (cmd == "setfile")
    {
        if (toks.size() < 2)
            return "ERR: setfile <path> <relPath> [plist]\n";
        Node n = resolveByPathArg(toks[1]);
        if (!n.valid()) return "ERR: no such path\n";
        // Blank relPath = clear the FileData reference (renders nothing).
        const std::string rel   = toks.size() >= 3 ? toks[2] : std::string();
        const std::string plist = toks.size() >= 4 ? toks[3] : std::string();
        // Pick Type based on the inputs:
        //   plist set     → PlistSubImage (atlas frame)
        //   rel empty     → Normal (no-op-render until set)
        //   else          → Normal (loose file)
        const char* type = plist.empty() ? "Normal" : "PlistSubImage";
        pushUndo();
        std::vector<std::pair<std::string, std::string>> attrs = {
            {"Type",  type},
            {"Path",  rel},
            {"Plist", plist},
        };
        n.setSubAttrs("FileData", attrs);
        syncNodeCsdToAx(n);
        if (_doc) _doc->dirty = true;
        return "ok\n";
    }

    if (cmd == "setvisible")
    {
        if (toks.size() < 3)
            return "ERR: setvisible <path> 0|1 [self|cascade]\n";
        Node n = resolveByPathArg(toks[1]);
        if (!n.valid()) return "ERR: no such path\n";
        const bool vis = !(toks[2] == "0" || toks[2] == "false" ||
                           toks[2] == "False" || toks[2] == "no");
        // Cascade by default when HIDING (matches what CSLoader needs
        // for PanelObjectData: the parent's Visible="False" doesn't
        // always propagate to the runtime, so we stamp the same flag
        // on every descendant). Showing defaults to self-only — a
        // user "show panel" shouldn't force its children to ignore
        // their own Visible="False" state.
        bool cascade = !vis;
        for (std::size_t i = 3; i < toks.size(); ++i)
        {
            if (toks[i] == "cascade") cascade = true;
            else if (toks[i] == "self") cascade = false;
        }
        pushUndo();
        n.setVisible(vis);
        syncNodeCsdToAx(n);
        if (cascade)
        {
            // 1. csd-tree cascade so the saved .csd survives reload.
            std::function<void(const Node&)> walk = [&](const Node& p) {
                for (auto& c : p.children())
                {
                    Node child = c;
                    child.setVisible(vis);
                    syncNodeCsdToAx(child);
                    walk(child);
                }
            };
            walk(n);
            // 2. ax-tree cascade — covers ProjectNodeObjectData
            // embeds whose loaded children live OUTSIDE our pugi
            // tree (they came from a separate .csd loaded by
            // CSLoader at runtime). axmol's setVisible(false) on
            // every descendant guarantees the entire subtree stops
            // drawing, regardless of whether csd children() saw it.
            auto it = _csdToAx.find(n.element().internal_object());
            if (it != _csdToAx.end() && it->second)
            {
                std::function<void(ax::Node*)> axWalk = [&](ax::Node* p) {
                    if (!p) return;
                    for (auto* c : p->getChildren())
                    {
                        c->setVisible(vis);
                        axWalk(c);
                    }
                };
                axWalk(it->second);
            }
        }
        if (_doc) _doc->dirty = true;
        return vis ? "ok visible\n" : "ok hidden\n";
    }

    if (cmd == "setpanelimage")
    {
        // Like setfile but tuned for PanelObjectData: writes the
        // FileData XML *and* invokes ax::ui::Layout::setBackGroundImage
        // on the mapped runtime node so the swap is visible without an
        // export/reload round-trip. Empty <relPath> clears the panel's
        // back image entirely.
        if (toks.size() < 2)
            return "ERR: setpanelimage <path> <relPath> [plist]\n";
        Node n = resolveByPathArg(toks[1]);
        if (!n.valid()) return "ERR: no such path\n";

        const std::string rel   = toks.size() >= 3 ? toks[2] : std::string();
        const std::string plist = toks.size() >= 4 ? toks[3] : std::string();
        const bool atlas = !plist.empty();
        const char* type = atlas ? "PlistSubImage" : "Normal";

        pushUndo();
        if (rel.empty())
        {
            // Clear: drop the FileData element entirely so CSLoader
            // doesn't try to load anything on next export.
            n.setSubAttrs("FileData", {});
        }
        else
        {
            std::vector<std::pair<std::string, std::string>> attrs = {
                {"Type",  type},
                {"Path",  rel},
                {"Plist", plist},
            };
            n.setSubAttrs("FileData", attrs);
        }
        // Apply directly to the runtime layout so the canvas updates
        // now. Falls back gracefully when the node isn't a Layout
        // (e.g. a misrouted setpanelimage on an ImageView).
        auto it = _csdToAx.find(n.element().internal_object());
        if (it != _csdToAx.end())
        {
            if (auto* layout = dynamic_cast<ax::ui::Layout*>(it->second))
            {
                using TR = ax::ui::Widget::TextureResType;
                layout->setBackGroundImage(rel,
                    atlas ? TR::PLIST : TR::LOCAL);
            }
        }
        syncNodeCsdToAx(n);
        if (_doc) _doc->dirty = true;
        return "ok\n";
    }

    if (cmd == "delete")
    {
        if (toks.size() < 2) return "ERR: delete <path>\n";
        Node n = resolveByPathArg(toks[1]);
        if (!n.valid()) return "ERR: no such path\n";
        if (!_doc || n.element() == _doc->rootNode.element())
            return "ERR: can't delete root\n";
        deleteNode(n);   // handles pushUndo + refreshAxFromCsd
        return _statusMsg + "\n";
    }

    if (cmd == "addchild")
    {
        if (toks.size() < 2)
            return "ERR: addchild <parentPath> [ctype] [name]\n";
        Node parent = resolveByPathArg(toks[1]);
        if (!parent.valid()) return "ERR: no such parent\n";
        const char* ctype =
            toks.size() >= 3 ? toks[2].c_str() : "NodeObjectData";
        Node created = addChildNode(parent, ctype);
        if (!created.valid()) return "ERR: addchild failed\n";
        if (toks.size() >= 4) created.setName(toks[3]);
        return pathToString(pathTo(created)) + "\n";
    }

    if (cmd == "list-textures")
    {
        // Aggregate every asset reference in the current doc, deduped
        // by (Path, Plist). For each unique reference report:
        //   <Path> [Plist=<plist>]  refs=<count>  via=<kind>
        // Caller can pair with `used-by <relPath>` to drill into the
        // nodes that use a given asset.
        if (!_doc) return "ERR: no doc\n";
        struct Key { std::string path, plist, kind; };
        struct Cmp { bool operator()(const Key& a, const Key& b) const {
            if (a.path != b.path)   return a.path < b.path;
            if (a.plist != b.plist) return a.plist < b.plist;
            return a.kind < b.kind;
        } };
        std::map<Key, int, Cmp> counts;
        auto nodes = _doc->allNodes();
        for (const auto& n : nodes)
            for (const auto& r : n.imageRefs())
                counts[Key{r.path, r.plist, r.kind}]++;
        if (counts.empty()) return "(no asset references)\n";
        std::string out;
        for (const auto& [k, c] : counts)
        {
            out += k.path;
            if (!k.plist.empty()) { out += "  Plist="; out += k.plist; }
            out += "  refs=" + std::to_string(c);
            out += "  via=" + k.kind;
            out.push_back('\n');
        }
        return out;
    }

    if (cmd == "used-by")
    {
        // Inverse of list-textures: every csd node whose imageRefs
        // contain the given Path. Matches the FULL Path string so
        // callers can distinguish "icon.png" inside an atlas from a
        // loose "icon.png" on disk.
        if (toks.size() < 2) return "ERR: used-by <relPath>\n";
        if (!_doc) return "ERR: no doc\n";
        const std::string& target = toks[1];
        std::string out;
        std::function<void(const Node&)> walk = [&](const Node& n) {
            for (const auto& r : n.imageRefs())
            {
                if (r.path == target)
                {
                    out += pathToString(pathTo(n));
                    out.push_back('\t');
                    out += n.name();
                    out.push_back('\t');
                    out += n.ctype();
                    out.push_back('\t');
                    out += r.kind;
                    out.push_back('\n');
                    break;
                }
            }
            for (auto& c : n.children()) walk(c);
        };
        walk(_doc->rootNode);
        return out.empty() ? "(no references)\n" : out;
    }

    if (cmd == "script")
    {
        // Execute a file of mutation commands line-by-line. Empty
        // lines and lines starting with '#' are skipped. Each line's
        // reply is preceded by `> <cmd>` so the caller can see which
        // line produced which output. Aborts on the first ERR line
        // (use `script-continue` if a non-aborting variant is needed
        // later — keeping the strict default avoids partially-applied
        // mutations going unnoticed).
        if (toks.size() < 2) return "ERR: script <file>\n";
        std::ifstream f(toks[1]);
        if (!f) return "ERR: cannot open " + toks[1] + "\n";
        std::string out;
        std::string scriptLine;
        std::size_t idx = 0;
        while (std::getline(f, scriptLine))
        {
            ++idx;
            // Strip leading whitespace for the comment / empty check.
            std::string trimmed = scriptLine;
            while (!trimmed.empty() &&
                   std::isspace((unsigned char)trimmed.front()))
                trimmed.erase(trimmed.begin());
            if (trimmed.empty() || trimmed.front() == '#') continue;
            out += "> ";
            out += scriptLine;
            out.push_back('\n');
            std::string r = handleInspectCommand(scriptLine);
            out += r;
            if (!r.empty() && r.back() != '\n') out.push_back('\n');
            if (r.compare(0, 4, "ERR:") == 0)
            {
                out += "aborted at line " + std::to_string(idx) + "\n";
                break;
            }
        }
        return out.empty() ? "(empty script)\n" : out;
    }

    if (cmd == "apply-to-scenes")
    {
        // Batch-mutation across .csd files. For every path that
        // matches `<glob>` (interpreted as a directory-or-glob via
        // `*` as a wildcard on the basename — keeps the command
        // independent of shell expansion across nc/echo pipelines):
        //   reload force → run <script> → save → export → next.
        // The current document is restored at the end so the editor
        // settles back on what the user had open.
        if (toks.size() < 3)
            return "ERR: apply-to-scenes <glob|dir> <script>\n";
        namespace fs = std::filesystem;
        const fs::path pattern(toks[1]);
        const std::string scriptPath = toks[2];

        // Collect candidate paths: explicit file, directory listing,
        // or basename glob with '*' as the wildcard.
        std::vector<fs::path> targets;
        std::error_code ec;
        if (fs::is_regular_file(pattern, ec))
        {
            targets.push_back(pattern);
        }
        else if (fs::is_directory(pattern, ec))
        {
            for (auto& e : fs::directory_iterator(pattern, ec))
                if (e.path().extension() == ".csd")
                    targets.push_back(e.path());
        }
        else
        {
            // Basename glob like "scenes/Scene*.csd".
            const fs::path parent = pattern.parent_path().empty()
                ? fs::path(".") : pattern.parent_path();
            const std::string mask = pattern.filename().string();
            const auto star = mask.find('*');
            const std::string prefix = star == std::string::npos
                ? mask : mask.substr(0, star);
            const std::string suffix = star == std::string::npos
                ? std::string() : mask.substr(star + 1);
            if (fs::is_directory(parent, ec))
                for (auto& e : fs::directory_iterator(parent, ec))
                {
                    const std::string n = e.path().filename().string();
                    if (n.size() < prefix.size() + suffix.size()) continue;
                    if (n.compare(0, prefix.size(), prefix) != 0) continue;
                    if (!suffix.empty() &&
                        n.compare(n.size() - suffix.size(),
                                  suffix.size(), suffix) != 0) continue;
                    targets.push_back(e.path());
                }
        }
        std::sort(targets.begin(), targets.end());
        if (targets.empty())
            return "ERR: no matches for " + toks[1] + "\n";

        const fs::path original = _csdPath;
        std::string out;
        std::size_t okCount = 0, errCount = 0;
        for (std::size_t idx = 0; idx < targets.size(); ++idx)
        {
            const auto& p = targets[idx];
            // Per-scene progress as a watch event so subscribers see
            // it stream; the final reply stays short.
            emitEvent("apply-to-scenes " +
                      std::to_string(idx + 1) + "/" +
                      std::to_string(targets.size()) + " " +
                      p.filename().string());
            std::string stepLog;
            stepLog += handleInspectCommand("reload " + p.string() + " force");
            stepLog += handleInspectCommand("script " + scriptPath);
            stepLog += handleInspectCommand("save");
            stepLog += handleInspectCommand("export");
            const bool failed = stepLog.find("ERR:") != std::string::npos;
            if (failed) ++errCount; else ++okCount;
            out += p.filename().string();
            out += failed ? "\tERR" : "\tok";
            out.push_back('\n');
        }
        if (!original.empty() && original != _csdPath)
            handleInspectCommand("reload " + original.string() + " force");
        out += "summary: " + std::to_string(okCount) + " ok / " +
               std::to_string(errCount) + " err / " +
               std::to_string(targets.size()) + " total\n";
        return out;
    }

    if (cmd == "setlabel")
    {
        // Shortcut for TextObjectData (or any node that uses the
        // LabelText attribute). Treats EVERY token after <path> as
        // the literal text, joined with single spaces — sidesteps
        // setattr's typed-sub detection so values containing '=' or
        // odd whitespace pass through verbatim.
        if (toks.size() < 3) return "ERR: setlabel <path> <text...>\n";
        Node n = resolveByPathArg(toks[1]);
        if (!n.valid()) return "ERR: no such path\n";
        std::string value = toks[2];
        for (std::size_t i = 3; i < toks.size(); ++i)
        { value.push_back(' '); value += toks[i]; }
        pushUndo();
        n.setAttr("LabelText", value);
        syncNodeCsdToAx(n);
        if (_doc) _doc->dirty = true;
        return "ok\n";
    }

    if (cmd == "gen-controller")
    {
        // gen-controller <outDir> [className]
        // Writes a starter <Class>.h + <Class>.cpp under <outDir>
        // containing a method-stub per unique <OCSBindings>/<Binding>
        // Method attribute in the current doc + a static wire(scene)
        // that calls ocs::ext::applyBindingsFromJson and dispatches
        // each event to the matching method. The skeleton compiles
        // as-is; designer fills the method bodies.
        if (toks.size() < 2) return "ERR: gen-controller <outDir> [className]\n";
        if (!_doc) return "ERR: no doc\n";
        namespace fs = std::filesystem;
        fs::path outDir = toks[1];
        if (!fs::exists(outDir))
        {
            std::error_code ec;
            fs::create_directories(outDir, ec);
            if (ec) return "ERR: cannot create " + outDir.string() + "\n";
        }
        std::string className = toks.size() >= 3
            ? toks[2] : (_csdPath.stem().string() + "Controller");

        // Collect unique method names + a map method → set<event>.
        std::map<std::string, std::set<std::string>> methodEvents;
        std::function<void(const Node&)> walk = [&](const Node& n) {
            auto root = n.element().child("OCSBindings");
            if (root)
                for (auto b = root.child("Binding"); b;
                     b = b.next_sibling("Binding"))
                {
                    const std::string m(b.attribute("Method").as_string(""));
                    const std::string e(b.attribute("Event").as_string(""));
                    if (!m.empty()) methodEvents[m].insert(e);
                }
            for (auto& c : n.children()) walk(c);
        };
        walk(_doc->rootNode);

        const fs::path hPath = outDir / (className + ".h");
        const fs::path cPath = outDir / (className + ".cpp");
        const std::string sceneStem = _csdPath.stem().string();

        std::ostringstream h;
        h << "// Auto-generated by OCS gen-controller. Edit method\n"
          << "// bodies; the framing (#includes, class shell, wire())\n"
          << "// is rewritten on every regen — keep customisations\n"
          << "// inside the method bodies only.\n\n"
          << "#pragma once\n\n"
          << "#include \"OCSExtension/OCSExtension.h\"\n"
          << "#include \"axmol.h\"\n\n"
          << "class " << className << " : public ax::Ref\n{\npublic:\n";
        for (const auto& [method, events] : methodEvents)
        {
            h << "    // Triggered by:";
            for (const auto& e : events) h << ' ' << e;
            h << '\n';
            h << "    void " << method << "(ax::Node* node);\n";
        }
        h << "\n    /// Wire every binding from the sidecar to a fresh\n"
          << "    /// instance of this controller. Returns the instance\n"
          << "    /// (retained on scene as a user-data pointer so it\n"
          << "    /// outlives the scope of the caller).\n"
          << "    static " << className << "* wire(\n"
          << "        ax::Node* sceneRoot,\n"
          << "        const std::string& sidecarPath);\n"
          << "};\n";

        // Build the wire() block separately so we can splice it
        // between sentinels on re-runs without touching anything
        // else in the .cpp the developer may have customised.
        std::ostringstream wireSs;
        wireSs << "// >>> OCS GEN BEGIN WIRE (do not edit this block)\n"
          << className << "* " << className << "::wire(\n"
          << "    ax::Node* sceneRoot,\n"
          << "    const std::string& sidecarPath)\n{\n"
          << "    auto* self = new " << className << "();\n"
          << "    self->autorelease();\n"
          << "    sceneRoot->setUserObject(self);\n"
          << "    ocs::ext::applyBindingsFromJson(\n"
          << "        sceneRoot, sidecarPath,\n"
          << "        [self](ax::Node* n, const ocs::ext::SceneBinding& b) {\n";
        for (const auto& [method, _events] : methodEvents)
            wireSs << "            if (b.method == \"" << method << "\") "
              << "{ self->" << method << "(n); return; }\n";
        wireSs << "            AXLOGW(\"[" << className
          << "] no handler for {}\", b.method);\n"
          << "        });\n"
          << "    return self;\n"
          << "}\n"
          << "// <<< OCS GEN END WIRE\n";
        const std::string wireBlock = wireSs.str();

        std::string cppOut;
        if (fs::exists(cPath))
        {
            // Re-run mode: preserve user-filled bodies. Replace only
            // the wire() block (between sentinels), append stubs for
            // any newly-added methods, leave the rest untouched.
            std::ifstream cin(cPath);
            std::stringstream ss; ss << cin.rdbuf();
            cppOut = ss.str();
            const std::string beg = "// >>> OCS GEN BEGIN WIRE";
            const std::string end = "// <<< OCS GEN END WIRE";
            const auto a = cppOut.find(beg);
            const auto b = cppOut.find(end);
            if (a != std::string::npos && b != std::string::npos && b > a)
                cppOut.replace(a, b - a + end.size(), wireBlock);
            else
            { cppOut += "\n"; cppOut += wireBlock; }
            // Append missing method stubs.
            for (const auto& [method, _events] : methodEvents)
            {
                const std::string sig =
                    "void " + className + "::" + method + "(";
                if (cppOut.find(sig) != std::string::npos) continue;
                cppOut += "\nvoid " + className + "::" + method +
                          "(ax::Node* /*node*/)\n{\n    // TODO: implement " +
                          method + "\n}\n";
            }
        }
        else
        {
            std::ostringstream c;
            c << "#include \"" << className << ".h\"\n\n"
              << "// NOTE: every method body below is a stub. Fill in the\n"
              << "// game logic that should fire when the matching event\n"
              << "// hits the bound node. Re-running gen-controller is\n"
              << "// safe — only the wire() block (between sentinels) is\n"
              << "// regenerated; existing method bodies are preserved.\n\n";
            for (const auto& [method, _events] : methodEvents)
                c << "void " << className << "::" << method
                  << "(ax::Node* /*node*/)\n{\n"
                  << "    // TODO: implement " << method << "\n"
                  << "}\n\n";
            c << wireBlock;
            cppOut = c.str();
        }

        std::ofstream hOut(hPath, std::ios::trunc);
        std::ofstream cOut(cPath, std::ios::trunc);
        if (!hOut || !cOut)
            return "ERR: cannot write to " + outDir.string() + "\n";
        hOut << h.str();
        cOut << cppOut;

        return "ok " + hPath.string() + "\nok " + cPath.string() +
               "\nmethods: " + std::to_string(methodEvents.size()) + "\n";
    }

    if (cmd == "litsprite-add-light")
    {
        // litsprite-add-light <path> K=V K=V ...
        // Adds <Light X Y Radius Color Falloff> under <OCSLights>.
        // Creates the OCSLights container with a default Ambient if
        // not present. Returns the new index.
        if (toks.size() < 2)
            return "ERR: litsprite-add-light <path> K=V ...\n";
        Node n = resolveByPathArg(toks[1]);
        if (!n.valid()) return "ERR: no such path\n";
        if (n.ctype() != "LitSpriteObjectData")
            return "ERR: node ctype is not LitSpriteObjectData\n";
        pushUndo();
        auto root = n.element().child("OCSLights");
        if (!root)
        {
            root = n.element().append_child("OCSLights");
            root.append_attribute("Ambient").set_value("60,60,80,255");
        }
        auto L = root.append_child("Light");
        // Sane defaults so a malformed call still produces a visible light.
        L.append_attribute("X").set_value("0");
        L.append_attribute("Y").set_value("0");
        L.append_attribute("Radius").set_value("200");
        L.append_attribute("Color").set_value("255,210,160,255");
        L.append_attribute("Falloff").set_value("1.5");
        for (std::size_t i = 2; i < toks.size(); ++i)
        {
            const auto eq = toks[i].find('=');
            if (eq == std::string::npos) continue;
            const std::string k = toks[i].substr(0, eq);
            const std::string v = toks[i].substr(eq + 1);
            auto a = L.attribute(k.c_str());
            if (!a) a = L.append_attribute(k.c_str());
            a.set_value(v.c_str());
        }
        if (_doc) _doc->dirty = true;
        syncNodeCsdToAx(n);
        int idx = 0;
        for (auto p = root.child("Light"); p; p = p.next_sibling("Light"), ++idx) {}
        return "ok " + std::to_string(idx - 1) + "\n";
    }

    if (cmd == "vmap-add-layer")
    {
        // vmap-add-layer <path> <layerName>
        // Appends an empty <Layer Name Visible="True"/> under
        // <VectorMapData>. Creates VectorMapData if missing.
        if (toks.size() < 3)
            return "ERR: vmap-add-layer <path> <layerName>\n";
        Node n = resolveByPathArg(toks[1]);
        if (!n.valid()) return "ERR: no such path\n";
        if (n.ctype() != "VectorMapObjectData")
            return "ERR: node ctype is not VectorMapObjectData\n";
        pushUndo();
        auto el = n.element();
        auto vmd = el.child("VectorMapData");
        if (!vmd) vmd = el.append_child("VectorMapData");
        auto layer = vmd.append_child("Layer");
        layer.append_attribute("Name").set_value(toks[2].c_str());
        layer.append_attribute("Visible").set_value("True");
        if (_doc) _doc->dirty = true;
        syncNodeCsdToAx(n);
        return "ok\n";
    }

    if (cmd == "vmap-add-keyframe")
    {
        // vmap-add-keyframe <path> <layer> <primIdx> <attr> <frame> <value...>
        // Inserts a row under <Layer>/<Animation>/<Keyframe>. Creates
        // the Animation element with default Duration=60 FPS=30 Loop
        // if absent. Multi-token <value> rejoined.
        if (toks.size() < 7)
            return "ERR: vmap-add-keyframe <path> <layer> <primIdx> "
                   "<attr> <frame> <value...>\n";
        Node n = resolveByPathArg(toks[1]);
        if (!n.valid()) return "ERR: no such path\n";
        if (n.ctype() != "VectorMapObjectData")
            return "ERR: node ctype is not VectorMapObjectData\n";
        auto vmd = n.element().child("VectorMapData");
        if (!vmd) return "ERR: VectorMapData missing\n";

        pugi::xml_node target;
        bool isInt = !toks[2].empty();
        for (char c : toks[2])
            if (!std::isdigit((unsigned char)c)) { isInt = false; break; }
        if (isInt)
        {
            int want = std::atoi(toks[2].c_str()); int idx = 0;
            for (auto L = vmd.child("Layer"); L; L = L.next_sibling("Layer"), ++idx)
                if (idx == want) { target = L; break; }
        }
        else
            for (auto L = vmd.child("Layer"); L; L = L.next_sibling("Layer"))
                if (std::string(L.attribute("Name").as_string("")) == toks[2])
                { target = L; break; }
        if (!target) return "ERR: layer not found\n";

        pushUndo();
        auto anim = target.child("Animation");
        if (!anim)
        {
            anim = target.append_child("Animation");
            anim.append_attribute("Duration").set_value(60);
            anim.append_attribute("FPS").set_value(30);
            anim.append_attribute("Loop").set_value("True");
        }
        std::string value = toks[6];
        for (std::size_t i = 7; i < toks.size(); ++i)
        { value.push_back(' '); value += toks[i]; }
        auto kf = anim.append_child("Keyframe");
        kf.append_attribute("Frame").set_value(toks[5].c_str());
        kf.append_attribute("PrimIdx").set_value(toks[3].c_str());
        kf.append_attribute("Attr").set_value(toks[4].c_str());
        kf.append_attribute("Value").set_value(value.c_str());
        if (_doc) _doc->dirty = true;
        syncNodeCsdToAx(n);
        return "ok\n";
    }

    if (cmd == "vmap-set-anim")
    {
        // vmap-set-anim <path> <layer> Duration=N FPS=N Loop=True|False
        if (toks.size() < 4)
            return "ERR: vmap-set-anim <path> <layer> K=V ...\n";
        Node n = resolveByPathArg(toks[1]);
        if (!n.valid()) return "ERR: no such path\n";
        auto vmd = n.element().child("VectorMapData");
        if (!vmd) return "ERR: VectorMapData missing\n";
        pugi::xml_node target;
        for (auto L = vmd.child("Layer"); L; L = L.next_sibling("Layer"))
            if (std::string(L.attribute("Name").as_string("")) == toks[2])
            { target = L; break; }
        if (!target) return "ERR: layer not found\n";
        pushUndo();
        auto anim = target.child("Animation");
        if (!anim) anim = target.append_child("Animation");
        for (std::size_t i = 3; i < toks.size(); ++i)
        {
            const auto eq = toks[i].find('=');
            if (eq == std::string::npos) continue;
            const std::string k = toks[i].substr(0, eq);
            const std::string v = toks[i].substr(eq + 1);
            auto a = anim.attribute(k.c_str());
            if (!a) a = anim.append_attribute(k.c_str());
            a.set_value(v.c_str());
        }
        if (_doc) _doc->dirty = true;
        syncNodeCsdToAx(n);
        return "ok\n";
    }

    if (cmd == "vmap-add-prim")
    {
        // vmap-add-prim <path> <layerIdx|layerName> <kind> K=V K=V ...
        // <kind> = Polygon | Polyline | Circle.
        // For Polygon / Polyline: pass `Points="x,y x,y …"` as a K=V.
        // For Circle: `X=… Y=… Radius=…`.
        // Common: `Fill=r,g,b,a` `Stroke=r,g,b,a` `StrokeWidth=N`.
        if (toks.size() < 4)
            return "ERR: vmap-add-prim <path> <layer> <kind> K=V ...\n";
        Node n = resolveByPathArg(toks[1]);
        if (!n.valid()) return "ERR: no such path\n";
        if (n.ctype() != "VectorMapObjectData")
            return "ERR: node ctype is not VectorMapObjectData\n";
        auto vmd = n.element().child("VectorMapData");
        if (!vmd) return "ERR: VectorMapData missing (use vmap-add-layer first)\n";

        // Locate target layer by integer index or by Name.
        pugi::xml_node target;
        bool isInt = !toks[2].empty();
        for (char c : toks[2])
            if (!std::isdigit((unsigned char)c)) { isInt = false; break; }
        if (isInt)
        {
            int want = std::atoi(toks[2].c_str());
            int idx = 0;
            for (auto L = vmd.child("Layer"); L; L = L.next_sibling("Layer"), ++idx)
                if (idx == want) { target = L; break; }
        }
        else
        {
            for (auto L = vmd.child("Layer"); L; L = L.next_sibling("Layer"))
                if (std::string(L.attribute("Name").as_string("")) == toks[2])
                { target = L; break; }
        }
        if (!target) return "ERR: layer not found\n";

        const std::string kind = toks[3];
        if (kind != "Polygon" && kind != "Polyline" && kind != "Circle")
            return "ERR: kind must be Polygon|Polyline|Circle\n";
        pushUndo();
        auto prim = target.append_child(kind.c_str());
        for (std::size_t i = 4; i < toks.size(); ++i)
        {
            const auto eq = toks[i].find('=');
            if (eq == std::string::npos) continue;
            const std::string k = toks[i].substr(0, eq);
            const std::string v = toks[i].substr(eq + 1);
            auto a = prim.attribute(k.c_str());
            if (!a) a = prim.append_attribute(k.c_str());
            a.set_value(v.c_str());
        }
        if (_doc) _doc->dirty = true;
        syncNodeCsdToAx(n);
        return "ok\n";
    }

    if (cmd == "addbinding")
    {
        // addbinding <path> <event> <method> [params...]
        // Stores under <OCSBindings>/<Binding> on the node. Bindings
        // ride out of band in <basename>.bindings.json at export.
        if (toks.size() < 4)
            return "ERR: addbinding <path> <event> <method> [params...]\n";
        Node n = resolveByPathArg(toks[1]);
        if (!n.valid()) return "ERR: no such path\n";
        std::string params;
        for (std::size_t i = 4; i < toks.size(); ++i)
        { if (!params.empty()) params.push_back(' '); params += toks[i]; }
        pushUndo();
        auto el = n.element();
        auto root = el.child("OCSBindings");
        if (!root) root = el.append_child("OCSBindings");
        auto b = root.append_child("Binding");
        b.append_attribute("Event").set_value(toks[2].c_str());
        b.append_attribute("Method").set_value(toks[3].c_str());
        if (!params.empty()) b.append_attribute("Params").set_value(params.c_str());
        if (_doc) _doc->dirty = true;
        return "ok\n";
    }

    if (cmd == "delbinding")
    {
        // delbinding <path> <event> [method]  — drops first match.
        if (toks.size() < 3)
            return "ERR: delbinding <path> <event> [method]\n";
        Node n = resolveByPathArg(toks[1]);
        if (!n.valid()) return "ERR: no such path\n";
        const std::string& ev = toks[2];
        const std::string method = toks.size() >= 4 ? toks[3] : std::string();
        auto root = n.element().child("OCSBindings");
        if (!root) return "(no bindings)\n";
        for (auto b = root.child("Binding"); b; b = b.next_sibling("Binding"))
        {
            if (std::string(b.attribute("Event").as_string("")) != ev) continue;
            if (!method.empty() &&
                std::string(b.attribute("Method").as_string("")) != method)
                continue;
            pushUndo();
            root.remove_child(b);
            if (_doc) _doc->dirty = true;
            return "ok\n";
        }
        return "(no match)\n";
    }

    if (cmd == "listbindings")
    {
        // listbindings [path]  — when path omitted, lists every node
        // with bindings across the doc; otherwise lists for one node.
        if (!_doc) return "ERR: no doc\n";
        std::string out;
        auto dump = [&](const Node& n) {
            auto root = n.element().child("OCSBindings");
            if (!root) return;
            for (auto b = root.child("Binding"); b;
                 b = b.next_sibling("Binding"))
            {
                out += pathToString(pathTo(n));
                out.push_back('\t');
                out += n.name();
                out.push_back('\t');
                out += b.attribute("Event").as_string("");
                out.push_back('\t');
                out += b.attribute("Method").as_string("");
                const std::string params(
                    b.attribute("Params").as_string(""));
                if (!params.empty())
                { out.push_back('\t'); out += params; }
                out.push_back('\n');
            }
        };
        if (toks.size() >= 2)
        {
            Node n = resolveByPathArg(toks[1]);
            if (!n.valid()) return "ERR: no such path\n";
            dump(n);
        }
        else
        {
            std::function<void(const Node&)> walk = [&](const Node& n) {
                dump(n);
                for (auto& c : n.children()) walk(c);
            };
            walk(_doc->rootNode);
        }
        return out.empty() ? "(no bindings)\n" : out;
    }

    if (cmd == "download")
    {
        // download <url> <dst>  — saves under _assetsRoot when dst is
        // relative. Useful for the Sprite Catalog import flow + any
        // scripted asset pull.
        if (toks.size() < 3) return "ERR: download <url> <dst>\n";
        namespace fs = std::filesystem;
        fs::path dst = toks[2];
        if (dst.is_relative() && !_assetsRoot.empty()) dst = _assetsRoot / dst;
        auto err = httpDownload(toks[1], dst.string());
        if (!err.empty()) return "ERR: " + err + "\n";
        return std::string("ok ") + dst.string() + "\n";
    }

    if (cmd == "svg-rasterize")
    {
        // svg-rasterize <src> <dst> [W H]  — bake one SVG to PNG.
        // Paths can be absolute or relative; relatives resolve under
        // the editor's _assetsRoot so the agent doesn't have to know
        // the on-disk layout.
        if (toks.size() < 3)
            return "ERR: svg-rasterize <src> <dst> [W H]\n";
        namespace fs = std::filesystem;
        fs::path src(toks[1]);
        fs::path dst(toks[2]);
        if (src.is_relative() && !_assetsRoot.empty()) src = _assetsRoot / src;
        if (dst.is_relative() && !_assetsRoot.empty()) dst = _assetsRoot / dst;
        int W = 0, H = 0;
        if (toks.size() >= 5)
        {
            W = std::atoi(toks[3].c_str());
            H = std::atoi(toks[4].c_str());
        }
        auto err = rasterizeSvg(src, dst, W, H);
        if (!err.empty()) return "ERR: " + err + "\n";
        return std::string("ok ") + dst.string() + "\n";
    }

    if (cmd == "svg-bake")
    {
        // svg-bake [rewrite] [w=N] [h=N]
        // Walks every FileData / asset-ref Path in the current doc,
        // rasterizes each .svg to a sibling .png. `rewrite` flips the
        // FileData Path to point at the .png so the next export uses
        // the raster. `w=N h=N` overrides the per-asset size; default
        // lets the converter use the SVG viewBox.
        if (!_doc) return "ERR: no doc\n";
        namespace fs = std::filesystem;
        bool rewrite = false;
        int W = 0, H = 0;
        for (std::size_t i = 1; i < toks.size(); ++i)
        {
            if (toks[i] == "rewrite") rewrite = true;
            else if (toks[i].rfind("w=", 0) == 0)
                W = std::atoi(toks[i].c_str() + 2);
            else if (toks[i].rfind("h=", 0) == 0)
                H = std::atoi(toks[i].c_str() + 2);
        }

        // Pre-resolve converter so we can fail fast if none is present.
        if (findSvgConverter().empty())
            return "ERR: no SVG converter found "
                   "(install rsvg-convert via `brew install librsvg`)\n";

        std::string out;
        int baked = 0, errors = 0, skipped = 0;
        // Walk the doc and look at every immediate child element of
        // every node whose Path attr ends in `.svg`. Covers FileData,
        // BackGroundImageFileData, NormalFileData, etc — same set
        // imageRefs() handles, but we mutate the live pugi attribute
        // in place when `rewrite` is on.
        std::function<void(const Node&)> walk = [&](const Node& n)
        {
            for (auto c = n.element().first_child(); c; c = c.next_sibling())
            {
                auto pathAttr = c.attribute("Path");
                if (!pathAttr) continue;
                std::string rel(pathAttr.as_string(""));
                if (rel.size() < 4) continue;
                std::string ext = rel.substr(rel.size() - 4);
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char ch){ return std::tolower(ch); });
                if (ext != ".svg") continue;

                fs::path src = _assetsRoot.empty()
                    ? fs::path(rel) : _assetsRoot / rel;
                fs::path dst = src; dst.replace_extension(".png");
                std::error_code ec;
                if (!fs::exists(src, ec))
                {
                    out += "miss\t" + rel + "\n";
                    ++skipped; continue;
                }
                // mtime cache: skip when PNG newer than SVG.
                if (fs::exists(dst, ec) &&
                    fs::last_write_time(dst, ec) >=
                    fs::last_write_time(src, ec))
                {
                    out += "cache\t" + rel + "\n";
                    ++skipped;
                }
                else
                {
                    auto err = rasterizeSvg(src, dst, W, H);
                    if (!err.empty())
                    {
                        out += "err\t" + rel + "\t" + err + "\n";
                        ++errors; continue;
                    }
                    out += "ok\t" + rel + "\n";
                    ++baked;
                }

                if (rewrite)
                {
                    std::string newRel = dst.lexically_relative(_assetsRoot)
                                            .generic_string();
                    if (newRel.empty()) newRel = dst.string();
                    pathAttr.set_value(newRel.c_str());
                    if (_doc) _doc->dirty = true;
                }
            }
            for (auto& k : n.children()) walk(k);
        };
        walk(_doc->rootNode);
        out += "summary: " + std::to_string(baked) + " baked, " +
               std::to_string(skipped) + " skipped, " +
               std::to_string(errors) + " errors\n";
        return out;
    }

    if (cmd == "describe")
    {
        // Per-ctype attribute / sub-element schema. Lets an agent
        // discover the legal surface of a node type without having to
        // grep the axmol source or trial-and-error every attribute.
        // The schema is intentionally a hand-curated subset of the
        // most-used Cocos Studio types — exhaustive coverage would
        // require generating from axmol's flatbuffers spec.
        struct Entry { const char* attrs; const char* subs; };
        static const std::unordered_map<std::string, Entry> kSchema = {
            {"NodeObjectData", {
                "Name(string) Tag(int) ActionTag(int) ZOrder(int) "
                "Rotation(float) RotationSkewX(float) RotationSkewY(float) "
                "Visible(True|False) Alpha(0-255) IconVisible(True|False)",
                "Size{X,Y} Position{X,Y} Scale{ScaleX,ScaleY} "
                "AnchorPoint{ScaleX,ScaleY} CColor{R,G,B} BlendFunc{Src,Dst}"
            }},
            {"PanelObjectData", {
                "Name Tag ActionTag ZOrder Rotation Visible Alpha "
                "ClipAble(True|False) BackColorAlpha(0-255) "
                "ComboBoxIndex(0=none|1=solid|2=gradient) "
                "ColorAngle(float) BackgroudImageScale9Enable(True|False)",
                "Size Position Scale AnchorPoint CColor "
                "SingleColor{R,G,B,A} FirstColor{R,G,B,A} EndColor{R,G,B,A} "
                "ColorVector{ScaleX,ScaleY} FileData{Type,Path,Plist} "
                "PreSize{X,Y} PrePosition{X,Y}"
            }},
            {"ImageViewObjectData", {
                "Name Tag ActionTag ZOrder Rotation Visible Alpha "
                "Scale9Enable(True|False) "
                "Scale9OriginX(int) Scale9OriginY(int) "
                "Scale9Width(int) Scale9Height(int)",
                "Size Position Scale AnchorPoint CColor "
                "FileData{Type=Normal|PlistSubImage|MarkedSubImage,Path,Plist}"
            }},
            {"TextObjectData", {
                "Name Tag ActionTag ZOrder Rotation Visible Alpha "
                "LabelText(string) FontSize(int) "
                "HorizontalAlignmentType(HT_Left|HT_Center|HT_Right) "
                "VerticalAlignmentType(VT_Top|VT_Center|VT_Bottom) "
                "AreaWidth(int) AreaHeight(int) IsCustomSize(True|False) "
                "TouchScaleChangeAble(True|False) OutlineEnabled(True|False) "
                "ShadowEnabled(True|False)",
                "Size Position Scale AnchorPoint CColor "
                "FontResource{Type=Normal,Path,Plist} "
                "OutlineColor{R,G,B,A} ShadowColor{R,G,B,A}"
            }},
            {"TextBMFontObjectData", {
                "Name Tag ActionTag ZOrder Rotation Visible Alpha "
                "LabelText(string)",
                "Size Position Scale AnchorPoint CColor "
                "LabelBMFontFile_CNB{Type=Normal,Path,Plist}"
            }},
            {"ButtonObjectData", {
                "Name Tag ActionTag ZOrder Rotation Visible Alpha "
                "ButtonText(string) FontSize(int) DisplayState(True|False) "
                "Scale9Enable(True|False)",
                "Size Position Scale AnchorPoint CColor "
                "NormalFileData{Type,Path,Plist} "
                "PressedFileData{Type,Path,Plist} "
                "DisabledFileData{Type,Path,Plist} "
                "TextColor{R,G,B}"
            }},
            {"GameLayerObjectData", {
                "Name Tag ZOrder Visible Alpha "
                "CanEdit(True|False) FrameEvent(string)",
                "Size Position Scale AnchorPoint CColor"
            }},
        };
        if (toks.size() < 2)
        {
            std::string out = "known ctypes:\n";
            for (const auto& [k, _] : kSchema)
                out += "  " + k + "\n";
            out += "use `describe <ctype>` for attrs + sub-elements\n";
            return out;
        }
        auto it = kSchema.find(toks[1]);
        if (it == kSchema.end()) return "ERR: unknown ctype\n";
        std::string out;
        out += "ctype: ";   out += toks[1]; out.push_back('\n');
        out += "attrs:\n  "; out += it->second.attrs; out.push_back('\n');
        out += "subs:\n  ";  out += it->second.subs;  out.push_back('\n');
        return out;
    }

    if (cmd == "validate")
    {
        // Walk the doc, flag every asset reference whose Path can't be
        // resolved under the editor's _assetsRoot. Catches "the .csb
        // will silently render nothing" bugs before the user has to
        // launch the game. Output one finding per line:
        //   <path>\t<name>\t<kind>\t<problem>\t<ref>
        if (!_doc) return "ERR: no doc\n";
        if (_assetsRoot.empty()) return "ERR: no assets root scanned yet\n";
        namespace fs = std::filesystem;
        // Search roots: _assetsRoot is the primary, plus every
        // sibling of _csdPath (typically Resources-src or another
        // out-of-tree authoring dir for .csd embeds). This avoids
        // false positives for ProjectNodeObjectData references that
        // live under a different root than the runtime Resources/.
        std::vector<fs::path> roots;
        roots.push_back(_assetsRoot);
        std::error_code ec;
        fs::path here = _csdPath.parent_path();
        for (int i = 0; i < 4 && !here.empty() && here != here.root_path(); ++i)
        {
            for (const char* sib : {"Resources-src", "Resources", "assets"})
            {
                fs::path cand = here.parent_path() / sib;
                if (fs::is_directory(cand, ec)) roots.push_back(cand);
            }
            here = here.parent_path();
        }
        // Dedupe roots, drop empties.
        {
            std::vector<fs::path> uniq;
            for (auto& r : roots)
            {
                if (r.empty()) continue;
                if (std::find(uniq.begin(), uniq.end(), r) == uniq.end())
                    uniq.push_back(r);
            }
            roots.swap(uniq);
        }
        auto refResolves = [&](const std::string& rel) -> bool {
            for (const auto& root : roots)
            {
                std::error_code e;
                if (fs::exists(root / rel, e)) return true;
            }
            return false;
        };

        std::string out;
        std::function<void(const Node&)> walk = [&](const Node& n)
        {
            for (const auto& r : n.imageRefs())
            {
                std::string problem;
                if (r.path.empty() && !r.plist.empty())
                    problem = "empty Path (atlas frame name missing)";
                else if (!r.path.empty() && r.plist.empty())
                {
                    if (!refResolves(r.path)) problem = "missing file";
                }
                else if (!r.plist.empty())
                {
                    if (!refResolves(r.plist))
                        problem = "missing plist";
                }
                if (!problem.empty())
                {
                    out += pathToString(pathTo(n));
                    out.push_back('\t');
                    out += n.name();
                    out.push_back('\t');
                    out += r.kind;
                    out.push_back('\t');
                    out += problem;
                    out.push_back('\t');
                    out += r.path;
                    if (!r.plist.empty())
                    { out += " [Plist="; out += r.plist; out.push_back(']'); }
                    out.push_back('\n');
                }
            }
            for (auto& c : n.children()) walk(c);
        };
        walk(_doc->rootNode);
        return out.empty() ? "ok (no broken refs)\n" : out;
    }

    if (cmd == "preview")
    {
        // Textual dry-run: snapshot the doc, run the wrapped command,
        // emit a unified-ish diff of the XML before/after, then
        // restore. Pure read-only from the caller's point of view —
        // _doc->dirty is reset and the canvas isn't touched (we run
        // the wrapped cmd against the snapshot and never sync). The
        // visual variant (apply, screenshot, rollback) would need a
        // frame round-trip + capture; not implemented.
        if (toks.size() < 2) return "ERR: preview <cmd...>\n";
        if (!_doc) return "ERR: no doc\n";

        // Snapshot current state.
        std::ostringstream before;
        _doc->doc.save(before, "",
                       pugi::format_raw | pugi::format_no_declaration);
        const std::string beforeXml = before.str();
        const bool wasDirty = _doc->dirty;
        const std::size_t undoBefore = _undoStack.size();
        const std::size_t redoBefore = _redoStack.size();

        // Reassemble the wrapped command line.
        std::string inner = toks[1];
        for (std::size_t i = 2; i < toks.size(); ++i)
        { inner.push_back(' '); inner += toks[i]; }

        // Reject nested preview to avoid recursive snapshot bloat.
        if (inner.rfind("preview ", 0) == 0)
            return "ERR: nested preview not allowed\n";

        // Run the wrapped command. It will mutate _doc, push undo,
        // potentially sync canvas. We undo all of that below.
        const std::string wrappedReply = handleInspectCommand(inner);

        // Capture mutated state.
        std::ostringstream after;
        _doc->doc.save(after, "",
                       pugi::format_raw | pugi::format_no_declaration);
        const std::string afterXml = after.str();

        // Restore. reloadFromString re-parses, ditches dangling pugi
        // handles — caller-side selection paths stay valid because
        // pathTo / nodeAtPath address by index.
        try { _doc->reloadFromString(beforeXml); }
        catch (const std::exception& e)
        {
            return std::string("ERR: rollback failed: ") + e.what() + "\n";
        }
        _doc->dirty = wasDirty;
        // Trim undo/redo back to pre-preview length so the wrapped
        // command's pushUndo doesn't accumulate.
        while (_undoStack.size() > undoBefore) _undoStack.pop_back();
        while (_redoStack.size() > redoBefore) _redoStack.pop_back();
        _selected = _doc->rootNode;
        _selectionChanged = true;
        refreshAxFromCsd();

        // Diff: emit only lines that differ. Avoids reproducing huge
        // unchanged XML. Walks both as line streams.
        auto splitLines = [](const std::string& s) {
            std::vector<std::string> lines;
            std::string cur;
            for (char c : s)
            {
                if (c == '\n') { lines.push_back(std::move(cur)); cur.clear(); }
                else cur.push_back(c);
            }
            if (!cur.empty()) lines.push_back(std::move(cur));
            return lines;
        };
        const auto A = splitLines(beforeXml);
        const auto B = splitLines(afterXml);

        std::string diff;
        diff += "wrapped reply:\n  " + wrappedReply;
        if (!wrappedReply.empty() && wrappedReply.back() != '\n')
            diff.push_back('\n');
        diff += "diff (before -> after):\n";
        // Cheap O(N) skip-matching diff: walk both, emit non-matching
        // lines as -/+. Good enough for the small per-line XML deltas
        // a single mutation produces; not a real LCS.
        std::size_t ia = 0, ib = 0;
        std::size_t emitted = 0;
        while (ia < A.size() || ib < B.size())
        {
            if (ia < A.size() && ib < B.size() && A[ia] == B[ib])
            { ++ia; ++ib; continue; }
            // Find next match window: emit one side until they realign.
            if (ia < A.size())
            { diff += "- "; diff += A[ia]; diff.push_back('\n'); ++ia; ++emitted; }
            if (ib < B.size())
            { diff += "+ "; diff += B[ib]; diff.push_back('\n'); ++ib; ++emitted; }
            if (emitted > 200)
            { diff += "...(truncated)\n"; break; }
        }
        if (emitted == 0) diff += "(no change)\n";
        return diff;
    }

    if (cmd == "preview-visual")
    {
        // Visual dry-run: snapshot doc, apply wrapped mutation,
        // schedule captureScreen, defer the RPC reply. The state
        // machine in `advanceVisualPreviews` rolls back as soon as
        // the capture callback fires (frame N+1) and fulfils the
        // PendingReply with the absolute PNG path.
        //
        // Tradeoff vs textual `preview`: the mutated state IS visible
        // on screen for ~2 frames before rollback. Use this only when
        // the agent actually needs a pixel render of the proposed
        // change; for XML-only diffing, prefer `preview`.
        if (toks.size() < 3)
            return "ERR: preview-visual <out.png> <cmd...>\n";
        if (!_doc) return "ERR: no doc\n";
        if (!reply) return "ERR: deferred reply not available here\n";

        std::filesystem::path out = toks[1];
        if (out.is_relative()) out = std::filesystem::absolute(out);
        std::string inner = toks[2];
        for (std::size_t i = 3; i < toks.size(); ++i)
        { inner.push_back(' '); inner += toks[i]; }
        if (inner.rfind("preview", 0) == 0)
            return "ERR: nested preview not allowed\n";

        // Snapshot before any mutation so rollback is a clean reload.
        std::ostringstream ss;
        _doc->doc.save(ss, "",
                       pugi::format_raw | pugi::format_no_declaration);

        auto job          = std::make_shared<VisualPreviewJob>();
        job->reply        = reply->defer();
        job->outPath      = out.string();
        job->snapXml      = ss.str();
        job->undoSize     = _undoStack.size();
        job->redoSize     = _redoStack.size();
        job->dirtyBefore  = _doc->dirty;
        _visualPreviews.push_back(job);

        // Apply mutation against live doc + canvas. Recursive call so
        // every existing mutation cmd is reachable; pass nullptr so
        // the inner cmd cannot re-defer.
        handleInspectCommand(inner, nullptr);

        // Schedule the capture. axmol invokes the callback on the
        // cocos main thread at end of next render; setting `captured`
        // is therefore safe without atomics — `advanceVisualPreviews`
        // also runs on the cocos main thread.
        ax::utils::captureScreen(
            [job](bool ok, std::string_view) {
                (void)ok;
                job->captured = true;
            },
            out.string());

        return "";  // already deferred; advanceVisualPreviews fulfils
    }

    if (cmd == "screenshot-canvas")
    {
        // Canvas-only PNG capture via off-screen RenderTexture. No
        // ImGui chrome ends up in the output — useful for visual
        // diffing against the live game capture. Output res defaults
        // to the loaded root's contentSize; explicit W H override.
        if (toks.size() < 2)
            return "ERR: screenshot-canvas <out.png> [W H]\n";
        if (!_axmolRoot) return "ERR: no axmol root\n";
        std::filesystem::path out = toks[1];
        if (out.is_relative()) out = std::filesystem::absolute(out);
        const auto cs = _axmolRoot->getContentSize();
        int W = (int)cs.width;
        int H = (int)cs.height;
        if (toks.size() >= 4)
        {
            W = std::atoi(toks[2].c_str());
            H = std::atoi(toks[3].c_str());
        }
        if (W <= 0 || H <= 0) return "ERR: bad size\n";

        auto* rt = ax::RenderTexture::create(W, H,
                                              ax::backend::PixelFormat::RGBA8);
        if (!rt) return "ERR: RenderTexture::create failed\n";
        // Visit the loaded root through an isolated transform: temp
        // detach + re-attach to capture in its own coord space. The
        // ax::Camera::project Y-flip is handled by setKeepMatrix.
        rt->beginWithClear(0, 0, 0, 0);
        // Save + reset root transform so the capture lands at (0,0).
        const auto origPos = _axmolRoot->getPosition();
        const auto origAnchor = _axmolRoot->getAnchorPoint();
        const auto origScale = ax::Vec2(_axmolRoot->getScaleX(),
                                         _axmolRoot->getScaleY());
        _axmolRoot->setPosition(0, 0);
        _axmolRoot->setAnchorPoint(ax::Vec2(0, 0));
        _axmolRoot->setScale(1.0f, 1.0f);
        _axmolRoot->visit();
        _axmolRoot->setPosition(origPos);
        _axmolRoot->setAnchorPoint(origAnchor);
        _axmolRoot->setScaleX(origScale.x);
        _axmolRoot->setScaleY(origScale.y);
        rt->end();
        rt->saveToFile(out.string(), /*isRGBA=*/true);
        return std::string("scheduled ") + out.string() + "\n";
    }

    if (cmd == "screenshot")
    {
        // Async full-window capture via axmol::utils. Reply is
        // `scheduled <abs path>` — the file lands after the current
        // frame finishes its renderer flush. Caller polls existence
        // or waits ~50 ms. Captures the WHOLE window including the
        // ImGui chrome; a canvas-only crop would need a per-Editor
        // RenderTexture round-trip.
        if (toks.size() < 2) return "ERR: screenshot <out.png>\n";
        std::filesystem::path out = toks[1];
        if (out.is_relative()) out = std::filesystem::absolute(out);
        const std::string outStr = out.string();
        ax::utils::captureScreen(
            [](bool ok, std::string_view path) {
                OCS_LOG_INFO("screenshot",
                    "captureScreen %s: %.*s",
                    ok ? "ok" : "fail",
                    (int)path.size(), path.data());
            },
            outStr);
        return std::string("scheduled ") + outStr + "\n";
    }

    return "ERR: unknown command; try 'help'\n";
}

}  // namespace opencs
