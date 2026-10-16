#include "core/scene.h"
#include "test_util.h"
#include <doctest.h>

using namespace core;
using namespace testutil;

static bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

namespace {
// maps/a.map.json -> prefabs/p.map.json -> prefabs/q.map.json
struct Fixture {
    TempDir dir;
    Scene scene{dir.path};
    Brush box = Brush::makeBox("wall", {0, 0, 0}, {4, 3, 0.2});

    Fixture() {
        dir.write("maps/a.map.json", docJson({entityJson("e_world", "worldspawn", {box})},
                                              {instanceJson("pi_p", "prefabs/p.map.json", {1, 0, 0})}));
        dir.write("prefabs/p.map.json",
                  docJson({entityJson("e_p", "func_group", {Brush::makeBox("p_b", {0, 0, 0}, {1, 1, 1})})},
                          {instanceJson("pi_q", "prefabs/q.map.json")}));
        dir.write("prefabs/q.map.json", docJson({entityJson("e_q", "func_detail", {lPrism("q_b")})}));
        scene.openDocument("maps/a.map.json");
    }
    json currentDoc() const { return scene.documentSyncJson()["doc"]; }
};
}  // namespace

TEST_CASE("openDocument adopts the file and bumps the revision") {
    Fixture f;
    json sync = f.scene.documentSyncJson();
    CHECK(sync["type"] == "document_sync");
    CHECK(sync["rev"] == 1);
    CHECK(sync["path"] == "maps/a.map.json");
    CHECK(sync["doc"] == f.dir.read("maps/a.map.json"));
}

TEST_CASE("openDocument failures leave the current document untouched") {
    Fixture f;
    const json before = f.scene.documentSyncJson();
    f.dir.write("maps/bad.map.json", json{{"schemaVersion", "0.2"}, {"entities", json::array({json{{"nope", 1}}})}});
    CHECK_THROWS_WITH_AS(f.scene.openDocument("maps/missing.map.json"), doctest::Contains("could not open"), std::runtime_error);
    CHECK_THROWS_AS(f.scene.openDocument("maps/bad.map.json"), std::runtime_error);
    CHECK_THROWS_WITH_AS(f.scene.openDocument("../outside.json"), doctest::Contains("escapes"), std::runtime_error);
    CHECK_THROWS_WITH_AS(f.scene.openDocument("/etc/passwd"), doctest::Contains("absolute"), std::runtime_error);
    CHECK(f.scene.documentSyncJson() == before);
}

TEST_CASE("applyUpdate adopts a valid document and preserves fields Core doesn't know about") {
    Fixture f;
    json doc = f.currentDoc();
    doc["editor"] = {{"visgroups", json::array({"walls"})}};                       // unknown top-level data
    doc["entities"][0]["editorColor"] = "#ff8800";                                 // unknown entity data
    doc["entities"][0]["brushes"][0]["faces"][0]["projection"] = {{"mode", "box"}};  // unknown face data

    auto r = f.scene.applyUpdate(doc);
    REQUIRE(r.ok);
    CHECK(r.rev == 2);
    CHECK(f.currentDoc() == doc);  // byte-for-byte what the Shell sent
}

TEST_CASE("applyUpdate rejects bad documents and changes nothing") {
    Fixture f;
    const json before = f.scene.documentSyncJson();
    json doc = f.currentDoc();
    std::string expected;

    SUBCASE("malformed") {
        doc["entities"][0].erase("id");
        expected = "malformed";
    }
    SUBCASE("non-convex worldspawn brush") {
        doc["entities"][0]["brushes"].push_back(brushJson(lPrism("l")));
        expected = "must be convex";
    }
    SUBCASE("duplicate entity id") {
        doc["entities"].push_back(doc["entities"][0]);
        expected = "duplicate";
    }
    SUBCASE("prefab file does not exist") {
        doc["prefabInstances"].push_back(instanceJson("pi_x", "prefabs/nope.map.json"));
        expected = "not found";
    }
    SUBCASE("prefab path escapes the sandbox") {
        doc["prefabInstances"].push_back(instanceJson("pi_x", "../secret.json"));
        expected = "escapes";
    }
    SUBCASE("prefab cycle") {
        f.dir.write("prefabs/q.map.json", docJson({}, {instanceJson("pi_back", "prefabs/p.map.json")}));
        expected = "cycle";
    }
    SUBCASE("a broken prefab file poisons only the documents that use it") {
        f.dir.write("prefabs/q.map.json", docJson({entityJson("e_q", "func_group", {lPrism("q_b")})}));
        expected = "must be convex";
    }

    auto r = f.scene.applyUpdate(doc);
    CHECK_FALSE(r.ok);
    CHECK(has(r.error, expected.c_str()));
    CHECK(f.scene.documentSyncJson() == before);  // revision and content unchanged
}

TEST_CASE("prefab paths are normalized, so aliases of one file are one file") {
    Fixture f;
    // p includes q; a second alias of p must not be mistaken for a different file.
    json doc = f.currentDoc();
    doc["prefabInstances"].push_back(instanceJson("pi_alias", "prefabs/../prefabs/./p.map.json", {5, 0, 0}));
    CHECK(f.scene.applyUpdate(doc).ok);
    CHECK(f.scene.prefabLibraryJson()["prefabs"].size() == 2);  // p and q, once each
}

TEST_CASE("saveDocument writes atomically, defaults to the current path, and supports save-as") {
    Fixture f;
    json doc = f.currentDoc();
    doc["editor"] = {{"note", "hi"}};
    REQUIRE(f.scene.applyUpdate(doc).ok);

    CHECK(f.scene.saveDocument("") == "maps/a.map.json");
    CHECK(f.dir.read("maps/a.map.json") == doc);
    CHECK_FALSE(f.dir.exists("maps/a.map.json.tmp"));

    CHECK(f.scene.saveDocument("maps/sub/b.map.json") == "maps/sub/b.map.json");  // creates directories
    CHECK(f.scene.currentPath() == "maps/sub/b.map.json");
    CHECK(f.dir.read("maps/sub/b.map.json") == doc);

    Scene reopened(f.dir.path);
    reopened.openDocument("maps/sub/b.map.json");  // round trip through disk
    CHECK(reopened.documentSyncJson()["doc"] == doc);
}

TEST_CASE("saveDocument refuses paths outside the assets root and pathless documents") {
    Fixture f;
    CHECK_THROWS_WITH_AS(f.scene.saveDocument("../evil.json"), doctest::Contains("escapes"), std::runtime_error);
    CHECK_THROWS_WITH_AS(f.scene.saveDocument("/tmp/evil.json"), doctest::Contains("absolute"), std::runtime_error);

    Scene fresh(f.dir.path);  // nothing opened yet
    CHECK_THROWS_WITH_AS(fresh.saveDocument(""), doctest::Contains("no path"), std::runtime_error);
}

TEST_CASE("prefab library contains every reachable prefab, and reports missing ones") {
    Fixture f;
    json lib = f.scene.prefabLibraryJson();
    CHECK(lib["type"] == "prefab_library");
    REQUIRE(lib["prefabs"].size() == 2);
    CHECK(lib["prefabs"]["prefabs/p.map.json"] == f.dir.read("prefabs/p.map.json"));
    CHECK(lib["prefabs"]["prefabs/q.map.json"] == f.dir.read("prefabs/q.map.json"));

    std::filesystem::remove(f.dir.path / "prefabs/q.map.json");
    lib = f.scene.prefabLibraryJson();
    CHECK(lib["prefabs"]["prefabs/q.map.json"].contains("error"));
}

TEST_CASE("library fingerprint changes when a prefab file changes on disk") {
    Fixture f;
    const std::string before = f.scene.libraryFingerprint();
    CHECK(f.scene.libraryFingerprint() == before);  // stable when nothing changed

    f.dir.write("prefabs/q.map.json", docJson({entityJson("e_q2", "func_detail", {lPrism("q_b")})}));
    auto p = f.dir.path / "prefabs/q.map.json";
    std::filesystem::last_write_time(p, std::filesystem::last_write_time(p) + std::chrono::seconds(5));

    CHECK(f.scene.libraryFingerprint() != before);
    CHECK(f.scene.prefabLibraryJson()["prefabs"]["prefabs/q.map.json"]["entities"][0]["id"] == "e_q2");  // cache reloaded
}
