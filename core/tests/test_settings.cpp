#include "core/router.h"
#include "core/scene.h"
#include "core/settings.h"
#include "test_util.h"
#include <doctest.h>

using namespace core;
using namespace testutil;
namespace fs = std::filesystem;

static void touch(const TempDir& d, const std::string& rel, const std::string& bytes = "x") {
    const auto full = d.path / rel;
    fs::create_directories(full.parent_path());
    std::ofstream(full, std::ios::binary) << bytes;
}

static json find(const std::vector<std::string>& msgs, const char* type) {
    for (const auto& m : msgs) {
        json j = json::parse(m);
        if (j["type"] == type) return j;
    }
    return json();
}

// Makes a file look newer than any previous write, whatever the timestamp granularity of the file system.
static void bump(const fs::path& f) { fs::last_write_time(f, fs::last_write_time(f) + std::chrono::seconds(10)); }

TEST_CASE("settings: round trip, and damaged or missing files fall back to defaults") {
    TempDir d;
    CHECK(loadSettings(d.path / "s.json").assetsRoot.empty());
    CHECK(saveSettings(d.path / "sub" / "s.json", Settings{"C:/assets", "maps/a.map.json"}));  // creates the folder
    const Settings s = loadSettings(d.path / "sub" / "s.json");
    CHECK(s.assetsRoot == "C:/assets");
    CHECK(s.lastMap == "maps/a.map.json");
    std::ofstream(d.path / "bad.json") << "{ not json";
    CHECK(loadSettings(d.path / "bad.json").assetsRoot.empty());
    std::ofstream(d.path / "wrong.json") << R"({"assetsRoot": 5})";
    CHECK(loadSettings(d.path / "wrong.json").assetsRoot.empty());
    touch(d, "blocker", "file");
    CHECK_FALSE(saveSettings(d.path / "blocker" / "x" / "s.json", Settings{}));  // cannot be written: reports false, does not throw
}

TEST_CASE("settings: UTF-8 path conversion round-trips") {
    const std::string s = "assets/caf\xC3\xA9";
    CHECK(pathToUtf8(pathFromUtf8(s)) == s);
}

TEST_CASE("scene: setAssetsRoot reloads profile and definitions, drops caches and reopens the map") {
    TempDir a, b;
    a.write("maps/m.map.json", docJson({}, {instanceJson("pi", "prefabs/p.map.json")}));
    a.write("prefabs/p.map.json", docJson({entityJson("e", "func_detail", {lPrism("pb")})}));
    b.write("maps/m.map.json", docJson({entityJson("w", "worldspawn", {Brush::makeBox("bb", {0, 0, 0}, {1, 1, 1})})}));
    b.write("game_profile.json", json{{"name", "second"}, {"textureRoot", "tex"}});
    std::ofstream(b.path / "entities.ent") << R"(<point name="only_here"/>)";

    Scene scene(a.path);
    scene.loadGameProfile();
    scene.openDocument("maps/m.map.json");
    CHECK(scene.prefabLibraryJson()["prefabs"].size() == 1);
    const auto rev = scene.revision();

    scene.setAssetsRoot(b.path);
    CHECK(scene.profile().name == "second");
    CHECK(scene.profile().textureRoot == "tex");
    CHECK(scene.entityDefinitions().find("only_here"));
    CHECK(scene.currentPath() == "maps/m.map.json");
    CHECK(scene.revision() > rev);
    CHECK(scene.documentSyncJson()["doc"]["entities"].size() == 1);  // the map from the NEW folder
    CHECK(scene.prefabLibraryJson()["prefabs"].empty());
    CHECK_FALSE(scene.dirty());
}

TEST_CASE("scene: setAssetsRoot falls back to an empty document when the map is not in the new folder") {
    TempDir a, b;
    a.write("maps/m.map.json", docJson({entityJson("w", "worldspawn", {Brush::makeBox("bb", {0, 0, 0}, {1, 1, 1})})}));
    Scene scene(a.path);
    scene.openDocument("maps/m.map.json");
    scene.setAssetsRoot(b.path);
    CHECK(scene.currentPath().empty());
    CHECK(scene.documentSyncJson()["doc"]["entities"].empty());
}

TEST_CASE("scene: setAssetsRoot with a bad folder or profile changes nothing") {
    TempDir a, b;
    a.write("maps/m.map.json", docJson({}));
    Scene scene(a.path);
    scene.openDocument("maps/m.map.json");
    const auto rev = scene.revision();
    CHECK_THROWS(scene.setAssetsRoot(a.path / "does_not_exist"));
    b.write("game_profile.json", json{{"textureRoot", "../x"}});
    CHECK_THROWS(scene.setAssetsRoot(b.path));
    CHECK(scene.assetsRoot() == a.path);
    CHECK(scene.currentPath() == "maps/m.map.json");
    CHECK(scene.revision() == rev);
}

TEST_CASE("scene: dirty flag follows edits, open and save") {
    TempDir d;
    d.write("maps/m.map.json", docJson({}));
    Scene scene(d.path);
    scene.openDocument("maps/m.map.json");
    CHECK_FALSE(scene.dirty());
    CHECK(scene.documentSyncJson()["dirty"] == false);
    REQUIRE(scene.applyUpdate(docJson({entityJson("w", "worldspawn", {Brush::makeBox("b", {0, 0, 0}, {1, 1, 1})})})).ok);
    CHECK(scene.dirty());
    CHECK(scene.documentSyncJson()["dirty"] == true);
    scene.saveDocument("");
    CHECK_FALSE(scene.dirty());
}

TEST_CASE("scene: listDirectory lists folders first, hides dot-files and stays inside the root") {
    TempDir d;
    touch(d, "maps/b.map.json"); touch(d, "maps/A.map.json"); touch(d, "maps/sub/x.map.json"); touch(d, "maps/.hidden");
    Scene scene(d.path);
    const json root = scene.listDirectory("");
    CHECK(root["path"] == "");
    CHECK(root["entries"].size() == 1);
    const json maps = scene.listDirectory("maps");
    REQUIRE(maps["entries"].size() == 3);
    CHECK(maps["entries"][0] == json{{"name", "sub"}, {"dir", true}});
    CHECK(maps["entries"][1]["name"] == "A.map.json");  // case-insensitive order
    CHECK(maps["entries"][2]["name"] == "b.map.json");
    CHECK(scene.listDirectory("maps/sub/..")["path"] == "maps");
    for (const char* bad : {"..", "../x", "maps/../..", "/etc", "C:/Windows"}) CHECK_THROWS(scene.listDirectory(bad));
    CHECK_THROWS(scene.listDirectory("maps/b.map.json"));  // a file, not a folder
    CHECK_THROWS(scene.listDirectory("nope"));
}

TEST_CASE("router: list_directory") {
    TempDir d;
    touch(d, "maps/a.map.json");
    Scene scene(d.path);
    Router router(scene);
    json r = json::parse(router.onMessage(R"({"type":"list_directory","requestId":7,"path":"maps"})").toSender.at(0));
    CHECK(r["ok"] == true);
    CHECK(r["requestId"] == 7);
    CHECK(r["entries"][0]["name"] == "a.map.json");
    r = json::parse(router.onMessage(R"({"type":"list_directory","requestId":8,"path":"../"})").toSender.at(0));
    CHECK(r["ok"] == false);
    r = json::parse(router.onMessage(R"({"type":"list_directory","requestId":9})").toSender.at(0));
    CHECK(r["ok"] == true);  // no path = the assets root
}

TEST_CASE("router: browse_filesystem lists folders only and reports whether a game profile is there") {
    TempDir d;
    touch(d, "one/x.txt"); touch(d, "two/y"); touch(d, "file.txt"); touch(d, "game_profile.json", "{}");
    Scene scene(d.path);
    Router router(scene);
    const std::string p = pathToUtf8(d.path);
    json r = json::parse(router.onMessage(json{{"type", "browse_filesystem"}, {"requestId", 1}, {"path", p}}.dump()).toSender.at(0));
    REQUIRE(r["ok"] == true);
    REQUIRE(r["dirs"].size() == 2);
    CHECK(r["dirs"][0]["name"] == "one");
    CHECK(r["hasProfile"] == true);
    CHECK(r["parent"].is_string());
    for (const auto& dir : r["dirs"]) CHECK(dir["name"] != "file.txt");
    // relative paths are refused, and so are files
    CHECK(json::parse(router.onMessage(R"({"type":"browse_filesystem","requestId":2,"path":"relative/dir"})").toSender.at(0))["ok"] == false);
    CHECK(json::parse(router.onMessage(json{{"type", "browse_filesystem"}, {"requestId", 3}, {"path", pathToUtf8(d.path / "file.txt")}}.dump()).toSender.at(0))["ok"] == false);
    // no path: something browsable to start from
    CHECK(json::parse(router.onMessage(R"({"type":"browse_filesystem","requestId":4})").toSender.at(0))["ok"] == true);
}

TEST_CASE("router: set_assets_root broadcasts the new world, persists, and reports failures") {
    TempDir a, b, cfg;
    a.write("maps/m.map.json", docJson({}));
    b.write("maps/m.map.json", docJson({entityJson("w", "worldspawn", {Brush::makeBox("bb", {0, 0, 0}, {1, 1, 1})})}));
    b.write("game_profile.json", json{{"name", "second"}});
    Scene scene(a.path);
    scene.loadGameProfile();
    scene.openDocument("maps/m.map.json");
    const fs::path settingsFile = cfg.path / "settings.json";
    Router router(scene, settingsFile);

    RouterOutput out = router.onMessage(json{{"type", "set_assets_root"}, {"requestId", 5}, {"path", pathToUtf8(b.path)}}.dump());
    const json reply = json::parse(out.toSender.at(0));
    CHECK(reply["ok"] == true);
    CHECK(reply["persisted"] == true);
    CHECK(find(out.toAll, "profile")["profile"]["name"] == "second");
    CHECK(find(out.toAll, "entity_defs")["type"] == "entity_defs");
    CHECK(find(out.toAll, "settings")["assetsRoot"] == pathToUtf8(b.path.lexically_normal()));
    CHECK(find(out.toAll, "document_sync")["doc"]["entities"].size() == 1);
    CHECK(find(out.toAll, "prefab_library")["type"] == "prefab_library");
    CHECK(out.toOthers.empty());
    CHECK(loadSettings(settingsFile).assetsRoot == pathToUtf8(b.path.lexically_normal()));
    CHECK(loadSettings(settingsFile).lastMap == "maps/m.map.json");

    out = router.onMessage(json{{"type", "set_assets_root"}, {"requestId", 6}, {"path", pathToUtf8(b.path / "nope")}}.dump());
    CHECK(json::parse(out.toSender.at(0))["ok"] == false);
    CHECK(out.toAll.empty());
    CHECK(json::parse(router.onMessage(R"({"type":"set_assets_root","requestId":7,"path":""})").toSender.at(0))["ok"] == false);
    CHECK(scene.assetsRoot() == b.path.lexically_normal());
}

TEST_CASE("router: open and save remember the last map") {
    TempDir d, cfg;
    d.write("maps/a.map.json", docJson({}));
    d.write("maps/b.map.json", docJson({}));
    Scene scene(d.path);
    scene.openDocument("maps/a.map.json");
    Router router(scene, cfg.path / "settings.json");
    router.onMessage(R"({"type":"open_document","requestId":1,"path":"maps/b.map.json"})");
    CHECK(loadSettings(cfg.path / "settings.json").lastMap == "maps/b.map.json");
    router.onMessage(R"({"type":"save_document","requestId":2,"path":"maps/c.map.json"})");
    CHECK(loadSettings(cfg.path / "settings.json").lastMap == "maps/c.map.json");
}

TEST_CASE("router: onTick pushes a changed prefab library exactly once") {
    TempDir d;
    d.write("maps/a.map.json", docJson({}, {instanceJson("pi", "prefabs/p.map.json")}));
    d.write("prefabs/p.map.json", docJson({entityJson("e", "func_detail", {lPrism("pb")})}));
    Scene scene(d.path);
    scene.openDocument("maps/a.map.json");
    Router router(scene);
    CHECK(router.onTick().toAll.empty());  // nothing changed since construction

    d.write("prefabs/p.map.json", docJson({entityJson("e2", "func_detail", {lPrism("pb")})}));
    bump(d.path / "prefabs/p.map.json");
    RouterOutput out = router.onTick();
    REQUIRE(out.toAll.size() == 1);
    const json lib = json::parse(out.toAll[0]);
    CHECK(lib["type"] == "prefab_library");
    CHECK(lib["prefabs"]["prefabs/p.map.json"]["entities"][0]["id"] == "e2");
    CHECK(router.onTick().toAll.empty());  // and only once
}

TEST_CASE("router: onTick reloads the profile and definitions when they change on disk") {
    TempDir d;
    d.write("maps/a.map.json", docJson({}));
    std::ofstream(d.path / "entities.ent") << R"(<point name="one"/>)";
    Scene scene(d.path);
    scene.loadGameProfile();
    scene.openDocument("maps/a.map.json");
    Router router(scene);
    CHECK(router.onTick().toAll.empty());

    std::ofstream(d.path / "entities.ent") << R"(<point name="one"/><point name="two"/>)";
    bump(d.path / "entities.ent");
    RouterOutput out = router.onTick();
    CHECK(find(out.toAll, "entity_defs")["classes"].size() == 2);
    CHECK(find(out.toAll, "profile")["type"] == "profile");
    CHECK(router.onTick().toAll.empty());

    // a broken game_profile.json is reported once and the old profile stays in force
    d.write("game_profile.json", json{{"textureRoot", "../oops"}});
    out = router.onTick();
    CHECK(find(out.toAll, "error")["type"] == "error");
    CHECK(router.onTick().toAll.empty());
    CHECK(scene.profile().textureRoot == "textures");
}

TEST_CASE("router: write_prefab creates a prefab file, validated and sandboxed, that read_prefab then returns") {
    TempDir d;
    Scene scene(d.path);
    Router router(scene);
    const json doc = {{"schemaVersion", "0.2"}, {"entities", json::array({json{{"id", "e1"}, {"classname", "info_player_start"}, {"origin", json::array({1, 2, 3})}}})}, {"prefabInstances", json::array()}};
    json r = json::parse(router.onMessage(json{{"type", "write_prefab"}, {"requestId", 5}, {"path", "./prefabs/new/crate.map.json"}, {"doc", doc}}.dump()).toSender.at(0));
    CHECK(r["ok"] == true);
    CHECK(r["requestId"] == 5);
    CHECK(r["path"] == "prefabs/new/crate.map.json");
    r = json::parse(router.onMessage(R"({"type":"read_prefab","requestId":6,"path":"prefabs/new/crate.map.json"})").toSender.at(0));
    CHECK(r["ok"] == true);
    CHECK(r["doc"]["entities"][0]["origin"][2] == 3);
    // overwriting replaces the content (no stale cache)
    json doc2 = doc;
    doc2["entities"][0]["origin"] = json::array({9, 9, 9});
    CHECK(json::parse(router.onMessage(json{{"type", "write_prefab"}, {"requestId", 7}, {"path", "prefabs/new/crate.map.json"}, {"doc", doc2}}.dump()).toSender.at(0))["ok"] == true);
    r = json::parse(router.onMessage(R"({"type":"read_prefab","requestId":8,"path":"prefabs/new/crate.map.json"})").toSender.at(0));
    CHECK(r["doc"]["entities"][0]["origin"][0] == 9);
    // refused: outside the root, wrong extension, not a document, not valid, a prefab that contains itself
    const json selfRef = {{"schemaVersion", "0.2"}, {"entities", json::array()}, {"prefabInstances", json::array({json{{"id", "p1"}, {"file", "prefabs/loop.map.json"}}})}};
    for (const auto& bad : {json{{"path", "../x.map.json"}, {"doc", doc}}, json{{"path", "/tmp/x.map.json"}, {"doc", doc}}, json{{"path", "prefabs/x.json"}, {"doc", doc}},
                            json{{"path", "prefabs/x.map.json"}, {"doc", 5}}, json{{"path", "prefabs/x.map.json"}, {"doc", json::object()}},
                            json{{"path", "prefabs/loop.map.json"}, {"doc", selfRef}}}) {
        json m = bad;
        m["type"] = "write_prefab";
        m["requestId"] = 9;
        r = json::parse(router.onMessage(m.dump()).toSender.at(0));
        CHECK(r["ok"] == false);
        CHECK(r["requestId"] == 9);
    }
    CHECK_FALSE(fs::exists(d.path / "prefabs" / "x.map.json"));
    CHECK_FALSE(fs::exists(d.path / "prefabs" / "loop.map.json"));
}

TEST_CASE("router: read_prefab returns one prefab file, sandboxed, even when the document does not use it") {
    TempDir d;
    touch(d, "prefabs/box.map.json", R"({"schemaVersion":"0.2","entities":[{"id":"e1","classname":"info_player_start","origin":[1,2,3]}],"prefabInstances":[]})");
    touch(d, "prefabs/broken.map.json", "{ not json");
    Scene scene(d.path);
    Router router(scene);
    json r = json::parse(router.onMessage(R"({"type":"read_prefab","requestId":3,"path":"./prefabs/box.map.json"})").toSender.at(0));
    CHECK(r["ok"] == true);
    CHECK(r["requestId"] == 3);
    CHECK(r["path"] == "prefabs/box.map.json");
    CHECK(r["doc"]["entities"][0]["classname"] == "info_player_start");
    for (const char* bad : {"../x.map.json", "/etc/passwd", "prefabs/broken.map.json", "prefabs/missing.map.json"}) {
        r = json::parse(router.onMessage(json{{"type", "read_prefab"}, {"requestId", 4}, {"path", bad}}.dump()).toSender.at(0));
        CHECK(r["ok"] == false);
        CHECK(r["requestId"] == 4);
    }
}
