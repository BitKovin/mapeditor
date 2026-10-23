#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace core {

class WsSession;  // defined in ws_server.cpp; needs friend access to Impl

// Async, multi-session local WebSocket server. Sessions are separate browser
// tabs / tools connected to the same Core (DESIGN.md §3/§10).
//
// Single-threaded by design: one io_context, run() on one thread, so every
// callback and every send/broadcast made from inside one executes serially --
// no locking. If this ever gets IO threads, the session registry needs a mutex.
//
// Only connections from local origins are accepted (see origin.h).
class WsServer {
public:
    using OnConnect = std::function<void(uint64_t sessionId)>;
    // Called for every text frame. Replies go through sendTo()/broadcast*().
    using OnMessage = std::function<void(uint64_t sessionId, const std::string& text)>;
    using OnDisconnect = std::function<void(uint64_t sessionId)>;

    WsServer(uint16_t port, OnConnect onConnect, OnMessage onMessage, OnDisconnect onDisconnect);
    ~WsServer();

    // Runs `fn` on the server thread every `interval` once run() starts (used to notice files changed
    // on disk, and to refine lighting). May be called several times: every call adds an independent tick.
    // Call before run().
    void setTick(std::chrono::milliseconds interval, std::function<void()> fn);
    // Like setTick, but `fn` returns true while it has more work: it then runs again right after the pending I/O instead of after
    // `interval` (used by the live lighting, which must not idle between slices).
    void setWorkTick(std::chrono::milliseconds interval, std::function<bool()> fn);

    // Blocks, serving until the process is killed.
    void run();

    // Thread safe: runs `fn` on the server thread (the only one that may call sendTo / broadcast*). How other threads (the bake thread) deliver messages.
    void post(std::function<void()> fn);

    void sendTo(uint64_t sessionId, const std::string& text);
    // `done` (optional) is released when every client has been sent this message (or has gone): a null shared_ptr with a deleter makes a completion callback.
    void broadcast(const std::string& text, std::shared_ptr<void> done = nullptr);
    void broadcastExcept(uint64_t exceptSessionId, const std::string& text);

private:
    friend class WsSession;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace core
