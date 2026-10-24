// Quake 3 .map importer (map_import.h, DESIGN §12b).
#include "core/map_import.h"
#include "core/lights.h"
#include "core/router.h"
#include "core/scene.h"
#include "test_util.h"
#include <cmath>
#include <doctest.h>
#include <fstream>

using namespace core;
using namespace testutil;

namespace {

// A 64 unit cube [0,64]^3, every face written by `faceTail` (what follows the three points of a face line).
std::string cube(const std::string& tex, const std::string& tail = "0 0 0 0.5 0.5 0 0 0") {
    const char* pts[6] = {"( 0 0 64 ) ( 0 64 64 ) ( 64 0 64 )",   // +z
                          "( 0 0 0 ) ( 64 0 0 ) ( 0 64 0 )",       // -z
                          "( 64 0 0 ) ( 64 0 64 ) ( 64 64 0 )",    // +x
                          "( 0 0 0 ) ( 0 64 0 ) ( 0 0 64 )",       // -x
                          "( 0 64 0 ) ( 64 64 0 ) ( 0 64 64 )",    // +y
                          "( 0 0 0 ) ( 0 0 64 ) ( 64 0 0 )"};      // -y
    std::string s = "{\n";
    for (const char* p : pts) s += std::string(p) + " " + tex + " " + tail + "\n";
    return s + "}\n";
}

std::string world(const std::string& brushes, const std::string& keys = "") {
    return "{\n\"classname\" \"worldspawn\"\n" + keys + brushes + "}\n";
}

MapImportOptions sized(int w, int h) {
    MapImportOptions o;
    o.textureSize = [w, h](const std::string&, int& ow, int& oh) { ow = w; oh = h; return true; };
    return o;
}

const json& entityOf(const MapImportResult& r, const std::string& id) {
    for (const json& e : r.doc["entities"]) if (e["id"] == id) return e;
    FAIL("no entity " << id);
    return r.doc;
}

std::vector<double> centreOf(const json& brush) {
    std::vector<double> c(3, 0.0);
    for (const json& v : brush["vertices"]) for (int i = 0; i < 3; ++i) c[static_cast<size_t>(i)] += v[static_cast<size_t>(i)].get<double>() / brush["vertices"].size();
    return c;
}

const json* faceWithNormal(const json& brush, int axis, double sign) {  // outward unit normal along editor axis
    const json& V = brush["vertices"];
    for (const json& f : brush["faces"]) {
        double n[3] = {0, 0, 0};
        const auto& loop = f["loop"];
        for (size_t i = 0; i < loop.size(); ++i) {
            const json& a = V[loop[i].get<size_t>()];
            const json& b = V[loop[(i + 1) % loop.size()].get<size_t>()];
            n[0] += (a[1].get<double>() - b[1].get<double>()) * (a[2].get<double>() + b[2].get<double>());
            n[1] += (a[2].get<double>() - b[2].get<double>()) * (a[0].get<double>() + b[0].get<double>());
            n[2] += (a[0].get<double>() - b[0].get<double>()) * (a[1].get<double>() + b[1].get<double>());
        }
        if (n[axis] * sign > 0 && std::fabs(n[axis]) > 0.9 * (std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]))) return &f;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("map import: a classic brush becomes a closed convex solid in metres, Y up") {
    const MapImportResult r = importQuakeMap(world(cube("dev/grey")), sized(64, 64));
    CHECK(r.stats.brushes == 1);
    CHECK(r.stats.skipped == 0);
    const Document doc = documentFromJson(r.doc);
    CHECK_FALSE(validateDocument(doc).has_value());
    REQUIRE(doc.entities.size() == 1);
    const Brush& b = doc.entities[0].brushes[0];
    CHECK(b.faces.size() == 6);
    CHECK(b.vertices.size() == 8);
    CHECK(b.signedVolume() == doctest::Approx(8.0));  // 64 units = 2 m
}

TEST_CASE("map import: 32 Quake units are a metre; Quake +Y (north) is the editor's -Z, Quake Z is Y") {
    const MapImportResult r = importQuakeMap(world(cube("dev/grey")), sized(64, 64));
    const json& brush = entityOf(r, "e0")["brushes"][0];
    double lo[3] = {1e9, 1e9, 1e9}, hi[3] = {-1e9, -1e9, -1e9};
    for (const json& v : brush["vertices"]) for (int i = 0; i < 3; ++i) {
        lo[i] = std::min(lo[i], v[static_cast<size_t>(i)].get<double>());
        hi[i] = std::max(hi[i], v[static_cast<size_t>(i)].get<double>());
    }
    CHECK(lo[0] == doctest::Approx(0)); CHECK(hi[0] == doctest::Approx(2));    // x east
    CHECK(lo[1] == doctest::Approx(0)); CHECK(hi[1] == doctest::Approx(2));    // y up (Quake z)
    CHECK(lo[2] == doctest::Approx(-2)); CHECK(hi[2] == doctest::Approx(0));   // z south (Quake -y)
    CHECK(faceWithNormal(brush, 1, 1) != nullptr);                              // a face looks up
}

TEST_CASE("map import: classic texture coordinates follow q3map2 (pixels, scale, shift, rotation)") {
    // floor face +z: s axis +x, t axis -y; 64 px texture, scale 0.5 -> 2 px per unit -> u = x * 2 / 64 = x / 32
    const MapImportResult r = importQuakeMap(world(cube("t/a", "16 8 0 0.5 0.5 0 0 0")), sized(64, 64));
    const json& brush = entityOf(r, "e0")["brushes"][0];
    const json* top = faceWithNormal(brush, 1, 1);
    REQUIRE(top);
    const json& V = brush["vertices"];
    for (size_t k = 0; k < (*top)["loop"].size(); ++k) {
        const json& p = V[(*top)["loop"][k].get<size_t>()];
        const double qx = p[0].get<double>() * 32, qy = -p[2].get<double>() * 32;
        CHECK((*top)["uvs"][k][0].get<double>() == doctest::Approx((qx * 2 + 16) / 64));
        CHECK((*top)["uvs"][k][1].get<double>() == doctest::Approx((-qy * 2 + 8) / 64));
    }
    CHECK((*top)["texture"] == "t/a");
    CHECK((*top)["projection"]["kind"] == "free");
}

TEST_CASE("map import: a 90 degree rotation turns the texture axes") {
    const MapImportResult r = importQuakeMap(world(cube("t/a", "0 0 90 0.5 0.5 0 0 0")), sized(64, 64));
    const json& brush = entityOf(r, "e0")["brushes"][0];
    const json* top = faceWithNormal(brush, 1, 1);
    REQUIRE(top);
    const json& V = brush["vertices"];
    for (size_t k = 0; k < (*top)["loop"].size(); ++k) {
        const json& p = V[(*top)["loop"][k].get<size_t>()];
        const double qx = p[0].get<double>() * 32, qy = -p[2].get<double>() * 32;
        // rotation 90: s = (cos * x - sin * (-y))... the s axis becomes the old t axis (q3map2 QuakeTextureVecs)
        CHECK((*top)["uvs"][k][0].get<double>() == doctest::Approx((qy * 2) / 64).epsilon(1e-6));
        CHECK((*top)["uvs"][k][1].get<double>() == doctest::Approx((qx * 2) / 64).epsilon(1e-6));
    }
}

TEST_CASE("map import: Valve 220 axes, offsets and scales") {
    std::string c = "{\n";
    c += "( 0 0 64 ) ( 0 64 64 ) ( 64 0 64 ) t/v [ 0 1 0 8 ] [ 1 0 0 4 ] 0 2 4\n";
    c += "( 0 0 0 ) ( 64 0 0 ) ( 0 64 0 ) t/v [ 1 0 0 0 ] [ 0 -1 0 0 ] 0 1 1\n";
    c += "( 64 0 0 ) ( 64 0 64 ) ( 64 64 0 ) t/v [ 0 1 0 0 ] [ 0 0 -1 0 ] 0 1 1\n";
    c += "( 0 0 0 ) ( 0 64 0 ) ( 0 0 64 ) t/v [ 0 1 0 0 ] [ 0 0 -1 0 ] 0 1 1\n";
    c += "( 0 64 0 ) ( 64 64 0 ) ( 0 64 64 ) t/v [ 1 0 0 0 ] [ 0 0 -1 0 ] 0 1 1\n";
    c += "( 0 0 0 ) ( 0 0 64 ) ( 64 0 0 ) t/v [ 1 0 0 0 ] [ 0 0 -1 0 ] 0 1 1\n}\n";
    const MapImportResult r = importQuakeMap(world(c), sized(128, 32));
    const json& brush = entityOf(r, "e0")["brushes"][0];
    const json* top = faceWithNormal(brush, 1, 1);
    REQUIRE(top);
    const json& V = brush["vertices"];
    for (size_t k = 0; k < (*top)["loop"].size(); ++k) {
        const json& p = V[(*top)["loop"][k].get<size_t>()];
        const double qx = p[0].get<double>() * 32, qy = -p[2].get<double>() * 32;
        CHECK((*top)["uvs"][k][0].get<double>() == doctest::Approx((qy / 2 + 8) / 128));
        CHECK((*top)["uvs"][k][1].get<double>() == doctest::Approx((qx / 4 + 4) / 32));
    }
}

TEST_CASE("map import: brush primitives (brushDef) use the plane's own axes and repeats") {
    std::string c = "{\nbrushDef\n{\n";
    const char* pts[6] = {"( 0 0 64 ) ( 0 64 64 ) ( 64 0 64 )", "( 0 0 0 ) ( 64 0 0 ) ( 0 64 0 )", "( 64 0 0 ) ( 64 0 64 ) ( 64 64 0 )",
                          "( 0 0 0 ) ( 0 64 0 ) ( 0 0 64 )", "( 0 64 0 ) ( 64 64 0 ) ( 0 64 64 )", "( 0 0 0 ) ( 0 0 64 ) ( 64 0 0 )"};
    for (const char* p : pts) c += std::string(p) + " ( ( 0.015625 0 0.25 ) ( 0 0.015625 0.5 ) ) t/b 0 0 0\n";
    c += "}\n}\n";
    const MapImportResult r = importQuakeMap(world(c), sized(256, 256));  // the size must not matter
    CHECK(r.stats.brushes == 1);
    const json& brush = entityOf(r, "e0")["brushes"][0];
    const json* top = faceWithNormal(brush, 1, 1);
    REQUIRE(top);
    const json& V = brush["vertices"];
    for (size_t k = 0; k < (*top)["loop"].size(); ++k) {
        const json& p = V[(*top)["loop"][k].get<size_t>()];
        const double qx = p[0].get<double>() * 32, qy = -p[2].get<double>() * 32;
        // +z: texS = (0,1,0), texT = (1,0,0) (Radiant ComputeAxisBase)
        CHECK((*top)["uvs"][k][0].get<double>() == doctest::Approx(qy / 64 + 0.25));
        CHECK((*top)["uvs"][k][1].get<double>() == doctest::Approx(qx / 64 + 0.5));
    }
}

TEST_CASE("map import: the projection stored with a face reproduces its uvs") {
    const MapImportResult r = importQuakeMap(world(cube("t/a", "5 9 30 0.75 0.5 0 0 0")), sized(64, 128));
    const json& brush = entityOf(r, "e0")["brushes"][0];
    const json& V = brush["vertices"];
    for (const json& f : brush["faces"]) {
        REQUIRE(f.contains("projection"));
        const json& pj = f["projection"];
        for (size_t k = 0; k < f["loop"].size(); ++k) {
            const json& p = V[f["loop"][k].get<size_t>()];
            double u = pj["shift"][0].get<double>(), v = pj["shift"][1].get<double>();
            for (int i = 0; i < 3; ++i) {
                u += p[static_cast<size_t>(i)].get<double>() * pj["basis"][0][static_cast<size_t>(i)].get<double>() / pj["scale"][0].get<double>();
                v += p[static_cast<size_t>(i)].get<double>() * pj["basis"][1][static_cast<size_t>(i)].get<double>() / pj["scale"][1].get<double>();
            }
            CHECK(u == doctest::Approx(f["uvs"][k][0].get<double>()));
            CHECK(v == doctest::Approx(f["uvs"][k][1].get<double>()));
        }
    }
}

TEST_CASE("map import: entities keep their keys; origin, angles and references are converted") {
    const std::string text = world(cube("t/a")) +
        "{\n\"classname\" \"info_player_start\"\n\"origin\" \"64 96 32\"\n\"angle\" \"90\"\n\"targetname\" \"start\"\n}\n"
        "{\n\"classname\" \"trigger_once\"\n\"target\" \"start\"\n\"killtarget\" \"nobody\"\n\"origin\" \"0 0 0\"\n}\n"
        "{\n\"classname\" \"misc_model\"\n\"origin\" \"0 0 0\"\n\"angles\" \"0 90 0\"\n\"model\" \"models/a.md3\"\n}\n";
    const MapImportResult r = importQuakeMap(text, sized(64, 64));
    const json& start = entityOf(r, "e1");
    CHECK(start["origin"] == json::array({2.0, 1.0, -3.0}));
    CHECK(start["keyvalues"]["angle"] == "90");
    CHECK(start["keyvalues"]["targetname"] == "start");
    const json& trig = entityOf(r, "e2");
    CHECK(trig["refs"]["target"] == "e1");
    CHECK_FALSE(trig["keyvalues"].contains("target"));
    CHECK(trig["keyvalues"]["killtarget"] == "nobody");  // matches nothing: kept as text, with a warning
    CHECK(!r.warnings.empty());
    const json& model = entityOf(r, "e3");
    CHECK(model["keyvalues"]["model"] == "models/a.md3");
    CHECK(model["angles"][1].get<double>() == doctest::Approx(90));  // a yaw about Quake Z is a yaw about the editor's Y
    CHECK_FALSE(model["keyvalues"].contains("angles"));
}

TEST_CASE("map import: a target name shared by several entities becomes a reference to all of them") {
    const std::string text = world("") + "{\n\"classname\" \"a\"\n\"targetname\" \"x\"\n}\n{\n\"classname\" \"a\"\n\"targetname\" \"x\"\n}\n{\n\"classname\" \"b\"\n\"target\" \"x\"\n}\n";
    const MapImportResult r = importQuakeMap(text, sized(64, 64));
    CHECK(entityOf(r, "e3")["refs"]["target"] == json::array({"e1", "e2"}));
    CHECK_FALSE(entityOf(r, "e3")["keyvalues"].contains("target"));
}

TEST_CASE("map import: a brush entity keeps world-space brushes and gets the centre of its geometry as origin; origin brushes set the pivot") {
    const std::string door = "{\n\"classname\" \"func_door\"\n" + cube("t/a") + "}\n";
    const MapImportResult r = importQuakeMap(world("") + door, sized(64, 64));
    const json& e = entityOf(r, "e1");
    CHECK(e["brushes"].size() == 1);
    CHECK(e["origin"] == json::array({1.0, 1.0, -1.0}));

    std::string origin = "{\n( 32 32 40 ) ( 32 40 40 ) ( 40 32 40 ) common/origin 0 0 0 1 1 0 0 0\n( 32 32 32 ) ( 40 32 32 ) ( 32 40 32 ) common/origin 0 0 0 1 1 0 0 0\n"
        "( 40 32 32 ) ( 40 32 40 ) ( 40 40 32 ) common/origin 0 0 0 1 1 0 0 0\n( 32 32 32 ) ( 32 40 32 ) ( 32 32 40 ) common/origin 0 0 0 1 1 0 0 0\n"
        "( 32 40 32 ) ( 40 40 32 ) ( 32 40 40 ) common/origin 0 0 0 1 1 0 0 0\n( 32 32 32 ) ( 32 32 40 ) ( 40 32 32 ) common/origin 0 0 0 1 1 0 0 0\n}\n";
    const MapImportResult r2 = importQuakeMap(world("") + "{\n\"classname\" \"func_door\"\n" + cube("t/a") + origin + "}\n", sized(64, 64));
    const json& e2 = entityOf(r2, "e1");
    CHECK(e2["brushes"].size() == 1);                     // the origin brush is not geometry
    CHECK(e2["origin"][0].get<double>() == doctest::Approx(36.0 / 32));
    CHECK(e2["origin"][1].get<double>() == doctest::Approx(36.0 / 32));
    CHECK(e2["origin"][2].get<double>() == doctest::Approx(-36.0 / 32));
}

TEST_CASE("map import: lights become the editor's light classes") {
    const std::string text = world("", "\"_ambient\" \"51\"\n\"_color\" \"1 0.5 0\"\n") +
        "{\n\"classname\" \"light\"\n\"origin\" \"320 0 64\"\n\"light\" \"300\"\n\"_color\" \"1 0.5 0.25\"\n\"spawnflags\" \"1\"\n\"style\" \"3\"\n}\n"
        "{\n\"classname\" \"light\"\n\"origin\" \"0 0 640\"\n\"light\" \"100\"\n\"_sun\" \"1\"\n\"target\" \"t\"\n}\n"
        "{\n\"classname\" \"info_null\"\n\"origin\" \"0 640 0\"\n\"targetname\" \"t\"\n}\n"
        "{\n\"classname\" \"light\"\n\"origin\" \"0 0 64\"\n\"light\" \"200\"\n\"target\" \"t\"\n\"radius\" \"64\"\n}\n"
        "{\n\"classname\" \"light_point\"\n\"origin\" \"32 32 32\"\n\"light_color\" \"0.1 0.2 0.3\"\n\"intensity\" \"2\"\n\"radius\" \"7\"\n}\n";
    const MapImportResult r = importQuakeMap(text, sized(64, 64));
    CHECK(r.stats.lights == 4);
    const json& ws = entityOf(r, "e0");
    CHECK(ws["keyvalues"]["_ambient"] == "0.2 0.1 0");
    const json& pt = entityOf(r, "e1");
    CHECK(pt["classname"] == "light_point");
    CHECK(pt["keyvalues"]["color"] == "1 0.5 0.25");
    CHECK(pt["keyvalues"]["style"] == "3");
    CHECK(std::stod(pt["keyvalues"]["radius"].get<std::string>()) == doctest::Approx(300.0 / 32).epsilon(1e-3));  // linear light: reach = intensity
    CHECK(std::stod(pt["keyvalues"]["brightness"].get<std::string>()) > 1);
    CHECK_FALSE(pt["keyvalues"].contains("light"));
    CHECK_FALSE(pt["keyvalues"].contains("spawnflags"));
    CHECK(pt["origin"] == json::array({10.0, 2.0, 0.0}));
    const json& sun = entityOf(r, "e2");
    CHECK(sun["classname"] == "light_directional");
    // from (0,0,640) to (0,640,0) in Quake = (0,-20,0) .. in editor space the sun looks down and towards -Z: pitch 45
    CHECK(sun["angles"][0].get<double>() == doctest::Approx(45).epsilon(1e-6));
    CHECK(sun["refs"]["target"] == "e3");
    CHECK(std::stod(sun["keyvalues"]["brightness"].get<std::string>()) == doctest::Approx(100.0 / 255).epsilon(1e-3));
    const json& spot = entityOf(r, "e4");
    CHECK(spot["classname"] == "light_spot");
    CHECK(std::stod(spot["keyvalues"]["outer_angle"].get<std::string>()) == doctest::Approx(std::atan2(64.0, std::hypot(640.0, 64.0)) / 3.14159265358979 * 180).epsilon(1e-3));
    const json& game = entityOf(r, "e5");
    CHECK(game["classname"] == "light_point");
    CHECK(game["keyvalues"]["color"] == "0.1 0.2 0.3");
    CHECK(game["keyvalues"]["brightness"] == "2");
    CHECK(game["keyvalues"]["radius"] == "7");
}

TEST_CASE("map import: a negative light keeps its sign (a dark light), its reach and strength come from the absolute value") {
    auto light = [](const std::string& value, const std::string& extra) {
        return "{\n\"classname\" \"light\"\n\"origin\" \"320 0 64\"\n\"light\" \"" + value + "\"\n" + extra + "}\n";
    };
    const std::string text = world("") + light("-1500", "") + light("1500", "") + light("-300", "\"spawnflags\" \"1\"\n") + light("300", "\"spawnflags\" \"1\"\n") +
        "{\n\"classname\" \"light\"\n\"origin\" \"0 0 640\"\n\"light\" \"-100\"\n\"_sun\" \"1\"\n}\n"
        "{\n\"classname\" \"light_point\"\n\"origin\" \"32 32 32\"\n\"intensity\" \"-2\"\n\"radius\" \"7\"\n}\n";
    const MapImportResult r = importQuakeMap(text, sized(64, 64));
    CHECK(r.stats.lights == 6);
    for (const std::string& w : r.warnings) CHECK(w.find("negative light") == std::string::npos);  // no longer "has no equivalent"
    auto num = [&](const json& e, const char* key) { return std::stod(e["keyvalues"][key].get<std::string>()); };
    const json &dark = entityOf(r, "e1"), &bright = entityOf(r, "e2");
    CHECK(dark["classname"] == "light_point");
    CHECK(num(dark, "brightness") < 0);
    CHECK(num(dark, "brightness") == doctest::Approx(-num(bright, "brightness")).epsilon(1e-6));
    CHECK(num(dark, "radius") == doctest::Approx(num(bright, "radius")));
    const json &darkLinear = entityOf(r, "e3"), &brightLinear = entityOf(r, "e4");
    CHECK(num(darkLinear, "brightness") == doctest::Approx(-num(brightLinear, "brightness")).epsilon(1e-6));
    CHECK(num(darkLinear, "radius") == doctest::Approx(300.0 / 32).epsilon(1e-3));  // reach = |intensity|
    const json& sun = entityOf(r, "e5");
    CHECK(sun["classname"] == "light_directional");
    CHECK(num(sun, "brightness") == doctest::Approx(-100.0 / 255).epsilon(1e-3));
    CHECK(entityOf(r, "e6")["keyvalues"]["brightness"] == "-2");
    // what the editor makes of it: the Core keeps the sign
    const Document doc = documentFromJson(r.doc);
    const LightScene scene = collectLights(doc, {});
    REQUIRE(scene.lights.size() == 6);
    CHECK(scene.lights[0].brightness < 0);
    CHECK(scene.lights[1].brightness > 0);
}

TEST_CASE("map import: a quadratic patch becomes a closed slab behind its front side, smooth on top") {
    // a 3x3 patch over 128 x 128 units, the middle control point raised: the front faces +z (columns run along +x, rows along +y)
    std::string p = "{\npatchDef2\n{\nt/p\n( 3 3 0 0 0 )\n(\n";
    for (int i = 0; i < 3; ++i) {
        p += "(";
        for (int j = 0; j < 3; ++j) p += " ( " + std::to_string(i * 64) + " " + std::to_string(j * 64) + " " + std::to_string(i == 1 && j == 1 ? 48 : 0) + " " + std::to_string(i) + " " + std::to_string(j) + " )";
        p += " )\n";
    }
    p += ")\n}\n}\n";
    const MapImportResult r = importQuakeMap(world(p), sized(64, 64));
    CHECK(r.stats.patches == 1);
    CHECK(r.stats.skipped == 0);
    const json& detail = entityOf(r, "e_patches");
    CHECK(detail["classname"] == "func_detail");
    REQUIRE(detail["brushes"].size() == 1);
    const json& b = detail["brushes"][0];
    CHECK(b["id"] == "e0_p0");
    CHECK(b.contains("smooth"));
    const Document doc = documentFromJson(r.doc);
    CHECK_FALSE(validateDocument(doc).has_value());
    const Brush& brush = doc.entities.back().brushes[0];
    CHECK(brush.signedVolume() > 0);
    // the top surface faces up, the first tessellation point is the first control point
    int up = 0, caulk = 0;
    for (const BrushFace& f : brush.faces) {
        if (f.texture == "t/p") { CHECK(brush.faceNormal(f).y > 0); ++up; }
        else if (f.texture == "common/caulk") ++caulk;
    }
    CHECK(up >= 8);
    CHECK(caulk > up);
    // uvs come from the control points' s t
    for (const BrushFace& f : brush.faces) if (f.texture == "t/p") for (const Vec2& uv : f.uvs) { CHECK(uv.u >= -1e-9); CHECK(uv.u <= 2 + 1e-9); CHECK(uv.v >= -1e-9); CHECK(uv.v <= 2 + 1e-9); }
    // worldspawn itself keeps no patch
    CHECK(entityOf(r, "e0")["brushes"].empty());
}

TEST_CASE("map import: brushes with the detail flag and editor layer entities") {
    const std::string detail = "{\n( 0 0 64 ) ( 0 64 64 ) ( 64 0 64 ) t/a 0 0 0 0.5 0.5 134217728 0 0\n( 0 0 0 ) ( 64 0 0 ) ( 0 64 0 ) t/a 0 0 0 0.5 0.5 134217728 0 0\n"
        "( 64 0 0 ) ( 64 0 64 ) ( 64 64 0 ) t/a 0 0 0 0.5 0.5 134217728 0 0\n( 0 0 0 ) ( 0 64 0 ) ( 0 0 64 ) t/a 0 0 0 0.5 0.5 134217728 0 0\n"
        "( 0 64 0 ) ( 64 64 0 ) ( 0 64 64 ) t/a 0 0 0 0.5 0.5 134217728 0 0\n( 0 0 0 ) ( 0 0 64 ) ( 64 0 0 ) t/a 0 0 0 0.5 0.5 134217728 0 0\n}\n";
    const std::string text = world(detail + cube("t/b")) +
        "{\n\"classname\" \"func_group\"\n\"_tb_type\" \"_tb_layer\"\n\"_tb_name\" \"nav\"\n\"_tb_id\" \"1\"\n}\n"
        "{\n\"classname\" \"info_x\"\n\"origin\" \"0 0 0\"\n\"_tb_layer\" \"1\"\n}\n";
    const MapImportResult r = importQuakeMap(text, sized(64, 64));
    CHECK(r.stats.droppedEntities == 1);
    CHECK(entityOf(r, "e0")["brushes"].size() == 1);
    CHECK(entityOf(r, "e_detail")["brushes"].size() == 1);
    CHECK(entityOf(r, "e_detail")["brushes"][0]["faces"][0]["q3"]["contents"] == 134217728);
}

TEST_CASE("map import: texture size reading and the unknown-texture warning") {
    const std::vector<unsigned char> tga = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0, 0x20, 0, 24, 0};
    int w = 0, h = 0;
    CHECK(imageSize(tga, ".tga", w, h));
    CHECK(w == 128);
    CHECK(h == 32);
    const std::vector<unsigned char> png = {0x89, 'P', 'N', 'G', 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 1, 0, 0, 0, 0, 64};
    CHECK(imageSize(png, ".png", w, h));
    CHECK(w == 256);
    CHECK(h == 64);
    MapImportOptions o;  // no textureSize: unknown
    const MapImportResult r = importQuakeMap(world(cube("some/where")), o);
    bool named = false;
    for (const auto& s : r.warnings) named = named || s.find("some/where") != std::string::npos;
    CHECK(named);
    CHECK(importQuakeMap(world(cube("common/caulk")), o).warnings.empty());  // shaders without an image are expected
}

TEST_CASE("map import: malformed text is an error with a line number; planes that enclose nothing are skipped") {
    CHECK_THROWS_WITH_AS(importQuakeMap("{\n\"classname\" \"worldspawn\"\n", {}), doctest::Contains("unexpected end"), std::runtime_error);
    CHECK_THROWS_WITH_AS(importQuakeMap("{\n\"classname\" \"worldspawn\"\n{\n( 0 0 0 ) ( 1 0 0 ) tex\n}\n}\n", {}), doctest::Contains("line 4"), std::runtime_error);
    // two parallel planes facing each other without volume between them
    const std::string flat = "{\n( 0 0 0 ) ( 64 0 0 ) ( 0 64 0 ) t 0 0 0 1 1\n( 0 0 0 ) ( 0 64 0 ) ( 64 0 0 ) t 0 0 0 1 1\n( 0 0 0 ) ( 0 0 64 ) ( 64 0 0 ) t 0 0 0 1 1\n( 0 64 0 ) ( 64 64 0 ) ( 0 64 64 ) t 0 0 0 1 1\n}\n";
    const MapImportResult r = importQuakeMap(world(flat + cube("t/ok")), sized(64, 64));
    CHECK(r.stats.skipped == 1);
    CHECK(r.stats.brushes == 1);
    CHECK_FALSE(validateDocument(documentFromJson(r.doc)).has_value());
}

TEST_CASE("map import: the router converts a .map beside itself and opens the result") {
    TempDir dir;
    std::filesystem::create_directories(dir.path / "maps");
    {
        std::ofstream f(dir.path / "maps" / "q.map", std::ios::binary);
        f << world(cube("t/a"));
    }
    dir.write("maps/a.map.json", docJson({entityJson("e_world", "worldspawn", {Brush::makeBox("b", {0, 0, 0}, {1, 1, 1})})}));
    Scene scene{dir.path};
    scene.openDocument("maps/a.map.json");
    Router router{scene};
    auto send = [&](const json& m) { return router.onMessage(m.dump()); };
    RouterOutput out = send({{"type", "import_map"}, {"requestId", 1}, {"path", "maps/q.map"}});
    REQUIRE_FALSE(out.toSender.empty());
    json res = json::parse(out.toSender.back());
    CHECK(res["ok"] == true);
    CHECK(res["path"] == "maps/q.map.json");
    CHECK(res["stats"]["brushes"] == 1);
    CHECK(scene.currentPath() == "maps/q.map.json");
    CHECK(dir.exists("maps/q.map.json"));
    // again: refuses to replace the file unless told to
    out = send({{"type", "import_map"}, {"requestId", 2}, {"path", "maps/q.map"}});
    res = json::parse(out.toSender.back());
    CHECK(res["ok"] == false);
    CHECK(res["exists"] == true);
    out = send({{"type", "import_map"}, {"requestId", 3}, {"path", "maps/q.map"}, {"overwrite", true}});
    CHECK(json::parse(out.toSender.back())["ok"] == true);
    // only .map files inside the game folder
    for (const char* bad : {"maps/q.map.json", "../q.map", "/etc/q.map"}) {
        out = send({{"type", "import_map"}, {"requestId", 4}, {"path", bad}});
        CHECK(json::parse(out.toSender.back())["ok"] == false);
    }
}

TEST_CASE("map import: the sample maps of the game, when present, all convert without skips") {
    const char* root = std::getenv("MAPIMPORT_SAMPLES");  // NOLINT  // a game folder with maps/*.map (e.g. SdlWasmEngine ShooterGame/GameData)
    if (!root) return;
    const std::filesystem::path assets = root;
    const MapImportOptions opt = importOptionsFor(assets, loadProfile(assets));
    int maps = 0;
    for (const auto& e : std::filesystem::directory_iterator(assets / "maps")) {
        if (e.path().extension() != ".map") continue;
        const MapImportResult r = importQuakeMapFile(e.path(), opt);  // throws when the document is invalid
        INFO(e.path().string());
        CHECK(r.stats.skipped == 0);
        ++maps;
    }
    CHECK(maps > 0);
}
