#include "IpcBridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <process.h>
#    include <windows.h>
#else
#    include <unistd.h>
#endif

namespace ocs::ipc
{
namespace
{

namespace fs = std::filesystem;

static fs::path makeBaseDir()
{
#if defined(_WIN32)
    const char* tmp = std::getenv("TEMP");
    if (!tmp || !*tmp) tmp = std::getenv("TMP");
    if (!tmp || !*tmp) tmp = "C:\\Temp";
    return fs::path(tmp) / "walshard";
#else
    return fs::path("/tmp/walshard");
#endif
}

const fs::path kBaseDir = makeBaseDir();

// Cross-platform process helpers. POSIX uses getpid + kill(0)/kill(SIGTERM).
// Windows uses _getpid + OpenProcess + TerminateProcess.
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
    return errno != ESRCH;  // EPERM = exists but not ours; still alive
#endif
}

static void platTerminate(int pid)
{
#if defined(_WIN32)
    HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE,
                             static_cast<DWORD>(pid));
    if (h)
    {
        ::TerminateProcess(h, 1);
        ::CloseHandle(h);
    }
#else
    ::kill(pid, SIGTERM);
#endif
}

// The registry filenames live under kBaseDir and use the current PID
// as their stem so each process has a unique, easily-discoverable
// identity on disk. See IpcBridge.h for the full layout.
std::string   gInitialized;     // guard — non-empty once init() ran
int           gSelfPid = 0;

fs::path pathFor(const std::string& suffix, int pid)
{
    return kBaseDir / (std::to_string(pid) + suffix);
}
fs::path selfWindowFile()  { return pathFor(".window",  gSelfPid); }
fs::path selfInboxDir()    { return pathFor(".inbox",   gSelfPid); }
fs::path selfHoverFile()   { return pathFor(".hover",   gSelfPid); }
fs::path peerInboxDir(int p)  { return pathFor(".inbox",  p); }
fs::path peerHoverFile(int p) { return pathFor(".hover",  p); }

// Detached-panel registry files share the base directory and use the
// panel name (not the PID) as the stem: `detached.<Name>`.  Content is
// the owning PID; readers validate liveness via processAlive().
fs::path detachedPanelFile(const std::string& name)
{
    return kBaseDir / ("detached." + name);
}

fs::path focusFile(int pid) { return pathFor(".focus", pid); }
fs::path mirrorFile(int pid, const std::string& panel)
{
    return kBaseDir / (std::to_string(pid) + "." + panel + ".mirror");
}

// Tracks the last panel name this process registered so shutdown can
// remove the correct file without the caller repeating the name.
std::string gSelfDetachedPanelName;

/// Atomic-ish write via temp+rename so a concurrent reader never
/// observes a half-written file.
void writeAtomic(const fs::path& target, const std::string& content)
{
    fs::path tmp = target;
    tmp += ".tmp";
    std::error_code ec;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(content.data(),
                  static_cast<std::streamsize>(content.size()));
    }
    fs::rename(tmp, target, ec);
    if (ec) fs::remove(tmp, ec);
}

/// Read file fully — returns empty string if unreadable.
std::string readAll(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string s((std::istreambuf_iterator<char>(in)),
                  std::istreambuf_iterator<char>());
    return s;
}

/// PID-liveness probe. Cross-platform; delegates to the platform
/// helper above (POSIX: kill(pid, 0); Windows: OpenProcess +
/// GetExitCodeProcess).
bool processAlive(int pid)
{
    if (pid <= 0) return false;
    return platProcessAlive(pid);
}

/// Called at exit/signal — best-effort cleanup of our registry so
/// crashed or killed instances leave minimal residue.
extern "C" void atexitCleanup()
{
    shutdown();
}

/// Signal handler — forward to atexit-style cleanup, then restore
/// default disposition and re-raise so the OS can record the crash.
extern "C" void onFatalSignal(int sig)
{
    shutdown();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

}  // anon

void init()
{
    if (!gInitialized.empty()) return;
    gSelfPid = platGetPid();
    std::error_code ec;
    fs::create_directories(kBaseDir, ec);
    fs::create_directories(selfInboxDir(), ec);
    // Best-effort hook so our files get cleaned up on normal exit
    // and on the most common fatal signals.
    std::atexit(atexitCleanup);
    std::signal(SIGINT,  onFatalSignal);
    std::signal(SIGTERM, onFatalSignal);
    gInitialized = "1";
}

void shutdown()
{
    if (gSelfPid <= 0) return;
    std::error_code ec;
    fs::remove(selfWindowFile(), ec);
    fs::remove(selfHoverFile(),  ec);
    fs::remove(focusFile(gSelfPid), ec);
    fs::remove_all(selfInboxDir(), ec);
    if (!gSelfDetachedPanelName.empty())
    {
        fs::remove(detachedPanelFile(gSelfDetachedPanelName), ec);
        gSelfDetachedPanelName.clear();
    }
    // Drop any mirror files this process published so stale data
    // doesn't linger for the next process at the same pid.
    for (auto& entry : fs::directory_iterator(kBaseDir, ec))
    {
        if (ec) break;
        const auto p = entry.path();
        const auto stem = p.stem().string();   // e.g. "<pid>.Messages"
        const auto dot = stem.find('.');
        if (dot == std::string::npos) continue;
        if (std::atoi(stem.substr(0, dot).c_str()) == gSelfPid &&
            p.extension() == ".mirror")
            fs::remove(p, ec);
    }
    gSelfPid = 0;
}

void registerDetachedPanel(const std::string& panelName)
{
    if (gSelfPid <= 0 || panelName.empty()) return;
    // If this process already owned a different panel (shouldn't happen
    // in practice — one detached panel per process — but defensive),
    // clear the old file before writing the new one.
    std::error_code ec;
    if (!gSelfDetachedPanelName.empty() &&
        gSelfDetachedPanelName != panelName)
        fs::remove(detachedPanelFile(gSelfDetachedPanelName), ec);
    gSelfDetachedPanelName = panelName;
    writeAtomic(detachedPanelFile(panelName),
                std::to_string(gSelfPid) + "\n");
}

void unregisterDetachedPanel()
{
    if (gSelfDetachedPanelName.empty()) return;
    std::error_code ec;
    fs::remove(detachedPanelFile(gSelfDetachedPanelName), ec);
    gSelfDetachedPanelName.clear();
}

std::vector<std::string> listDetachedPanels()
{
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::exists(kBaseDir, ec)) return out;
    for (auto& entry : fs::directory_iterator(kBaseDir, ec))
    {
        if (ec) break;
        const auto p = entry.path();
        const auto fname = p.filename().string();
        // Accept files named `detached.<Name>` only.
        constexpr const char* kPrefix = "detached.";
        if (fname.rfind(kPrefix, 0) != 0) continue;
        const std::string name = fname.substr(std::strlen(kPrefix));
        if (name.empty()) continue;
        // GC stale entries — if the registered pid is dead, drop it
        // so a crashed detached panel doesn't keep peers suppressing
        // the panel forever.
        const int pid = std::atoi(readAll(p).c_str());
        if (!processAlive(pid))
        {
            fs::remove(p, ec);
            continue;
        }
        out.push_back(name);
    }
    return out;
}

void terminateDetachedPanels()
{
    std::error_code ec;
    if (!fs::exists(kBaseDir, ec)) return;
    for (auto& entry : fs::directory_iterator(kBaseDir, ec))
    {
        if (ec) break;
        const auto p = entry.path();
        const auto fname = p.filename().string();
        if (fname.rfind("detached.", 0) != 0) continue;
        const int pid = std::atoi(readAll(p).c_str());
        if (pid > 0 && processAlive(pid)) platTerminate(pid);
        // Remove the registry file synchronously so peers see the
        // detach clear on the very next listDetachedPanels() scan —
        // no wait for the child's SIGTERM handler to unwind its own
        // shutdown(). Double-removal by the child is idempotent.
        fs::remove(p, ec);
    }
}

void terminateDetachedPanel(const std::string& panelName)
{
    if (panelName.empty()) return;
    std::error_code ec;
    const auto path = detachedPanelFile(panelName);
    if (!fs::exists(path, ec)) return;
    const int pid = std::atoi(readAll(path).c_str());
    if (pid > 0 && processAlive(pid)) platTerminate(pid);
    // Remove the registry file synchronously (same rationale as the
    // plural terminate): main-app's next singleton-sync scan sees the
    // panel as "no longer detached", flipping _show back to true so
    // the panel reappears at its last docked position immediately —
    // not after a multi-frame delay waiting for the child's own
    // shutdown handler to unwind.
    fs::remove(path, ec);
}

void publishFocus()
{
    if (gSelfPid <= 0) return;
    // An empty 0-byte file suffices — readers compare mtimes, not
    // content. Rewriting an existing file is enough to refresh mtime
    // on every platform we target.
    std::error_code ec;
    fs::create_directories(kBaseDir, ec);
    std::ofstream out(focusFile(gSelfPid), std::ios::binary | std::ios::trunc);
    // Stream is closed implicitly — fs::last_write_time sees the new mtime.
}

int mostRecentlyFocusedPeer()
{
    std::error_code ec;
    if (!fs::exists(kBaseDir, ec)) return 0;
    int bestPid = 0;
    fs::file_time_type bestTime{};
    for (auto& entry : fs::directory_iterator(kBaseDir, ec))
    {
        if (ec) break;
        const auto p = entry.path();
        if (p.extension() != ".focus") continue;
        const int pid = std::atoi(p.stem().string().c_str());
        if (pid <= 0 || pid == gSelfPid) continue;
        if (!processAlive(pid))
        {
            fs::remove(p, ec);
            continue;
        }
        const auto t = fs::last_write_time(p, ec);
        if (ec) continue;
        if (bestPid == 0 || t > bestTime)
        {
            bestPid = pid;
            bestTime = t;
        }
    }
    return bestPid;
}

void publishMirror(const std::string& panel, const std::string& body)
{
    if (gSelfPid <= 0 || panel.empty()) return;
    writeAtomic(mirrorFile(gSelfPid, panel), body);
}

std::string readMirror(int peerPid, const std::string& panel)
{
    if (peerPid <= 0 || panel.empty()) return {};
    return readAll(mirrorFile(peerPid, panel));
}

std::vector<std::pair<int, std::string>> listPeerEventMirrors()
{
    std::vector<std::pair<int, std::string>> out;
    std::error_code ec;
    if (!fs::exists(kBaseDir, ec)) return out;
    for (auto& entry : fs::directory_iterator(kBaseDir, ec))
    {
        if (ec) break;
        const auto p = entry.path();
        const auto fname = p.filename().string();
        // Only `<pid>.events.mirror` files. The mirror filename
        // schema is `<pid>.<panel>.mirror`, so look for the
        // ".events.mirror" suffix and parse the pid prefix.
        constexpr const char* kSuffix = ".events.mirror";
        const auto sufLen = std::strlen(kSuffix);
        if (fname.size() <= sufLen) continue;
        if (fname.compare(fname.size() - sufLen, sufLen, kSuffix) != 0)
            continue;
        const std::string stem = fname.substr(0, fname.size() - sufLen);
        const int pid = std::atoi(stem.c_str());
        if (pid <= 0 || pid == gSelfPid) continue;
        if (!processAlive(pid))
        {
            fs::remove(p, ec);
            continue;
        }
        out.emplace_back(pid, readAll(p));
    }
    std::sort(out.begin(), out.end(),
        [](auto& a, auto& b) { return a.first < b.first; });
    return out;
}

void publish(const SelfInfo& info)
{
    if (gSelfPid <= 0) return;
    std::string body;
    body.reserve(256 + info.tabRects.size() * 32);
    char hdr[256];
    const int n = std::snprintf(hdr, sizeof(hdr),
        "pid %d\nframe %d %d %d %d\ntabbar %d %d %d %d\ntabs %d\n",
        gSelfPid,
        info.frameX, info.frameY, info.frameW, info.frameH,
        info.tabBarX0, info.tabBarY0, info.tabBarX1, info.tabBarY1,
        info.tabCount);
    if (n > 0) body.assign(hdr, static_cast<size_t>(n));
    // Per-tab X extents for the drag-source to compute insertion idx.
    for (std::size_t i = 0; i < info.tabRects.size(); ++i)
    {
        char line[64];
        std::snprintf(line, sizeof(line), "tabrect %zu %d %d\n",
                      i, info.tabRects[i].first,
                      info.tabRects[i].second);
        body.append(line);
    }
    writeAtomic(selfWindowFile(), body);
}

void drainInbox(
    const std::function<void(const std::string&, int)>& onOpen,
    const std::function<void(const std::string&)>& onReattach,
    const std::function<void(const std::string&,
                             const std::string&,
                             const std::string&)>& onEdit,
    const std::function<void(const std::string&)>& onSelect,
    const std::function<void(const std::string&)>& onSelectAsset,
    const std::function<void(const std::string&)>& onReplaceDoc)
{
    if (gSelfPid <= 0) return;
    std::error_code ec;
    if (!fs::exists(selfInboxDir(), ec)) return;
    for (auto& entry : fs::directory_iterator(selfInboxDir(), ec))
    {
        if (ec) break;
        const auto p = entry.path();
        if (p.extension() != ".req") continue;
        // Request forms:
        //   "open <path>\n"                 — append at end
        //   "open_at <idx> <path>\n"        — insert at idx
        //   "reattach <panelName>\n"        — re-show hidden panel
        //   "edit <nodepath> <k>=<v>\n"     — apply attribute write
        std::string body = readAll(p);
        fs::remove(p, ec);
        if (body.rfind("reattach ", 0) == 0)
        {
            std::string name = body.substr(9);
            while (!name.empty() &&
                   (name.back() == '\n' || name.back() == '\r' ||
                    name.back() == ' '  || name.back() == '\t'))
                name.pop_back();
            if (!name.empty() && onReattach) onReattach(name);
            continue;
        }
        if (body.rfind("replacedoc\n", 0) == 0)
        {
            std::string xml = body.substr(11);
            if (!xml.empty() && onReplaceDoc) onReplaceDoc(xml);
            continue;
        }
        if (body.rfind("selectasset ", 0) == 0)
        {
            std::string absPath = body.substr(12);
            while (!absPath.empty() &&
                   (absPath.back() == '\n' || absPath.back() == '\r' ||
                    absPath.back() == ' '  || absPath.back() == '\t'))
                absPath.pop_back();
            if (onSelectAsset) onSelectAsset(absPath);
            continue;
        }
        if (body.rfind("select ", 0) == 0)
        {
            std::string nodepath = body.substr(7);
            while (!nodepath.empty() &&
                   (nodepath.back() == '\n' || nodepath.back() == '\r' ||
                    nodepath.back() == ' '  || nodepath.back() == '\t'))
                nodepath.pop_back();
            if (onSelect) onSelect(nodepath);
            continue;
        }
        if (body.rfind("edit ", 0) == 0)
        {
            // Strip trailing whitespace/newline once.
            while (!body.empty() &&
                   (body.back() == '\n' || body.back() == '\r'))
                body.pop_back();
            const std::string rest = body.substr(5);
            const auto sp = rest.find(' ');
            if (sp == std::string::npos) continue;
            const std::string nodepath = rest.substr(0, sp);
            const std::string kv       = rest.substr(sp + 1);
            const auto eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const std::string key   = kv.substr(0, eq);
            const std::string value = kv.substr(eq + 1);
            if (onEdit) onEdit(nodepath, key, value);
            continue;
        }
        int insertIdx = -1;
        std::string path;
        if (body.rfind("open_at ", 0) == 0)
        {
            int idx = -1;
            char buf[4096];
            if (std::sscanf(body.c_str(), "open_at %d %4095[^\n]",
                            &idx, buf) == 2)
            {
                insertIdx = idx;
                path = buf;
            }
        }
        else if (body.rfind("open ", 0) == 0)
        {
            path = body.substr(5);
        }
        while (!path.empty() &&
               (path.back() == '\n' || path.back() == '\r' ||
                path.back() == ' '  || path.back() == '\t'))
            path.pop_back();
        if (!path.empty()) onOpen(path, insertIdx);
    }
}

std::vector<PeerInfo> listPeers()
{
    std::vector<PeerInfo> out;
    std::error_code ec;
    if (!fs::exists(kBaseDir, ec)) return out;
    for (auto& entry : fs::directory_iterator(kBaseDir, ec))
    {
        if (ec) break;
        const auto p = entry.path();
        if (p.extension() != ".window") continue;
        const int pid = std::atoi(p.stem().string().c_str());
        if (pid <= 0) continue;
        if (pid == gSelfPid) continue;
        // GC stale entries so the list stays short and accurate.
        if (!processAlive(pid)) {
            fs::remove(p, ec);
            fs::remove(peerHoverFile(pid), ec);
            fs::remove_all(peerInboxDir(pid), ec);
            continue;
        }
        PeerInfo info;
        info.pid = pid;
        std::ifstream in(p, std::ios::binary);
        std::string line;
        while (std::getline(in, line))
        {
            // Very small format — trivial parsing suffices.
            if (line.rfind("frame ", 0) == 0)
                std::sscanf(line.c_str(), "frame %d %d %d %d",
                    &info.frameX, &info.frameY,
                    &info.frameW, &info.frameH);
            else if (line.rfind("tabbar ", 0) == 0)
                std::sscanf(line.c_str(), "tabbar %d %d %d %d",
                    &info.tabBarX0, &info.tabBarY0,
                    &info.tabBarX1, &info.tabBarY1);
            else if (line.rfind("tabs ", 0) == 0)
                std::sscanf(line.c_str(), "tabs %d", &info.tabCount);
            else if (line.rfind("tabrect ", 0) == 0)
            {
                std::size_t idx = 0;
                int x0 = 0, x1 = 0;
                if (std::sscanf(line.c_str(),
                        "tabrect %zu %d %d", &idx, &x0, &x1) == 3)
                {
                    if (info.tabRects.size() <= idx)
                        info.tabRects.resize(idx + 1, {0, 0});
                    info.tabRects[idx] = {x0, x1};
                }
            }
        }
        out.push_back(info);
    }
    return out;
}

void sendOpen(int targetPid, const std::string& csdPath, int insertIdx)
{
    if (targetPid <= 0 || csdPath.empty()) return;
    // Unique filename so concurrent drops from different sources
    // don't clobber each other in the same inbox.
    static std::atomic<uint64_t> seq{0};
    const uint64_t n = seq.fetch_add(1);
    char name[64];
    std::snprintf(name, sizeof(name), "%d-%llu.req",
                  gSelfPid, static_cast<unsigned long long>(n));
    fs::path target = peerInboxDir(targetPid) / name;
    std::string body;
    if (insertIdx >= 0)
        body = "open_at " + std::to_string(insertIdx) + " " +
               csdPath + "\n";
    else
        body = "open " + csdPath + "\n";
    writeAtomic(target, body);
}

void sendReattach(int targetPid, const std::string& panelName)
{
    if (targetPid <= 0 || panelName.empty()) return;
    static std::atomic<uint64_t> seq{0};
    const uint64_t n = seq.fetch_add(1);
    char name[64];
    std::snprintf(name, sizeof(name), "%d-r%llu.req",
                  gSelfPid, static_cast<unsigned long long>(n));
    fs::path target = peerInboxDir(targetPid) / name;
    writeAtomic(target, "reattach " + panelName + "\n");
}

void sendEdit(int targetPid,
              const std::string& nodepath,
              const std::string& key,
              const std::string& value)
{
    if (targetPid <= 0 || nodepath.empty() || key.empty()) return;
    static std::atomic<uint64_t> seq{0};
    const uint64_t n = seq.fetch_add(1);
    char name[64];
    std::snprintf(name, sizeof(name), "%d-e%llu.req",
                  gSelfPid, static_cast<unsigned long long>(n));
    fs::path target = peerInboxDir(targetPid) / name;
    writeAtomic(target,
        "edit " + nodepath + " " + key + "=" + value + "\n");
}

void sendSelect(int targetPid, const std::string& nodepath)
{
    if (targetPid <= 0) return;
    static std::atomic<uint64_t> seq{0};
    const uint64_t n = seq.fetch_add(1);
    char name[64];
    std::snprintf(name, sizeof(name), "%d-s%llu.req",
                  gSelfPid, static_cast<unsigned long long>(n));
    fs::path target = peerInboxDir(targetPid) / name;
    writeAtomic(target, "select " + nodepath + "\n");
}

void sendSelectAsset(int targetPid, const std::string& absPath)
{
    if (targetPid <= 0) return;
    static std::atomic<uint64_t> seq{0};
    const uint64_t n = seq.fetch_add(1);
    char name[64];
    std::snprintf(name, sizeof(name), "%d-a%llu.req",
                  gSelfPid, static_cast<unsigned long long>(n));
    fs::path target = peerInboxDir(targetPid) / name;
    writeAtomic(target, "selectasset " + absPath + "\n");
}

void sendReplaceDoc(int targetPid, const std::string& xmlBody)
{
    if (targetPid <= 0 || xmlBody.empty()) return;
    static std::atomic<uint64_t> seq{0};
    const uint64_t n = seq.fetch_add(1);
    char name[64];
    std::snprintf(name, sizeof(name), "%d-d%llu.req",
                  gSelfPid, static_cast<unsigned long long>(n));
    fs::path target = peerInboxDir(targetPid) / name;
    // The body is multi-line XML so we sentinel it with a single
    // header line; everything after the first newline is the doc.
    writeAtomic(target, "replacedoc\n" + xmlBody);
}

void writeHoverSignal(int targetPid,
                      int sourcePid,
                      int cursorScreenX,
                      const std::string& dragLabel)
{
    if (targetPid <= 0) return;
    // Simple line-per-field format. Label may contain spaces, so it
    // goes on its own line.
    std::string body;
    body.reserve(64 + dragLabel.size());
    body  = "source " + std::to_string(sourcePid) + "\n";
    body += "cursorx " + std::to_string(cursorScreenX) + "\n";
    body += "label " + dragLabel + "\n";
    writeAtomic(peerHoverFile(targetPid), body);
}

void clearHoverSignal(int targetPid)
{
    if (targetPid <= 0) return;
    std::error_code ec;
    fs::remove(peerHoverFile(targetPid), ec);
}

bool readHoverSignal(int& sourcePidOut,
                     int& cursorScreenXOut,
                     std::string& dragLabelOut)
{
    sourcePidOut = 0;
    cursorScreenXOut = 0;
    dragLabelOut.clear();
    if (gSelfPid <= 0) return false;
    std::error_code ec;
    const auto path = selfHoverFile();
    if (!fs::exists(path, ec)) return false;
    // Stale guard — if the source crashed without clearing, ignore
    // the file once it's older than 500 ms.
    const auto mod = fs::last_write_time(path, ec);
    if (ec) return false;
    const auto now = decltype(mod)::clock::now();
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - mod).count();
    if (age > 500) return false;
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.rfind("source ",  0) == 0)
            sourcePidOut = std::atoi(line.c_str() + 7);
        else if (line.rfind("cursorx ", 0) == 0)
            cursorScreenXOut = std::atoi(line.c_str() + 8);
        else if (line.rfind("label ",  0) == 0)
            dragLabelOut = line.substr(6);
    }
    return sourcePidOut > 0;
}

}  // namespace ocs::ipc
