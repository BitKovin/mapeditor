#include "automation/server.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QHostAddress>
#include <QImage>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cmath>
#include <deque>

#include "base/log.h"
#include "doc/document.h"
#include "doc/io.h"
#include "render/viewport_settings.h"
#include "render/viewport_window.h"
#include "ui/main_window.h"

namespace native::automation {

using json = nlohmann::json;

namespace {

constexpr qsizetype kMaxLine = 256 * 1024 * 1024;   // a map as one line

json failure(const std::string& message) { return json{{"ok", false}, {"error", message}}; }

bool readVec3(const json& v, double out[3]) {
    if (!v.is_array() || v.size() != 3) return false;
    for (int i = 0; i < 3; ++i) {
        if (!v[i].is_number()) return false;
        out[i] = v[i].get<double>();
        if (!std::isfinite(out[i])) return false;
    }
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

template <class E>
bool enumByName(const std::string& name, int count, const char* (*nameOf)(E), E& out) {
    for (int i = 0; i < count; ++i)
        if (lower(nameOf(static_cast<E>(i))) == lower(name)) {
            out = static_cast<E>(i);
            return true;
        }
    return false;
}

json documentInfo(const ui::MainWindow& w) {
    const auto d = w.document();
    if (!d) return json{{"open", false}};
    return json{{"open", true}, {"path", d->path}, {"dirty", w.isDirty()}, {"entities", d->entities.size()}, {"instances", d->instances.size()}, {"brushes", d->brushCount()}};
}

} // namespace

struct Server::Outcome {
    json reply;                                  // answered at once unless `ready` is set
    std::function<bool()> ready;                 // the editor has finished what was asked
    std::function<json(bool timedOut)> finish;   // the reply, when ready or after timeoutMs
    int timeoutMs = 0;
};

struct Server::Waiting {
    Outcome outcome;
    QElapsedTimer clock;
    std::function<void(json)> respond;
};

struct Server::Client {
    QPointer<QTcpSocket> socket;
    QByteArray buffer;
    std::deque<QByteArray> lines;
    bool busy = false, inPump = false, alive = true;
};

Server::Server(ui::MainWindow& window, QObject* parent) : QObject(parent), window_(window) {
    tcp_ = new QTcpServer(this);
    QObject::connect(tcp_, &QTcpServer::newConnection, this, [this] { onConnection(); });
    timer_ = new QTimer(this);
    timer_->setInterval(20);
    QObject::connect(timer_, &QTimer::timeout, this, [this] { poll(); });
    window_.setErrorHandler([this](const QString& m) { errors_.push_back(m.toUtf8().toStdString()); });
    window_.setConfirmHandler([](const QString&, const QString&) { return true; });
}

Server::~Server() {
    window_.setErrorHandler(nullptr);
    window_.setConfirmHandler(nullptr);
}

bool Server::listen(int port) {
    if (!tcp_->listen(QHostAddress::LocalHost, static_cast<quint16>(port))) {
        error_ = tcp_->errorString().toUtf8().toStdString();
        return false;
    }
    base::logInfo("automation: listening on 127.0.0.1:" + std::to_string(tcp_->serverPort()));
    return true;
}

int Server::port() const { return tcp_->isListening() ? tcp_->serverPort() : 0; }

void Server::onConnection() {
    while (QTcpSocket* socket = tcp_->nextPendingConnection()) {
        auto client = std::make_shared<Client>();
        client->socket = socket;
        clients_.push_back(client);
        QObject::connect(socket, &QTcpSocket::readyRead, this, [this, weak = std::weak_ptr<Client>(client)] {
            const auto c = weak.lock();
            if (!c || !c->socket) return;
            c->buffer += c->socket->readAll();
            for (qsizetype nl; (nl = c->buffer.indexOf('\n')) >= 0;) {
                const QByteArray line = c->buffer.left(nl).trimmed();
                c->buffer.remove(0, nl + 1);
                if (!line.isEmpty()) c->lines.push_back(line);
            }
            if (c->buffer.size() > kMaxLine) {
                c->socket->disconnectFromHost();
                return;
            }
            pump(*c);
        });
        QObject::connect(socket, &QTcpSocket::disconnected, this, [this, weak = std::weak_ptr<Client>(client), socket] {
            if (const auto c = weak.lock()) c->alive = false;
            std::erase_if(clients_, [](const std::shared_ptr<Client>& c) { return !c->alive; });
            socket->deleteLater();
        });
    }
}

void Server::pump(Client& client) {
    if (client.inPump) return;
    client.inPump = true;
    while (!client.busy && !client.lines.empty() && client.socket) {
        const QByteArray line = client.lines.front();
        client.lines.pop_front();
        json request;
        try {
            request = json::parse(line.constData(), line.constData() + line.size());
        } catch (const std::exception& e) {
            client.socket->write(QByteArray::fromStdString(failure(std::string("not JSON: ") + e.what()).dump() + "\n"));
            continue;
        }
        client.busy = true;
        // The client outlives the request unless it disconnects: then the answer is dropped.
        std::shared_ptr<Client> self;
        for (const auto& c : clients_)
            if (c.get() == &client) self = c;
        dispatch(request, [this, weak = std::weak_ptr<Client>(self)](json reply) {
            const auto c = weak.lock();
            if (!c) return;
            c->busy = false;
            if (c->socket && c->alive) c->socket->write(QByteArray::fromStdString(reply.dump() + "\n"));
            pump(*c);
        });
    }
    client.inPump = false;
}

void Server::dispatch(const json& request, std::function<void(json)> respond) {
    json id;
    const bool hasId = request.is_object() && request.contains("id");
    if (hasId) id = request["id"];
    auto withId = [id, hasId](json reply) {
        if (hasId) reply["id"] = id;
        return reply;
    };
    Outcome o;
    try {
        errors_.clear();
        o = run(request);
    } catch (const std::exception& e) {
        o = Outcome{failure(std::string("internal error: ") + e.what()), nullptr, nullptr, 0};
    }
    if (!o.ready) {
        respond(withId(std::move(o.reply)));
        return;
    }
    auto w = std::make_shared<Waiting>();
    w->outcome = std::move(o);
    w->clock.start();
    w->respond = [respond = std::move(respond), withId](json reply) { respond(withId(std::move(reply))); };
    waiting_.push_back(std::move(w));
    timer_->start();
}

void Server::poll() {
    // Answering may start new requests: work on a copy.
    std::vector<std::shared_ptr<Waiting>> list;
    list.swap(waiting_);
    std::vector<std::shared_ptr<Waiting>> keep;
    for (auto& w : list) {
        bool timedOut = false;
        const bool ready = w->outcome.ready();
        if (!ready) timedOut = w->clock.elapsed() >= w->outcome.timeoutMs;
        if (!ready && !timedOut) {
            keep.push_back(w);
            continue;
        }
        json reply;
        try {
            reply = w->outcome.finish(timedOut);
        } catch (const std::exception& e) {
            reply = failure(std::string("internal error: ") + e.what());
        }
        w->respond(std::move(reply));
    }
    for (auto& w : keep) waiting_.push_back(std::move(w));
    if (waiting_.empty()) timer_->stop();
}

Server::Outcome Server::run(const json& r) {
    if (!r.is_object() || !r.contains("cmd") || !r["cmd"].is_string()) return Outcome{failure("a request is an object with a \"cmd\""), nullptr, nullptr, 0};
    const std::string cmd = r["cmd"].get<std::string>();
    if (cmd == "open") return cmdOpen(r);
    if (cmd == "save") return cmdSave(r);
    if (cmd == "get_document") return cmdGetDocument();
    if (cmd == "set_document") return cmdSetDocument(r);
    if (cmd == "camera") return cmdCamera(r);
    if (cmd == "settings") return cmdSettings(r);
    if (cmd == "screenshot") return cmdScreenshot(r);
    if (cmd == "stats") return cmdStats();
    if (cmd == "lighting") return cmdLighting(r);
    if (cmd == "help") return cmdHelp();
    return Outcome{failure("unknown command \"" + cmd + "\" (try help)"), nullptr, nullptr, 0};
}

Server::Outcome Server::cmdHelp() {
    Outcome o;
    o.reply = json{{"ok", true},
                   {"commands", json::array({"open {path}", "save {path?}", "get_document", "set_document {doc, label?}", "camera {viewport?, eye, target, fov?}",
                                             "settings {viewport?, key, value}", "screenshot {viewport?, path, timeoutMs?}", "stats", "lighting {action: wait_converged, timeoutMs?}", "help"})}};
    return o;
}

Server::Outcome Server::cmdOpen(const json& r) {
    Outcome o;
    if (!r.contains("path") || !r["path"].is_string()) {
        o.reply = failure("open needs \"path\" (relative to the assets folder)");
        return o;
    }
    if (window_.busy()) {
        o.reply = failure("busy: a load or save is running");
        return o;
    }
    window_.openDocument(r["path"].get<std::string>());
    o.timeoutMs = r.value("timeoutMs", 120000);
    o.ready = [this] { return !window_.busy(); };
    o.finish = [this](bool timedOut) {
        if (timedOut) return failure("timeout while loading");
        if (!errors_.empty()) return failure(errors_.front());
        json reply = documentInfo(window_);
        reply["ok"] = true;
        return reply;
    };
    return o;
}

Server::Outcome Server::cmdSave(const json& r) {
    Outcome o;
    if (!window_.document()) {
        o.reply = failure("no map is open");
        return o;
    }
    if (window_.busy()) {
        o.reply = failure("busy: a load or save is running");
        return o;
    }
    if (r.contains("path") && r["path"].is_string()) {
        window_.saveDocumentAs(r["path"].get<std::string>());
    } else if (window_.document()->path.empty()) {
        o.reply = failure("the map has no path yet: save needs \"path\"");
        return o;
    } else {
        window_.saveDocument();
    }
    o.timeoutMs = r.value("timeoutMs", 120000);
    o.ready = [this] { return !window_.busy(); };
    o.finish = [this](bool timedOut) {
        if (timedOut) return failure("timeout while saving");
        if (!errors_.empty()) return failure(errors_.front());
        json reply = documentInfo(window_);
        reply["ok"] = true;
        return reply;
    };
    return o;
}

Server::Outcome Server::cmdGetDocument() {
    Outcome o;
    const auto d = window_.document();
    if (!d) {
        o.reply = failure("no map is open");
        return o;
    }
    o.reply = json{{"ok", true}, {"doc", json::parse(doc::serializeDocument(*d))}};
    return o;
}

Server::Outcome Server::cmdSetDocument(const json& r) {
    Outcome o;
    if (!r.contains("doc") || !r["doc"].is_object()) {
        o.reply = failure("set_document needs \"doc\" (the map as an object)");
        return o;
    }
    if (window_.busy()) {
        o.reply = failure("busy: a load or save is running");
        return o;
    }
    const auto current = window_.document();
    const doc::LoadResult loaded = doc::loadDocumentText(r["doc"].dump(), current ? current->path : std::string());
    if (!loaded.version) {
        o.reply = failure("not a valid map: " + loaded.error);
        return o;
    }
    if (current) {
        if (!window_.commitEdit(r.value("label", std::string("Set document (automation)")), loaded.version)) {
            o.reply = failure("the map was not replaced");
            return o;
        }
    } else {
        window_.setDocument(loaded.version, false);
    }
    o.reply = documentInfo(window_);
    o.reply["ok"] = true;
    return o;
}

namespace {

render::ViewportWindow* viewportOf(const ui::MainWindow& w, const json& r, std::string& error) {
    const int index = r.value("viewport", 0);
    if (index < 0 || static_cast<size_t>(index) >= w.viewports().size()) {
        error = w.viewports().empty() ? "the window has no viewports" : "no viewport " + std::to_string(index) + " (0.." + std::to_string(w.viewports().size() - 1) + ")";
        return nullptr;
    }
    return w.viewports()[static_cast<size_t>(index)].window;
}

json settingsJson(const render::ViewportSettings& s) { return json::parse(render::settingsToJson(s)); }

} // namespace

Server::Outcome Server::cmdCamera(const json& r) {
    Outcome o;
    std::string error;
    render::ViewportWindow* vp = viewportOf(window_, r, error);
    if (!vp) {
        o.reply = failure(error);
        return o;
    }
    double eye[3], target[3];
    if (!r.contains("eye") || !r.contains("target") || !readVec3(r["eye"], eye) || !readVec3(r["target"], target)) {
        o.reply = failure("camera needs \"eye\" and \"target\" as [x, y, z] (metres, Y up)");
        return o;
    }
    if (eye[0] == target[0] && eye[1] == target[1] && eye[2] == target[2]) {
        o.reply = failure("eye and target are the same point");
        return o;
    }
    if (vp->rig().mode() != render::CameraMode::Fly) vp->setCameraMode(render::CameraMode::Fly);
    vp->rig().fly().lookAt(render::Vec3{eye[0], eye[1], eye[2]}, render::Vec3{target[0], target[1], target[2]});
    if (r.contains("fov")) {
        if (!r["fov"].is_number() || r["fov"].get<double>() < 5 || r["fov"].get<double>() > 150) {
            o.reply = failure("fov is the vertical field of view in degrees (5..150)");
            return o;
        }
        vp->rig().fly().fovY = r["fov"].get<double>() * 3.14159265358979323846 / 180.0;
    }
    vp->viewChanged();
    o.reply = json{{"ok", true}, {"viewport", r.value("viewport", 0)}};
    return o;
}

Server::Outcome Server::cmdSettings(const json& r) {
    Outcome o;
    std::string error;
    render::ViewportWindow* vp = viewportOf(window_, r, error);
    if (!vp) {
        o.reply = failure(error);
        return o;
    }
    if (!r.contains("key") || !r["key"].is_string() || !r.contains("value")) {
        o.reply = failure("settings needs \"key\" and \"value\"");
        return o;
    }
    const std::string key = r["key"].get<std::string>();
    const json& v = r["value"];
    render::ViewportSettings s = vp->rig().settings();
    auto badValue = [&](const char* want) { return failure("\"" + key + "\" takes " + want); };

    struct Flag { const char* name; bool render::ViewportSettings::*field; };
    static const Flag flags[] = {{"showOutlines", &render::ViewportSettings::showOutlines}, {"showSubtractive", &render::ViewportSettings::showSubtractive},
                                 {"showVolumes", &render::ViewportSettings::showVolumes}, {"backfaceCulling", &render::ViewportSettings::backfaceCulling},
                                 {"showOrigins", &render::ViewportSettings::showOrigins}, {"showDirections", &render::ViewportSettings::showDirections},
                                 {"showLinks", &render::ViewportSettings::showLinks}, {"showLightGizmos", &render::ViewportSettings::showLightGizmos},
                                 {"showSizes", &render::ViewportSettings::showSizes}, {"snapEnabled", &render::ViewportSettings::snapEnabled}};
    bool handled = false;
    if (key == "renderMode") {
        render::RenderMode m = render::RenderMode::Unlit;
        if (!v.is_string() || !enumByName<render::RenderMode>(v.get<std::string>(), 4, render::renderModeName, m)) {
            o.reply = badValue("unlit, lit, wireframe or lightingOnly");
            return o;
        }
        if ((m == render::RenderMode::Lit || m == render::RenderMode::LightingOnly) && !window_.litModesAvailable()) {
            o.reply = failure("the lit modes are not available in this run (no ray query device)");
            return o;
        }
        vp->setRenderMode(m);
        handled = true;
    } else if (key == "dlss") {
        if (!v.is_string()) {
            o.reply = badValue("off, quality, dlaa, ultraPerformance, performance or balanced");
            return o;
        }
        vp->setDlssMode(render::dlssModeFromName(v.get<std::string>()));
        handled = true;
    } else if (key == "showGrid") {
        if (!v.is_boolean()) {
            o.reply = badValue("true or false");
            return o;
        }
        vp->setShowGrid(v.get<bool>());
        handled = true;
    } else if (key == "cameraMode") {
        render::CameraMode m = render::CameraMode::Fly;
        if (!v.is_string() || !enumByName<render::CameraMode>(v.get<std::string>(), 5, render::cameraModeName, m)) {
            o.reply = badValue("fly, orbit, top, front or side");
            return o;
        }
        vp->setCameraMode(m);
        handled = true;
    } else if (key == "flySpeed") {
        if (!v.is_number()) {
            o.reply = badValue("metres per second");
            return o;
        }
        vp->rig().setFlySpeed(v.get<double>());
        handled = true;
    } else if (key == "gridSize") {
        if (!v.is_number() || v.get<double>() <= 0) {
            o.reply = badValue("a size in metres above 0");
            return o;
        }
        s.gridSize = v.get<double>();
        vp->rig().applySettings(s);
        handled = true;
    } else {
        for (const Flag& f : flags)
            if (key == f.name) {
                if (!v.is_boolean()) {
                    o.reply = badValue("true or false");
                    return o;
                }
                s.*(f.field) = v.get<bool>();
                vp->rig().applySettings(s);
                handled = true;
            }
    }
    if (!handled) {
        o.reply = failure("unknown setting \"" + key + "\" (renderMode, dlss, showGrid, cameraMode, flySpeed, gridSize, showOutlines, showSubtractive, showVolumes, backfaceCulling, "
                          "showOrigins, showDirections, showLinks, showLightGizmos, showSizes, snapEnabled)");
        return o;
    }
    vp->viewChanged();
    o.reply = json{{"ok", true}, {"settings", settingsJson(vp->rig().settings())}};
    return o;
}

Server::Outcome Server::cmdScreenshot(const json& r) {
    Outcome o;
    std::string error;
    render::ViewportWindow* vp = viewportOf(window_, r, error);
    if (!vp) {
        o.reply = failure(error);
        return o;
    }
    if (!r.contains("path") || !r["path"].is_string()) {
        o.reply = failure("screenshot needs \"path\" (a .png file)");
        return o;
    }
    const std::string path = r["path"].get<std::string>();
    o.timeoutMs = r.value("timeoutMs", 60000);
    // The shot is taken when the visible scene is complete and, in the lit modes, the picture has converged.
    o.ready = [this, vp] {
        if (vp->viewportId() == 0) return false;
        if (window_.document() && !window_.sceneReady()) return false;
        const render::RenderMode m = vp->rig().settings().renderMode;
        return (m != render::RenderMode::Lit && m != render::RenderMode::LightingOnly) || window_.lightingSettled();
    };
    const bool sizeAsked = r.contains("width") || r.contains("height");
    o.finish = [vp, path, sizeAsked](bool timedOut) {
        const render::CapturedImage frame = vp->capture();
        if (!frame.ok()) return failure(timedOut ? "timeout, and the viewport has no picture" : "the viewport could not render a picture");
        const QImage img(frame.rgba.data(), static_cast<int>(frame.width), static_cast<int>(frame.height), static_cast<qsizetype>(frame.width) * 4, QImage::Format_RGBA8888);
        if (!img.save(QString::fromUtf8(path.c_str()))) return failure("cannot write " + path);
        json reply{{"ok", true}, {"path", path}, {"width", frame.width}, {"height", frame.height}, {"converged", !timedOut}};
        if (sizeAsked) reply["note"] = "width and height are not honoured: the picture has the size of the viewport";
        return reply;
    };
    return o;
}

Server::Outcome Server::cmdStats() {
    Outcome o;
    json views = json::array();
    for (size_t i = 0; i < window_.viewports().size(); ++i) {
        render::ViewportWindow* vp = window_.viewports()[i].window;
        views.push_back(json{{"index", i}, {"width", vp->width()}, {"height", vp->height()}, {"cameraMode", render::cameraModeName(vp->rig().mode())},
                             {"renderMode", render::renderModeName(vp->rig().settings().renderMode)}, {"dlss", render::dlssModeName(vp->rig().settings().dlss)}});
    }
    o.reply = json{{"ok", true},
                   {"document", documentInfo(window_)},
                   {"busy", window_.busy()},
                   {"sceneReady", window_.sceneReady()},
                   {"selection", window_.selection().items().size()},
                   {"canUndo", window_.canUndo()},
                   {"canRedo", window_.canRedo()},
                   {"lighting", json{{"available", window_.lightingAvailable()}, {"litModes", window_.litModesAvailable()}, {"settled", window_.lightingSettled()}, {"text", window_.lightingText()}}},
                   {"viewports", views}};
    return o;
}

Server::Outcome Server::cmdLighting(const json& r) {
    Outcome o;
    const std::string action = r.value("action", std::string());
    if (action == "wait_converged") {
        o.timeoutMs = r.value("timeoutMs", 120000);
        o.ready = [this] { return window_.lightingSettled(); };
        o.finish = [this](bool timedOut) { return json{{"ok", true}, {"converged", !timedOut}, {"text", window_.lightingText()}}; };
        return o;
    }
    if (action == "pause" || action == "resume") {
        o.reply = failure("\"" + action + "\" is not supported: the ray traced Lit path renders continuously and converges on its own");
        return o;
    }
    o.reply = failure("lighting needs \"action\": wait_converged");
    return o;
}

} // namespace native::automation
