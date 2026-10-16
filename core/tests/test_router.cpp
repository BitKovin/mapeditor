#include "core/origin.h"
#include "core/router.h"
#include "test_util.h"
#include <doctest.h>

using namespace core;
using namespace testutil;

namespace {
struct Files {
    TempDir dir;
    Files() {
        dir.write("maps/a.map.json", docJson({entityJson("e_world", "worldspawn", {Brush::makeBox("b", {0, 0, 0}, {1, 1, 1})})}));
        dir.write("prefabs/p.map.json", docJson({entityJson("e_p", "func_detail", {Brush::makeBox("pb", {0, 0, 0}, {1, 1, 1})})}));
    }
};

// Files are written and the map opened *before* the Router is constructed,
// so its initial library fingerprint matches the opened document.
struct Fixture : Files {
    Scene scene{dir.path};
    Router router{(scene.openDocument("maps/a.map.json"), scene)};
    static json update(const json& doc) { return {{"type", "document_update"}, {"doc", doc}}; }
    RouterOutput send(const json& msg) { return router.onMessage(msg.dump()); }
    static json parse(const std::string& s) { return json::parse(s); }
};
}  // namespace

TEST_CASE("a connecting session gets the document, library, profile, entity definitions and settings - only itself") {
    Fixture f;
    RouterOutput out = f.router.onConnect();
    REQUIRE(out.toSender.size() == 5);
    CHECK(Fixture::parse(out.toSender[0])["type"] == "document_sync");
    CHECK(Fixture::parse(out.toSender[1])["type"] == "prefab_library");
    CHECK(Fixture::parse(out.toSender[2])["type"] == "profile");
    CHECK(Fixture::parse(out.toSender[2])["profile"]["textureRoot"] == "textures");
    CHECK(Fixture::parse(out.toSender[3])["type"] == "entity_defs");
    CHECK(Fixture::parse(out.toSender[4])["type"] == "settings");
    CHECK(out.toOthers.empty());
    CHECK(out.toAll.empty());
}

TEST_CASE("an accepted update acks the sender and syncs everyone else - no echo to the sender") {
    Fixture f;
    json doc = f.scene.documentSyncJson()["doc"];
    doc["entities"][0]["origin"] = json::array({5, 0, 0});

    RouterOutput out = f.send(Fixture::update(doc));
    REQUIRE(out.toSender.size() == 1);
    CHECK(Fixture::parse(out.toSender[0]) == json{{"type", "update_ack"}, {"rev", 2}});
    REQUIRE(out.toOthers.size() == 1);
    json sync = Fixture::parse(out.toOthers[0]);
    CHECK(sync["type"] == "document_sync");
    CHECK(sync["rev"] == 2);
    CHECK(sync["doc"] == doc);
    CHECK(out.toAll.empty());  // library unchanged -> not re-sent
}

TEST_CASE("the prefab library is pushed to everyone when an update makes it change") {
    Fixture f;
    json doc = f.scene.documentSyncJson()["doc"];
    doc["prefabInstances"].push_back(instanceJson("pi", "prefabs/p.map.json"));

    RouterOutput out = f.send(Fixture::update(doc));
    REQUIRE(out.toAll.size() == 1);
    json lib = Fixture::parse(out.toAll[0]);
    CHECK(lib["type"] == "prefab_library");
    CHECK(lib["prefabs"].contains("prefabs/p.map.json"));

    // A second update that doesn't touch prefabs must not resend it.
    doc["entities"][0]["origin"] = json::array({1, 1, 1});
    CHECK(f.send(Fixture::update(doc)).toAll.empty());
}

TEST_CASE("a rejected update tells only the sender and broadcasts nothing") {
    Fixture f;
    json doc = f.scene.documentSyncJson()["doc"];
    doc["prefabInstances"].push_back(instanceJson("pi", "prefabs/missing.map.json"));

    RouterOutput out = f.send(Fixture::update(doc));
    REQUIRE(out.toSender.size() == 1);
    json err = Fixture::parse(out.toSender[0]);
    CHECK(err["type"] == "error");
    CHECK(err["message"].get<std::string>().find("not found") != std::string::npos);
    CHECK(out.toOthers.empty());
    CHECK(out.toAll.empty());
    CHECK(f.scene.revision() == 1);
}

TEST_CASE("garbage input produces an error for the sender and never throws") {
    Fixture f;
    for (const char* bad : {"not json", "[]", "{}", "{\"type\":5}", "{\"type\":\"bogus\"}",
                             "{\"type\":\"document_update\"}", "{\"type\":\"document_update\",\"doc\":7}",
                             "{\"type\":\"open_document\"}"}) {
        RouterOutput out;
        REQUIRE_NOTHROW(out = f.router.onMessage(bad));
        REQUIRE(out.toSender.size() == 1);
        CHECK(Fixture::parse(out.toSender[0])["type"] == "error");
        CHECK(out.toOthers.empty());
        CHECK(out.toAll.empty());
    }
}

TEST_CASE("open_document replaces the document for every session and answers the request") {
    Fixture f;
    f.dir.write("maps/b.map.json", docJson({entityJson("e_b", "worldspawn", {Brush::makeBox("bb", {0, 0, 0}, {2, 2, 2})})}));

    RouterOutput out = f.send({{"type", "open_document"}, {"requestId", 7}, {"path", "maps/b.map.json"}});
    REQUIRE(out.toAll.size() == 2);
    CHECK(Fixture::parse(out.toAll[0])["type"] == "document_sync");
    CHECK(Fixture::parse(out.toAll[0])["path"] == "maps/b.map.json");
    CHECK(Fixture::parse(out.toAll[1])["type"] == "prefab_library");
    REQUIRE(out.toSender.size() == 1);
    json res = Fixture::parse(out.toSender[0]);
    CHECK(res["type"] == "response");
    CHECK(res["requestId"] == 7);
    CHECK(res["ok"] == true);

    // State is delivered before the reply that refers to it.
    auto order = out.inDeliveryOrder();
    REQUIRE(order.size() == 3);
    CHECK(Fixture::parse(*order[0].text)["type"] == "document_sync");
    CHECK(Fixture::parse(*order[1].text)["type"] == "prefab_library");
    CHECK(order[2].to == RouterOutput::To::Sender);
    CHECK(Fixture::parse(*order[2].text)["type"] == "response");

    out = f.send({{"type", "open_document"}, {"requestId", "r2"}, {"path", "maps/none.map.json"}});
    REQUIRE(out.toSender.size() == 1);
    res = Fixture::parse(out.toSender[0]);
    CHECK(res["ok"] == false);
    CHECK(res["requestId"] == "r2");
    CHECK(out.toAll.empty());
}

TEST_CASE("save_document answers the sender; save-as also tells the other sessions the new path") {
    Fixture f;
    RouterOutput out = f.send({{"type", "save_document"}, {"requestId", 1}});
    REQUIRE(out.toSender.size() == 1);
    CHECK(Fixture::parse(out.toSender[0])["ok"] == true);
    CHECK(out.toOthers.empty());  // path unchanged

    out = f.send({{"type", "save_document"}, {"requestId", 2}, {"path", "maps/copy.map.json"}});
    CHECK(f.dir.exists("maps/copy.map.json"));
    REQUIRE(out.toOthers.size() == 1);
    CHECK(Fixture::parse(out.toOthers[0])["path"] == "maps/copy.map.json");

    out = f.send({{"type", "save_document"}, {"requestId", 3}, {"path", "../evil.json"}});
    CHECK(Fixture::parse(out.toSender[0])["ok"] == false);
}

TEST_CASE("only local origins may connect") {
    for (const char* ok : {"", "http://localhost", "http://localhost:5173", "https://localhost:8080", "http://127.0.0.1:3000",
                            "http://[::1]", "http://[::1]:3000"})
        CHECK_MESSAGE(isLocalOrigin(ok), ok);
    for (const char* bad : {"http://evil.com", "https://example.org:443", "null", "file://", "http://localhost.evil.com",
                             "http://localhost@evil.com", "http://localhost:abc", "http://localhost:", "ftp://localhost",
                             "http://127.0.0.1.evil.com", "localhost"})
        CHECK_MESSAGE(!isLocalOrigin(bad), bad);
}
