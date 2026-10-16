#include "core/router.h"
#include "core/textures.h"
#include "test_util.h"
#include <doctest.h>
#include <fstream>

using namespace core;
using namespace testutil;

static void touch(const TempDir& d, const std::string& rel, const std::string& bytes = "x") {
    auto full = d.path / rel;
    std::filesystem::create_directories(full.parent_path());
    std::ofstream(full, std::ios::binary) << bytes;
}

TEST_CASE("base64 matches the RFC 4648 vectors") {
    auto enc = [](const std::string& s) { return base64Encode({s.begin(), s.end()}); };
    CHECK(enc("") == "");
    CHECK(enc("f") == "Zg==");
    CHECK(enc("fo") == "Zm8=");
    CHECK(enc("foo") == "Zm9v");
    CHECK(enc("foobar") == "Zm9vYmFy");
}

TEST_CASE("game profile: defaults, overrides, and loud failure on a broken file") {
    TempDir d;
    CHECK(loadProfile(d.path).textureRoot == "textures");
    CHECK(loadProfile(d.path).textureExtensions.front() == ".tga");

    d.write("game_profile.json", json{{"name", "q3"}, {"textureRoot", "baseq3/textures"}, {"textureExtensions", {".png", ".tga"}}});
    GameProfile p = loadProfile(d.path);
    CHECK(p.name == "q3");
    CHECK(p.textureRoot == "baseq3/textures");
    CHECK(p.textureExtensions == std::vector<std::string>{".png", ".tga"});
    CHECK(p.toJson()["name"] == "q3");

    d.write("game_profile.json", json{{"textureRoot", "../outside"}});
    CHECK_THROWS_WITH_AS(loadProfile(d.path), doctest::Contains("inside"), std::runtime_error);
    d.write("game_profile.json", json{{"textureExtensions", {"tga"}}});
    CHECK_THROWS_AS(loadProfile(d.path), std::runtime_error);
    std::ofstream(d.path / "game_profile.json") << "{nope";
    CHECK_THROWS_WITH_AS(loadProfile(d.path), doctest::Contains("not valid JSON"), std::runtime_error);
}

TEST_CASE("listTextures: recursive, extension-less, deduplicated, sorted, format-filtered") {
    TempDir d;
    touch(d, "textures/base/wall.tga");
    touch(d, "textures/base/wall.jpg");  // same texture, second format
    touch(d, "textures/base/floor.PNG"); // extension match is case-insensitive
    touch(d, "textures/top.jpg");
    touch(d, "textures/base/readme.txt");
    touch(d, "elsewhere/nope.tga");
    CHECK(listTextures(d.path, GameProfile{}) == std::vector<std::string>{"base/floor", "base/wall", "top"});
    CHECK(listTextures(d.path, GameProfile{}, 2).size() == 2);  // limit respected
    CHECK(listTextures(d.path / "missing", GameProfile{}).empty());
}

TEST_CASE("readTexture: extension priority follows the profile, and unsafe paths are refused") {
    TempDir d;
    touch(d, "textures/a/t.tga", "TGA");
    touch(d, "textures/a/t.jpg", "JPG");
    auto t = readTexture(d.path, GameProfile{}, "a/t");
    REQUIRE(t);
    CHECK(t->ext == ".tga");
    CHECK(std::string(t->bytes.begin(), t->bytes.end()) == "TGA");

    GameProfile jpgFirst;
    jpgFirst.textureExtensions = {".jpg", ".tga"};
    CHECK(readTexture(d.path, jpgFirst, "a/t")->ext == ".jpg");

    CHECK_FALSE(readTexture(d.path, GameProfile{}, "a/missing"));
    for (const char* bad : {"", "../textures/a/t", "a/../../x", "/etc/passwd", "a\\t", "C:/x", "a/t*"})
        CHECK_FALSE(readTexture(d.path, GameProfile{}, bad));
    CHECK_FALSE(readTexture(d.path, GameProfile{}, "a/t", 2));  // over the size cap
}

TEST_CASE("router: list_textures and read_texture") {
    TempDir d;
    d.write("maps/a.map.json", docJson({}));
    touch(d, "textures/dev/grey.png", "PNGDATA");
    Scene scene(d.path);
    scene.openDocument("maps/a.map.json");
    Router router(scene);

    RouterOutput out = router.onMessage(R"({"type":"list_textures","requestId":1})");
    REQUIRE(out.toSender.size() == 1);
    json r = json::parse(out.toSender[0]);
    CHECK(r["ok"] == true);
    CHECK(r["textures"] == json::array({"dev/grey"}));

    r = json::parse(router.onMessage(R"({"type":"read_texture","requestId":2,"path":"dev/grey"})").toSender[0]);
    CHECK(r["ok"] == true);
    CHECK(r["ext"] == ".png");
    CHECK(r["data"] == base64Encode({'P', 'N', 'G', 'D', 'A', 'T', 'A'}));

    r = json::parse(router.onMessage(R"({"type":"read_texture","requestId":3,"path":"../secret"})").toSender[0]);
    CHECK(r["ok"] == false);
    CHECK(json::parse(router.onMessage(R"({"type":"read_texture","requestId":4})").toSender[0])["type"] == "error");
}

TEST_CASE("texture properties: sanitising keeps what is valid and clamps the rest") {
    json p = {{"pixelsPerMeter", 1e9}, {"junk", 1},
              {"repeat", {{"v", {{"mode", "anchor"}, {"anchor", "end"}, {"split", 7}, {"fill", "bogus"},
                                  {"alt", {{"texture", "wall/pt_1_1"}, {"offset", {0.25, 1}}}}}},
                          {"u", {{"mode", "nope"}}}}}};
    json s = sanitizeTextureProps(p);
    CHECK(s["pixelsPerMeter"] == 4096);
    CHECK_FALSE(s.contains("junk"));
    CHECK(s["repeat"]["v"]["split"] == 0.98);
    CHECK_FALSE(s["repeat"]["v"].contains("fill"));
    CHECK(s["repeat"]["v"]["alt"]["texture"] == "wall/pt_1_1");
    CHECK(s["repeat"]["v"]["alt"]["offset"] == json::array({0.25, 1.0}));
    CHECK_FALSE(s["repeat"]["u"].contains("mode"));
    CHECK(sanitizeTextureProps(json::array()).empty());
    // hotspots: valid rectangles clamped into the texture, junk dropped
    s = sanitizeTextureProps({{"hotspots", json::array({{{"x", -1}, {"y", 0.5}, {"w", 5}, {"h", 0.2}}, {{"x", "a"}}, 7, {{"x", 0.2}, {"y", 0.2}, {"w", 0.001}, {"h", 0.3}}})}});
    REQUIRE(s["hotspots"].size() == 2);
    CHECK(s["hotspots"][0]["x"] == 0.0);
    CHECK(s["hotspots"][0]["w"] == 1.0);
    CHECK(s["hotspots"][1]["w"] == 0.01);
    CHECK(sanitizeTextureProps({{"hotspots", "x"}}).contains("hotspots") == false);
    // edge handling: only auto / wrap / clamp survive
    s = sanitizeTextureProps({{"repeat", {{"u", {{"edge", "clamp"}}}, {"v", {{"edge", "sideways"}}}}}});
    CHECK(s["repeat"]["u"]["edge"] == "clamp");
    CHECK_FALSE(s["repeat"]["v"].contains("edge"));
    // an unsafe alternate path is dropped to null; a negative ppm is dropped
    s = sanitizeTextureProps({{"pixelsPerMeter", -2}, {"repeat", {{"v", {{"alt", {{"texture", "../x"}}}}}}}});
    CHECK_FALSE(s.contains("pixelsPerMeter"));
    CHECK(s["repeat"]["v"]["alt"].is_null());
}

TEST_CASE("texture properties: a relief mesh is clamped, junk faces dropped, null removes it") {
    json r = {{"vertices", json::array({{0, 0, 0}, {2, 0, -20}, {1, 1, 0.5}, {0, 1, 0}, "bad"})},
              {"faces", json::array({{0, 1, 2, 3}, {0, 1}, {0, 1, 9}, {0, 0, 1}, {0, 1, 2}})}};
    json s = sanitizeTextureProps({{"relief", r}});
    REQUIRE(s["relief"].is_object());
    CHECK(s["relief"]["vertices"][1] == json::array({1.0, 0.0, -8.0}));
    CHECK(s["relief"]["vertices"].size() == 5);
    CHECK(s["relief"]["faces"].size() == 2);  // too short, out of range and repeated-vertex faces dropped
    CHECK(sanitizeTextureProps({{"relief", json::object()}})["relief"].is_null());
    CHECK(sanitizeTextureProps({{"relief", nullptr}})["relief"].is_null());
    CHECK_FALSE(sanitizeTextureProps({{"pixelsPerMeter", 8}}).contains("relief"));
}

TEST_CASE("texture properties: stored next to the texture file, merged, and returned with the image") {
    TempDir d;
    d.write("maps/a.map.json", docJson({}));
    touch(d, "textures/wall/pt_1.jpg", "JPG");
    Scene scene(d.path);
    scene.openDocument("maps/a.map.json");
    Router router(scene);

    json r = json::parse(router.onMessage(R"({"type":"read_texture","requestId":1,"path":"wall/pt_1"})").toSender[0]);
    CHECK_FALSE(r.contains("props"));  // no sidecar yet

    r = json::parse(router.onMessage(R"({"type":"write_texture_props","requestId":2,"path":"wall/pt_1","props":{"pixelsPerMeter":96,"repeat":{"v":{"mode":"anchor","anchor":"end","split":0.4,"fill":"mirror","alt":null}}}})").toSender[0]);
    CHECK(r["ok"] == true);
    CHECK(std::filesystem::exists(d.path / "textures/wall/pt_1.jpg.json"));  // <texture file>.json, extension included

    r = json::parse(router.onMessage(R"({"type":"read_texture","requestId":3,"path":"wall/pt_1"})").toSender[0]);
    CHECK(r["props"]["pixelsPerMeter"] == 96);
    CHECK(r["props"]["repeat"]["v"]["mode"] == "anchor");

    // unknown keys written by hand survive an edit; omitting pixelsPerMeter resets it to the profile default
    std::ofstream(d.path / "textures/wall/pt_1.jpg.json") << R"({"pixelsPerMeter":96,"note":"keep me","repeat":{}})";
    r = json::parse(router.onMessage(R"({"type":"write_texture_props","requestId":4,"path":"wall/pt_1","props":{"repeat":{"u":{"mode":"mirror"}}}})").toSender[0]);
    CHECK(r["ok"] == true);
    std::ifstream in(d.path / "textures/wall/pt_1.jpg.json");
    json file = json::parse(in);
    CHECK(file["note"] == "keep me");
    CHECK_FALSE(file.contains("pixelsPerMeter"));
    CHECK(file["repeat"]["u"]["mode"] == "mirror");

    // refused: missing texture, unsafe path
    CHECK(json::parse(router.onMessage(R"({"type":"write_texture_props","requestId":5,"path":"nope/x","props":{}})").toSender[0])["ok"] == false);
    CHECK(json::parse(router.onMessage(R"({"type":"write_texture_props","requestId":6,"path":"../x","props":{}})").toSender[0])["ok"] == false);
    // a damaged sidecar means defaults, never an error
    std::ofstream(d.path / "textures/wall/pt_1.jpg.json") << "{broken";
    r = json::parse(router.onMessage(R"({"type":"read_texture","requestId":7,"path":"wall/pt_1"})").toSender[0]);
    CHECK(r["ok"] == true);
    CHECK_FALSE(r.contains("props"));
}

TEST_CASE("game profile: pixelsPerMeter defaults to 64, is validated, and is sent to the Shell") {
    TempDir d;
    CHECK(loadProfile(d.path).pixelsPerMeter == 64);
    d.write("game_profile.json", json{{"pixelsPerMeter", 128}});
    GameProfile p = loadProfile(d.path);
    CHECK(p.pixelsPerMeter == 128);
    CHECK(p.toJson()["pixelsPerMeter"] == 128);
    d.write("game_profile.json", json{{"pixelsPerMeter", 0}});
    CHECK_THROWS_AS(loadProfile(d.path), std::runtime_error);
    d.write("game_profile.json", json{{"pixelsPerMeter", "lots"}});
    CHECK_THROWS_AS(loadProfile(d.path), std::runtime_error);
}
