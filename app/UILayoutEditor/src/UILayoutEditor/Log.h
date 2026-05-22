// Walshard Studio logger.
//
// Central sink-based logger. One entry point (`Log::instance().log(...)` or
// the `OCS_LOG_*` macros) fans out to every attached sink. Sinks are
// responsible for their own formatting / filtering / persistence; the
// core just carries level, category, timestamp, and a formatted message.
//
// Built-ins:
//   StdoutSink — prints to stderr with a colourless `[lv cat] msg` prefix.
//   RingSink   — fixed-capacity in-memory ring; tail readable via inspect
//                or rendered into the future ImGui "Log" panel.
//   FileSink   — appends to a rotating file; safe across process crashes.
//   InspectTailSink — pushes each record into a subscriber queue so
//                the inspect server can stream logs to a client socket.
//
// Threading: Log::log() takes a mutex around the sink fan-out. Sinks are
// expected to keep their `write` short — no blocking I/O on hot paths.
//
// Integration: existing `std::printf("[ocs] …")` / `std::fprintf(stderr,…)`
// sites should migrate to OCS_LOG_* over time.  `dumpSceneGraph` prints a
// readable hierarchy — hook it to a keybind or the inspect channel when
// you need a snapshot of what's currently loaded.

#pragma once

#include <cstdarg>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ax { class Node; }

namespace opencs
{

enum class LogLevel
{
    Trace = 0,  ///< per-frame firehose, off by default
    Debug,      ///< developer diagnostics
    Info,       ///< user-visible narration (load / save / export)
    Warn,       ///< recoverable anomaly
    Error,      ///< failed operation
    Off         ///< silence — never actually emitted
};

/// One logged record. Sinks receive this by const ref so they can reuse
/// the same string buffer without copying.
struct LogRecord
{
    LogLevel    level    = LogLevel::Info;
    const char* category = "ocs";
    double      time     = 0.0;          ///< seconds since process start
    std::string message;
    /// Owning window label of the emitting process — set by Log::log
    /// from `windowLabel()` for new records, or by `tailIpcLogs`
    /// from the on-disk field for cross-process replay. Used by
    /// the Messages panel to show which window emitted each row.
    std::string source;
};

/// Base class for log sinks. Override `write` to persist the record
/// wherever you want (stdout, file, socket, ImGui ring buffer, …).
class ILogSink
{
public:
    virtual ~ILogSink() = default;
    virtual void write(const LogRecord& r) = 0;
    /// Per-sink minimum level. Records below this level skip this sink.
    LogLevel minLevel = LogLevel::Trace;
};


/// Singleton logger. `instance()` is construct-on-first-use; safe to call
/// before main if needed (no ordering with static sinks).
class Log
{
public:
    static Log& instance();

    /// Set the global minimum level — records below this are discarded
    /// before any sink is consulted. Individual sinks can further filter
    /// via `ILogSink::minLevel`.
    void setLevel(LogLevel lv) { _level = lv; }
    LogLevel level() const     { return _level; }

    /// Register a sink. Takes shared ownership; the sink stays alive as
    /// long as the logger references it.
    void addSink(std::shared_ptr<ILogSink> sink);
    /// Remove a previously added sink. No-op if not present.
    void removeSink(const std::shared_ptr<ILogSink>& sink);

    /// Printf-style log entry. `category` is a short tag (file / module
    /// name) shown in the formatted output; keep it stable so sinks can
    /// filter on it.
    void logv(LogLevel lv, const char* category,
              const char* fmt, std::va_list ap);
    void log (LogLevel lv, const char* category,
              const char* fmt, ...);

private:
    Log() = default;

    std::mutex _mu;
    std::vector<std::shared_ptr<ILogSink>> _sinks;
    LogLevel _level = LogLevel::Info;
};


// -- Macros -----------------------------------------------------------------

#define OCS_LOG(level, cat, ...) \
    ::opencs::Log::instance().log((level), (cat), __VA_ARGS__)

#define OCS_LOG_TRACE(cat, ...) OCS_LOG(::opencs::LogLevel::Trace, (cat), __VA_ARGS__)
#define OCS_LOG_DEBUG(cat, ...) OCS_LOG(::opencs::LogLevel::Debug, (cat), __VA_ARGS__)
#define OCS_LOG_INFO(cat, ...)  OCS_LOG(::opencs::LogLevel::Info,  (cat), __VA_ARGS__)
#define OCS_LOG_WARN(cat, ...)  OCS_LOG(::opencs::LogLevel::Warn,  (cat), __VA_ARGS__)
#define OCS_LOG_ERROR(cat, ...) OCS_LOG(::opencs::LogLevel::Error, (cat), __VA_ARGS__)


// -- Built-in sinks ---------------------------------------------------------

/// Formats `[time level category] message\n` to stderr. Thread-safe via
/// the std lib's FILE* locking. The default sink if no other is attached.
class StdoutSink : public ILogSink
{
public:
    void write(const LogRecord& r) override;
};

/// Fixed-capacity ring buffer. Latest `capacity` records available via
/// `snapshot()`. Useful as the backing store for a Log panel or for
/// "print last 200 lines" debug commands.
class RingSink : public ILogSink
{
public:
    explicit RingSink(std::size_t capacity = 1024);
    void write(const LogRecord& r) override;
    std::vector<LogRecord> snapshot() const;
    void clear();

private:
    mutable std::mutex  _mu;
    std::deque<LogRecord> _ring;
    std::size_t           _cap;
};

/// Appends records to a file, flushing each line. Safe across crashes.
/// Path defaults to `~/.walshard.log` unless overridden.
class FileSink : public ILogSink
{
public:
    explicit FileSink(std::string path = {});
    ~FileSink() override;
    void write(const LogRecord& r) override;

private:
    std::mutex _mu;
    void*      _fp = nullptr;   // FILE* (void* to avoid stdio.h here)
    std::string _path;
};


/// Cross-process message queue. Every OCS process (main + each
/// detached panel) appends its log records to its own
/// `<ipcDir>/<pid>.messages.log` file. The Messages panel — whether
/// docked in the main app or running in a detached child — reads
/// ALL `*.messages.log` files in the IPC dir, merges by timestamp,
/// and renders the tail. Each process owns its own file so writes
/// never need locking; reads are best-effort with stale-pid GC.
///
/// Format on disk: one record per line, fields separated by `|`:
///   <unix-time-secs>|<level-int>|<pid>|<category>|<message>
/// Multi-line messages are flattened with `\\n` so each row stays
/// one line. Older entries are truncated lazily when the file
/// passes ~1 MB.
class IpcLogSink : public ILogSink
{
public:
    /// Path defaults to `<ipcDir>/<pid>.messages.log` for the
    /// current process. Pass an explicit path to override (used
    /// when restoring from settings or testing).
    explicit IpcLogSink(std::string path = {});
    ~IpcLogSink() override;
    void write(const LogRecord& r) override;

private:
    std::mutex  _mu;
    void*       _fp = nullptr;
    std::string _path;
};

/// Read every `<ipcDir>/*.messages.log` file, parse the records,
/// merge by timestamp, return the newest `tailMax` entries.
/// Robust to truncated tail lines (skipped) and missing files
/// (ignored). Used by the Messages panel — same call works in the
/// main process AND in a detached panel process so both render
/// the same shared queue.
std::vector<LogRecord> tailIpcLogs(std::size_t tailMax = 1024);

/// Cross-process log-level coordinator. Writes the chosen level to
/// `~/.walshard.loglevel` so every OCS process picks it up —
/// without it, dropping the level in one Messages panel only
/// affects that panel's own process and you'd miss events from peer
/// windows. The home-dir file also persists across sessions so the
/// next launch starts at the same level.
void publishSharedLogLevel(LogLevel lv);
/// Poll the shared file and apply it to this process's Log if it
/// has changed since the last poll. Cheap (a single fopen per
/// call); call once per frame from the render loop.
void pollSharedLogLevel();

/// Set the per-process label that the Messages panel shows next
/// to each log record so the user can tell which window emitted
/// it. The main app passes "main"; detached panels pass their
/// panel name (e.g. "Messages", "Properties"). Empty / unset
/// falls back to a "pid:<n>" tag.
void setWindowLabel(std::string label);
/// Get the current window label. Returns "pid:<n>" if unset.
std::string windowLabel();

/// Tiny key=value prefs file at `~/.walshard.uiprefs`.
/// Used by the Messages panel to persist its pane height + auto-
/// scroll flag across sessions. Cheap (one fopen per get/set);
/// the panel calls `getUiPrefF` / `setUiPrefF` lazily.
float       getUiPrefF(const char* key, float dflt);
void        setUiPrefF(const char* key, float value);
bool        getUiPrefBool(const char* key, bool dflt);
void        setUiPrefBool(const char* key, bool value);


// -- Utilities --------------------------------------------------------------

/// Return a short string for a level (`"INFO "`, `"WARN "`, …).
const char* levelName(LogLevel lv);

/// Verbosity for `dumpSceneGraph`. Pick the smallest one that answers
/// your current question — bounding boxes are cheap to scan, visual
/// props already stretch across a wide terminal, and Full is verbose
/// enough to drown console pipes.
enum class GraphDumpVerbosity
{
    Bounds = 0,   ///< name + indent + world AABB
    Visual,       ///< + colour / opacity / first image path
    Full          ///< + anchor / scale / rotation / z / flags / address
};

/// Walk the axmol node subtree rooted at `root` and emit one log line
/// per node. The verbosity knob controls how much detail per node is
/// emitted — default is the cheapest, "just the boxes" view.
void dumpSceneGraph(ax::Node* root,
                    LogLevel level = LogLevel::Debug,
                    GraphDumpVerbosity verbosity = GraphDumpVerbosity::Bounds);

/// Install the default stdout + ring sinks. Called at app startup.
/// Safe to call multiple times — re-adds missing sinks only.
void initDefaultLogging();

/// Accessor for the process-wide ring sink so the inspect server and
/// the planned ImGui Log panel can both tail the same buffer.
std::shared_ptr<RingSink> defaultRingSink();

}  // namespace opencs
