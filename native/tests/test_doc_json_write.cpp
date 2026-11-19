// T2.3: typed node -> JSON writers. Writing back an unchanged node gives its old JSON; a change touches exactly the keys it changed;
// unknown keys survive; a node written from scratch reads back to the same typed value. Headless, no GPU.
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "doc/document.h"
#include "doc/io.h"
#include "doc/json_write.h"

namespace fs = std::filesystem;
using namespace native;
using Json = nlohmann::json;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const fs::path kLvl2 = "F:/Git/SdlWasmEngine/source/Games/ShooterGame/GameData/maps/lvl2.map.json";

std::vector<fs::path> shippedDocuments() {
    std::vector<fs::path> out;
    for (const char* sub : {"maps", "prefabs"}) {
        const fs::path dir = fs::path(NATIVE_CORE_ASSETS_DIR) / sub;
        if (!fs::exists(dir)) continue;
        for (const auto& e : fs::directory_iterator(dir)) {
            const std::string name = e.path().filename().string();
            if (e.is_regular_file() && name.size() > 9 && name.compare(name.size() - 9, 9, ".map.json") == 0) out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Counts the nodes checked so the test cannot pass on an empty document.
struct Counts {
    size_t brushes = 0, entities = 0, instances = 0;
};

void checkIdentity(const doc::DocumentVersion& v, Counts& c) {
    for (const auto& en : v.entities) {
        const Json w = doc::writeEntity(en->typed, en->raw);
        CHECK(w == en->raw);
        ++c.entities;
        for (const auto& bn : en->brushes) {
            const Json wb = doc::writeBrush(bn->brush, bn->raw);
            CHECK(wb == bn->raw);
            ++c.brushes;
        }
    }
    for (const auto& in : v.instances) {
        const Json w = doc::writeInstance(in->typed, in->raw);
        CHECK(w == in->raw);
        ++c.instances;
    }
}

std::shared_ptr<const doc::DocumentVersion> loadOrFail(const fs::path& p) {
    const doc::LoadResult r = doc::loadDocumentFile(p);
    INFO(p.string() << ": " << r.error);
    REQUIRE(r.version);
    return r.version;
}

// First brush of the demo map, with unknown keys at brush and face level.
Json demoBrush() {
    const Json demo = Json::parse(readFile(fs::path(NATIVE_CORE_ASSETS_DIR) / "maps" / "demo.map.json"));
    for (const auto& e : demo.at("entities"))
        if (e.contains("brushes") && !e["brushes"].empty()) {
            Json b = e["brushes"][0];
            b["editorColor"] = "#ff0000";
            b["faces"][0]["weirdFaceKey"] = 7;
            return b;
        }
    FAIL("demo map has no brush");
    return {};
}

// The set of JSON-pointer paths at which two values differ (replace / add / remove), by the library's diff.
std::vector<std::string> diffPaths(const Json& a, const Json& b) {
    std::vector<std::string> out;
    for (const auto& op : Json::diff(a, b)) out.push_back(op.at("op").get<std::string>() + " " + op.at("path").get<std::string>());
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

TEST_CASE("json write: an unchanged node writes back to its old JSON, every shipped map and prefab") {
    Counts c;
    for (const fs::path& p : shippedDocuments()) {
        const auto v = loadOrFail(p);
        checkIdentity(*v, c);
    }
    CHECK(c.brushes > 0);
    CHECK(c.entities > 0);
}

TEST_CASE("json write: an unchanged node writes back to its old JSON, lvl2") {
    if (!fs::exists(kLvl2)) {
        MESSAGE("lvl2 not found, skipped");
        return;
    }
    const auto v = loadOrFail(kLvl2);
    Counts c;
    checkIdentity(*v, c);
    CHECK(c.brushes > 1000);
    CHECK(c.entities > 100);
}

TEST_CASE("json write: moving one vertex changes exactly that vertex") {
    const Json raw = demoBrush();
    core::Brush b = core::brushFromJson(raw);
    REQUIRE(b.vertices.size() > 3);
    b.vertices[2].x += 0.25;
    const Json w = doc::writeBrush(b, raw);
    const auto d = diffPaths(raw, w);
    REQUIRE(d.size() == 1);
    CHECK(d[0] == "replace /vertices/2/0");
    CHECK(core::brushFromJson(w).vertices[2].x == doctest::Approx(b.vertices[2].x));

    // all three components of one vertex: still only that vertex
    core::Brush b2 = core::brushFromJson(raw);
    b2.vertices[1] = {b2.vertices[1].x + 1, b2.vertices[1].y + 2, b2.vertices[1].z + 3};
    for (const std::string& s : diffPaths(raw, doc::writeBrush(b2, raw))) CHECK(s.find("/vertices/1/") != std::string::npos);
}

TEST_CASE("json write: face edits touch only that face's keys and unknown keys stay") {
    const Json raw = demoBrush();
    core::Brush b = core::brushFromJson(raw);
    REQUIRE(b.faces.size() >= 2);

    b.faces[1].texture = "base_wall/concrete01";
    Json w = doc::writeBrush(b, raw);
    CHECK(diffPaths(raw, w) == std::vector<std::string>{"replace /faces/1/texture"});
    CHECK(w["faces"][0]["weirdFaceKey"] == 7);
    CHECK(w["editorColor"] == "#ff0000");

    core::Brush b2 = core::brushFromJson(raw);
    b2.faces[0].lightmapScale = 4;
    w = doc::writeBrush(b2, raw);
    CHECK(w["faces"][0]["lightmapScale"] == 4);
    CHECK(core::brushFromJson(w).faces[0].lightmapScale == 4);
    b2.faces[0].lightmapScale = 1;   // back to the default: the key goes away
    CHECK_FALSE(doc::writeBrush(b2, w)["faces"][0].contains("lightmapScale"));

    core::Brush b3 = core::brushFromJson(raw);
    b3.faces[0].projection.rotate = 15;
    b3.faces[0].projection.shiftU = 0.5;
    w = doc::writeBrush(b3, raw);
    const core::Brush r3 = core::brushFromJson(w);
    CHECK(r3.faces[0].projection.rotate == 15);
    CHECK(r3.faces[0].projection.shiftU == 0.5);
    CHECK(r3.faces[0].projection.scaleU == b3.faces[0].projection.scaleU);
    CHECK(w["faces"][0]["weirdFaceKey"] == 7);

    core::Brush b4 = core::brushFromJson(raw);
    b4.faces[0].uvs[0].u += 0.125;
    w = doc::writeBrush(b4, raw);
    CHECK(core::brushFromJson(w).faces[0].uvs[0].u == doctest::Approx(b4.faces[0].uvs[0].u));
}

TEST_CASE("json write: faces added and removed, op, carve and smooth edges") {
    const Json raw = demoBrush();
    core::Brush b = core::brushFromJson(raw);
    const size_t n = b.faces.size();
    REQUIRE(n >= 3);

    core::Brush removed = b;
    removed.faces.erase(removed.faces.begin());
    Json w = doc::writeBrush(removed, raw);
    CHECK(w["faces"].size() == n - 1);
    CHECK(w["faces"][0] == raw["faces"][1]);   // surviving faces keep their old JSON

    core::Brush added = b;
    core::BrushFace nf = b.faces[0];
    nf.id = 9999;
    nf.texture = "dev/new";
    added.faces.push_back(nf);
    w = doc::writeBrush(added, raw);
    REQUIRE(w["faces"].size() == n + 1);
    CHECK(w["faces"][n]["id"] == 9999);
    CHECK(w["faces"][n]["texture"] == "dev/new");
    CHECK(w["faces"][0] == raw["faces"][0]);

    core::Brush sub = b;
    sub.op = core::BrushOp::Subtract;
    sub.carve.nonStructural = true;
    sub.smoothEdges = {{1, 2}, {3, 4}};
    w = doc::writeBrush(sub, raw);
    CHECK(w["op"] == "subtract");
    const core::Brush rs = core::brushFromJson(w);
    CHECK(rs.op == core::BrushOp::Subtract);
    CHECK(rs.carve.nonStructural);
    CHECK(rs.smoothEdges == sub.smoothEdges);
    sub.op = core::BrushOp::Add;
    sub.smoothEdges.clear();
    w = doc::writeBrush(sub, w);
    CHECK_FALSE(w.contains("op"));
    CHECK_FALSE(w.contains("smooth"));
}

TEST_CASE("json write: a brush written from scratch reads back to the same typed value and is stable") {
    core::Brush b = core::Brush::makeBox("b_new", {0, 0, 0}, {2, 3, 4}, "dev/grey");
    const Json w = doc::writeBrush(b, Json());
    const core::Brush r = core::brushFromJson(w);
    CHECK(r.id == b.id);
    CHECK(r.vertices.size() == b.vertices.size());
    CHECK(r.faces.size() == b.faces.size());
    CHECK(doc::writeBrush(r, w) == w);
    CHECK(doc::writeBrush(r, Json()) == w);
    CHECK(doc::numberJson(2.0).is_number_integer());
    CHECK(doc::numberJson(2.5).is_number_float());
    CHECK(w["vertices"][1].dump().find('.') == std::string::npos);   // integral coordinates are integers
}

TEST_CASE("json write: entity edits patch keyvalues, refs, origin and keep unknown keys") {
    const Json raw = Json::parse(R"({
      "id": "e1", "classname": "light_point",
      "keyvalues": {"intensity": "5", "color": "1 1 1"},
      "refs": {"model": "models/a.obj"},
      "origin": [1, 2, 3], "editorNote": {"a": [1, 2]}
    })");
    core::Entity e = core::entityFromJson(raw);
    CHECK(doc::writeEntity(e, raw) == raw);

    e.keyvalues["intensity"] = "9";
    e.keyvalues["newkey"] = "x";
    e.keyvalues.erase("color");
    Json w = doc::writeEntity(e, raw);
    CHECK(w["keyvalues"] == Json::parse(R"({"intensity": "9", "newkey": "x"})"));
    CHECK(w["editorNote"] == raw["editorNote"]);
    CHECK(w["origin"] == raw["origin"]);

    core::Entity e2 = core::entityFromJson(raw);
    e2.origin = {1, 2, 7.5};
    e2.refs["model"] = core::RefTargets(std::vector<std::string>{"models/a.obj", "models/b.obj"});
    w = doc::writeEntity(e2, raw);
    CHECK(diffPaths(raw, w) == std::vector<std::string>{"replace /origin/2", "replace /refs/model"});
    const core::Entity r2 = core::entityFromJson(w);
    CHECK(r2.refs.at("model").paths.size() == 2);

    core::Entity e3 = core::entityFromJson(raw);
    e3.keyvalues.clear();
    e3.refs.clear();
    w = doc::writeEntity(e3, raw);
    CHECK_FALSE(w.contains("keyvalues"));
    CHECK_FALSE(w.contains("refs"));

    // a brushes array is not the entity writer's business
    Json withBrushes = raw;
    withBrushes["brushes"] = Json::array();
    CHECK(doc::writeEntity(core::entityFromJson(raw), withBrushes) == withBrushes);

    // from scratch
    core::Entity fresh;
    fresh.id = "e_new";
    fresh.classname = "info_player_start";
    fresh.keyvalues["k"] = "v";
    fresh.origin = {4, 5, 6};
    const Json fw = doc::writeEntity(fresh, Json());
    const core::Entity fr = core::entityFromJson(fw);
    CHECK(fr.id == "e_new");
    CHECK(fr.keyvalues.at("k") == "v");
    CHECK(fr.origin.z == 6);
    CHECK(doc::writeEntity(fr, fw) == fw);
}

TEST_CASE("json write: prefab instance edits and overrides") {
    const Json raw = Json::parse(R"({
      "id": "i1", "file": "prefabs/door.map.json", "origin": [0, 0, 0], "editorTag": 3,
      "overrides": {
        "ent_a": {"keyvalues": {"speed": "2"}, "origin": [1, 1, 1], "extra": true},
        "ent_b": {"deleted": true}
      }
    })");
    core::PrefabInstance p = core::instanceFromJson(raw);
    CHECK(doc::writeInstance(p, raw) == raw);

    p.origin = {5, 0, 0};
    p.name = "door 1";
    Json w = doc::writeInstance(p, raw);
    CHECK(diffPaths(raw, w) == std::vector<std::string>{"add /name", "replace /origin/0"});
    CHECK(w["editorTag"] == 3);
    CHECK(w["overrides"] == raw["overrides"]);

    core::PrefabInstance p2 = core::instanceFromJson(raw);
    p2.overrides["ent_a"].keyvalues["speed"] = "4";
    p2.overrides["ent_b"].deleted = false;
    p2.overrides["ent_c"].keyvalues["k"] = "v";
    w = doc::writeInstance(p2, raw);
    CHECK(w["overrides"]["ent_a"]["keyvalues"]["speed"] == "4");
    CHECK(w["overrides"]["ent_a"]["extra"] == true);   // unknown key inside an override stays
    CHECK(w["overrides"]["ent_a"]["origin"] == raw["overrides"]["ent_a"]["origin"]);
    CHECK_FALSE(w["overrides"]["ent_b"].contains("deleted"));
    CHECK(w["overrides"]["ent_c"]["keyvalues"]["k"] == "v");
    const core::PrefabInstance r2 = core::instanceFromJson(w);
    CHECK(r2.overrides.at("ent_a").keyvalues.at("speed") == "4");
    CHECK_FALSE(r2.overrides.at("ent_b").deleted);

    core::PrefabInstance p3 = core::instanceFromJson(raw);
    p3.overrides.erase("ent_a");
    p3.overrides.erase("ent_b");
    CHECK_FALSE(doc::writeInstance(p3, raw).contains("overrides"));

    core::PrefabInstance fresh;
    fresh.id = "i_new";
    fresh.file = "prefabs/x.map.json";
    fresh.scale = core::Vec3{2, 2, 2};
    const Json fw = doc::writeInstance(fresh, Json());
    const core::PrefabInstance fr = core::instanceFromJson(fw);
    CHECK(fr.file == fresh.file);
    REQUIRE(fr.scale.has_value());
    CHECK(fr.scale->x == 2);
    CHECK(doc::writeInstance(fr, fw) == fw);
}
