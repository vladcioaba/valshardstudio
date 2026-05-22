// Dev-only localhost TCP server for headless verification. The running app
// listens on 127.0.0.1:<port>; a shell script can `nc 127.0.0.1 <port>` and
// send newline-terminated commands ("graph", "click 120 340", "mode run",
// "dump", "select 0,2,1") to introspect or drive the editor.
//
// Commands must execute on the main (ImGui/axmol) thread. The server thread
// only parks socket I/O and the editor's `drain()` call pops queued commands
// once per frame, runs them synchronously, and hands replies back to the
// waiting worker via a promise.
//
// Not for production. The protocol is plain text and the server accepts from
// loopback only.

#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace opencs
{

/// Token returned by `InspectReply::defer` — wraps the promise the
/// inspect-server worker is blocked on. The handler stashes the token
/// somewhere editor-side and later calls `send(reply)` once the
/// async work (screenshot, multi-frame preview, ...) is done. The
/// connection thread wakes up immediately and ships the reply.
class PendingReply
{
public:
    PendingReply() = default;
    PendingReply(PendingReply&&) = default;
    PendingReply& operator=(PendingReply&&) = default;
    PendingReply(const PendingReply&) = delete;
    PendingReply& operator=(const PendingReply&) = delete;

    bool valid() const { return static_cast<bool>(_p); }
    void send(std::string s)
    {
        if (!_p) return;
        try { _p->set_value(std::move(s)); } catch (...) {}
        _p.reset();
    }

private:
    explicit PendingReply(std::shared_ptr<std::promise<std::string>> p)
        : _p(std::move(p)) {}
    std::shared_ptr<std::promise<std::string>> _p;
    friend class InspectReply;
};

/// Handed to each handler by `drain`. The handler either replies
/// synchronously (`send`) — the common case — or steals the promise
/// via `defer()` to fulfil it on a later frame. Either action sets
/// `delivered()` true; the drain loop uses that to decide whether to
/// fill in a fallback reply when the handler did neither.
class InspectReply
{
public:
    explicit InspectReply(std::shared_ptr<std::promise<std::string>> p)
        : _p(std::move(p)) {}
    InspectReply(const InspectReply&) = delete;
    InspectReply& operator=(const InspectReply&) = delete;

    void send(std::string s)
    {
        if (!_p) return;
        try { _p->set_value(std::move(s)); } catch (...) {}
        _p.reset();
        _delivered = true;
    }
    PendingReply defer()
    {
        _delivered = true;
        return PendingReply(std::move(_p));
    }
    bool delivered() const { return _delivered; }

private:
    std::shared_ptr<std::promise<std::string>> _p;
    bool _delivered = false;
};

class InspectServer
{
public:
    using Handler = std::function<void(const std::string& line,
                                       InspectReply& reply)>;

    ~InspectServer();

    /// Launch the accept loop on a background thread. Idempotent.
    bool start(int port);

    /// Close the socket, signal shutdown, join the thread.
    void stop();

    /// Pop all queued commands and invoke `h` on each on the calling thread.
    /// Must be called from the main thread every frame.
    void drain(const Handler& h);

    /// Push an unsolicited event line to every subscriber. Lines are
    /// emitted with an `EVENT ` prefix and a trailing newline so a
    /// client can filter them from regular command replies. Safe to
    /// call from the main thread; no-op when no subscribers / server
    /// not started. Dead sockets are evicted on send failure.
    void publishEvent(const std::string& line);

    int port() const { return _port; }

private:
    struct Cmd;

    void runAccept();
    void runConn(int fd);

    std::thread       _thread;
    std::atomic<bool> _running{false};
    int               _listenFd = -1;
    int               _port = 0;
    std::mutex        _mu;
    std::deque<std::shared_ptr<Cmd>> _queue;

    // Per-conn worker threads spawned by `runAccept`. Joined on stop().
    std::mutex                _connsMu;
    std::vector<std::thread>  _conns;

    // Subscribers (file descriptors flagged for event fan-out).
    // `_subMu` guards membership; `_writeMu` serialises every send()
    // across reply + event paths so a publishEvent + reply on the same
    // fd cannot interleave bytes mid-frame.
    std::mutex                _subMu;
    std::set<int>             _subscribers;
    std::mutex                _writeMu;
};

}  // namespace opencs
