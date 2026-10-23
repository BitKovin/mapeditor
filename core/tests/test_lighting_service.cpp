#include "core/router.h"
#include "core/denoise.h"
#include "core/textures.h"
#include "noise_session.h"
#include "test_util.h"
#include <doctest.h>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <tuple>

using namespace core;
using namespace testutil;
using nlohmann::json;

namespace {

json lightJson(const std::string& id, Vec3 o, double brightness, double radius) {
    json e = entityJson(id, "light_point", {}, o);
    e["keyvalues"] = {{"brightness", std::to_string(brightness)}, {"radius", std::to_string(radius)}};
    return e;
}

struct Rig {
    TempDir dir;
    Scene scene;
    Router router;
    explicit Rig(const json& doc, const LightingService::Options& opt = LightingService::Options()) : scene(prepare(dir, doc)), router(scene, {}, opt) {
        scene.loadGameProfile();
        scene.openDocument("maps/a.map.json");
    }
    static std::filesystem::path prepare(TempDir& d, const json& doc) {
        d.write("maps/a.map.json", doc);
        return d.path;
    }
    // drains lighting ticks until the status says converged; returns every message seen
    std::vector<json> run(int maxTicks = 400) {
        std::vector<json> all;
        for (int i = 0; i < maxTicks; ++i) {
            const RouterOutput o = router.onLightingTick(0.05);
            bool converged = false;
            for (const auto& m : o.toAll) {
                all.push_back(json::parse(m));
                if (all.back()["type"] == "lighting_status" && all.back()["converged"] == true) converged = true;
            }
            if (converged) break;
        }
        return all;
    }
};
json roomDoc(double lx = 0) {
    return docJson({entityJson("w", "worldspawn", {Brush::makeBox("floor", {-4, -1, -4}, {4, 0, 4})}), lightJson("l", {lx, 2, 0}, 8, 20)});
}
std::vector<unsigned char> decode(const std::string& b64) {
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> out;
    unsigned v = 0;
    int bits = -8;
    for (char c : b64) {
        const auto p = chars.find(c);
        if (p == std::string::npos) break;
        v = (v << 6) | static_cast<unsigned>(p);
        bits += 6;
        if (bits >= 0) { out.push_back(static_cast<unsigned char>((v >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}
}  // namespace

TEST_CASE("lighting service: nothing happens until the Shell asks for lighting") {
    Rig rig(roomDoc());
    CHECK(rig.router.onLightingTick().toAll.empty());
    const RouterOutput on = rig.router.onMessage(R"({"type":"lighting","enabled":true,"requestId":"r1"})");
    REQUIRE(on.toSender.size() == 1);
    const json resp = json::parse(on.toSender[0]);
    CHECK(resp["ok"] == true);
    CHECK(resp["requestId"] == "r1");
    const auto msgs = rig.run();
    REQUIRE(!msgs.empty());
    CHECK(msgs.front()["type"] == "lit_mesh");  // geometry first, then pixels
}

TEST_CASE("lighting service: lit_mesh has lightmap coordinates inside the atlas, tiles arrive progressively, status converges") {
    Rig rig(roomDoc());
    rig.router.onMessage(R"({"type":"lighting","enabled":true})");
    const auto msgs = rig.run();
    const json* mesh = nullptr;
    int tileMessages = 0, statuses = 0;
    json last;
    for (const auto& m : msgs) {
        if (m["type"] == "lit_mesh" && !mesh) mesh = &m;
        if (m["type"] == "lightmap_tiles") ++tileMessages;
        if (m["type"] == "lighting_status") { ++statuses; last = m; }
    }
    REQUIRE(mesh);
    const size_t tris = (*mesh)["triangleCount"].get<size_t>();
    CHECK(tris == 12);  // one box
    const auto bytes = decode((*mesh)["vertices"].get<std::string>());
    REQUIRE(bytes.size() == tris * 36 * sizeof(float));
    std::vector<float> v(bytes.size() / sizeof(float));
    std::memcpy(v.data(), bytes.data(), bytes.size());
    for (size_t i = 0; i < tris * 3; ++i) {
        const float u = v[i * 12 + 6], w = v[i * 12 + 7], page = v[i * 12 + 8];
        CHECK(u >= 0.f);
        CHECK(u <= 1.f);
        CHECK(w >= 0.f);
        CHECK(w <= 1.f);
        CHECK(page == 0.f);
    }
    CHECK(tileMessages >= 1);  // the initial set; refinements only follow where the 8 bit pixels still change (unchanged tiles are not re-sent)
    CHECK(statuses >= 1);
    CHECK(last["converged"] == true);
    CHECK(last["stale"] == 0);
    CHECK(last["minPasses"] == last["maxPasses"]);
}

namespace {
// All tile pixels of the messages painted into one page image of floats (last write wins): rgb linear, alpha 0 = never written. Pixels are RGBA16F.
std::vector<float> paintTiles(const std::vector<json>& msgs, int atlas, int style = 0) {
    std::vector<float> img(static_cast<size_t>(atlas) * atlas * 4, 0.f);
    for (const auto& m : msgs) {
        if (m["type"] != "lightmap_tiles" || m.value("style", 0) != style) continue;
        for (const auto& t : m["tiles"]) {
            const auto raw = decode(t["rgba"].get<std::string>());
            const int x0 = t["x"], y0 = t["y"], w = t["w"], h = t["h"];
            REQUIRE(raw.size() == static_cast<size_t>(w) * h * 8);  // 4 halves per pixel
            std::vector<uint16_t> px(raw.size() / 2);
            std::memcpy(px.data(), raw.data(), raw.size());
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const uint16_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
                    if (halfToFloat(p[3]) == 0.f) continue;
                    float* o = &img[(static_cast<size_t>(y0 + y) * atlas + x0 + x) * 4];
                    for (int c = 0; c < 4; ++c) o[c] = halfToFloat(p[c]);
                }
        }
    }
    return img;
}
// Ticks until nothing was sent for a few ticks (lightmap converged, light volume published); returns everything seen.
std::vector<json> settle(Rig& rig, int maxTicks = 800) {
    std::vector<json> all;
    int quiet = 0;
    for (int i = 0; i < maxTicks && quiet < 6; ++i) {
        const RouterOutput o = rig.router.onLightingTick(0.05);
        // a refinement pass that changes no pixel sends nothing (the Shell already holds those pixels), so silence alone does not mean it is over
        quiet = (o.toAll.empty() && !rig.router.lighting().busy() && !rig.router.lighting().lightvolPending()) ? quiet + 1 : 0;
        for (const auto& m : o.toAll) all.push_back(json::parse(m));
    }
    return all;
}
}  // namespace

TEST_CASE("lighting service: tiles carry the lit floor as linear half floats: bright under the light, dark far away") {
    Rig rig(roomDoc());
    rig.router.onMessage(R"({"type":"lighting","enabled":true})");
    const auto msgs = rig.run();
    const int atlas = 1024;
    const std::vector<float> img = paintTiles(msgs, atlas);
    // the floor top face: find its texels through the same layout the Core used
    const LightmapLayout& L = rig.router.lighting().baker().layout();
    const LightmapFace* top = L.find("", "w", "", "floor", 5, FaceSide::Front);
    REQUIRE(top);
    auto pixel = [&](Vec3 p) {
        const Vec2 t = top->texel(p);
        return img[(static_cast<size_t>(t.v) * atlas + static_cast<size_t>(t.u)) * 4];  // red
    };
    const float under = pixel({0.1, 0, 0.1}), far = pixel({3.8, 0, 3.8});
    MESSAGE("under the light " << under << ", far corner " << far);
    CHECK(under > 0.2f);
    CHECK(far < under / 2);
    CHECK(far > 0);  // lit a little (radius 20), and the tile really was written
}

TEST_CASE("lighting service: tiles keep brightness above 1 (HDR): a strong light is not clamped, and nothing is encoded or scaled") {
    Rig rig(docJson({entityJson("w", "worldspawn", {Brush::makeBox("floor", {-4, -1, -4}, {4, 0, 4})}), lightJson("l", {0, 2, 0}, 200, 40)}));
    rig.router.onMessage(R"({"type":"lighting","enabled":true,"exposure":0.25})");  // a display setting: must not change the tiles
    const auto msgs = rig.run();
    const std::vector<float> img = paintTiles(msgs, 1024);
    float peak = 0;
    for (size_t i = 0; i < img.size(); i += 4) peak = std::max(peak, img[i]);
    MESSAGE("brightest tile value " << peak);
    CHECK(peak > 2.0f);
    // the same scene at brightness 100 gives half the light (linear, as baked), whatever the exposure
    Rig half(docJson({entityJson("w", "worldspawn", {Brush::makeBox("floor", {-4, -1, -4}, {4, 0, 4})}), lightJson("l", {0, 2, 0}, 100, 40)}));
    half.router.onMessage(R"({"type":"lighting","enabled":true})");
    const std::vector<float> img2 = paintTiles(half.run(), 1024);
    const LightmapLayout& L = rig.router.lighting().baker().layout();
    const LightmapFace* top = L.find("", "w", "", "floor", 5, FaceSide::Front);
    REQUIRE(top);
    const Vec2 t = top->texel({0.1, 0, 0.1});
    const size_t at = (static_cast<size_t>(t.v) * 1024 + static_cast<size_t>(t.u)) * 4;
    CHECK(img[at] / img2[at] == doctest::Approx(2.0).epsilon(0.05));
}

TEST_CASE("lighting: half float conversion round trips what lightmaps hold") {
    for (float v : {0.f, 1e-6f, 0.0003f, 0.01f, 0.5f, 1.f, 1.5f, 2.0f, 7.25f, 100.f, 1000.f, 60000.f}) {
        const float r = halfToFloat(floatToHalf(v));
        if (v < 6.2e-5f) CHECK(std::fabs(r - v) <= 6e-8f);  // subnormal halves: absolute spacing 2^-24
        else CHECK(std::fabs(r - v) <= v * 0.0006f);         // 11 bit significand
    }
    CHECK(floatToHalf(-1.f) == 0);
    CHECK(floatToHalf(std::nanf("")) == 0);
    CHECK(halfToFloat(floatToHalf(1e9f)) == 65504.f);  // saturates
    CHECK(floatToHalf(1.f) == 0x3C00);
}

TEST_CASE("lighting service: editing the document rebuilds the mesh and keeps results of what did not change") {
    Rig rig(roomDoc(0));
    rig.router.onMessage(R"({"type":"lighting","enabled":true})");
    rig.run();
    const int rev0 = rig.router.lighting().geometryRevision();
    // add a far box: the floor's lighting must survive, so the status is not "everything stale"
    json doc = roomDoc(0);
    doc["entities"][0]["brushes"].push_back(brushJson(Brush::makeBox("crate", {2.5, 0, 2.5}, {3.5, 1, 3.5})));
    const RouterOutput up = rig.router.onMessage(json{{"type", "document_update"}, {"doc", doc}}.dump());
    (void)up;
    const auto msgs = rig.run();
    REQUIRE(!msgs.empty());
    CHECK(msgs.front()["type"] == "lit_mesh");
    CHECK(rig.router.lighting().geometryRevision() == rev0 + 1);
    CHECK(msgs.front()["triangleCount"].get<int>() == 24);
    json st;
    for (const auto& m : msgs) if (m["type"] == "lighting_status") { st = m; break; }
    REQUIRE(!st.is_null());
    CHECK(st["stale"].get<int>() < st["chunks"].get<int>());  // chunks far from the crate were not reset
}

TEST_CASE("lighting service: exposure is a status value only (no tiles re-sent), disabling stops work, a late Shell gets the full state") {
    Rig rig(roomDoc());
    rig.router.onMessage(R"({"type":"lighting","enabled":true})");
    settle(rig);
    CHECK(rig.router.onLightingTick().toAll.empty());  // converged, light volume published, idle: silence
    rig.router.onMessage(R"({"type":"lighting","exposure":2})");
    int tiles = 0;
    double exposure = 0;
    for (const auto& s : rig.router.onLightingTick().toAll) {
        const json j = json::parse(s);
        tiles += j["type"] == "lightmap_tiles";
        if (j["type"] == "lighting_status") exposure = j["exposure"].get<double>();
    }
    CHECK(tiles == 0);
    CHECK(exposure == 2.0);
    const RouterOutput late = rig.router.onConnect();
    bool mesh = false, status = false;
    for (const auto& s : late.toSender) {
        const json j = json::parse(s);
        mesh |= j["type"] == "lit_mesh";
        status |= j["type"] == "lighting_status";
    }
    CHECK(mesh);
    CHECK(status);
    rig.router.onMessage(R"({"type":"lighting","enabled":false})");
    const RouterOutput off = rig.router.onLightingTick();
    REQUIRE(off.toAll.size() == 1);
    CHECK(json::parse(off.toAll[0])["enabled"] == false);
    CHECK(rig.router.onLightingTick().toAll.empty());
    for (const auto& s : rig.router.onConnect().toSender) CHECK(json::parse(s)["type"] != "lit_mesh");
}

TEST_CASE("lighting service: a moved light is picked up through the document") {
    Rig rig(roomDoc(-3));
    rig.router.onMessage(R"({"type":"lighting","enabled":true,"camera":[0,1,0]})");
    rig.run();
    const LiveBaker& b = rig.router.lighting().baker();
    auto valueNear = [&](double x) {
        size_t best = 0;
        double bd = 1e300;
        for (size_t i = 0; i < b.sampleCount(); ++i) {
            if (b.samples().nrm[i * 4 + 1] < 0.9f) continue;
            const double d = std::fabs(b.samples().pos[i * 4] - x) + std::fabs(b.samples().pos[i * 4 + 2]);
            if (d < bd) { bd = d; best = i; }
        }
        return b.value(best).x;
    };
    const double beforeLeft = valueNear(-3), beforeRight = valueNear(3);
    CHECK(beforeLeft > beforeRight);
    rig.router.onMessage(json{{"type", "document_update"}, {"doc", roomDoc(3)}}.dump());
    rig.run();
    CHECK(valueNear(3) > valueNear(-3));
}

TEST_CASE("lighting service: the new lit mesh is held back until a tenth of the chunks have data, then sent once with tiles") {
    Rig rig(roomDoc());
    rig.router.lighting().setEnabled(true);
    // tiny ticks: the first ones cannot finish a tenth of the level
    int meshAt = -1, tilesBeforeMesh = 0, meshCount = 0;
    for (int i = 0; i < 200 && meshAt < 0; ++i) {
        const RouterOutput o = rig.router.onLightingTick(0.0005);
        for (const auto& m : o.toAll) {
            const json j = json::parse(m);
            if (j["type"] == "lit_mesh") { meshAt = i; ++meshCount; }
            else if (j["type"] == "lightmap_tiles" && meshAt < 0) ++tilesBeforeMesh;
        }
    }
    REQUIRE(meshAt >= 0);
    CHECK(tilesBeforeMesh == 0);  // nothing is sent before the mesh: the Shell never shows a blank lightmap
    // moving a light does not resend the mesh (only refined tiles follow)
    rig.run();
    rig.scene.openDocument("maps/a.map.json");
    for (int i = 0; i < 50; ++i) {
        const RouterOutput o = rig.router.onLightingTick(0.01);
        for (const auto& m : o.toAll) if (json::parse(m)["type"] == "lit_mesh") ++meshCount;
    }
    CHECK(meshCount >= 1);
}

TEST_CASE("lighting settings: file values are clamped, unknown keys ignored, defaults written once") {
    TempDir dir;
    const auto file = dir.path / "lighting.json";
    CHECK(LightingService::writeDefaultOptions(file));
    std::string warn;
    LightingService::Options d = LightingService::loadOptions(file, &warn);
    CHECK(warn.empty());
    CHECK(d.live.directSupersample == 8);
    CHECK(d.live.giScale == 2);
    CHECK(d.live.denoiseIndirect);
    CHECK(d.layout.padding == 2);  // the bicubic lightmap lookup reads 2 texels around a chart (REQUIREMENTS §30)
    dir.write("lighting.json", json{{"padding", 1}, {"texelsPerMeter", 32}, {"atlasSize", 1000}, {"directSupersample", 99}, {"giScale", 3}, {"gpuTargetStepMs", 9000},
                                    {"tickBudgetMs", 250}, {"nonsense", 1}, {"denoise", false}});
    const LightingService::Options o = LightingService::loadOptions(file, &warn);
    CHECK(o.layout.texelsPerMeter == doctest::Approx(32));
    CHECK(o.layout.atlasSize == 1024);              // rounded up to a power of two
    CHECK(o.layout.padding == 2);                   // an old file's 1 is raised to 2
    CHECK(o.live.directSupersample == kMaxSuperSamples);
    CHECK(o.live.giScale == 4);                     // 3 -> 4
    CHECK(o.live.gpuTargetStepMs == doctest::Approx(400));
    CHECK(o.tickBudgetSeconds == doctest::Approx(0.25));
    CHECK_FALSE(o.live.denoiseIndirect);
    CHECK(LightingService::writeDefaultOptions(file));  // does not overwrite
    CHECK(LightingService::loadOptions(file).live.giScale == 4);
    dir.write("lighting.json", json::array());
    LightingService::loadOptions(file, &warn);
    CHECK_FALSE(warn.empty());
}

namespace {
json doorRoomDoc() {
    return docJson({entityJson("w", "worldspawn", {Brush::makeBox("floor", {-4, -1, -4}, {4, 0, 4})}),
                    entityJson("door", "func_door", {Brush::makeBox("d", {1, 0, 1}, {2, 2, 1.3})}), lightJson("l", {0, 2.5, 0}, 8, 20)});
}
// Ticks until a lightvol and an entity_light message have been seen (or the budget of ticks ran out).
std::vector<json> runUntilLightvol(Rig& rig, int maxTicks = 600) {
    std::vector<json> all;
    bool vol = false, ent = false;
    for (int i = 0; i < maxTicks && !(vol && ent); ++i) {
        for (const auto& m : rig.router.onLightingTick(0.05).toAll) {
            all.push_back(json::parse(m));
            vol |= all.back()["type"] == "lightvol";
            ent |= all.back()["type"] == "entity_light";
        }
    }
    return all;
}
}  // namespace

TEST_CASE("lighting service: lit_mesh carries the face table in vertex order") {
    Rig rig(doorRoomDoc());
    rig.router.onMessage(R"({"type":"lighting","enabled":true,"requestId":"r1"})");
    const auto msgs = rig.run();
    const json* mesh = nullptr;
    for (const auto& m : msgs) if (m["type"] == "lit_mesh") mesh = &m;
    REQUIRE(mesh);
    REQUIRE((*mesh)["faces"].is_array());
    size_t tris = 0;
    for (const auto& f : (*mesh)["faces"]) {
        CHECK(f.contains("e"));
        CHECK(f.contains("b"));
        CHECK(f.contains("f"));
        CHECK(f.contains("t"));
        tris += f["n"].get<size_t>();
        CHECK(f["e"] != "door");  // the door is not lightmapped
    }
    CHECK(tris == (*mesh)["triangleCount"].get<size_t>());
    // every lightmapped face sends its lightmap mapping once; the mapping reproduces the vertices' own lightmap coordinates
    const auto bytes = decode((*mesh)["vertices"].get<std::string>());
    std::vector<float> v(bytes.size() / 4);
    std::memcpy(v.data(), bytes.data(), bytes.size());
    std::map<std::string, json> maps;
    size_t at = 0, withMap = 0;
    for (const auto& f : (*mesh)["faces"]) {
        const std::string key = f["i"].get<std::string>() + "|" + f["e"].get<std::string>() + "|" + f["b"].get<std::string>() + "|" + std::to_string(f["f"].get<int>());
        if (f.contains("m")) { ++withMap; CHECK(maps.count(key) == 0); maps[key] = f["m"]; }
        REQUIRE(maps.count(key) == 1);
        const json& m = maps[key];
        for (size_t t = 0; t < f["n"].get<size_t>() * 3; ++t, ++at) {
            const float* p = &v[at * 12];
            const double u = p[0] * m["u"][0].get<double>() + p[1] * m["u"][1].get<double>() + p[2] * m["u"][2].get<double>() + m["u"][3].get<double>();
            const double w = p[0] * m["v"][0].get<double>() + p[1] * m["v"][1].get<double>() + p[2] * m["v"][2].get<double>() + m["v"][3].get<double>();
            CHECK(u == doctest::Approx(p[6]).epsilon(1e-4));
            CHECK(w == doctest::Approx(p[7]).epsilon(1e-4));
            CHECK(m["pg"].get<float>() == p[8]);
            CHECK(m["k"][0].get<float>() == p[9]);
        }
    }
    CHECK(withMap == maps.size());
    CHECK(withMap >= 6);  // the six faces of the box floor
}

TEST_CASE("lighting service: light volume and per-entity light arrive after the lightmap converged, lit from the room's light") {
    Rig rig(doorRoomDoc());
    rig.router.onMessage(R"({"type":"lighting","enabled":true,"requestId":"r1"})");
    const auto msgs = runUntilLightvol(rig);
    const json *vol = nullptr, *ent = nullptr;
    for (const auto& m : msgs) { if (m["type"] == "lightvol") vol = &m; if (m["type"] == "entity_light") ent = &m; }
    REQUIRE(vol);
    REQUIRE(ent);
    const auto dims = (*vol)["dims"];
    const size_t n = dims[0].get<size_t>() * dims[1].get<size_t>() * dims[2].get<size_t>();
    CHECK(decode((*vol)["probes"].get<std::string>()).size() == n * 9);
    REQUIRE((*ent)["entities"].size() == 1);
    const json& d = (*ent)["entities"][0];
    CHECK(d["e"] == "door");
    REQUIRE(d["lights"].size() == 1);  // one style layer
    // the door is at (1.5, 1, 1.15), the point light at (0, 2.5, 0): the light comes from above, towards the origin
    const json& l0 = d["lights"][0];
    CHECK(l0["s"] == 0);
    CHECK(d["dir"][1].get<double>() > 0.3);   // one direction per entity (REQUIREMENTS §27 G4)
    CHECK(d["dir"][0].get<double>() < 0.0);
    CHECK_FALSE(l0.contains("dir"));
    CHECK(l0["directed"][0].get<double>() > 0.05);
}

TEST_CASE("lighting service: moving the door resends the entity light only; a new light bakes the volume again") {
    Rig rig(doorRoomDoc());
    rig.router.onMessage(R"({"type":"lighting","enabled":true,"requestId":"r1"})");
    runUntilLightvol(rig);
    for (int i = 0; i < 20; ++i) rig.router.onLightingTick(0.05);  // settle
    rig.router.lighting().markDirty();
    bool vol = false, ent = false;
    for (int i = 0; i < 4; ++i)
        for (const auto& m : rig.router.onLightingTick(0.05).toAll) {
            const json j = json::parse(m);
            vol |= j["type"] == "lightvol";
            ent |= j["type"] == "entity_light";
        }
    CHECK(ent);   // resampled immediately from the published grid
}


namespace {
json styledRoomDoc(int style) {
    json lamp = lightJson("styled", {0, 2.5, 0}, 20, 30);
    lamp["keyvalues"]["style"] = std::to_string(style);
    return docJson({entityJson("w", "worldspawn", {Brush::makeBox("floor", {-4, -1, -4}, {4, 0, 4})}), lamp,
                    entityJson("door", "func_door", {Brush::makeBox("d", {1, 0, 1}, {2, 2, 1.3})})});
}
double sumRed(const std::vector<float>& img) {
    double t = 0;
    for (size_t i = 0; i < img.size(); i += 4) t += img[i];
    return t;
}
}  // namespace

TEST_CASE("light styles (service): a styled light is its own lightmap layer; the base layer has none of it; layers add up to the unstyled bake") {
    Rig styled(styledRoomDoc(5));
    styled.router.onMessage(R"({"type":"lighting","enabled":true})");
    const auto msgs = settle(styled);
    std::vector<json> all;
    // run() output of the earlier ticks is already in msgs
    const std::vector<float> base = paintTiles(msgs, 1024, 0), layer = paintTiles(msgs, 1024, 5);
    const json* status = nullptr;
    for (const auto& m : msgs) if (m["type"] == "lighting_status") status = &m;
    REQUIRE(status);
    CHECK((*status)["styles"] == json::array({0, 5}));
    bool sawStyles = false, sawTile5 = false;
    for (const auto& m : msgs) {
        sawStyles |= m["type"] == "light_styles";
        sawTile5 |= m["type"] == "lightmap_tiles" && m.value("style", 0) == 5;
        if (m["type"] == "light_styles") { CHECK(m["patterns"].size() == 64); CHECK(m["patterns"][5] == "jklmnopqrstuvwxyzyxwvutsrqponmlkj"); CHECK(m["hz"] == 10.0); }
    }
    CHECK(sawStyles);
    CHECK(sawTile5);
    // the lamp is style 5, the scene has no other light: the base layer is black, the layer carries all light
    CHECK(sumRed(base) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(sumRed(layer) > 100.0);

    // the same scene with the lamp unstyled: its base layer equals the styled scene's style layer (linearity: a layer is the bake of its lights alone)
    Rig plain(styledRoomDoc(0));
    plain.router.onMessage(R"({"type":"lighting","enabled":true})");
    const auto pmsgs = settle(plain);
    const std::vector<float> pbase = paintTiles(pmsgs, 1024, 0);
    CHECK(sumRed(pbase) == doctest::Approx(sumRed(layer)).epsilon(0.02));
    // pixel by pixel on the lit floor
    size_t compared = 0;
    double worst = 0;
    for (size_t i = 0; i < layer.size(); i += 4) {
        if (layer[i + 3] == 0.f || pbase[i + 3] == 0.f || pbase[i] < 0.05f) continue;
        worst = std::max(worst, static_cast<double>(std::fabs(layer[i] - pbase[i]) / pbase[i]));
        ++compared;
    }
    CHECK(compared > 100);
    CHECK(worst < 0.15);  // independent Monte Carlo runs: noise only
}

TEST_CASE("light styles (service): chart layers - only charts the styled light reaches carry its layer; layers appear and vanish with the lights") {
    // two floors far apart; the styled lamp (radius 6) only reaches the first
    json lamp = lightJson("styled", {0, 2, 0}, 10, 6);
    lamp["keyvalues"]["style"] = "3";
    Rig rig(docJson({entityJson("w", "worldspawn", {Brush::makeBox("near", {-3, -1, -3}, {3, 0, 3}), Brush::makeBox("far", {30, -1, -3}, {36, 0, 3})}), lamp}));
    rig.router.onMessage(R"({"type":"lighting","enabled":true})");
    const auto msgs = settle(rig);
    const LightmapLayout& L = rig.router.lighting().baker().layout();
    REQUIRE(L.charts.size() >= 2);
    size_t withStyle = 0;
    for (const auto& c : L.charts) {
        const bool has = std::find(c.styles.begin(), c.styles.end(), 3) != c.styles.end();
        withStyle += has;
        CHECK(c.styles[0] == 0);
        for (int f : c.faces) if (L.faces[static_cast<size_t>(f)].brushId == "far") CHECK(!has);
    }
    CHECK(withStyle >= 1);
    const LiveBaker* l3 = rig.router.lighting().layerBaker(3);
    REQUIRE(l3);
    CHECK(l3->layout().charts.size() == withStyle);
    CHECK(l3->sampleCount() < rig.router.lighting().baker().sampleCount());  // fewer texels than the base layer
    CHECK(rig.router.lighting().layerBaker(9) == nullptr);

    // the lit mesh tells the Shell per vertex which layers its chart carries: style 3 = bit 2 of the first mask float
    const json* mesh = nullptr;
    for (const auto& m : msgs) if (m["type"] == "lit_mesh") mesh = &m;
    REQUIRE(mesh);
    const auto bytes = decode((*mesh)["vertices"].get<std::string>());
    std::vector<float> v(bytes.size() / 4);
    std::memcpy(v.data(), bytes.data(), bytes.size());
    size_t at = 0, nearStyled = 0, farStyled = 0;
    for (const auto& f : (*mesh)["faces"]) {
        for (size_t t = 0; t < f["n"].get<size_t>() * 3; ++t, ++at) {
            const float m0 = v[at * 12 + 9], m1 = v[at * 12 + 10], m2 = v[at * 12 + 11];
            CHECK(m1 == 0.f);
            CHECK(m2 == 0.f);
            CHECK((m0 == 0.f || m0 == 4.f));
            if (f["b"] == "far") farStyled += m0 != 0.f;
            else nearStyled += m0 == 4.f;
        }
    }
    CHECK(at * 12 == v.size());
    CHECK(nearStyled > 0);
    CHECK(farStyled == 0);
}

TEST_CASE("light styles (service): the light volume has probes per style; the entity gets one light per layer") {
    Rig rig(styledRoomDoc(5));
    // add an unstyled lamp too so the base layer has light
    rig.router.onMessage(R"({"type":"lighting","enabled":true})");
    const auto msgs = runUntilLightvol(rig);
    const json *vol = nullptr, *ent = nullptr;
    for (const auto& m : msgs) { if (m["type"] == "lightvol") vol = &m; if (m["type"] == "entity_light") ent = &m; }
    REQUIRE(vol);
    REQUIRE(ent);
    REQUIRE((*vol)["layers"].size() == 1);
    CHECK((*vol)["layers"][0]["s"] == 5);
    const auto dims = (*vol)["dims"];
    const size_t n = dims[0].get<size_t>() * dims[1].get<size_t>() * dims[2].get<size_t>();
    CHECK(decode((*vol)["layers"][0]["probes"].get<std::string>()).size() == n * 9);
    REQUIRE((*ent)["entities"].size() == 1);
    const json& lights = (*ent)["entities"][0]["lights"];
    REQUIRE(lights.size() == 2);
    CHECK(lights[0]["s"] == 0);
    CHECK(lights[1]["s"] == 5);
    CHECK(lights[0]["directed"][0].get<double>() == doctest::Approx(0.0).epsilon(1e-3));  // base layer: no light
    CHECK(lights[1]["directed"][0].get<double>() > 0.05);                                  // the styled lamp lights the door
}

// ---- the bake thread, the mirror of what the Shell holds, progressive tiles -------------------------------------------------------------

namespace {

// What the Shell does with the messages (shell/src/render/lightmapStore.ts), so the mirror can be compared with a real client.
struct ClientSim {
    int atlas = 0, pageCount = 0, rev = -1, meshes = 0;
    bool lastKeep = false;
    size_t triangles = 0, tilesApplied = 0, tileMessages = 0, droppedTileMessages = 0;
    std::vector<std::string> log;
    std::map<int, std::vector<std::vector<uint16_t>>> pages;  // per style layer: RGBA16F pages (the base always; another layer with its first tile)
    std::set<std::tuple<int, int, int>> touched;  // distinct tile rectangles (page, x, y) applied since the test last cleared it
    void apply(const json& m) {
        const std::string type = m["type"];
        if (type == "lit_mesh") {
            const bool keep = m.value("keepPages", false) && m["atlas"] == atlas && m["pageCount"] == pageCount && pages.count(0);
            lastKeep = keep;
            if (!keep) {
                atlas = m["atlas"];
                pageCount = m["pageCount"];
                pages.clear();
                pages[0].assign(static_cast<size_t>(pageCount), std::vector<uint16_t>(static_cast<size_t>(atlas) * atlas * 4, 0));
            }
            rev = m["rev"];
            log.push_back("mesh rev " + m["rev"].dump() + (keep ? " keep" : " fresh"));
            triangles = m["triangleCount"];
            ++meshes;
        } else if (type == "lightmap_tiles") {
            ++tileMessages;
            log.push_back("tiles rev " + m["rev"].dump() + " style " + std::to_string(m.value("style", 0)) + " n " + std::to_string(m["tiles"].size()) + (m["rev"] != rev ? " DROPPED" : ""));
            if (m["rev"] != rev) { ++droppedTileMessages; return; }
            const int style = m.value("style", 0);
            auto& layer = pages[style];
            if (layer.empty()) layer.assign(static_cast<size_t>(pageCount), std::vector<uint16_t>(static_cast<size_t>(atlas) * atlas * 4, 0));
            for (const auto& t : m["tiles"]) {
                const auto bytes = decode(t["rgba"].get<std::string>());
                const int x0 = t["x"], y0 = t["y"], w = t["w"], h = t["h"], pg = t["page"];
                REQUIRE(bytes.size() == static_cast<size_t>(w) * h * 8);
                REQUIRE(x0 >= 0);
                REQUIRE(y0 >= 0);
                REQUIRE(x0 + w <= atlas);
                REQUIRE(y0 + h <= atlas);
                std::vector<uint16_t> px(bytes.size() / 2);
                std::memcpy(px.data(), bytes.data(), bytes.size());
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        const uint16_t* sp = &px[(static_cast<size_t>(y) * w + x) * 4];
                        if (sp[3] == 0) continue;
                        uint16_t* d = &layer[static_cast<size_t>(pg)][(static_cast<size_t>(y0 + y) * atlas + x0 + x) * 4];
                        d[0] = sp[0]; d[1] = sp[1]; d[2] = sp[2]; d[3] = 0x3C00;
                    }
                ++tilesApplied;
                touched.insert({pg, x0, y0});
            }
        }
    }
};

// A Rig whose lighting runs on the bake thread; the sink plays the socket thread and collects what would be broadcast.
struct ThreadedRig : Rig {
    std::mutex m;
    std::condition_variable cv;
    std::vector<json> msgs;
    ThreadedRig(const json& doc, double gpuMs = 0, const LightingService::Options& opt = LightingService::Options()) : Rig(doc, opt) {
        router.lighting().setSessionFactory(noiseFactory(gpuMs));
        router.lighting().startWorker([this](std::vector<std::string>&& batch) {
            {
                std::lock_guard<std::mutex> lock(m);
                for (const auto& s : batch) msgs.push_back(json::parse(s));
            }
            cv.notify_all();
            router.lighting().delivered();
        });
    }
    ~ThreadedRig() { router.lighting().stopWorker(); }
    // blocks until pred(msgs) holds (seconds at most); false on timeout
    template <class F> bool waitFor(F pred, double seconds = 20) {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, std::chrono::duration<double>(seconds), [&] { return pred(msgs); });
    }
    // blocks until no message arrived for `quietMs` (the bake thread has nothing left to send), `seconds` at most
    bool waitQuiet(int quietMs = 400, double seconds = 20) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        std::unique_lock<std::mutex> lock(m);
        size_t n = msgs.size();
        while (std::chrono::steady_clock::now() < end) {
            if (!cv.wait_for(lock, std::chrono::milliseconds(quietMs), [&] { return msgs.size() != n; })) return true;
            n = msgs.size();
        }
        return false;
    }
    static bool convergedAfterMesh(const std::vector<json>& v, size_t fromIndex = 0) {
        bool mesh = false;
        for (size_t i = fromIndex; i < v.size(); ++i) {
            if (v[i]["type"] == "lit_mesh") mesh = true;
            if (mesh && v[i]["type"] == "lighting_status" && v[i]["converged"] == true) return true;
        }
        return false;
    }
    // ... and the denoised indirect light of every page went out too (or the denoiser is not available): nothing is left to send.
    static bool settledAfterMesh(const std::vector<json>& v, size_t fromIndex = 0) {
        bool mesh = false;
        for (size_t i = fromIndex; i < v.size(); ++i) {
            if (v[i]["type"] == "lit_mesh") mesh = true;
            if (mesh && v[i]["type"] == "lighting_status" && v[i]["converged"] == true &&
                (v[i].value("denoisedPages", 0) >= v[i].value("pages", 0) || !v[i].value("denoiseError", std::string()).empty()))
                return true;
        }
        return false;
    }
};
json crateDoc(double x) {
    // a wide floor and a light that reaches only part of it: moving the crate leaves most chunks alone
    return docJson({entityJson("w", "worldspawn", {Brush::makeBox("floor", {-8, -1, -8}, {8, 0, 8}), Brush::makeBox("crate", {x, 0, -1}, {x + 1, 1, 0})}), lightJson("l", {-6, 2, 0}, 8, 4)});
}
}  // namespace

TEST_CASE("lighting service (thread): a live client and a client that joins later hold the same pixels, an edit keeps the pages and sends only what changed") {
    LightingService::Options direct;
    direct.live.bounces = false;  // with bounces a moved crate refreshes the indirect light of the whole room
    ThreadedRig rig(crateDoc(5), 0, direct);
    rig.router.onMessage(R"({"type":"lighting","enabled":true,"camera":[0,1,0]})");
    REQUIRE(rig.waitFor([](auto& v) { return ThreadedRig::convergedAfterMesh(v); }));
    ClientSim live;
    size_t seen = 0;
    {
        std::lock_guard<std::mutex> lock(rig.m);
        // the geometry comes before any pixels (a status may precede it: the worker reports "on, nothing yet" if it wakes before the snapshot arrives)
        size_t meshAt = rig.msgs.size(), tilesAt = rig.msgs.size();
        for (size_t i = 0; i < rig.msgs.size(); ++i) {
            if (rig.msgs[i]["type"] == "lit_mesh") meshAt = std::min(meshAt, i);
            if (rig.msgs[i]["type"] == "lightmap_tiles") tilesAt = std::min(tilesAt, i);
        }
        REQUIRE(meshAt < tilesAt);
        for (const auto& mm : rig.msgs) live.apply(mm);
        seen = rig.msgs.size();
    }
    CHECK(live.meshes == 1);
    CHECK(live.lastKeep == false);
    CHECK(live.tilesApplied > 4);
    const size_t firstTiles = live.touched.size();
    live.touched.clear();
    const size_t firstMeshes = live.meshes;
    {   // a window opened now
        ClientSim late;
        for (const auto& s : rig.router.onConnect().toSender) late.apply(json::parse(s));
        CHECK(late.meshes == 1);
        CHECK(late.pages == live.pages);
    }
    // move the crate: new geometry, same atlas shape
    rig.router.onMessage(json{{"type", "document_update"}, {"doc", crateDoc(5.5)}}.dump());
    REQUIRE(rig.waitFor([&](auto& v) { return ThreadedRig::convergedAfterMesh(v, seen); }));
    {
        std::lock_guard<std::mutex> lock(rig.m);
        for (size_t i = seen; i < rig.msgs.size(); ++i) live.apply(rig.msgs[i]);
    }
    CHECK(live.meshes == static_cast<int>(firstMeshes) + 1);
    CHECK(live.lastKeep == true);  // the Shell keeps its pages and its texture
    CHECK(live.touched.size() > 0);
    CHECK(live.touched.size() < firstTiles / 2);  // not the whole atlas again: what did not change is not sent
    ClientSim late;
    for (const auto& s : rig.router.onConnect().toSender) late.apply(json::parse(s));
    CHECK(late.pages == live.pages);
    CHECK(late.lastKeep == false);  // a client served from the mirror starts from empty pages, though the stored mesh was a keepPages one
    CHECK(late.rev == live.rev);
    CHECK(late.triangles == live.triangles);
}

TEST_CASE("lighting service (thread): edits in quick succession end in the state of the last one, nothing queues up") {
    ThreadedRig rig(crateDoc(5), 15);  // 15 ms per GPU run: an edit usually arrives while a bake is in progress
    rig.router.onMessage(R"({"type":"lighting","enabled":true,"camera":[0,1,0]})");
    REQUIRE(rig.waitFor([](auto& v) { return ThreadedRig::convergedAfterMesh(v); }));
    size_t seen;
    {
        std::lock_guard<std::mutex> lock(rig.m);
        seen = rig.msgs.size();
    }
    for (int i = 0; i < 12; ++i) {
        rig.router.onMessage(json{{"type", "document_update"}, {"doc", crateDoc(5 + 0.2 * i)}}.dump());
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    // settled, not only converged: the denoised page is sent after convergence, and the late client below gets the mirror, which has it at once
    REQUIRE(rig.waitFor([&](auto& v) { return ThreadedRig::settledAfterMesh(v, seen); }));
    // the mesh the Shell ends up with is the one of the last document
    TempDir d2;
    d2.write("maps/a.map.json", crateDoc(5 + 0.2 * 11));
    Scene s2(d2.path);
    s2.loadGameProfile();
    s2.openDocument("maps/a.map.json");
    Router r2(s2);
    r2.lighting().setSessionFactory(noiseFactory());
    r2.onMessage(R"({"type":"lighting","enabled":true})");
    int expectedTriangles = -1;
    for (int i = 0; i < 400 && expectedTriangles < 0; ++i)
        for (const auto& m : r2.onLightingTick(0.05).toAll) {
            const json j = json::parse(m);
            if (j["type"] == "lit_mesh") expectedTriangles = j["triangleCount"];
        }
    ClientSim c;
    {
        std::lock_guard<std::mutex> lock(rig.m);
        for (const auto& mm : rig.msgs) c.apply(mm);
        // fewer meshes than edits is fine (latest wins), but the last one is the final geometry
        CHECK(c.meshes >= 2);
        CHECK(c.triangles == static_cast<size_t>(expectedTriangles));
    }
    ClientSim late;
    for (const auto& s : rig.router.onConnect().toSender) late.apply(json::parse(s));
    CHECK(late.pages == c.pages);
}

TEST_CASE("lighting service (thread): disabling drops the state, enabling again sends a complete one") {
    ThreadedRig rig(crateDoc(5));
    rig.router.onMessage(R"({"type":"lighting","enabled":true})");
    REQUIRE(rig.waitFor([](auto& v) { return ThreadedRig::convergedAfterMesh(v); }));
    size_t seen;
    {
        std::lock_guard<std::mutex> lock(rig.m);
        seen = rig.msgs.size();
    }
    rig.router.onMessage(R"({"type":"lighting","enabled":false})");
    REQUIRE(rig.waitFor([&](auto& v) {
        for (size_t i = seen; i < v.size(); ++i) if (v[i]["type"] == "lighting_status" && v[i]["enabled"] == false) return true;
        return false;
    }));
    for (const auto& m : rig.router.onConnect().toSender) {  // nothing to show
        const std::string type = json::parse(m)["type"];
        CHECK(type != "lit_mesh");
        CHECK(type != "lightmap_tiles");
    }
    {
        std::lock_guard<std::mutex> lock(rig.m);
        seen = rig.msgs.size();
    }
    rig.router.onMessage(R"({"type":"lighting","enabled":true})");
    const bool ok = rig.waitFor([&](auto& v) { return ThreadedRig::convergedAfterMesh(v, seen); }, 5);
    if (!ok) { std::lock_guard<std::mutex> lock(rig.m); for (size_t i = seen; i < rig.msgs.size(); ++i) MESSAGE("msg ", i, " ", rig.msgs[i]["type"].dump(), " ", rig.msgs[i].dump().substr(0, 160)); }
    REQUIRE(ok);
    ClientSim c;
    {
        std::lock_guard<std::mutex> lock(rig.m);
        for (size_t i = seen; i < rig.msgs.size(); ++i) c.apply(rig.msgs[i]);
    }
    CHECK(c.meshes == 1);
    CHECK(c.lastKeep == false);  // a Shell that dropped its lightmap needs the pages again
    ClientSim late;
    for (const auto& s : rig.router.onConnect().toSender) late.apply(json::parse(s));
    CHECK(late.pages == c.pages);
}

TEST_CASE("lighting service: moving only a light does not rebuild the geometry, and the Shell keeps its mesh") {
    Rig rig(roomDoc(0));
    rig.router.lighting().setSessionFactory(noiseFactory());
    rig.router.onMessage(R"({"type":"lighting","enabled":true})");
    rig.run();
    const int rev0 = rig.router.lighting().geometryRevision();
    rig.router.onMessage(json{{"type", "document_update"}, {"doc", roomDoc(1.5)}}.dump());
    const auto msgs = rig.run();
    CHECK(rig.router.lighting().geometryRevision() == rev0);
    CHECK(rig.router.lighting().timings().geometry == 0);  // no layout, no samples, no chunks
    int meshes = 0, tiles = 0;
    for (const auto& m : msgs) {
        if (m["type"] == "lit_mesh") ++meshes;
        if (m["type"] == "lightmap_tiles") ++tiles;
    }
    CHECK(meshes == 0);
    CHECK(tiles > 0);  // the new light shows
}

TEST_CASE("lighting service (thread): tiles arrive while the refinement is still running, not when it is over") {
    LightingService::Options opt;
    opt.live.bounces = false;
    opt.flushIntervalMs = 5;
    ThreadedRig rig(crateDoc(5), 10, opt);  // 10 ms per GPU run, noise that changes with every pass
    rig.router.onMessage(R"({"type":"lighting","enabled":true,"camera":[0,1,0]})");
    REQUIRE(rig.waitFor([](auto& v) { return ThreadedRig::convergedAfterMesh(v); }));
    std::lock_guard<std::mutex> lock(rig.m);
    size_t firstTiles = rig.msgs.size(), converged = rig.msgs.size(), tiles = 0;
    for (size_t i = 0; i < rig.msgs.size(); ++i) {
        const std::string t = rig.msgs[i]["type"];
        if (t == "lightmap_tiles") { ++tiles; firstTiles = std::min(firstTiles, i); }
        if (t == "lighting_status" && rig.msgs[i]["converged"] == true) { converged = std::min(converged, i); }
    }
    CHECK(tiles >= 3);                // several flushes, not one at the end
    CHECK(firstTiles < converged);    // the first pixels were out before everything converged
    // and a status said "not converged yet" while they arrived
    bool sawRunning = false;
    for (const auto& m : rig.msgs) if (m["type"] == "lighting_status" && m["converged"] == false) sawRunning = true;
    CHECK(sawRunning);
}

TEST_CASE("lighting service (thread): the denoiser runs on its own thread, edits meanwhile are served, nothing stale is shown") {
    if (!denoise::probe().available) { MESSAGE("OpenImageDenoise not available, skipped"); return; }
    LightingService::Options opt;
    opt.live.bounces = true;
    opt.live.denoiseIndirect = true;
    opt.live.maxPasses = 3;
    ThreadedRig rig(crateDoc(5), 0, opt);
    rig.router.onMessage(R"({"type":"lighting","enabled":true,"camera":[0,1,0]})");
    auto denoisedAfter = [](const std::vector<json>& v, size_t from) {
        for (size_t i = from; i < v.size(); ++i) if (v[i]["type"] == "lighting_status" && v[i]["denoisedPages"].get<int>() > 0) return true;
        return false;
    };
    REQUIRE(rig.waitFor([&](auto& v) { return denoisedAfter(v, 0); }, 60));
    size_t seen;
    {
        std::lock_guard<std::mutex> lock(rig.m);
        seen = rig.msgs.size();
    }
    // edits right after each other while the filter of the next round may be running
    for (int i = 0; i < 4; ++i) {
        rig.router.onMessage(json{{"type", "document_update"}, {"doc", crateDoc(5 + 0.3 * i)}}.dump());
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    REQUIRE(rig.waitFor([&](auto& v) { return ThreadedRig::convergedAfterMesh(v, seen); }, 60));
    REQUIRE(rig.waitFor([&](auto& v) { return denoisedAfter(v, seen); }, 60));  // and the last state gets its denoised light too
    // a denoised page can still be on its way after the status that announced it (busy machine): compare once the service is quiet
    // The record a late client is served from is updated just before a batch leaves, so on a busy machine a snapshot can fall between the two (a denoise
    // pass takes hundreds of ms). Compare at rest: a live client and a late one must agree once nothing is on its way any more.
    ClientSim live, late;
    for (int attempt = 0; attempt < 50; ++attempt) {
        REQUIRE(rig.waitQuiet());
        live = ClientSim();
        {
            std::lock_guard<std::mutex> lock(rig.m);
            for (const auto& m : rig.msgs) live.apply(m);
        }
        late = ClientSim();
        for (const auto& s : rig.router.onConnect().toSender) late.apply(json::parse(s));
        if (late.pages == live.pages) break;
    }
    CHECK(live.droppedTileMessages == 0);
    CHECK(late.pages == live.pages);
}

TEST_CASE("lighting service (thread): while edits keep coming the Shell still gets results, it does not wait for the last edit") {
    LightingService::Options opt;
    opt.live.bounces = false;
    ThreadedRig rig(crateDoc(5), 25, opt);  // a step takes 25 ms, edits come every millisecond: every snapshot is superseded before its bake could finish
    rig.router.onMessage(R"({"type":"lighting","enabled":true,"camera":[0,1,0]})");
    REQUIRE(rig.waitFor([](auto& v) { return ThreadedRig::convergedAfterMesh(v); }));
    size_t before;
    {
        std::lock_guard<std::mutex> lock(rig.m);
        before = rig.msgs.size();
    }
    // Edits back to back for a fixed time, far faster than a bake takes. Without a progress guarantee every snapshot is superseded before its first pass
    // is out and nothing at all reaches the Shell until the drag stops; with it a result arrives per bake cycle.
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; std::chrono::steady_clock::now() - start < std::chrono::seconds(3); ++i) {
        rig.router.onMessage(json{{"type", "document_update"}, {"doc", crateDoc(5 + 0.02 * (i % 40))}}.dump());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    size_t meshes = 0, tiles = 0;
    {   // what was delivered while the drag was going on
        std::lock_guard<std::mutex> lock(rig.m);
        for (size_t k = before; k < rig.msgs.size(); ++k) {
            if (rig.msgs[k]["type"] == "lit_mesh") ++meshes;
            if (rig.msgs[k]["type"] == "lightmap_tiles") ++tiles;
        }
    }
    CHECK(meshes >= 2);
    CHECK(tiles >= 2);
    REQUIRE(rig.waitFor([&](auto& v) { return ThreadedRig::convergedAfterMesh(v, before); }));
}

TEST_CASE("lighting service (thread): style layers and the light volume reach a live client and a client that joins later alike, an edit keeps the layers' pages") {
    auto doc = [](double doorX) {
        json lamp = lightJson("styled", {0, 2.5, 0}, 20, 30);
        lamp["keyvalues"]["style"] = "5";
        return docJson({entityJson("w", "worldspawn", {Brush::makeBox("floor", {-4, -1, -4}, {4, 0, 4})}), lamp, lightJson("plain", {-2, 2.5, 0}, 8, 20),
                        entityJson("door", "func_door", {Brush::makeBox("d", {doorX, 0, 1}, {doorX + 1, 2, 1.3})})});
    };
    LightingService::Options opt;
    opt.live.bounces = false;
    ThreadedRig rig(doc(1), 0, opt);
    rig.router.onMessage(R"({"type":"lighting","enabled":true,"camera":[0,1,0]})");
    auto has = [](const std::vector<json>& v, const char* type, size_t from = 0) {
        for (size_t i = from; i < v.size(); ++i) if (v[i]["type"] == type) return true;
        return false;
    };
    REQUIRE(rig.waitFor([&](auto& v) { return ThreadedRig::convergedAfterMesh(v) && has(v, "lightvol") && has(v, "entity_light"); }, 60));
    ClientSim live;
    size_t seen = 0;
    {
        std::lock_guard<std::mutex> lock(rig.m);
        for (const auto& mm : rig.msgs) live.apply(mm);
        seen = rig.msgs.size();
    }
    CHECK(live.pages.count(0) == 1);
    CHECK(live.pages.count(5) == 1);  // the styled lamp is its own layer
    {
        ClientSim late;
        std::vector<json> lateMsgs;
        for (const auto& s : rig.router.onConnect().toSender) { lateMsgs.push_back(json::parse(s)); late.apply(lateMsgs.back()); }
        CHECK(late.pages == live.pages);
        CHECK(has(lateMsgs, "light_styles"));
        CHECK(has(lateMsgs, "lightvol"));
        CHECK(has(lateMsgs, "entity_light"));
    }
    // move the door: the geometry changes, the atlas and the set of layers stay, so the Shell keeps its pages (of every layer)
    rig.router.onMessage(json{{"type", "document_update"}, {"doc", doc(1.5)}}.dump());
    REQUIRE(rig.waitFor([&](auto& v) { return ThreadedRig::convergedAfterMesh(v, seen) && has(v, "entity_light", seen); }, 60));
    {
        std::lock_guard<std::mutex> lock(rig.m);
        for (size_t i = seen; i < rig.msgs.size(); ++i) live.apply(rig.msgs[i]);
        // the table is not sent again when it did not change: the Shell would rebuild its texture for nothing
        CHECK_FALSE(has(rig.msgs, "light_styles", seen));
    }
    CHECK(live.lastKeep == true);
    ClientSim late;
    for (const auto& s : rig.router.onConnect().toSender) late.apply(json::parse(s));
    CHECK(late.pages == live.pages);
}

TEST_CASE("lighting service: a face's lightmapScale reaches the baker (more texels), also when nothing else in the document changed") {
    auto doc = [](double scale) {
        json j = roomDoc();
        for (auto& f : j["entities"][0]["brushes"][0]["faces"]) f["lightmapScale"] = scale;
        return j;
    };
    Rig rig(doc(1));
    rig.router.onMessage(R"({"type":"lighting","enabled":true})");
    auto texels = [&] {
        double n = 0;
        for (const auto& m : settle(rig)) if (m["type"] == "lighting_status") n = m["texels"].get<double>();
        return n;
    };
    const double base = texels();
    REQUIRE(base > 0);
    rig.router.onMessage(json{{"type", "document_update"}, {"doc", doc(2)}}.dump());
    const double dense = texels();
    CHECK(dense > base * 3);  // twice the texels per metre = about four times the texels
    rig.router.onMessage(json{{"type", "document_update"}, {"doc", doc(1)}}.dump());
    CHECK(texels() == doctest::Approx(base).epsilon(0.05));
}
