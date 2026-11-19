// T4.1: grab / rotate / scale of brushes, entities and prefab instances, converted from shell/tests/transform.test.ts (the shape cases belong to T4.2).
// Headless.
#include <doctest.h>

#include <algorithm>
#include <cmath>

#include "doc/edit.h"
#include "doc/io.h"
#include "tools/transform.h"

using namespace native;
using namespace native::tools;

namespace {

constexpr double kPi = 3.14159265358979323846;

void near(const V3& a, const V3& b, double eps = 1e-9) {
    for (int i = 0; i < 3; ++i) CHECK(std::fabs(a[static_cast<size_t>(i)] - b[static_cast<size_t>(i)]) < eps);
}
V3 v3(const core::Vec3& v) { return {v.x, v.y, v.z}; }

core::Entity point(const std::string& id, const std::string& classname, core::Vec3 origin, core::Vec3 angles = {0, 0, 0},
                   std::unordered_map<std::string, std::string> kv = {}) {
    core::Entity e;
    e.id = id;
    e.classname = classname;
    e.origin = origin;
    e.angles = angles;
    e.keyvalues = std::move(kv);
    return e;
}

std::shared_ptr<const doc::DocumentVersion> docOf(std::vector<core::Entity> entities) {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    for (auto& e : entities) REQUIRE(ed.addEntity(e));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

std::shared_ptr<const doc::DocumentVersion> sample() {
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("a", {0, 0, 0}, {2, 2, 2}));
    w.brushes.push_back(core::Brush::makeBox("c", {10, 0, 0}, {12, 2, 2}));
    return docOf({w, point("p", "light", {5, 5, 5}, {0, 10, 0})});
}

const core::Brush& brushOf(const doc::DocumentVersion& d, const std::string& entity, const std::string& brush) {
    const int ei = d.findEntity(entity), bi = d.findBrush(entity, brush);
    REQUIRE(ei >= 0);
    REQUIRE(bi >= 0);
    return d.entities[static_cast<size_t>(ei)]->brushes[static_cast<size_t>(bi)]->brush;
}
const core::Entity& entityOf(const doc::DocumentVersion& d, const std::string& id) {
    const int ei = d.findEntity(id);
    REQUIRE(ei >= 0);
    return d.entities[static_cast<size_t>(ei)]->typed;
}

const core::BrushFace& faceWithId(const core::Brush& b, uint32_t id) {
    for (const core::BrushFace& f : b.faces)
        if (f.id == id) return f;
    FAIL("no face with that id");
    return b.faces[0];
}
std::vector<core::Vec2> projected(const core::Brush& b, const core::BrushFace& f) {
    std::vector<core::Vec2> out;
    const core::Vec3 n = b.faceNormal(f);
    for (uint32_t i : f.loop) out.push_back(core::projectAlbedoUv(n, b.vertices[i], f.projection));
    return out;
}
void sameUvs(const std::vector<core::Vec2>& a, const std::vector<core::Vec2>& b) {
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].u == doctest::Approx(b[i].u));
        CHECK(a[i].v == doctest::Approx(b[i].v));
    }
}

using Sel = doc::SelRef;

} // namespace

TEST_CASE("point maps: rotate about a pivot, scale about a pivot") {
    near(rotateAbout({1, 0, 1}, {0, 1, 0}, kPi / 2)({2, 0, 1}), {1, 0, 0});
    near(rotateAbout({0, 0, 0}, {0, 0, 1}, kPi)({1, 2, 3}), {-1, -2, 3});
    near(scaleAbout({1, 1, 1}, {2, 1, 0.5})({2, 3, 3}), {3, 3, 2});
}

TEST_CASE("point maps: a rotation keeps volume; a mirror flips winding unless the brush is turned back") {
    const core::Brush b = core::Brush::makeBox("a", {0, 0, 0}, {1, 2, 3});
    CHECK(mapBrushPoints(b, rotateAbout({0, 0, 0}, {1, 1, 0}, 0.7)).signedVolume() == doctest::Approx(6));
    CHECK(mirrors({-1, 1, 1}));
    CHECK_FALSE(mirrors({-1, -1, 1}));
    const core::Brush m = mapBrushPoints(b, scaleAbout({0, 0, 0}, {-1, 1, 1}));
    CHECK(m.signedVolume() < 0);
    const core::Brush fixed = mapBrushPoints(b, scaleAbout({0, 0, 0}, {-1, 1, 1}), true);
    CHECK(fixed.signedVolume() == doctest::Approx(6));
    CHECK_FALSE(fixed.checkConvexSolid().has_value());
}

TEST_CASE("point maps: scaling one axis keeps faces planar and the brush convex") {
    const core::Brush b = mapBrushPoints(core::Brush::makeBox("a", {0, 0, 0}, {1, 1, 1}), scaleAbout({0.5, 0.5, 0.5}, {3, 0.5, 2}));
    CHECK_FALSE(b.checkConvexSolid().has_value());
}

TEST_CASE("selections: selected brushes only, whole entities entirely, never a brush twice") {
    const auto d = sample();
    const auto moved = transformSelections(d, {Sel::brush("w", "a")}, translateBy({1, 0, 0}));
    REQUIRE(moved);
    CHECK(brushOf(*moved, "w", "a").vertices[0].x == 1);
    // the untouched brush is the very same node
    CHECK(moved->entities[0]->brushes[1] == d->entities[0]->brushes[1]);
    const auto both = transformSelections(d, {Sel::entity("w"), Sel::brush("w", "a")}, translateBy({1, 0, 0}));
    REQUIRE(both);
    CHECK(brushOf(*both, "w", "a").vertices[0].x == 1);    // moved once, not twice
    CHECK(brushOf(*both, "w", "c").vertices[0].x == 11);
    CHECK(transformSelections(d, {}, translateBy({1, 0, 0})) == d);
}

TEST_CASE("selections: rotating a point entity moves its origin and turns its yaw") {
    const auto r = transformSelections(sample(), {Sel::entity("p")}, rotateAbout({0, 0, 0}, {0, 1, 0}, kPi / 2));
    REQUIRE(r);
    near(v3(entityOf(*r, "p").origin), {5, 5, -5}, 1e-9);
    CHECK(entityOf(*r, "p").angles.y == doctest::Approx(100));
}

TEST_CASE("selections: the pivot is the centre of the selection bounds") {
    const auto d = sample();
    near(*selectionPivot(*d, {Sel::brush("w", "a"), Sel::brush("w", "c")}), {6, 1, 1});
    near(*selectionPivot(*d, {Sel::entity("p")}), {5, 5, 5});
    CHECK_FALSE(selectionPivot(*d, {}).has_value());
}

TEST_CASE("selections: moving an entity's origin alone keeps its brushes") {
    core::Entity door = point("door", "func_door", {2, 0, 2});
    door.brushes.push_back(core::Brush::makeBox("d1", {2, 0, 2}, {3, 1, 3}));
    const auto d = docOf({door});
    const auto r = moveEntityOrigin(d, "door", {2, 0, 0});
    REQUIRE(r);
    near(v3(entityOf(*r, "door").origin), {4, 0, 2});
    CHECK(r->entities[0]->brushes[0] == d->entities[0]->brushes[0]);
}

TEST_CASE("UV lock: on, a moved brush keeps the UVs it had (the texture travels with the faces)") {
    const core::Brush b = core::Brush::makeBox("a", {0, 0, 0}, {2, 2, 2}, "wall/a");
    const auto before = projected(b, faceWithId(b, 5));
    const core::Brush moved = mapBrushPoints(b, translateBy({3, 0, 1}), false, true);
    sameUvs(faceWithId(moved, 5).uvs, before);
    near(v3(moved.vertices[0]), {3, 0, 1});
}

TEST_CASE("UV lock: off, the world-aligned projection is applied again (the texture stays put in the world)") {
    const core::Brush plain = core::Brush::makeBox("a", {0, 0, 0}, {2, 2, 2}, "wall/a");
    core::Brush withUv = plain;
    for (core::BrushFace& f : withUv.faces) f.uvs = projected(plain, f);
    const core::Brush moved = mapBrushPoints(withUv, translateBy({3, 0, 1}), false, false);
    const core::BrushFace& face = faceWithId(moved, 5);
    sameUvs(face.uvs, projected(moved, face));
    bool differs = false;
    const auto& old = faceWithId(withUv, 5).uvs;
    for (size_t i = 0; i < old.size(); ++i) differs = differs || std::fabs(old[i].u - face.uvs[i].u) > 1e-9 || std::fabs(old[i].v - face.uvs[i].v) > 1e-9;
    CHECK(differs);
    // a face without stored UVs simply uses the draw-time projection
    for (const core::BrushFace& f : mapBrushPoints(plain, translateBy({3, 0, 1}), false, false).faces) CHECK(f.uvs.empty());
}

TEST_CASE("UV lock: on survives rotation and mirroring, the UVs follow their corners") {
    const core::Brush b = core::Brush::makeBox("a", {0, 0, 0}, {1, 2, 3}, "wall/a");
    const core::Brush rot = mapBrushPoints(b, rotateAbout({0, 0, 0}, {0, 1, 0}, 0.7));
    sameUvs(faceWithId(rot, 5).uvs, projected(b, faceWithId(b, 5)));
    const core::Brush mir = mapBrushPoints(b, scaleAbout({0, 0, 0}, {-1, 1, 1}), true);
    auto orig = projected(b, faceWithId(b, 5));
    std::reverse(orig.begin(), orig.end());
    sameUvs(faceWithId(mir, 5).uvs, orig);
}

namespace {

// One entity moved by a map, through a document.
core::Entity run(const core::Entity& e, const Affine& map) {
    const auto d = docOf({e});
    const auto r = transformSelections(d, {Sel::entity(e.id)}, map);
    REQUIRE(r);
    return entityOf(*r, e.id);
}
core::Entity ent(const std::string& classname, std::unordered_map<std::string, std::string> kv = {}, core::Vec3 angles = {0, 0, 0}) {
    return point("e", classname, {0, 0, 0}, angles, std::move(kv));
}

} // namespace

TEST_CASE("entity properties: a pitch / roll rotation turns the angles of a spot light, not just yaw") {
    const core::Entity r = run(ent("light_spot", {}, {90, 0, 0}), rotateAbout({0, 0, 0}, {0, 1, 0}, kPi / 2));
    // pointing down stays pointing down when turned about the vertical axis; the frame turns with it
    near(scene::apply3(scene::rotationFromAngles(v3(r.angles)), {0, 0, 1}), {0, -1, 0}, 1e-6);
    const core::Entity x = run(ent("light_spot", {}, {0, 0, 0}), rotateAbout({0, 0, 0}, {1, 0, 0}, kPi / 2));
    near(scene::apply3(scene::rotationFromAngles(v3(x.angles)), {0, 0, 1}), {0, -1, 0}, 1e-6);   // +Z turned about X by 90 degrees: (0,0,1) -> (0,-1,0)
}

TEST_CASE("entity properties: scaling a point light scales its attenuation radius and source size") {
    const core::Entity r = run(ent("light_point", {{"radius", "10"}, {"source_size", "0.5"}}), scaleAbout({0, 0, 0}, {2, 2, 2}));
    CHECK(r.keyvalues.at("radius") == "20");
    CHECK(r.keyvalues.at("source_size") == "1");
    CHECK(run(ent("light_point"), scaleAbout({0, 0, 0}, {0.5, 0.5, 0.5})).keyvalues.at("radius") == "5");   // the .ent default of 10
    CHECK(run(ent("light_point", {{"radius", "10"}}), scaleAbout({0, 0, 0}, {1, 2, 1})).keyvalues.at("radius") == "12.59921");   // cbrt of the volume change
}

TEST_CASE("entity properties: a rect light scales width / height along its own axes, a decal along width / height / depth") {
    const core::Entity r = run(ent("light_rect", {{"width", "2"}, {"height", "1"}}), scaleAbout({0, 0, 0}, {3, 1, 1}));   // right = +X
    CHECK(r.keyvalues.at("width") == "6");
    CHECK(r.keyvalues.at("height") == "1");
    const core::Entity dc = run(ent("decal", {{"width", "1"}, {"height", "1"}, {"depth", "0.5"}}), scaleAbout({0, 0, 0}, {2, 4, 1}));   // right +X, up -Z, dir -Y
    CHECK(dc.keyvalues.at("width") == "2");
    CHECK(dc.keyvalues.at("height") == "1");
    CHECK(dc.keyvalues.at("depth") == "2");
}

TEST_CASE("entity properties: models get modelscale_vec, back to nothing at 1") {
    const core::Entity r = run(ent("misc_model"), scaleAbout({0, 0, 0}, {2, 1, 3}));
    CHECK(r.keyvalues.at("modelscale_vec") == "2 1 3");
    const core::Entity back = run(ent("misc_model", {{"modelscale_vec", "2 1 3"}}), scaleAbout({0, 0, 0}, {0.5, 1, 1.0 / 3}));
    CHECK(back.keyvalues.count("modelscale_vec") == 0);
}

TEST_CASE("entity properties: a plain move changes nothing but the origin") {
    const core::Entity e = ent("light_point", {{"radius", "3"}});
    const core::Entity r = run(e, translateBy({1, 2, 3}));
    CHECK(r.keyvalues == e.keyvalues);
    near(v3(r.origin), {1, 2, 3});
}

TEST_CASE("prefab instances: a move changes the origin, a rotation composes into the angles, a scale into the scale") {
    core::PrefabInstance p;
    p.id = "i";
    p.file = "prefabs/a.map.json";
    p.origin = {1, 0, 0};
    const core::PrefabInstance moved = mapInstancePoints(p, translateBy({2, 0, 0}));
    near(v3(moved.origin), {3, 0, 0});
    const core::PrefabInstance turned = mapInstancePoints(p, rotateAbout({0, 0, 0}, {0, 1, 0}, kPi / 2));
    CHECK(turned.angles.y == doctest::Approx(90));
    const core::PrefabInstance scaled = mapInstancePoints(p, scaleAbout({0, 0, 0}, {2, 2, 2}));
    REQUIRE(scaled.scale.has_value());
    CHECK(scaled.scale->x == doctest::Approx(2));
}
