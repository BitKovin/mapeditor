// Exporters (REQUIREMENTS §31): the registry, the game profile's format, the OBJ exporter's files.
#include "core/exporter.h"
#include "core/profile.h"
#include "core/router.h"
#include "core/scene.h"
#include "doctest.h"
#include "test_util.h"
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

using namespace core;
using namespace testutil;
using nlohmann::json;

namespace {

std::string readAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

}  // namespace

TEST_CASE("exporters: registry, unknown ids fall back to rBSP, the path takes the format's extension") {
    REQUIRE(findExporter("rbsp") != nullptr);
    REQUIRE(findExporter("obj") != nullptr);
    CHECK(findExporter("rbsp")->extension == ".bsp");
    CHECK(findExporter("obj")->extension == ".obj");
    CHECK(findExporter("md3") == nullptr);
    std::string warning;
    CHECK(exporterFor("md3", &warning).id == "rbsp");
    CHECK(warning.find("md3") != std::string::npos);
    warning.clear();
    CHECK(exporterFor("obj", &warning).id == "obj");
    CHECK(warning.empty());
    const Exporter& obj = *findExporter("obj");
    CHECK(withExporterExtension("maps/a.bsp", obj) == "maps/a.obj");
    CHECK(withExporterExtension("maps/a.map.json", obj) == "maps/a.obj");
    CHECK(withExporterExtension("maps/a", obj) == "maps/a.obj");
    CHECK(withExporterExtension("maps/a.obj", obj) == "maps/a.obj");
    CHECK(withExporterExtension("maps/v1.2/a.bsp", *findExporter("rbsp")) == "maps/v1.2/a.bsp");
}

TEST_CASE("exporters: the format is a game profile setting") {
    CHECK(GameProfile{}.exporter == "rbsp");
    CHECK(profileFromJson(json{{"exporter", "obj"}}).exporter == "obj");
    CHECK(profileFromJson(json{{"exporter", "obj"}}).toJson()["exporter"] == "obj");
    CHECK_THROWS(profileFromJson(json{{"exporter", 3}}));
}

TEST_CASE("exporters: OBJ export writes <name>.obj, <name>.json, <name>.lightvol and <name>/lm_NNNN.exr in the editor's space") {
    TempDir dir;
    dir.write("game_profile.json", json{{"exporter", "obj"}});
    json light = entityJson("lamp", "light_point", {}, {0, 2, 0});
    light["keyvalues"] = {{"brightness", "8"}, {"radius", "20"}};
    json door = entityJson("d", "func_door", {Brush::makeBox("door", {1, 0, 1}, {2, 2, 1.2})});
    const json doc = docJson({entityJson("w", "worldspawn", {Brush::makeBox("floor", {-4, -1, -4}, {4, 0, 4}), Brush::makeBox("wall", {-4, 0, -4}, {4, 3, -3.5})}),
                              light, door, entityJson("p", "info_player_start", {}, {1, 0.5, 1})});
    dir.write("maps/room.map.json", doc);
    Scene scene(dir.path);
    scene.loadGameProfile();
    scene.openDocument("maps/room.map.json");
    LightingService::Options opt;
    opt.live.maxPasses = 2;
    opt.live.useGpu = false;
    opt.live.denoiseIndirect = false;
    opt.live.giScale = 1;
    opt.lightvol.useGpu = false;
    opt.lightvol.gridSize = 2;
    opt.lightvol.normals = 8;
    opt.lightvol.giSamples = 4;
    Router router(scene, {}, opt);

    // the profile message names the game's format and lists the formats
    const RouterOutput hello = router.onConnect();
    bool sawProfile = false;
    for (const auto& m : hello.toSender) {
        const json j = json::parse(m);
        if (j["type"] != "profile") continue;
        sawProfile = true;
        CHECK(j["exporter"] == "obj");
        CHECK(j["exporters"].size() == exporters().size());
    }
    CHECK(sawProfile);

    const RouterOutput started = router.onMessage(R"({"type":"export_map","path":"maps/room.bsp","requestId":1})");
    REQUIRE(started.toSender.size() == 1);
    const json ack = json::parse(started.toSender[0]);
    CHECK(ack["ok"] == true);
    CHECK(ack["path"] == "maps/room.obj");  // the format's extension
    CHECK(ack["format"] == "obj");
    json result;
    for (int i = 0; i < 3000 && result.is_null(); ++i) {
        for (const auto& m : router.onTick().toAll) if (json::parse(m)["type"] == "export_result") result = json::parse(m);
        if (result.is_null()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(!result.is_null());
    INFO(result.dump());
    REQUIRE(result["ok"] == true);
    CHECK(result["format"] == "obj");
    CHECK(result["bsp"] == "maps/room.obj");
    CHECK(result["lightmaps"] == "maps/room");
    CHECK(std::filesystem::exists(dir.path / "maps/room/lm_0000.exr"));
    CHECK_FALSE(std::filesystem::exists(dir.path / "maps/room.bsp"));

    // .obj: every face index in range, faces counter-clockwise (their normal agrees with the vertex normals), metres and Y up
    std::istringstream obj(readAll(dir.path / "maps/room.obj"));
    std::vector<Vec3> v, vn;
    size_t vt = 0, vl = 0, faces = 0, badWinding = 0, outOfRange = 0;
    std::vector<std::string> objects, groups;
    double minY = 1e9, maxY = -1e9;
    for (std::string line; std::getline(obj, line);) {
        std::istringstream l(line);
        std::string t;
        l >> t;
        if (t == "v") { Vec3 p; l >> p.x >> p.y >> p.z; v.push_back(p); minY = std::min(minY, p.y); maxY = std::max(maxY, p.y); }
        else if (t == "vn") { Vec3 n; l >> n.x >> n.y >> n.z; vn.push_back(n); }
        else if (t == "vt") ++vt;
        else if (t == "vl") { double u = -1, w = -1; l >> u >> w; ++vl; CHECK((u >= 0 && u <= 1 && w >= 0 && w <= 1)); }
        else if (t == "o") { std::string n; l >> n; objects.push_back(n); }
        else if (t == "g") { std::string n; l >> n; groups.push_back(n); }
        else if (t == "f") {
            ++faces;
            size_t idx[3];
            for (size_t& i : idx) { std::string c; l >> c; i = std::stoul(c.substr(0, c.find('/'))); }
            for (size_t i : idx) if (i == 0 || i > v.size() || i > vn.size()) ++outOfRange;
            if (outOfRange) continue;
            const Vec3 &a = v[idx[0] - 1], &b = v[idx[1] - 1], &c = v[idx[2] - 1];
            if ((b - a).cross(c - a).dot(vn[idx[0] - 1]) <= 0) ++badWinding;
        }
    }
    CHECK(faces > 0);
    CHECK(outOfRange == 0);
    CHECK(badWinding == 0);
    CHECK(v.size() == vn.size());
    CHECK(vt == v.size());
    CHECK(vl == v.size());
    CHECK(minY == doctest::Approx(-1).epsilon(0.01));  // the floor's bottom, in metres, Y up
    CHECK(maxY == doctest::Approx(3).epsilon(0.01));   // the wall's top
    CHECK(objects.size() == 2);                        // world and the door
    CHECK(objects[0] == "world");
    bool lit = false;
    for (const auto& g : groups) lit |= g.find("@lm_0000") != std::string::npos;
    CHECK(lit);

    // .json: the entities, worldspawn first, point entities in the editor's space, the door points at its object
    const json ents = json::parse(readAll(dir.path / "maps/room.json"));
    REQUIRE(ents.is_array());
    REQUIRE(!ents.empty());
    CHECK(ents[0]["classname"] == "worldspawn");
    CHECK_FALSE(ents[0].contains("gridsize"));  // the rBSP's grid in Quake units; the .lightvol has the grid
    bool sawLight = false, sawDoor = false;
    for (const auto& e : ents) {
        if (e["classname"] == "light_point") { sawLight = true; CHECK(e["origin"] == "0 2 0"); }
        if (e["classname"] == "func_door") { sawDoor = true; CHECK(std::find(objects.begin(), objects.end(), "model_" + e["model"].get<std::string>().substr(1)) != objects.end()); }
    }
    CHECK(sawLight);
    CHECK(sawDoor);

    // .lightvol: header and one block of packed probes per light style
    const std::string vol = readAll(dir.path / "maps/room.lightvol");
    REQUIRE(vol.size() >= 40);
    CHECK(vol.substr(0, 4) == "LVOL");
    uint32_t version = 0;
    float cell = 0;
    int32_t dims[4] = {};
    std::memcpy(&version, vol.data() + 4, 4);
    std::memcpy(&cell, vol.data() + 20, 4);
    std::memcpy(dims, vol.data() + 24, 16);
    CHECK(version == 1);
    CHECK(cell == doctest::Approx(2));
    CHECK(dims[0] > 0);
    CHECK(dims[3] >= 1);
    const size_t probes = static_cast<size_t>(dims[0]) * dims[1] * dims[2];
    CHECK(vol.size() == 40 + static_cast<size_t>(dims[3]) * (4 + probes * kPackedProbeBytes));
}
