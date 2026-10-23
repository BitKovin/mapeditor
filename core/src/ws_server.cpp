#include "core/ws_server.h"
#include "core/origin.h"
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <deque>
#include <iostream>
#include <unordered_map>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace core {

namespace {
// Whole documents travel in one frame (DESIGN.md §10); Beast's 16 MB default
// would cap map size. 256 MB is "large map", not "unbounded".
constexpr std::size_t kMaxMessageBytes = 256u * 1024u * 1024u;
}  // namespace

struct WsServer::Impl {
    asio::io_context ioc{1};
    tcp::acceptor acceptor;
    uint16_t port;
    OnConnect onConnect;
    OnMessage onMessage;
    OnDisconnect onDisconnect;
    // Only sessions that completed the WebSocket handshake are in here.
    std::unordered_map<uint64_t, std::shared_ptr<WsSession>> sessions;
    uint64_t nextSessionId = 1;
    struct Tick {
        explicit Tick(asio::io_context& c) : timer(c) {}
        asio::steady_timer timer;
        std::chrono::milliseconds interval{0};
        std::function<void()> run;
        std::function<bool()> fn;  // true: more work is pending, run again without waiting for the interval
    };
    std::vector<std::unique_ptr<Tick>> ticks;

    Impl(uint16_t p, OnConnect c, OnMessage m, OnDisconnect d)
        : acceptor(ioc),
          port(p),
          onConnect(std::move(c)),
          onMessage(std::move(m)),
          onDisconnect(std::move(d)) {
        // The port is ours alone. Asio's default SO_REUSEADDR lets a second process listen on the same port on Windows, and a Shell then
        // reaches whichever Core Windows picks (an old build left running took the connections of a fresh one).
        const tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), p);
        acceptor.open(ep.protocol());
#ifdef _WIN32
        BOOL exclusive = TRUE;
        ::setsockopt(acceptor.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
#else
        acceptor.set_option(tcp::acceptor::reuse_address(true));  // quick restarts (TIME_WAIT); Linux never shares a listening port
#endif
        acceptor.bind(ep);
        acceptor.listen();
    }

    void doAccept();
    void scheduleTick(Tick& t);
    void scheduleTickNow(Tick& t);
    void registerSession(const std::shared_ptr<WsSession>& s);
    void handleMessage(uint64_t id, const std::string& text);
    void handleClosed(uint64_t id);
};

class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    WsSession(tcp::socket socket, uint64_t id, WsServer::Impl& server)
        : ws_(std::move(socket)), id_(id), server_(server) {}

    uint64_t id() const { return id_; }

    void start() {
        http::async_read(beast::get_lowest_layer(ws_), httpBuffer_, request_,
                          [self = shared_from_this()](beast::error_code ec, std::size_t) { self->onHttpRead(ec); });
    }

    // Queues behind any write in flight; safe to call from inside a handler.
    // `done` (optional) is released once the frame was written or the session is gone: its deleter tells the sender that this client took the message.
    void send(const std::string& text, std::shared_ptr<void> done = nullptr) {
        const bool idle = writeQueue_.empty();
        writeQueue_.push_back({text, std::move(done)});
        if (idle) doWrite();
    }

private:
    void onHttpRead(beast::error_code ec) {
        if (ec) return fail(ec, "http read");
        if (!websocket::is_upgrade(request_)) return reject(http::status::upgrade_required, "websocket upgrade required");

        const std::string origin(request_[http::field::origin]);
        if (!isLocalOrigin(origin)) {
            std::cerr << "refused connection from origin '" << origin << "'" << std::endl;
            return reject(http::status::forbidden, "origin not allowed");
        }

        ws_.read_message_max(kMaxMessageBytes);
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& res) { res.set(http::field::server, "mapeditor-core"); }));
        ws_.async_accept(request_, [self = shared_from_this()](beast::error_code e) { self->onAccept(e); });
    }

    void reject(http::status status, const char* why) {
        auto res = std::make_shared<http::response<http::string_body>>(status, request_.version());
        res->set(http::field::server, "mapeditor-core");
        res->set(http::field::content_type, "text/plain");
        res->keep_alive(false);
        res->body() = why;
        res->prepare_payload();
        http::async_write(beast::get_lowest_layer(ws_), *res,
                           [self = shared_from_this(), res](beast::error_code, std::size_t) {
                               beast::error_code ignored;
                               beast::get_lowest_layer(self->ws_).shutdown(tcp::socket::shutdown_send, ignored);
                           });
    }

    void onAccept(beast::error_code ec) {
        if (ec) return fail(ec, "accept");
        server_.registerSession(shared_from_this());
        doRead();
    }

    void doRead() {
        ws_.async_read(readBuffer_, [self = shared_from_this()](beast::error_code ec, std::size_t) { self->onRead(ec); });
    }

    void onRead(beast::error_code ec) {
        if (ec) {
            if (ec != websocket::error::closed && ec != asio::error::eof && ec != asio::error::connection_reset)
                fail(ec, "read");
            server_.handleClosed(id_);
            return;
        }
        const bool isText = ws_.got_text();
        const std::string msg = beast::buffers_to_string(readBuffer_.data());
        readBuffer_.consume(readBuffer_.size());
        if (isText) server_.handleMessage(id_, msg);
        doRead();
    }

    void doWrite() {
        ws_.text(true);
        ws_.async_write(asio::buffer(writeQueue_.front().text),
                         [self = shared_from_this()](beast::error_code ec, std::size_t) { self->onWrite(ec); });
    }

    void onWrite(beast::error_code ec) {
        writeQueue_.pop_front();
        if (ec) {
            fail(ec, "write");
            server_.handleClosed(id_);  // don't let a dead peer's queue grow forever
            return;
        }
        if (!writeQueue_.empty()) doWrite();
    }

    void fail(beast::error_code ec, const char* what) {
        std::cerr << "session " << id_ << " " << what << " error: " << ec.message() << std::endl;
    }

    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer httpBuffer_;
    http::request<http::string_body> request_;
    beast::flat_buffer readBuffer_;
    uint64_t id_;
    WsServer::Impl& server_;
    struct Outgoing { std::string text; std::shared_ptr<void> done; };
    std::deque<Outgoing> writeQueue_;
};

void WsServer::Impl::doAccept() {
    acceptor.async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (!ec) {
            std::make_shared<WsSession>(std::move(socket), nextSessionId++, *this)->start();
        } else {
            std::cerr << "accept error: " << ec.message() << std::endl;
        }
        doAccept();
    });
}

void WsServer::Impl::scheduleTick(Tick& t) {
    if (!t.fn || t.interval.count() <= 0) return;
    if (!t.run) t.run = [this, &t] {  // built once: it re-arms itself, so it must not be replaced while it runs
        bool again = false;
        try {
            again = t.fn();
        } catch (const std::exception& e) {
            std::cerr << "tick error: " << e.what() << std::endl;
        }
        if (again) {
            // Busy: queue the next run behind whatever I/O is already waiting instead of idling for a whole interval.
            asio::post(ioc, [this, &t] { scheduleTickNow(t); });
        } else {
            scheduleTick(t);
        }
    };
    t.timer.expires_after(t.interval);
    t.timer.async_wait([&t](beast::error_code ec) {
        if (ec) return;
        t.run();
    });
}

void WsServer::Impl::scheduleTickNow(Tick& t) { t.run(); }

void WsServer::Impl::registerSession(const std::shared_ptr<WsSession>& s) {
    sessions[s->id()] = s;
    std::cout << "shell " << s->id() << " connected" << std::endl;
    if (onConnect) onConnect(s->id());
}

void WsServer::Impl::handleMessage(uint64_t id, const std::string& text) {
    if (onMessage) onMessage(id, text);
}

void WsServer::Impl::handleClosed(uint64_t id) {
    if (sessions.erase(id) == 0) return;  // already gone (read and write can both fail)
    std::cout << "shell " << id << " disconnected" << std::endl;
    if (onDisconnect) onDisconnect(id);
}

WsServer::WsServer(uint16_t port, OnConnect onConnect, OnMessage onMessage, OnDisconnect onDisconnect)
    : impl_(std::make_unique<Impl>(port, std::move(onConnect), std::move(onMessage), std::move(onDisconnect))) {}

WsServer::~WsServer() = default;

void WsServer::run() {
    std::cout << "mapeditor_core listening on ws://127.0.0.1:" << impl_->port << std::endl;
    impl_->doAccept();
    for (auto& t : impl_->ticks) impl_->scheduleTick(*t);
    impl_->ioc.run();
}

void WsServer::setTick(std::chrono::milliseconds interval, std::function<void()> fn) {
    setWorkTick(interval, [f = std::move(fn)] { f(); return false; });
}

void WsServer::setWorkTick(std::chrono::milliseconds interval, std::function<bool()> fn) {
    auto t = std::make_unique<Impl::Tick>(impl_->ioc);
    t->interval = interval;
    t->fn = std::move(fn);
    impl_->ticks.push_back(std::move(t));
}

void WsServer::post(std::function<void()> fn) { asio::post(impl_->ioc, std::move(fn)); }

void WsServer::sendTo(uint64_t sessionId, const std::string& text) {
    auto it = impl_->sessions.find(sessionId);
    if (it != impl_->sessions.end()) it->second->send(text);
}

void WsServer::broadcast(const std::string& text, std::shared_ptr<void> done) {
    for (auto& [id, session] : impl_->sessions) session->send(text, done);
}

void WsServer::broadcastExcept(uint64_t exceptSessionId, const std::string& text) {
    for (auto& [id, session] : impl_->sessions) {
        if (id != exceptSessionId) session->send(text);
    }
}

}  // namespace core
