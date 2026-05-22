#include "UILayoutEditor/InspectServer.h"

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <winsock2.h>
#    include <ws2tcpip.h>
// Link the Winsock library implicitly so callers don't have to remember.
#    pragma comment(lib, "ws2_32.lib")
using ssize_t = SSIZE_T;
#    define VS_CLOSE_SOCK(fd) ::closesocket(fd)
#    define VS_SHUT_RDWR      SD_BOTH
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <sys/types.h>
#    include <unistd.h>
#    define VS_CLOSE_SOCK(fd) ::close(fd)
#    define VS_SHUT_RDWR      SHUT_RDWR
#endif

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <future>

namespace opencs
{

struct InspectServer::Cmd
{
    std::string                                   line;
    // shared_ptr so the promise can outlive the Cmd: a deferred
    // handler steals it via `InspectReply::defer()` into a
    // `PendingReply` that lives in editor-side state across frames.
    std::shared_ptr<std::promise<std::string>>    reply;
};

InspectServer::~InspectServer() { stop(); }

bool InspectServer::start(int port)
{
    if (_running.load()) return true;

#if defined(_WIN32)
    // One-shot Winsock init. WSAStartup is refcounted; pair with
    // WSACleanup in stop().
    WSADATA wsaData;
    if (::WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::perror("[inspect] WSAStartup");
        return false;
    }
#endif

    _listenFd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (_listenFd < 0) { std::perror("[inspect] socket"); return false; }

    int yes = 1;
    ::setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        std::perror("[inspect] bind");
        VS_CLOSE_SOCK(_listenFd); _listenFd = -1; return false;
    }
    if (::listen(_listenFd, 4) < 0)
    {
        std::perror("[inspect] listen");
        VS_CLOSE_SOCK(_listenFd); _listenFd = -1; return false;
    }

#if !defined(_WIN32)
    // Ignore SIGPIPE — if the peer hangs up while we're mid-write,
    // the write should return EPIPE instead of killing the process.
    // Windows has no SIGPIPE; Winsock returns WSAECONNRESET instead,
    // which we already tolerate via the error-return check in writeAll.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    _port = port;
    _running = true;
    _thread = std::thread(&InspectServer::runAccept, this);
    std::fprintf(stderr, "[inspect] listening on 127.0.0.1:%d\n", port);
    return true;
}

void InspectServer::stop()
{
    if (!_running.exchange(false)) return;
    if (_listenFd >= 0) { ::shutdown(_listenFd, VS_SHUT_RDWR); VS_CLOSE_SOCK(_listenFd); _listenFd = -1; }
    if (_thread.joinable()) _thread.join();

    // Wake per-connection workers by shutting down their sockets. Each
    // worker's readLine returns EOF, the conn loop exits, the thread
    // ends — then we join them under _connsMu.
    {
        std::lock_guard<std::mutex> lk(_subMu);
        for (int fd : _subscribers) { ::shutdown(fd, VS_SHUT_RDWR); }
        _subscribers.clear();
    }
    std::vector<std::thread> joined;
    {
        std::lock_guard<std::mutex> lk(_connsMu);
        joined.swap(_conns);
    }
    for (auto& t : joined) if (t.joinable()) t.join();

    // Fail any in-flight commands so the main thread's drain doesn't block.
    {
        std::lock_guard<std::mutex> lk(_mu);
        while (!_queue.empty())
        {
            auto& c = _queue.front();
            if (c->reply)
            { try { c->reply->set_value("ERR: server stopped\n"); } catch (...) {} }
            _queue.pop_front();
        }
    }

#if defined(_WIN32)
    // Pair the WSAStartup from start(). Refcounted, so multiple
    // start/stop cycles are safe.
    ::WSACleanup();
#endif
}

/// Read one '\n'-terminated line (max 4KB). Returns empty on EOF / error.
static std::string readLine(int fd)
{
    std::string out;
    out.reserve(128);
    char ch = 0;
    while (out.size() < 4096)
    {
        ssize_t n = ::recv(fd, &ch, 1, 0);
        if (n <= 0) return {};
        if (ch == '\n') break;
        if (ch != '\r') out.push_back(ch);
    }
    return out;
}

static bool writeAll(int fd, const std::string& s)
{
    const char* p = s.data();
    size_t left = s.size();
    while (left > 0)
    {
        ssize_t n = ::send(fd, p, left, 0);
        if (n <= 0) return false;
        p += n; left -= static_cast<size_t>(n);
    }
    return true;
}

void InspectServer::runAccept()
{
    while (_running.load())
    {
        sockaddr_in peer{};
        socklen_t   sz = sizeof(peer);
        int fd = ::accept(_listenFd,
                          reinterpret_cast<sockaddr*>(&peer), &sz);
        if (fd < 0)
        {
            if (_running.load())
                std::perror("[inspect] accept");
            break;
        }

        // Per-connection worker. Multiple clients can hold sessions in
        // parallel so an event subscriber doesn't block command-only
        // clients (and vice versa).
        std::lock_guard<std::mutex> lk(_connsMu);
        _conns.emplace_back(&InspectServer::runConn, this, fd);
    }
}

void InspectServer::runConn(int fd)
{
    // One command per line, one reply per line. Two side-channel
    // commands — `watch on` / `watch off` — toggle event subscription
    // and are handled inline (no main-thread roundtrip). Everything
    // else is queued for `drain` to invoke under the editor lock.
    while (_running.load())
    {
        std::string line = readLine(fd);
        if (line.empty()) break;

        if (line == "watch on")
        {
            { std::lock_guard<std::mutex> lk(_subMu); _subscribers.insert(fd); }
            std::lock_guard<std::mutex> lk(_writeMu);
            if (!writeAll(fd, "ok subscribed\n")) break;
            continue;
        }
        if (line == "watch off")
        {
            { std::lock_guard<std::mutex> lk(_subMu); _subscribers.erase(fd); }
            std::lock_guard<std::mutex> lk(_writeMu);
            if (!writeAll(fd, "ok unsubscribed\n")) break;
            continue;
        }

        auto cmd = std::make_shared<Cmd>();
        cmd->line  = std::move(line);
        cmd->reply = std::make_shared<std::promise<std::string>>();
        std::future<std::string> fut = cmd->reply->get_future();

        { std::lock_guard<std::mutex> lk(_mu); _queue.push_back(cmd); }

        std::string reply;
        try { reply = fut.get(); }
        catch (...) { reply = "ERR: reply failure\n"; }

        if (!reply.empty() && reply.back() != '\n') reply.push_back('\n');
        {
            std::lock_guard<std::mutex> lk(_writeMu);
            if (!writeAll(fd, reply)) break;
        }
    }
    // Drop subscription + close. publishEvent will no longer hit this fd.
    { std::lock_guard<std::mutex> lk(_subMu); _subscribers.erase(fd); }
    VS_CLOSE_SOCK(fd);
}

void InspectServer::drain(const Handler& h)
{
    std::deque<std::shared_ptr<Cmd>> local;
    { std::lock_guard<std::mutex> lk(_mu); local.swap(_queue); }
    for (auto& c : local)
    {
        InspectReply reply(c->reply);
        try { h(c->line, reply); }
        catch (const std::exception& e)
        {
            if (!reply.delivered())
                reply.send(std::string("ERR: ") + e.what());
        }
        catch (...)
        {
            if (!reply.delivered()) reply.send("ERR: unknown");
        }
        // If the handler neither sent nor deferred, the worker thread
        // would hang on its future forever — fall back to an empty ok.
        if (!reply.delivered()) reply.send("");
    }
}

void InspectServer::publishEvent(const std::string& line)
{
    if (!_running.load()) return;
    // Snapshot subscriber set under _subMu so the fan-out below
    // doesn't hold the membership lock while blocking on send().
    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> lk(_subMu);
        fds.assign(_subscribers.begin(), _subscribers.end());
    }
    if (fds.empty()) return;

    std::string framed = "EVENT ";
    framed += line;
    if (framed.empty() || framed.back() != '\n') framed.push_back('\n');

    std::vector<int> dead;
    for (int fd : fds)
    {
        std::lock_guard<std::mutex> lk(_writeMu);
        if (!writeAll(fd, framed)) dead.push_back(fd);
    }
    if (!dead.empty())
    {
        std::lock_guard<std::mutex> lk(_subMu);
        for (int fd : dead) _subscribers.erase(fd);
    }
}

}  // namespace opencs
