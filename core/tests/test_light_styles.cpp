#include "core/light_style_analysis.h"
#include "core/light_styles.h"
#include "core/lightmap_uv.h"
#include "test_util.h"
#include <doctest.h>
#include <cmath>
#include <fstream>

using namespace core;
using namespace testutil;

namespace {
Entity ent(const std::string& id, const std::string& cls, std::vector<Brush> brushes = {}) {
    Entity e;
    e.id = id;
    e.classname = cls;
    e.brushes = std::move(brushes);
    return e;
}
Entity lightAt(const std::string& id, Vec3 pos, int style, double radius = 10, double brightness = 1, bool shadows = false) {
    Entity e = ent(id, "light_point");
    e.origin = pos;
    e.keyvalues["style"] = std::to_string(style);
    e.keyvalues["radius"] = std::to_string(radius);
    e.keyvalues["brightness"] = std::to_string(brightness);
    e.keyvalues["cast_shadows"] = shadows ? "1" : "0";
    return e;
}
}  // namespace

TEST_CASE("light styles: the engine's pattern table, 10 Hz, a = 0, m = 1, z = 25/12") {
    const LightStyleTable t = LightStyleTable::defaults();
    CHECK(t.value(0, 0.0) == 1.0);
    CHECK(t.value(0, 123.4) == 1.0);
    // style 9 = slow strobe "aaaaaaaazzzzzzzz": 8 frames dark, 8 frames at 'z'
    CHECK(t.value(9, 0.0) == 0.0);
    CHECK(t.value(9, 0.79) == 0.0);
    CHECK(t.value(9, 0.80) == doctest::Approx(25.0 / 12.0));
    CHECK(t.value(9, 1.59) == doctest::Approx(25.0 / 12.0));
    CHECK(t.value(9, 1.60) == 0.0);  // wraps after 16 frames
    // style 2 pulse: a..z..a, 'm' is the 13th frame
    CHECK(t.value(2, 0.0) == 0.0);
    CHECK(t.value(2, 1.2) == doctest::Approx(1.0));
    // no pattern = constant 1; ids outside the table = 1; negative time still indexes inside the pattern
    CHECK(t.value(40, 3.3) == 1.0);
    CHECK(t.value(64, 3.3) == 1.0);
    CHECK(t.value(-1, 3.3) == 1.0);
    const double neg = t.value(9, -0.05);
    CHECK((neg == 0.0 || neg == doctest::Approx(25.0 / 12.0)));
    CHECK(validStylePattern("abcz"));
    CHECK(!validStylePattern("abC"));
    CHECK(t.toJson().size() == 64);
}

TEST_CASE("light styles: a game profile overrides patterns, rejects bad ones") {
    TempDir dir;
    dir.write("game_profile.json", json{{"name", "t"}, {"textureRoot", "textures"}, {"textureExtensions", {".png"}},
                                        {"lighting", {{"lightStyles", {{"12", "ma"}, {"3", "z"}}}}}});
    const GameProfile p = loadProfile(dir.path);
    const LightStyleTable t = LightStyleTable::fromProfile(p);
    CHECK(t.patterns[12] == "ma");
    CHECK(t.patterns[3] == "z");   // overrides the built-in candle
    CHECK(t.patterns[1] == LightStyleTable::defaults().patterns[1]);
    CHECK(t.value(12, 0.0) == 1.0);
    CHECK(t.value(12, 0.1) == 0.0);
    TempDir bad;
    bad.write("game_profile.json", json{{"name", "t"}, {"textureRoot", "textures"}, {"textureExtensions", {".png"}}, {"lighting", {{"lightStyles", {{"5", "mAm"}}}}}});
    CHECK_THROWS(loadProfile(bad.path));
    TempDir bad2;
    bad2.write("game_profile.json", json{{"name", "t"}, {"textureRoot", "textures"}, {"textureExtensions", {".png"}}, {"lighting", {{"lightStyles", {{"64", "m"}}}}}});
    CHECK_THROWS(loadProfile(bad2.path));
}

TEST_CASE("light styles: a light's style is read up to 63, lightsOfStyle splits the scene, ambient stays in style 0") {
    Document doc;
    Entity w = ent("w", "worldspawn");
    w.keyvalues["_ambient"] = "0.1 0.1 0.1";
    doc.entities = {w, lightAt("a", {0, 2, 0}, 0), lightAt("b", {1, 2, 0}, 5), lightAt("c", {2, 2, 0}, 63), lightAt("d", {3, 2, 0}, 99)};
    const LightScene all = collectLights(doc, {});
    CHECK(lightStyles(all) == std::vector<int>{0, 5, 63});
    CHECK(lightsOfStyle(all, 0).lights.size() == 1);
    CHECK(lightsOfStyle(all, 0).ambient.x == doctest::Approx(0.1));
    CHECK(lightsOfStyle(all, 5).lights.size() == 1);
    CHECK(lightsOfStyle(all, 5).ambient.x == 0.0);
    CHECK(lightsOfStyle(all, 63).lights.size() == 2);  // 99 is clamped to 63
}

namespace {
// Two rooms side by side, 12 m apart, one wall between them; floors only matter.
struct TwoRooms {
    Document doc;
    std::vector<LightSurface> surfaces;
    LightingClasses classes = LightingClasses::defaults();
    explicit TwoRooms(std::vector<Entity> lights) {
        doc.entities = {ent("w", "worldspawn", {Brush::makeBox("floorA", {-6, -1, -6}, {6, 0, 6}), Brush::makeBox("floorB", {20, -1, -6}, {32, 0, 6}),
                                                Brush::makeBox("wall", {7, -1, -6}, {8, 4, 6})})};
        for (auto& l : lights) doc.entities.push_back(l);
        surfaces = collectSurfaces(doc, {});
    }
    LightScene lights() const { return collectLights(doc, {}); }
};
std::string keyOf(const std::string& brush, uint32_t face) {
    LightmapFace f;
    f.entityId = "w"; f.brushId = brush; f.faceId = face; f.side = FaceSide::Front;
    return f.key();
}
size_t countStyleFaces(const StyleAnalysis& a, int style) {
    size_t n = 0;
    for (const auto& [k, v] : a.faceStyles) if (std::find(v.begin(), v.end(), style) != v.end()) ++n;
    return n;
}
}  // namespace

TEST_CASE("style analysis: a styled light touches only the faces it can reach (range, facing)") {
    TwoRooms r({lightAt("s", {0, 2, 0}, 3, 8)});  // radius 8 around room A's centre
    const StyleAnalysis a = analyseLightStyles(r.surfaces, r.classes, r.lights(), nullptr);
    REQUIRE(!a.faceStyles.empty());
    // every face of floorA's top that is within 8 m and faces up is touched; nothing of floorB (20 m away)
    for (const auto& [k, v] : a.faceStyles) {
        CHECK(k.find("|floorB|") == std::string::npos);
        CHECK(v == std::vector<int>{3});
    }
    bool floorA = false;
    for (const auto& [k, v] : a.faceStyles) if (k.find("|floorA|") != std::string::npos) floorA = true;
    CHECK(floorA);
    // unstyled lights never produce entries
    TwoRooms none({lightAt("u", {0, 2, 0}, 0, 8)});
    CHECK(analyseLightStyles(none.surfaces, none.classes, none.lights(), nullptr).faceStyles.empty());
}

TEST_CASE("style analysis: shadows - a wall hides a light from the other room, so that room's faces are not touched (unlike q3map2)") {
    // the light sits next to the wall in room A with a radius reaching over to room B's near edge, but the wall blocks it
    TwoRooms r({lightAt("s", {5, 2, 0}, 4, 30, 1, true)});
    OccluderMesh mesh = buildOccluders(r.surfaces, r.classes);
    TriangleBvh bvh(mesh);
    const StyleAnalysis lit = analyseLightStyles(r.surfaces, r.classes, r.lights(), &bvh);
    const StyleAnalysis noShadow = analyseLightStyles(r.surfaces, r.classes, r.lights(), nullptr);
    size_t bLit = 0, bNoShadow = 0;
    for (const auto& [k, v] : lit.faceStyles) if (k.find("|floorB|") != std::string::npos) ++bLit;
    for (const auto& [k, v] : noShadow.faceStyles) if (k.find("|floorB|") != std::string::npos) ++bNoShadow;
    CHECK(bNoShadow > 0);  // in range without the occlusion test (what a naive assignment does)
    CHECK(bLit == 0);      // the wall blocks every ray (6 m of wall height covers the line to the far room)
}

TEST_CASE("style analysis: more styles than slots - the weakest are discarded and reported") {
    std::vector<Entity> ls;
    for (int s = 1; s <= 10; ++s) ls.push_back(lightAt("l" + std::to_string(s), {0.0, 2.0, 0.0}, s, 20, 1.0 + s));  // brighter with the id
    TwoRooms r(ls);
    const StyleAnalysis a = analyseLightStyles(r.surfaces, r.classes, r.lights(), nullptr);
    const std::string top = keyOf("floorA", 0);
    bool found = false;
    for (const auto& [k, v] : a.faceStyles) {
        CHECK(v.size() <= 7);  // 8 slots including the base
        if (k.find("|floorA|") != std::string::npos && v.size() == 7) {
            found = true;
            // kept: the 7 strongest = styles 4..10; dropped: 1,2,3
            CHECK(v == std::vector<int>{4, 5, 6, 7, 8, 9, 10});
            CHECK(a.discarded.at(k) == std::vector<int>{1, 2, 3});
        }
    }
    CHECK(found);
    CHECK(!a.warnings.empty());
    CHECK(a.warnings[0].find("discarded") != std::string::npos);
    (void)top;
    // a smaller budget (the settings): 3 slots = base + 2
    StyleAnalysisSettings cfg;
    cfg.maxSlots = 3;
    for (const auto& [k, v] : analyseLightStyles(r.surfaces, r.classes, r.lights(), nullptr, cfg).faceStyles) CHECK(v.size() <= 2);
}

TEST_CASE("lightmap layout: faces that different styles touch are never merged into one chart; charts list their style layers") {
    Document doc;
    // one big floor made of two coplanar brushes side by side (they would merge into one chart)
    doc.entities = {ent("w", "worldspawn", {Brush::makeBox("a", {0, -1, 0}, {4, 0, 4}), Brush::makeBox("b", {4, -1, 0}, {8, 0, 4})})};
    const auto surfaces = collectSurfaces(doc, {});
    const LightingClasses classes = LightingClasses::defaults();
    LightmapSettings cfg;
    const LightmapLayout plain = buildLightmapLayout(surfaces, classes, cfg);
    size_t topA = 0;
    for (const auto& f : plain.faces) if (f.normal.y > 0.9) ++topA;
    REQUIRE(topA == 2);
    int chartA = -1, chartB = -1;
    for (const auto& f : plain.faces) if (f.normal.y > 0.9) (f.brushId == "a" ? chartA : chartB) = f.chart;
    CHECK(chartA == chartB);  // merged without styles
    CHECK(plain.charts[static_cast<size_t>(chartA)].styles == std::vector<int>{0});

    for (const auto& f : plain.faces) if (f.brushId == "a" && f.normal.y > 0.9) cfg.faceStyles[f.key()] = {2};
    const LightmapLayout styled = buildLightmapLayout(surfaces, classes, cfg);
    for (const auto& f : styled.faces) if (f.normal.y > 0.9) (f.brushId == "a" ? chartA : chartB) = f.chart;
    CHECK(chartA != chartB);  // different style sets: never merged
    CHECK(styled.charts[static_cast<size_t>(chartA)].styles == std::vector<int>{0, 2});
    CHECK(styled.charts[static_cast<size_t>(chartB)].styles == std::vector<int>{0});
    CHECK(styled.toJson()["charts"][static_cast<size_t>(chartA)]["styles"] == json::array({0, 2}));

    // equal sets merge again
    for (const auto& f : plain.faces) if (f.brushId == "b" && f.normal.y > 0.9) cfg.faceStyles[f.key()] = {2};
    const LightmapLayout both = buildLightmapLayout(surfaces, classes, cfg);
    for (const auto& f : both.faces) if (f.normal.y > 0.9) (f.brushId == "a" ? chartA : chartB) = f.chart;
    CHECK(chartA == chartB);
    CHECK(both.charts[static_cast<size_t>(chartA)].styles == std::vector<int>{0, 2});
}

TEST_CASE("lightmap layout: layoutForStyle keeps the charts of a style at the same page rectangle, renumbered") {
    Document doc;
    doc.entities = {ent("w", "worldspawn", {Brush::makeBox("a", {0, -1, 0}, {4, 0, 4}), Brush::makeBox("b", {10, -1, 0}, {14, 0, 4})})};
    const auto surfaces = collectSurfaces(doc, {});
    LightmapSettings cfg;
    for (const auto& f : buildLightmapLayout(surfaces, LightingClasses::defaults(), cfg).faces)
        if (f.brushId == "b" && f.normal.y > 0.9) cfg.faceStyles[f.key()] = {7};
    const LightmapLayout full = buildLightmapLayout(surfaces, LightingClasses::defaults(), cfg);
    const LightmapLayout base = layoutForStyle(full, 0);
    const LightmapLayout s7 = layoutForStyle(full, 7);
    CHECK(base.charts.size() == full.charts.size());  // every chart has the base
    CHECK(base.faces.size() == full.faces.size());
    REQUIRE(s7.charts.size() == 1);
    CHECK(s7.faces.size() == 1);
    CHECK(s7.faces[0].chart == 0);
    CHECK(s7.faces[0].brushId == "b");
    // same rectangle as in the full layout
    for (const auto& c : full.charts) {
        if (std::find(c.styles.begin(), c.styles.end(), 7) == c.styles.end()) continue;
        CHECK(s7.charts[0].page == c.page);
        CHECK(s7.charts[0].x == c.x);
        CHECK(s7.charts[0].y == c.y);
        CHECK(s7.charts[0].w == c.w);
    }
    CHECK(s7.pageCount == full.pageCount);
    CHECK(s7.faces[0].texel({12, 0, 2}).u == doctest::Approx(full.find("", "w", "", "b", s7.faces[0].faceId, FaceSide::Front)->texel({12, 0, 2}).u));
    CHECK(layoutForStyle(full, 3).charts.empty());
}
