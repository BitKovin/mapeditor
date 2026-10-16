// The Settings window's side of the protocol: recent maps, per-game launch settings, editor preferences, editing game_profile.json and
// lighting.json, starting the game, and native dialogs (which are only checked to be handed off, never shown).
#include "core/profile.h"
#include "core/router.h"
#include "core/scene.h"
#include "core/settings.h"
#include "test_util.h"
#include <doctest.h>

using namespace core;
using namespace testutil;
namespace fs = std::filesystem;

static json findMsg(const std::vector<std::string>& msgs, const char* type) {
    for (const auto& m : msgs) {
        json j = json::parse(m);
        if (j["type"] == type) return j;
    }
    return json();
}

TEST_CASE("settings: recent maps, games and editor preferences round trip; recent maps stay unique and bounded") {
    TempDir d;
    Settings s;
    for (int i = 0; i < 20; ++i) s.touchRecent("C:/g", "maps/m" + std::to_string(i) + ".map.json");
    s.touchRecent("C:/g", "maps/m5.map.json");
    CHECK(s.recentMaps.size() == Settings::kMaxRecentMaps);
    CHECK(s.recentMaps.front().path == "maps/m5.map.json");
    CHECK(std::count(s.recentMaps.begin(), s.recentMaps.end(), RecentMap{"C:/g", "maps/m5.map.json"}) == 1);
    s.games["C:/g"] = GameLaunch{"C:/g/game.exe", "-map {mapname}", "C:/g"};
    s.editor = json{{"gridSize", 0.25}};
    REQUIRE(saveSettings(d.path / "s.json", s));
    const Settings r = loadSettings(d.path / "s.json");
    CHECK(r.recentMaps == s.recentMaps);
    CHECK(r.games.at("C:/g") == s.games.at("C:/g"));
    CHECK(r.editor["gridSize"] == 0.25);
}

TEST_CASE("game profile: edits are merged into the file, validated first, and keep keys the editor does not know") {
    TempDir d;
    d.write("game_profile.json", json{{"name", "x"}, {"bsp", {{"contentFlags", {{"solid", 1}}}}}, {"lighting", {{"lightStyles", {{"12", "ma"}}}}}});
    CHECK(writeProfileFile(d.path, json{{"name", "renamed"}, {"pixelsPerMeter", 32}, {"lighting", {{"blockLight", {"func_door"}}}}}).empty());
    const json f = readProfileFile(d.path);
    CHECK(f["name"] == "renamed");
    CHECK(f["bsp"]["contentFlags"]["solid"] == 1);
    CHECK(f["lighting"]["lightStyles"]["12"] == "ma");
    CHECK(f["lighting"]["blockLight"][0] == "func_door");
    CHECK(loadProfile(d.path).pixelsPerMeter == 32);
    // refused: nothing is written
    CHECK_FALSE(writeProfileFile(d.path, json{{"textureRoot", "../outside"}}).empty());
    CHECK_FALSE(writeProfileFile(d.path, json{{"pixelsPerMeter", 0}}).empty());
    CHECK(readProfileFile(d.path)["name"] == "renamed");
    CHECK_FALSE(readProfileFile(d.path).contains("textureRoot"));
    // null removes a key (back to the default)
    CHECK(writeProfileFile(d.path, json{{"pixelsPerMeter", nullptr}}).empty());
    CHECK(loadProfile(d.path).pixelsPerMeter == 64);
}

TEST_CASE("router: write_game_profile applies at once and tells everyone; read_game_profile shows file and effective profile") {
    TempDir d;
    d.write("maps/a.map.json", docJson({}));
    Scene scene(d.path);
    scene.loadGameProfile();
    scene.openDocument("maps/a.map.json");
    Router router(scene);
    RouterOutput out = router.onMessage(R"({"type":"write_game_profile","requestId":1,"profile":{"name":"Slasher","mapRoot":"levels"}})");
    CHECK(json::parse(out.toSender.at(0))["ok"] == true);
    CHECK(findMsg(out.toAll, "profile")["profile"]["name"] == "Slasher");
    CHECK(scene.profile().mapRoot == "levels");
    CHECK(router.onTick().toAll.empty());  // the file change was already handled

    out = router.onMessage(R"({"type":"write_game_profile","requestId":2,"profile":{"mapRoot":"C:/abs"}})");
    CHECK(json::parse(out.toSender.at(0))["ok"] == false);
    CHECK(out.toAll.empty());

    const json r = json::parse(router.onMessage(R"({"type":"read_game_profile","requestId":3})").toSender.at(0));
    CHECK(r["file"]["name"] == "Slasher");
    CHECK(r["profile"]["textureRoot"] == "textures");  // effective: defaults filled in
}

TEST_CASE("router: set_settings stores the game launch per game folder and the editor preferences, and broadcasts settings") {
    TempDir d, cfg;
    d.write("maps/a.map.json", docJson({}));
    d.write("maps/b.map.json", docJson({}));
    Scene scene(d.path);
    scene.openDocument("maps/a.map.json");
    Router router(scene, cfg.path / "settings.json");
    RouterOutput out = router.onMessage(R"({"type":"set_settings","requestId":1,"game":{"exe":"C:/game/game.exe","args":"-map {mapname}"},"editor":{"flySpeed":12}})");
    CHECK(json::parse(out.toSender.at(0))["ok"] == true);
    const json st = findMsg(out.toAll, "settings");
    CHECK(st["game"]["exe"] == "C:/game/game.exe");
    CHECK(st["editor"]["flySpeed"] == 12);
    const Settings saved = loadSettings(cfg.path / "settings.json");
    CHECK(saved.games.at(pathToUtf8(scene.assetsRoot())).args == "-map {mapname}");

    // opening maps fills the recent list (newest first), which a new Router (next start) still has
    out = router.onMessage(R"({"type":"open_document","requestId":2,"path":"maps/b.map.json"})");
    CHECK(findMsg(out.toAll, "settings")["recentMaps"][0] == "maps/b.map.json");
    Router again(scene, cfg.path / "settings.json");
    const json st2 = json::parse(again.onConnect().toSender.back());
    CHECK(st2["type"] == "settings");
    CHECK(st2["recentMaps"][0] == "maps/b.map.json");
    CHECK(st2["editor"]["flySpeed"] == 12);
    out = again.onMessage(R"({"type":"set_settings","requestId":3,"clearRecent":true})");
    CHECK(findMsg(out.toAll, "settings")["recentMaps"].empty());
}

TEST_CASE("router: lighting.json is read with every key and written as a merge") {
    TempDir d, cfg;
    Scene scene(d.path);
    Router router(scene);
    CHECK(json::parse(router.onMessage(R"({"type":"read_lighting_config","requestId":1})").toSender.at(0))["ok"] == false);  // no file configured
    router.setLightingFile(cfg.path / "lighting.json");
    std::ofstream(cfg.path / "lighting.json") << R"({"bounces": false, "custom": 1})";
    json r = json::parse(router.onMessage(R"({"type":"read_lighting_config","requestId":2})").toSender.at(0));
    CHECK(r["config"]["bounces"] == false);
    CHECK(r["config"].contains("texelsPerMeter"));
    r = json::parse(router.onMessage(R"({"type":"write_lighting_config","requestId":3,"config":{"texelsPerMeter":16}})").toSender.at(0));
    CHECK(r["ok"] == true);
    CHECK(r["config"]["texelsPerMeter"] == 16);
    std::ifstream in(cfg.path / "lighting.json");
    const json f = json::parse(in);
    CHECK(f["custom"] == 1);
    CHECK(f["bounces"] == false);
}

TEST_CASE("launch: placeholders are expanded, and a missing game executable is reported") {
    const fs::path root = fs::path("C:/games/slasher/GameData");
    CHECK(expandLaunchText("-map {mapname} +load {map}", root, "maps/e1m1.bsp") == "-map e1m1 +load maps/e1m1.bsp");
    CHECK(expandLaunchText("{bsp}", root, "maps/e1m1.bsp") == pathToUtf8((root / "maps/e1m1.bsp").make_preferred()));
    TempDir d;
    Scene scene(d.path);
    Router router(scene);
    CHECK(json::parse(router.onMessage(R"({"type":"launch_game","requestId":1,"map":"maps/a.bsp"})").toSender.at(0))["ok"] == false);
    router.onMessage(R"({"type":"set_settings","requestId":2,"game":{"exe":"Z:/no/such/game.exe"}})");
    const json r = json::parse(router.onMessage(R"({"type":"launch_game","requestId":3,"map":"maps/a.bsp"})").toSender.at(0));
    CHECK(r["ok"] == false);
    CHECK(r["error"].get<std::string>().find("not found") != std::string::npos);
    CHECK(json::parse(router.onMessage(R"({"type":"launch_game","requestId":4,"map":"../x.bsp"})").toSender.at(0))["ok"] == false);
}

TEST_CASE("router: native_dialog never blocks the router: it is handed off as background work (or refused where unsupported)") {
    TempDir d;
    Scene scene(d.path);
    Router router(scene);
    RouterOutput out = router.onMessage(R"({"type":"native_dialog","requestId":9,"kind":"folder","title":"Game folder"})");
#ifdef _WIN32
    CHECK(out.toSender.empty());
    CHECK(out.background.size() == 1);  // not run here: it would show a dialog
#else
    CHECK(json::parse(out.toSender.at(0))["unsupported"] == true);
#endif
}
