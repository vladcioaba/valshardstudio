#include "UILayoutEditor/Log.h"

#include "axmol.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <process.h>
#    include <windows.h>
#else
#    include <unistd.h>
#endif

namespace opencs
{

namespace
{
/// Wall-time seconds since the first logger call. Relative timestamps
/// keep log output compact; absolute timestamps are left to the sinks
/// that care (FileSink prefixes with HH:MM:SS.mmm).
double elapsed()
{
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    return std::chrono::duration<double>(clock::now() - t0).count();
}

// Cross-platform helpers (mirror IpcBridge.cpp). Inlined per file rather
// than factored to keep the diff small; the same logic lives in
// IpcBridge.cpp.
static int platGetPid()
{
#if defined(_WIN32)
    return static_cast<int>(::_getpid());
#else
    return static_cast<int>(::getpid());
#endif
}

static bool platProcessAlive(int pid)
{
#if defined(_WIN32)
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                             static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD code = 0;
    BOOL ok = ::GetExitCodeProcess(h, &code);
    ::CloseHandle(h);
    return ok && code == STILL_ACTIVE;
#else
    if (::kill(pid, 0) == 0) return true;
    return errno != ESRCH;
#endif
}

static std::string platIpcDir()
{
#if defined(_WIN32)
    const char* tmp = std::getenv("TEMP");
    if (!tmp || !*tmp) tmp = std::getenv("TMP");
    if (!tmp || !*tmp) tmp = "C:\\Temp";
    return std::string(tmp) + "\\walshard";
#else
    return std::string("/tmp/walshard");
#endif
}

// Returns absolute path to per-user config file (no extension prefix).
// macOS / Linux: `<home>/.walshard.<suffix>` (legacy dotfile path).
// Windows: `%LOCALAPPDATA%\Walshard Studio\<suffix>`.
static std::string platUserConfigFile(const char* suffix)
{
#if defined(_WIN32)
    const char* base = std::getenv("LOCALAPPDATA");
    if (!base || !*base) base = std::getenv("APPDATA");
    if (!base || !*base) return std::string("walshard.") + suffix;
    namespace fs = std::filesystem;
    fs::path dir = fs::path(base) / "Walshard Studio";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / suffix).string();
#else
    const char* home = std::getenv("HOME");
    if (home && *home)
        return std::string(home) + "/.walshard." + suffix;
    return platIpcDir() + "/" + suffix;
#endif
}
}  // namespace


const char* levelName(LogLevel lv)
{
    switch (lv)
    {
        case LogLevel::Trace: return "TRC";
        case LogLevel::Debug: return "DBG";
        case LogLevel::Info:  return "INF";
        case LogLevel::Warn:  return "WRN";
        case LogLevel::Error: return "ERR";
        case LogLevel::Off:   return "OFF";
    }
    return "???";
}


// -- Log ---------------------------------------------------------------------

Log& Log::instance()
{
    static Log g;
    return g;
}

void Log::addSink(std::shared_ptr<ILogSink> sink)
{
    std::lock_guard<std::mutex> lk(_mu);
    _sinks.push_back(std::move(sink));
}

void Log::removeSink(const std::shared_ptr<ILogSink>& sink)
{
    std::lock_guard<std::mutex> lk(_mu);
    for (auto it = _sinks.begin(); it != _sinks.end(); ++it)
    {
        if (*it == sink) { _sinks.erase(it); return; }
    }
}

void Log::logv(LogLevel lv, const char* category,
               const char* fmt, std::va_list ap)
{
    if (static_cast<int>(lv) < static_cast<int>(_level)) return;

    // Format once into a stack buffer; on overflow fall back to a heap
    // string. Most log lines are short enough to stay on the stack.
    char stackBuf[512];
    std::va_list ap2;
    va_copy(ap2, ap);
    const int n = std::vsnprintf(stackBuf, sizeof(stackBuf), fmt, ap);
    std::string msg;
    if (n >= 0 && n < (int)sizeof(stackBuf))
    {
        msg.assign(stackBuf, (std::size_t)n);
    }
    else if (n > 0)
    {
        msg.resize((std::size_t)n);
        std::vsnprintf(msg.data(), msg.size() + 1, fmt, ap2);
    }
    else
    {
        msg = "<format error>";
    }
    va_end(ap2);

    LogRecord r;
    r.level    = lv;
    r.category = category ? category : "ocs";
    r.time     = elapsed();
    r.message  = std::move(msg);
    r.source   = windowLabel();

    std::vector<std::shared_ptr<ILogSink>> snapshot;
    {
        std::lock_guard<std::mutex> lk(_mu);
        snapshot = _sinks;
    }
    for (auto& s : snapshot)
    {
        if (static_cast<int>(lv) < static_cast<int>(s->minLevel)) continue;
        s->write(r);
    }
}

void Log::log(LogLevel lv, const char* category, const char* fmt, ...)
{
    std::va_list ap;
    va_start(ap, fmt);
    logv(lv, category, fmt, ap);
    va_end(ap);
}


// -- StdoutSink --------------------------------------------------------------

void StdoutSink::write(const LogRecord& r)
{
    // [t=12.345 INF cat ] message
    std::fprintf(stderr, "[t=%.3f %s %s] %s\n",
                 r.time, levelName(r.level), r.category,
                 r.message.c_str());
}


// -- RingSink ----------------------------------------------------------------

RingSink::RingSink(std::size_t capacity) : _cap(capacity) {}

void RingSink::write(const LogRecord& r)
{
    std::lock_guard<std::mutex> lk(_mu);
    _ring.push_back(r);
    while (_ring.size() > _cap) _ring.pop_front();
}

std::vector<LogRecord> RingSink::snapshot() const
{
    std::lock_guard<std::mutex> lk(_mu);
    return {_ring.begin(), _ring.end()};
}

void RingSink::clear()
{
    std::lock_guard<std::mutex> lk(_mu);
    _ring.clear();
}


// -- FileSink ----------------------------------------------------------------

FileSink::FileSink(std::string path) : _path(std::move(path))
{
    if (_path.empty())
        _path = platUserConfigFile("log");
    _fp = std::fopen(_path.c_str(), "a");
}

FileSink::~FileSink()
{
    if (_fp) std::fclose((FILE*)_fp);
    _fp = nullptr;
}

void FileSink::write(const LogRecord& r)
{
    if (!_fp) return;
    std::lock_guard<std::mutex> lk(_mu);
    std::fprintf((FILE*)_fp, "[t=%.3f %s %s] %s\n",
                 r.time, levelName(r.level), r.category,
                 r.message.c_str());
    std::fflush((FILE*)_fp);
}


// -- IpcLogSink + tail reader ------------------------------------------------

namespace
{
// kIpcDir is now a runtime value (platform-dependent — POSIX
// `/tmp/walshard`, Windows `%TEMP%\walshard`). Cached at first use.
const std::string& kIpcDir()
{
    static const std::string v = platIpcDir();
    return v;
}
constexpr std::size_t kIpcLogTruncateAt = 1 * 1024 * 1024;   // 1 MB

std::string defaultIpcLogPath()
{
    namespace fs = std::filesystem;
    return (fs::path(kIpcDir()) /
            (std::to_string(platGetPid()) + ".messages.log")).string();
}

/// Quote `\n` and `|` in `s` so each record stays a single line in
/// the file format. Cheap — copies the string once and replaces
/// the few problem characters in-place.
std::string flattenForLine(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s)
    {
        if (c == '\n')      out.append("\\n");
        else if (c == '|')  out.append("\\|");
        else                out.push_back(c);
    }
    return out;
}

std::string unflattenFromLine(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\\' && i + 1 < s.size())
        {
            const char nxt = s[i + 1];
            if (nxt == 'n') { out.push_back('\n'); ++i; continue; }
            if (nxt == '|') { out.push_back('|');  ++i; continue; }
        }
        out.push_back(s[i]);
    }
    return out;
}
}  // namespace

IpcLogSink::IpcLogSink(std::string path) : _path(std::move(path))
{
    if (_path.empty()) _path = defaultIpcLogPath();
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(_path).parent_path(), ec);
    _fp = std::fopen(_path.c_str(), "a");
}

IpcLogSink::~IpcLogSink()
{
    if (_fp) std::fclose((FILE*)_fp);
    _fp = nullptr;
}

void IpcLogSink::write(const LogRecord& r)
{
    if (!_fp) return;
    std::lock_guard<std::mutex> lk(_mu);

    // Lazy truncate: when the file gets large, rewind it. We're
    // single-writer per file, so the truncate is safe and tail
    // readers just see fewer historic records on the next poll.
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto sz = fs::file_size(_path, ec);
    if (!ec && sz > kIpcLogTruncateAt)
    {
        std::fclose((FILE*)_fp);
        _fp = std::fopen(_path.c_str(), "w");
        if (!_fp) return;
    }

    // Format: time|level|pid|window|category|message
    // The `window` field lets the panel show which OCS window
    // emitted each record so the user can tell main-app events
    // from detached-panel events at a glance.
    std::fprintf((FILE*)_fp, "%.6f|%d|%d|%s|%s|%s\n",
                 r.time, (int)r.level, (int)platGetPid(),
                 flattenForLine(windowLabel()).c_str(),
                 r.category ? r.category : "ocs",
                 flattenForLine(r.message).c_str());
    std::fflush((FILE*)_fp);
}

std::vector<LogRecord> tailIpcLogs(std::size_t tailMax)
{
    namespace fs = std::filesystem;
    std::vector<LogRecord> out;
    std::error_code ec;
    if (!fs::exists(kIpcDir(), ec)) return out;

    // Each process appends to its own `<pid>.messages.log` file —
    // collect all of them, parse the lines, then merge.
    for (auto& e : fs::directory_iterator(kIpcDir(), ec))
    {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        const auto& p = e.path();
        const auto name = p.filename().string();
        if (name.size() <= 13 ||
            name.compare(name.size() - 13, 13, ".messages.log") != 0)
            continue;
        std::FILE* f = std::fopen(p.c_str(), "r");
        if (!f) continue;
        char line[2048];
        while (std::fgets(line, sizeof(line), f))
        {
            // Strip trailing newline.
            std::size_t len = std::strlen(line);
            while (len > 0 && (line[len - 1] == '\n' ||
                                line[len - 1] == '\r'))
                line[--len] = '\0';
            if (len == 0) continue;

            // Split on '|' into time|level|pid|window|cat|msg.
            // Message is the rest after the 5th separator (so it
            // can contain `|` in flattened form).
            std::string s(line, len);
            std::size_t a = s.find('|');
            if (a == std::string::npos) continue;
            std::size_t b = s.find('|', a + 1);
            if (b == std::string::npos) continue;
            std::size_t c = s.find('|', b + 1);
            if (c == std::string::npos) continue;
            std::size_t d = s.find('|', c + 1);
            if (d == std::string::npos) continue;
            std::size_t e2 = s.find('|', d + 1);

            LogRecord r;
            r.time     = std::atof(s.substr(0, a).c_str());
            r.level    = static_cast<LogLevel>(
                            std::atoi(s.substr(a + 1, b - a - 1).c_str()));
            std::string winLabel;
            std::string cat;
            std::string msg;
            if (e2 == std::string::npos)
            {
                // Old-format file (no window field) — synthesize
                // a label from the pid for backwards compat.
                winLabel = "pid:" + s.substr(b + 1, c - b - 1);
                cat      = s.substr(c + 1, d - c - 1);
                msg      = unflattenFromLine(s.substr(d + 1));
            }
            else
            {
                winLabel = unflattenFromLine(
                            s.substr(c + 1, d - c - 1));
                cat      = s.substr(d + 1, e2 - d - 1);
                msg      = unflattenFromLine(s.substr(e2 + 1));
            }
            // `category` is const char* — no owned storage — so we
            // pin it to a static "ipc" sentinel and surface the
            // real category as the source's "/cat" suffix. The
            // panel-side renderer pulls source + message into
            // properly aligned columns.
            r.category = "ipc";
            r.source   = winLabel + "/" + cat;
            r.message  = std::move(msg);
            out.push_back(std::move(r));
        }
        std::fclose(f);
    }

    // Stable-sort by timestamp so multi-process records interleave
    // chronologically.
    std::sort(out.begin(), out.end(),
              [](const LogRecord& l, const LogRecord& r) {
                  return l.time < r.time;
              });
    if (out.size() > tailMax)
        out.erase(out.begin(), out.end() - tailMax);
    return out;
}


// -- Shared log level (cross-process + cross-session persisted) -------------

namespace
{
std::string sharedLogLevelPath()
{
    // Persisted to the user's app data dir so the level survives
    // reboots / /tmp wipes / fresh launches. Same file used both
    // for cross-process coordination (poll the mtime each frame)
    // and for session restoration (read once on startup).
    return platUserConfigFile("loglevel");
}

// Per-process label used by the Messages panel to tell the user
// which window emitted a record. Empty = fall back to "pid:<n>".
std::mutex gLabelMu;
std::string gWindowLabel;
}

void setWindowLabel(std::string label)
{
    std::lock_guard<std::mutex> lk(gLabelMu);
    gWindowLabel = std::move(label);
}

std::string windowLabel()
{
    std::lock_guard<std::mutex> lk(gLabelMu);
    if (!gWindowLabel.empty()) return gWindowLabel;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "pid:%d", (int)platGetPid());
    return std::string(buf);
}

void publishSharedLogLevel(LogLevel lv)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(kIpcDir(), ec);
    if (auto* fp = std::fopen(sharedLogLevelPath().c_str(), "w"))
    {
        std::fprintf(fp, "%d\n", static_cast<int>(lv));
        std::fclose(fp);
    }
    Log::instance().setLevel(lv);
}

void pollSharedLogLevel()
{
    static std::atomic<int64_t> sLastMtime{0};
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto path = sharedLogLevelPath();
    if (!fs::exists(path, ec)) return;
    auto mt = fs::last_write_time(path, ec);
    if (ec) return;
    const int64_t mtVal = mt.time_since_epoch().count();
    if (mtVal == sLastMtime.load()) return;
    sLastMtime.store(mtVal);
    if (auto* fp = std::fopen(path.c_str(), "r"))
    {
        int v = -1;
        if (std::fscanf(fp, "%d", &v) == 1 && v >= 0 &&
            v <= static_cast<int>(LogLevel::Off))
            Log::instance().setLevel(static_cast<LogLevel>(v));
        std::fclose(fp);
    }
}


// -- UI prefs (cross-session) ----------------------------------------------

namespace
{
std::string uiPrefsPath()
{
    return platUserConfigFile("uiprefs");
}

std::map<std::string, std::string> readUiPrefs()
{
    std::map<std::string, std::string> out;
    if (FILE* fp = std::fopen(uiPrefsPath().c_str(), "r"))
    {
        char line[512];
        while (std::fgets(line, sizeof(line), fp))
        {
            std::size_t len = std::strlen(line);
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
                line[--len] = '\0';
            const char* eq = std::strchr(line, '=');
            if (!eq) continue;
            out.emplace(std::string(line, eq - line), std::string(eq + 1));
        }
        std::fclose(fp);
    }
    return out;
}

void writeUiPrefs(const std::map<std::string, std::string>& prefs)
{
    if (FILE* fp = std::fopen(uiPrefsPath().c_str(), "w"))
    {
        for (const auto& [k, v] : prefs)
            std::fprintf(fp, "%s=%s\n", k.c_str(), v.c_str());
        std::fclose(fp);
    }
}
}  // namespace

float getUiPrefF(const char* key, float dflt)
{
    auto p = readUiPrefs();
    auto it = p.find(key);
    if (it == p.end()) return dflt;
    return std::atof(it->second.c_str());
}
void setUiPrefF(const char* key, float value)
{
    auto p = readUiPrefs();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", value);
    p[key] = buf;
    writeUiPrefs(p);
}
bool getUiPrefBool(const char* key, bool dflt)
{
    auto p = readUiPrefs();
    auto it = p.find(key);
    if (it == p.end()) return dflt;
    return it->second == "1" || it->second == "true";
}
void setUiPrefBool(const char* key, bool value)
{
    auto p = readUiPrefs();
    p[key] = value ? "1" : "0";
    writeUiPrefs(p);
}


// -- Defaults ----------------------------------------------------------------

namespace
{
std::shared_ptr<RingSink> gRingSink;
bool gInitialised = false;
}

std::shared_ptr<RingSink> defaultRingSink()
{
    return gRingSink;
}

void initDefaultLogging()
{
    if (gInitialised) return;
    gInitialised = true;
    auto& log = Log::instance();
    log.addSink(std::make_shared<StdoutSink>());
    gRingSink = std::make_shared<RingSink>(2048);
    log.addSink(gRingSink);

    // GC orphan IPC log files from previous (now-dead) sessions so
    // the panel doesn't show messages from PIDs that no longer
    // exist. Match by filename `<pid>.messages.log` and drop the
    // file when kill(pid, 0) reports ESRCH (no such process).
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(kIpcDir(), ec))
    {
        for (auto& e : fs::directory_iterator(kIpcDir(), ec))
        {
            if (ec) break;
            const auto& p = e.path();
            const auto name = p.filename().string();
            if (name.size() <= 13 ||
                name.compare(name.size() - 13, 13, ".messages.log") != 0)
                continue;
            const std::string pidStr = name.substr(0, name.size() - 13);
            int pid = std::atoi(pidStr.c_str());
            if (pid <= 0) continue;
            if (!platProcessAlive(pid))
            {
                std::error_code rmEc;
                fs::remove(p, rmEc);
            }
        }
    }

    // Cross-process message queue. Each OCS process (main + each
    // detached panel) appends here; every Messages panel reads ALL
    // *.messages.log files, so the detached Messages window can
    // see events coming out of the main app and vice-versa.
    log.addSink(std::make_shared<IpcLogSink>());

    // Pick up the cross-process level — if any sibling process has
    // already set Trace via its Messages panel, this freshly-
    // launched process should match.
    pollSharedLogLevel();
}


// -- Scene-graph dump --------------------------------------------------------
//
// Three verbosity levels. They share the name + indent prefix and each
// lower level is a strict subset of the next — the formatters build the
// line incrementally so adding a verbosity stays trivial.

namespace
{

/// Best-effort name: prefers node.getName, falls back to a compact
/// description ("<ClassName>"), ultimately "<node>".
std::string nodeLabel(ax::Node* n)
{
    if (!n) return "<null>";
    const auto name = n->getName();
    if (!name.empty()) return std::string(name);
    const auto desc = n->getDescription();
    if (!desc.empty()) return std::string("<") + std::string(desc) + ">";
    return "<node>";
}

/// World-space axis-aligned bounding box of a node. Rotated nodes get
/// projected to axis-aligned — good enough for a log.
struct WorldAABB { float minX, minY, maxX, maxY; };
WorldAABB worldAABB(ax::Node* n)
{
    const auto cs = n->getContentSize();
    const ax::Vec2 c[4] = {
        n->convertToWorldSpace(ax::Vec2(0,         0)),
        n->convertToWorldSpace(ax::Vec2(cs.width,  0)),
        n->convertToWorldSpace(ax::Vec2(cs.width,  cs.height)),
        n->convertToWorldSpace(ax::Vec2(0,         cs.height)),
    };
    WorldAABB b{ c[0].x, c[0].y, c[0].x, c[0].y };
    for (int i = 1; i < 4; ++i)
    {
        b.minX = std::min(b.minX, c[i].x);
        b.minY = std::min(b.minY, c[i].y);
        b.maxX = std::max(b.maxX, c[i].x);
        b.maxY = std::max(b.maxY, c[i].y);
    }
    return b;
}

}  // namespace


void dumpSceneGraph(ax::Node* root, LogLevel level,
                    GraphDumpVerbosity verbosity)
{
    if (!root)
    {
        Log::instance().log(level, "graph", "<null root>");
        return;
    }
    std::function<void(ax::Node*, int)> walk =
        [&](ax::Node* n, int depth)
    {
        if (!n) return;
        char indent[64] = {0};
        const int spaces = std::min(depth * 2, 60);
        for (int i = 0; i < spaces; ++i) indent[i] = ' ';

        const auto lbl = nodeLabel(n);
        const auto b   = worldAABB(n);

        if (verbosity == GraphDumpVerbosity::Bounds)
        {
            Log::instance().log(level, "graph",
                "%s- %s  aabb=(%.1f,%.1f)-(%.1f,%.1f)",
                indent, lbl.c_str(), b.minX, b.minY, b.maxX, b.maxY);
        }
        else if (verbosity == GraphDumpVerbosity::Visual)
        {
            // Add color, opacity, and the first child-of-type texture
            // hint if the node renders one (Sprite, SpriteFrame-based).
            const auto c = n->getColor();
            const uint8_t o = n->getOpacity();
            std::string texInfo;
            if (auto* sp = dynamic_cast<ax::Sprite*>(n))
            {
                if (auto* tex = sp->getTexture())
                    texInfo = " tex='" + tex->getPath() + "'";
            }
            Log::instance().log(level, "graph",
                "%s- %s  aabb=(%.1f,%.1f)-(%.1f,%.1f) "
                "rgb=(%d,%d,%d) a=%d%s",
                indent, lbl.c_str(), b.minX, b.minY, b.maxX, b.maxY,
                (int)c.r, (int)c.g, (int)c.b, (int)o, texInfo.c_str());
        }
        else  // Full
        {
            const auto p  = n->getPosition();
            const auto cs = n->getContentSize();
            const auto ap = n->getAnchorPoint();
            const auto c  = n->getColor();
            const uint8_t o = n->getOpacity();
            std::string texInfo;
            if (auto* sp = dynamic_cast<ax::Sprite*>(n))
            {
                if (auto* tex = sp->getTexture())
                    texInfo = " tex='" + tex->getPath() + "'";
            }
            Log::instance().log(level, "graph",
                "%s- %s  aabb=(%.1f,%.1f)-(%.1f,%.1f) "
                "pos=(%.1f,%.1f) cs=(%.0fx%.0f) anchor=(%.2f,%.2f) "
                "scale=(%.2f,%.2f) rot=%.2f z=%d ignoreAnchor=%d "
                "rgb=(%d,%d,%d) a=%d visible=%d%s  [%p]",
                indent, lbl.c_str(), b.minX, b.minY, b.maxX, b.maxY,
                p.x, p.y, cs.width, cs.height, ap.x, ap.y,
                n->getScaleX(), n->getScaleY(),
                n->getRotation(), n->getLocalZOrder(),
                n->isIgnoreAnchorPointForPosition() ? 1 : 0,
                (int)c.r, (int)c.g, (int)c.b, (int)o,
                n->isVisible() ? 1 : 0, texInfo.c_str(), (void*)n);
        }
        for (auto* cn : n->getChildren()) walk(cn, depth + 1);
    };
    Log::instance().log(level, "graph",
        "---- scene graph (%s) ----",
        verbosity == GraphDumpVerbosity::Bounds ? "bounds" :
        verbosity == GraphDumpVerbosity::Visual ? "visual" : "full");
    walk(root, 0);
    Log::instance().log(level, "graph", "---- end graph   ----");
}

}  // namespace opencs
