// T9.4: the automation socket. Offscreen platform, the window is never shown, no GPU: the commands that need a viewport are answered with an error
// here (their success paths are exercised by native/tests/automation_smoke.py against the running editor).
#include <doctest.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <functional>

#include <nlohmann/json.hpp>

#include "automation/server.h"
#include "base/jobs.h"
#include "doc/document.h"
#include "ui/main_window.h"

namespace fs = std::filesystem;
using namespace native;
using json = nlohmann::json;

namespace {

bool waitFor(const std::function<bool()>& cond, int timeoutMs = 20000) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (cond()) return true;
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return cond();
}

struct Game {
    QTemporaryDir dir;
    fs::path root;
    Game() {
        root = fs::path(dir.path().toStdString());
        fs::copy(fs::path(NATIVE_CORE_ASSETS_DIR), root, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    }
};

// One request through dispatch(), waiting for the answer.
json ask(automation::Server& server, const json& request, int timeoutMs = 20000) {
    json answer;
    bool got = false;
    server.dispatch(request, [&](json r) {
        answer = std::move(r);
        got = true;
    });
    REQUIRE(waitFor([&] { return got; }, timeoutMs));
    return answer;
}

} // namespace

TEST_CASE("automation: open, get_document, set_document (one undo step), save, stats") {
    Game game;
    base::JobPool jobs(2);
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    sv.assetsRoot = game.root.generic_string();
    sv.jobs = &jobs;
    ui::MainWindow window(sv);
    automation::Server server(window);

    json r = ask(server, {{"id", 7}, {"cmd", "help"}});
    CHECK(r["ok"] == true);
    CHECK(r["id"] == 7);   // the id comes back
    CHECK(ask(server, {{"cmd", "nope"}})["ok"] == false);
    CHECK(ask(server, json::array())["ok"] == false);
    CHECK(ask(server, {{"cmd", "get_document"}})["ok"] == false);   // nothing open yet
    CHECK(ask(server, {{"cmd", "stats"}})["document"]["open"] == false);

    r = ask(server, {{"cmd", "open"}, {"path", "maps/demo_lit.map.json"}});
    INFO(r.dump());
    REQUIRE(r["ok"] == true);
    CHECK(r["path"] == "maps/demo_lit.map.json");
    CHECK(r["brushes"].get<int>() > 0);
    CHECK(r["dirty"] == false);
    r = ask(server, {{"cmd", "open"}, {"path", "maps/does_not_exist.map.json"}});
    CHECK(r["ok"] == false);
    CHECK(window.document()->path == "maps/demo_lit.map.json");   // the map stays
    r = ask(server, {{"cmd", "open"}, {"path", "../outside.map.json"}});
    CHECK(r["ok"] == false);

    r = ask(server, {{"cmd", "get_document"}});
    REQUIRE(r["ok"] == true);
    json document = r["doc"];
    REQUIRE(document.contains("entities"));
    const size_t entities = document["entities"].size();
    REQUIRE(entities > 1);

    // set_document: the map without its last entity, validated, one undo step
    json smaller = document;
    smaller["entities"].erase(smaller["entities"].size() - 1);
    r = ask(server, {{"cmd", "set_document"}, {"doc", smaller}, {"label", "Cut"}});
    INFO(r.dump());
    REQUIRE(r["ok"] == true);
    CHECK(window.document()->entities.size() == entities - 1);
    CHECK(r["dirty"] == true);
    CHECK(window.canUndo());
    CHECK(window.undoText() == "Undo Cut");
    window.undo();
    CHECK(window.document()->entities.size() == entities);   // one step brings it all back
    CHECK_FALSE(window.canUndo());

    // an invalid map is refused and changes nothing
    r = ask(server, {{"cmd", "set_document"}, {"doc", {{"entities", 5}}}});
    CHECK(r["ok"] == false);
    CHECK(window.document()->entities.size() == entities);
    CHECK(ask(server, {{"cmd", "set_document"}})["ok"] == false);

    // save under a new path, then without one
    r = ask(server, {{"cmd", "save"}, {"path", "maps/automation_out.map.json"}});
    INFO(r.dump());
    REQUIRE(r["ok"] == true);
    CHECK(fs::exists(game.root / "maps" / "automation_out.map.json"));
    CHECK(r["path"] == "maps/automation_out.map.json");
    CHECK(r["dirty"] == false);
    r = ask(server, {{"cmd", "save"}});
    CHECK(r["ok"] == true);

    r = ask(server, {{"cmd", "stats"}});
    CHECK(r["ok"] == true);
    CHECK(r["document"]["open"] == true);
    CHECK(r["busy"] == false);
    CHECK(r["viewports"].empty());   // no viewport without a device

    // commands that need a viewport or the lighting say what is wrong
    for (const char* cmd : {"camera", "settings", "screenshot"}) {
        r = ask(server, {{"cmd", cmd}, {"eye", {0, 0, 0}}, {"target", {1, 0, 0}}, {"key", "showGrid"}, {"value", true}, {"path", "x.png"}});
        INFO(cmd);
        CHECK(r["ok"] == false);
        CHECK(r["error"].get<std::string>().find("viewport") != std::string::npos);
    }
    r = ask(server, {{"cmd", "lighting"}, {"action", "wait_converged"}, {"timeoutMs", 2000}});
    CHECK(r["ok"] == true);
    CHECK(r["converged"] == true);   // no lighting service: nothing to wait for
    CHECK(ask(server, {{"cmd", "lighting"}, {"action", "pause"}})["ok"] == false);
    CHECK(ask(server, {{"cmd", "lighting"}})["ok"] == false);
}

TEST_CASE("automation: the socket answers each line in order, survives bad input and several clients") {
    Game game;
    base::JobPool jobs(2);
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    sv.assetsRoot = game.root.generic_string();
    sv.jobs = &jobs;
    ui::MainWindow window(sv);
    automation::Server server(window);
    REQUIRE(server.listen(0));
    const int port = server.port();
    REQUIRE(port > 0);
    {   // the port is taken now
        automation::Server other(window);
        CHECK_FALSE(other.listen(port));
        CHECK_FALSE(other.error().empty());
    }

    QTcpSocket a, b;
    a.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(port));
    b.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(port));
    REQUIRE(waitFor([&] { return a.state() == QAbstractSocket::ConnectedState && b.state() == QAbstractSocket::ConnectedState; }));

    // reads `count` reply lines from `s`
    auto lines = [&](QTcpSocket& s, int count) {
        std::vector<json> out;
        QByteArray buffer;
        waitFor([&] {
            buffer += s.readAll();
            return buffer.count('\n') >= count;
        });
        for (const QByteArray& l : buffer.split('\n'))
            if (!l.trimmed().isEmpty()) out.push_back(json::parse(l.constData()));
        return out;
    };

    // three requests in one write: the load takes time, the ones behind it wait and still come back in order; one line is no JSON
    a.write("{\"id\":1,\"cmd\":\"open\",\"path\":\"maps/demo_lit.map.json\"}\nthis is not json\n{\"id\":3,\"cmd\":\"stats\"}\n");
    const std::vector<json> reply = lines(a, 3);
    REQUIRE(reply.size() == 3);
    CHECK(reply[0]["id"] == 1);
    CHECK(reply[0]["ok"] == true);
    CHECK(reply[1]["ok"] == false);   // the bad line is answered, the connection stays
    CHECK(reply[2]["id"] == 3);
    CHECK(reply[2]["document"]["open"] == true);   // the stats ran after the load had finished

    b.write("{\"id\":\"x\",\"cmd\":\"get_document\"}\n");
    const std::vector<json> other = lines(b, 1);
    REQUIRE(other.size() == 1);
    CHECK(other[0]["id"] == "x");
    CHECK(other[0]["ok"] == true);
    CHECK(other[0]["doc"].contains("entities"));

    // a client that disconnects while its request is running does not hurt the next one
    a.write("{\"cmd\":\"open\",\"path\":\"maps/demo_lit.map.json\"}\n");
    a.flush();
    a.disconnectFromHost();
    QTcpSocket c;
    c.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(port));
    REQUIRE(waitFor([&] { return c.state() == QAbstractSocket::ConnectedState; }));
    c.write("{\"id\":9,\"cmd\":\"help\"}\n");
    const std::vector<json> third = lines(c, 1);
    REQUIRE(third.size() == 1);
    CHECK(third[0]["id"] == 9);
}
