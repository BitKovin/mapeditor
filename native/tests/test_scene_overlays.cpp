// T3.6: the pure parts of the overlays, converted from the Shell's tests: point entity boxes (pointBox.test.ts), entity arrows (arrows.test.ts) and the size
// labels (the H7 case of batch33.test.ts). Headless.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <map>

#include "core/prefab.h"
#include "render/glyph_atlas.h"
#include "scene/arrows.h"
#include "scene/point_box.h"
#include "scene/size_labels.h"

using namespace native::scene;

namespace {

const Box kLong{{-1, -0.1, -0.1}, {1, 0.1, 0.1}};   // 2 m along x, thin
const BoxOf longBox = [](const std::string&) { return kLong; };
const Box kCube = boxFromQuake(std::array<double, 6>{-16, -16, -16, 16, 16, 16});   // 1 m cube, centred
const BoxOf cubeBox = [](const std::string&) { return kCube; };

core::Entity ent(const std::string& id, const std::string& classname = "info_x") {
    core::Entity e;
    e.id = id;
    e.classname = classname;
    return e;
}

bool near(const V3& a, const V3& b, double d = 1e-6) { return std::fabs(a[0] - b[0]) < d && std::fabs(a[1] - b[1]) < d && std::fabs(a[2] - b[2]) < d; }

struct Seg {
    V3 a, b;
};
Seg seg(const std::vector<double>& l, size_t i) { return {{l[i * 6], l[i * 6 + 1], l[i * 6 + 2]}, {l[i * 6 + 3], l[i * 6 + 4], l[i * 6 + 5]}}; }

} // namespace

TEST_CASE("point entity box follows angles") {
    SUBCASE("with no angles it is the class box at the origin") {
        core::Entity e = ent("e1");
        e.origin = {1, 2, 3};
        const Box b = boundsOfBox(pointBox(e, longBox));
        CHECK(near(b.min, {0, 1.9, 2.9}));
        CHECK(near(b.max, {2, 2.1, 3.1}));
    }
    SUBCASE("a yaw of 90 degrees turns the long side from x to z") {
        core::Entity e = ent("e1");
        e.angles = {0, 90, 0};
        const Box b = boundsOfBox(pointBox(e, longBox));
        CHECK(b.max[2] - b.min[2] == doctest::Approx(2));
        CHECK(b.max[0] - b.min[0] == doctest::Approx(0.2));
    }
    SUBCASE("an off-centre box swings round the origin, and the direction arrow starts at its turned centre") {
        const Box off{{0, 0, 0}, {2, 2, 2}};
        core::Entity e = ent("e1");
        e.angles = {0, 90, 0};
        const auto a = directionArrow(e, [&](const std::string&) { return off; });
        CHECK(near({a[0], a[1], a[2]}, {1, 1, -1}));   // R(yaw 90) * (1, 1, 1)
    }
    SUBCASE("the Quake box is converted to metres and Y-up, a missing box is half a metre") {
        const Box b = boxFromQuake(std::array<double, 6>{-16, -32, 0, 16, 32, 64});
        CHECK(near(b.min, {-0.5, 0, -1}));
        CHECK(near(b.max, {0.5, 2, 1}));
        const Box d = boxFromQuake(std::nullopt);
        CHECK(near(d.min, {-0.25, -0.25, -0.25}));
        CHECK(near(d.max, {0.25, 0.25, 0.25}));
    }
}

TEST_CASE("lights and decals have no drawn box but stay selectable") {
    CHECK(drawsBox(ent("e1")));
    CHECK_FALSE(drawsBox(ent("e1", "light_point")));
    CHECK_FALSE(drawsBox(ent("e1", "light_spot")));
    CHECK_FALSE(drawsBox(ent("e1", "decal")));
    core::Entity d = ent("e1", "decal");
    d.keyvalues = {{"width", "2"}, {"height", "1"}, {"depth", "0.5"}};
    const Box local = decalLocalBox(d);
    CHECK(near(local.min, {-1, -0.25, -0.5}));
    CHECK(near(local.max, {1, 0.25, 0.5}));
    // bad sizes fall back to the defaults Core uses
    core::Entity bad = ent("e2", "decal");
    bad.keyvalues = {{"width", "-3"}, {"height", "x"}};
    CHECK(near(decalLocalBox(bad).max, {0.5, 0.25, 0.5}));
}

TEST_CASE("direction arrow") {
    SUBCASE("starts at the box centre, points along R(angles) * (0, 0, 1) and is longer than the box half-size") {
        core::Entity e = ent("p");
        e.origin = {1, 2, 3};
        const auto a = directionArrow(e, cubeBox);
        const Seg s = seg(a, 0);
        CHECK(near(s.a, {1, 2, 3}));
        CHECK(s.b[0] == doctest::Approx(1));
        CHECK(s.b[1] == doctest::Approx(2));
        CHECK(s.b[2] > 3 + 0.5);   // yaw 0: +Z, past the box face
        CHECK(a.size() / 6 == 1 + 4);   // shaft + four barbs
    }
    SUBCASE("yaw 90 points +X, pitch changes the elevation") {
        core::Entity e = ent("p");
        e.angles = {0, 90, 0};
        const Seg s = seg(directionArrow(e, cubeBox), 0);
        CHECK(s.b[0] > 0.5);
        CHECK(s.b[2] == doctest::Approx(0).epsilon(1e-9));
        core::Entity up = ent("p");
        up.angles = {-90, 0, 0};
        const Seg u = seg(directionArrow(up, cubeBox), 0);
        CHECK(u.b[1] > 0.5);
        CHECK(std::fabs(u.b[2]) < 1e-9);
    }
    SUBCASE("a bigger box gives a longer arrow") {
        const Box big = boxFromQuake(std::array<double, 6>{-64, -64, -64, 64, 64, 64});
        const auto len = [](const std::vector<double>& a) { return std::hypot(a[3] - a[0], a[4] - a[1], a[5] - a[2]); };
        CHECK(len(directionArrow(ent("p"), [&](const std::string&) { return big; })) > len(directionArrow(ent("p"), cubeBox)) * 2);
    }
}

TEST_CASE("link arrow") {
    SUBCASE("runs between the boxes, head at the target's surface") {
        core::Entity a = ent("a"), b = ent("b");
        b.origin = {10, 0, 0};
        const Seg s = seg(linkArrow(boundsOf(a, cubeBox), boundsOf(b, cubeBox)), 0);
        CHECK(s.a[0] == doctest::Approx(0.5));
        CHECK(s.b[0] == doctest::Approx(9.5));   // trimmed to the faces of the 1 m boxes
        CHECK(s.a[1] == doctest::Approx(0));
    }
    SUBCASE("falls back to centre-to-centre when the boxes overlap, and draws nothing for one point") {
        core::Entity a = ent("a"), b = ent("b");
        b.origin = {0.5, 0, 0};
        const Box ba = boundsOf(a, cubeBox), bb = boundsOf(b, cubeBox);
        const Seg s = seg(linkArrow(ba, bb), 0);
        CHECK(s.a[0] == doctest::Approx(0));
        CHECK(s.b[0] == doctest::Approx(0.5));
        CHECK(linkArrow(ba, ba).empty());
    }
    SUBCASE("a brush entity is bounded by its brushes") {
        core::Entity e = ent("d", "func_door");
        e.brushes.push_back(core::Brush::makeBox("b", {0, 0, 0}, {2, 4, 6}, "dev/grey"));
        const Box b = boundsOf(e, cubeBox);
        CHECK(near(b.min, {0, 0, 0}));
        CHECK(near(b.max, {2, 4, 6}));
    }
}

namespace {
std::vector<std::string> keys(const std::string&) { return {"target", "killtarget"}; }
Placed P(const core::Entity& e, std::vector<std::string> ids = {}, std::map<std::string, size_t> scopes = {}) { return {&e, std::move(ids), e.id, std::move(scopes)}; }
std::string pair(const std::vector<Placed>& items, const Link& l) { return items[l.from].sourceId + ">" + items[l.to].sourceId; }
} // namespace

TEST_CASE("resolveLinks") {
    SUBCASE("follows refs (id paths) in the document") {
        core::Entity btn = ent("btn"), door = ent("door");
        btn.refs["target"] = core::RefTargets("door");
        const std::vector<Placed> items = {P(btn), P(door)};
        const auto l = resolveLinks(items, keys);
        REQUIRE(l.size() == 1);
        CHECK(pair(items, l[0]) == "btn>door");
        CHECK(l[0].key == "target");
    }
    SUBCASE("a ref with several targets links to each of them") {
        core::Entity btn = ent("btn"), d1 = ent("d1"), d2 = ent("d2");
        btn.refs["target"] = core::RefTargets(std::vector<std::string>{"d1", "nope", "d2"});
        const std::vector<Placed> items = {P(btn), P(d1), P(d2)};
        const auto l = resolveLinks(items, keys);
        REQUIRE(l.size() == 2);
        CHECK(items[l[0].to].sourceId == "d1");
        CHECK(items[l[1].to].sourceId == "d2");
    }
    SUBCASE("a plain name reaches every entity that has it") {
        core::Entity btn = ent("btn"), g1 = ent("g1"), g2 = ent("g2"), x = ent("x");
        btn.keyvalues["target"] = "gate";
        g1.keyvalues["targetname"] = "gate";
        g2.keyvalues["targetname"] = "gate";
        x.keyvalues["targetname"] = "other";
        const std::vector<Placed> items = {P(btn), P(g1), P(g2), P(x)};
        const auto l = resolveLinks(items, keys);
        REQUIRE(l.size() == 2);
        CHECK(items[l[0].to].sourceId == "g1");
        CHECK(items[l[1].to].sourceId == "g2");
    }
    SUBCASE("ignores dangling refs and self refs") {
        core::Entity a = ent("a"), b = ent("b");
        a.refs["target"] = core::RefTargets("nope");
        b.refs["target"] = core::RefTargets("b");
        CHECK(resolveLinks({P(a), P(b)}, keys).empty());
    }
    SUBCASE("resolves several reference keys of one entity") {
        core::Entity a = ent("a"), b = ent("b"), c = ent("c");
        a.refs["target"] = core::RefTargets("b");
        a.refs["killtarget"] = core::RefTargets("c");
        const std::vector<Placed> items = {P(a), P(b), P(c)};
        const auto l = resolveLinks(items, keys);
        REQUIRE(l.size() == 2);
        CHECK(l[0].key + ">" + items[l[0].to].sourceId == "target>b");
        CHECK(l[1].key + ">" + items[l[1].to].sourceId == "killtarget>c");
    }
    SUBCASE("a ref inside a prefab file is relative to that instance; one overridden in the root to the root") {
        core::Entity inner = ent("inner"), other = ent("other"), o2 = ent("o2"), world = ent("world");
        inner.refs["target"] = core::RefTargets("other");
        o2.refs["target"] = core::RefTargets("world");
        const std::vector<Placed> items = {P(inner, {"i1"}), P(other, {"i1"}), P(o2, {"i2"}, {{"target", 0}}), P(world)};
        const auto l = resolveLinks(items, keys);
        REQUIRE(l.size() == 2);
        CHECK(pair(items, l[0]) == "inner>other");
        CHECK(pair(items, l[1]) == "o2>world");
        CHECK(items[l[0].to].ids == std::vector<std::string>{"i1"});
        CHECK(items[l[1].to].ids.empty());
    }
    SUBCASE("a root entity can reach into a prefab (instance/child/entity path)") {
        core::Entity btn = ent("btn"), door = ent("door");
        btn.refs["target"] = core::RefTargets("i1/c1/door");
        const std::vector<Placed> items = {P(btn), P(door, {"i1", "c1"})};
        const auto l = resolveLinks(items, keys);
        REQUIRE(l.size() == 1);
        CHECK(items[l[0].to].ids == std::vector<std::string>{"i1", "c1"});
    }
    SUBCASE("the plain-name form: targetname in the same scope first, then a resolved global name") {
        core::Entity btn = ent("btn"), d = ent("d"), in = ent("in"), lamp1 = ent("lamp"), lamp2 = ent("lamp");
        btn.keyvalues["target"] = "door01";
        d.keyvalues["targetname"] = "door01";
        in.keyvalues["target"] = "lamp";
        lamp1.keyvalues["targetname"] = "lamp";
        lamp2.keyvalues["targetname"] = "lamp";
        const std::vector<Placed> items = {P(btn), P(d), P(in, {"i1"}), P(lamp1, {"i1"}), P(lamp2, {"i2"})};
        const auto l = resolveLinks(items, keys);
        REQUIRE(l.size() == 2);
        CHECK(pair(items, l[0]) == "btn>d");
        CHECK(pair(items, l[1]) == "in>lamp");
        CHECK(items[l[1].to].ids == std::vector<std::string>{"i1"});
        // a global resolved name (prefab content carries "instance|name")
        core::Entity b2 = ent("btn"), lock = ent("lock");
        b2.keyvalues["target"] = "door|lock";
        lock.keyvalues["targetname"] = "lock";
        const std::vector<Placed> items2 = {P(b2), P(lock, {"i9"})};
        const auto viaName = resolveLinks(items2, keys, [](const Placed& p) { return p.ids.empty() ? std::string() : "door|" + p.entity->keyvalues.at("targetname"); });
        CHECK(viaName.size() == 1);
    }
    SUBCASE("only reference keys count: targetname itself is not a link") {
        core::Entity a = ent("a"), b = ent("b");
        a.keyvalues["targetname"] = "b";
        b.keyvalues["targetname"] = "b";
        CHECK(resolveLinks({P(a), P(b)}, keys).empty());
    }
}

TEST_CASE("entityArrows: what the viewport draws") {
    core::Entity a = ent("a"), b = ent("b"), w = ent("w", "worldspawn"), dc = ent("dc", "decal");
    a.refs["target"] = core::RefTargets("b");
    b.origin = {5, 0, 0};
    w.brushes.push_back(core::Brush::makeBox("x", {0, 0, 0}, {1, 1, 1}, "dev/grey"));
    dc.origin = {9, 9, 9};
    const std::vector<Placed> items = {P(a), P(b), P(w), P(dc)};
    const auto opts = [&](bool dirs, bool all, std::function<bool(const Placed&)> sel = [](const Placed&) { return false; }) {
        ArrowOptions o;
        o.boxOf = cubeBox;
        o.colorOf = [](const std::string& c) -> std::optional<Rgb> { return c == "info_x" ? std::optional<Rgb>(Rgb{0.2, 0.4, 0.8}) : std::nullopt; };
        o.isSelected = std::move(sel);
        o.refKeys = keys;
        o.showDirections = dirs;
        o.showAllLinks = all;
        return o;
    };
    SUBCASE("every point entity gets an arrow in its class colour; worldspawn, brush entities and decals do not") {
        const auto o = entityArrows(items, opts(true, false));
        REQUIRE(o.size() == 1);
        CHECK(o[0].color == Rgb{0.2, 0.4, 0.8});
        CHECK(o[0].lines.size() / 6 == 2 * 5);   // a and b
    }
    SUBCASE("a selected entity's arrow is brighter") {
        const auto o = entityArrows(items, opts(true, false, [](const Placed& p) { return p.entity->id == "a"; }));
        REQUIRE(o.size() == 3);   // plain arrow, bright arrow, and a's link (touches the selection)
        const auto plain = std::find_if(o.begin(), o.end(), [](const Overlay& x) { return x.color[0] == 0.2; });
        const auto bright = std::find_if(o.begin(), o.end(), [](const Overlay& x) { return x.color[0] > 0.2; });
        REQUIRE(plain != o.end());
        REQUIRE(bright != o.end());
        CHECK(plain->lines.size() / 6 == 5);
        CHECK(bright->lines.size() / 6 == 5);
        for (size_t i = 0; i < 3; ++i) CHECK(bright->color[i] >= plain->color[i]);
    }
    SUBCASE("links: all of them when asked, only the selection's otherwise; the direction option is independent") {
        CHECK(entityArrows(items, opts(false, true)).size() == 1);    // one link, violet
        CHECK(entityArrows(items, opts(false, false)).empty());
        const auto sel = entityArrows(items, opts(false, false, [](const Placed& p) { return p.entity->id == "b"; }));   // the target being selected counts too
        REQUIRE(sel.size() == 1);
        CHECK(sel[0].color != entityArrows(items, opts(false, true))[0].color);
    }
}

TEST_CASE("prefab content: the expansion keeps the scope of every ref, so cross-instance links resolve like the picker") {
    core::Document file;
    file.entities.push_back(ent("btn"));
    file.entities.back().refs["target"] = core::RefTargets("door");
    file.entities.push_back(ent("door"));
    core::Document doc;
    doc.entities.push_back(ent("world_btn"));
    core::PrefabInstance inst;
    inst.id = "i1";
    inst.file = "p.map.json";
    inst.origin = {10, 0, 0};
    core::EntityOverride ov;
    ov.refs["target"] = core::RefTargets("world_btn");
    inst.overrides["door"] = ov;
    doc.prefabInstances.push_back(inst);
    const std::vector<core::ResolvedEntity> r = core::resolvePrefabs(doc, [&](const std::string&) -> const core::Document& { return file; });
    const auto find = [&](const char* id) { return std::find_if(r.begin(), r.end(), [&](const core::ResolvedEntity& x) { return x.sourceEntityId == id; }); };
    REQUIRE(find("btn") != r.end());
    REQUIRE(find("door") != r.end());
    CHECK(find("btn")->refScopes.at("target") == 1);
    CHECK(find("door")->refScopes.at("target") == 0);   // overridden in the root's instance: relative to the root
    const std::vector<Placed> items = placedEntities({&doc.entities[0]}, r);
    const auto links = resolveLinks(items, keys);
    std::vector<std::string> names;
    for (const Link& l : links) names.push_back(pair(items, l));
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"btn>door", "door>world_btn"});
    for (const Link& l : links)
        if (items[l.from].sourceId == "btn") CHECK(items[l.to].entity->origin.x == doctest::Approx(10));   // the arrow ends at the placed (root-space) door
}

TEST_CASE("selection size labels (H7): one per axis, outside the box; an orthographic view skips its depth axis; a face shows width and height") {
    // a simple perspective-like projection: x right, y up, z towards the viewer (shifted so every point is distinct)
    const ScreenOf screen = [](const V3& p) -> std::optional<ScreenPoint> { return ScreenPoint{400 + 160 * p[0] - 60 * p[2], 400 - 160 * p[1] + 40 * p[2]}; };
    const MetresPerPixel mpp = [](const V3&) { return 0.01; };
    const auto l3 = boxSizeLabels({0, 0, 0}, {2, 1, 3}, screen, mpp);
    REQUIRE(l3.size() == 3);
    CHECK(l3[0].text == "2 m");
    CHECK(l3[1].text == "1 m");
    CHECK(l3[2].text == "3 m");
    const V3 lo{0, 0, 0}, hi{2, 1, 3};
    for (const SizeLabel& l : l3) {
        bool outside = false;
        for (size_t i = 0; i < 3; ++i) outside = outside || l.at[i] < lo[i] || l.at[i] > hi[i];
        CHECK(outside);
    }
    // a far, small box: the numbers would cover each other, so fewer are shown
    const ScreenOf tinyScreen = [](const V3& p) -> std::optional<ScreenPoint> { return ScreenPoint{4 * p[0] - 1.5 * p[2], -4 * p[1] + p[2]}; };
    CHECK(boxSizeLabels({0, 0, 0}, {0.2, 0.2, 0.2}, tinyScreen, mpp).size() == 1);
    const ScreenOf topScreen = [](const V3& p) -> std::optional<ScreenPoint> { return ScreenPoint{160 * p[0], 160 * p[2]}; };
    const auto top = boxSizeLabels({0, 0, 0}, {2, 1, 3}, topScreen, mpp, 1);
    REQUIRE(top.size() == 2);
    CHECK(top[0].text == "2 m");
    CHECK(top[1].text == "3 m");
    const core::Brush b = core::Brush::makeBox("b", {0, 0, 0}, {2, 1, 3}, "dev/grey");
    const core::BrushFace* f = nullptr;
    for (const core::BrushFace& x : b.faces) {
        bool all = true;
        for (uint32_t i : x.loop) all = all && b.vertices[i].y == 1;
        if (all) f = &x;
    }
    REQUIRE(f != nullptr);
    std::vector<std::string> texts;
    for (const SizeLabel& l : faceSizeLabels(b, *f, mpp)) texts.push_back(l.text);
    std::sort(texts.begin(), texts.end());
    CHECK(texts == std::vector<std::string>{"2 m", "3 m"});
    CHECK(fmtSize(0.25) == "0.25 m");
    CHECK(fmtSize(1.23456) == "1.235 m");
    CHECK(fmtSize(-0.0001) == "0 m");
}

TEST_CASE("glyph atlas: text is laid out as quads centred on the anchor") {
    native::render::GlyphAtlas atlas;
    atlas.width = atlas.height = 4;
    atlas.rgba.assign(4 * 4 * 4, 255);
    atlas.lineHeight = 10;
    atlas.margin = 0;
    for (native::render::Glyph& g : atlas.glyphs) { g.w = 6; g.h = 10; g.advance = 6; g.u1 = g.v1 = 1; }
    REQUIRE(atlas.ok());
    CHECK(atlas.textWidth("ab c") == doctest::Approx(24));
    std::vector<native::render::LabelVertex> v;
    const double at[3] = {1, 2, 3};
    native::render::layoutText(atlas, "ab c", at, 0xFFFFFFFFu, v);
    CHECK(v.size() == 18);   // three visible glyphs, two triangles each; the space only moves the pen
    float minX = 1e9f, maxX = -1e9f;
    for (const auto& q : v) {
        CHECK(q.anchor[0] == doctest::Approx(1));
        CHECK(q.anchor[2] == doctest::Approx(3));
        minX = std::min(minX, q.offset[0]);
        maxX = std::max(maxX, q.offset[0]);
    }
    CHECK(minX == doctest::Approx(-12));   // half of the width to the left
    CHECK(maxX == doctest::Approx(12));    // 'c' ends at the right edge
    std::vector<native::render::LabelVertex> none;
    native::render::layoutText(atlas, "", at, 0xFFFFFFFFu, none);   // outside the atlas: nothing
    CHECK(none.empty());
}

// T14.13: the entity name labels: a selected entity's name is always drawn, every other name is marked "near only" (the label shader fades it out between 6 and 8 m).
#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "render/entity_overlay.h"

TEST_CASE("entity name labels: selected names always, the others near the camera only") {
    using namespace native;
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    for (const char* id : {"a", "b"}) {
        core::Entity e = ent(id, "info_player_start");
        REQUIRE(ed.addEntity(e));
    }
    scene::SceneSnapshot snap;
    snap.doc = ed.finish();
    REQUIRE(snap.doc);

    const render::EntityOverlay none = render::buildEntityOverlay(snap, {}, nullptr);
    REQUIRE(none.labels.size() == 2);
    for (const render::TextItem& t : none.labels) CHECK(t.rgba == render::kLabelNearRgba);

    const render::EntityOverlay picked = render::buildEntityOverlay(snap, {doc::SelRef::entity("a")}, nullptr);
    REQUIRE(picked.labels.size() == 2);
    int always = 0, near = 0;
    for (const render::TextItem& t : picked.labels) {
        if (t.rgba == render::kLabelAlwaysRgba) ++always;
        if (t.rgba == render::kLabelNearRgba) ++near;
    }
    CHECK(always == 1);
    CHECK(near == 1);
    CHECK((render::kLabelNearRgba >> 24) < 255);   // the shader tells the two apart by the alpha
}
