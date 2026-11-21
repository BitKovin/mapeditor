// T11.6a: parametric shapes (tools/parametric), converted from shell/tests/parametric.test.ts: the record on every part, rebuilding a shape from its parameters, and what keeps a shape
// and what turns it into ordinary brushes.
#include <doctest.h>

#include <cmath>
#include <set>

#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "tools/box_tool.h"
#include "tools/mesh/ops.h"
#include "tools/parametric.h"
#include "tools/shapes.h"
#include "tools/transform.h"

using namespace native;
namespace pm = native::tools::parametric;
using tools::Frame;
using tools::ShapeId;
using tools::ShapeParams;
using tools::Turn;
using tools::Vec3;
using Version = std::shared_ptr<const doc::DocumentVersion>;

namespace {

const tools::DrawPlane plane = tools::planeThrough({0, 0, 0}, {0, 1, 0});   // the floor: u = z, v = x, height = +y
const tools::Rect rect{{0, 0, 0}, 4, 2};
int counter = 0;
std::string nextId() { return "b" + std::to_string(counter++); }

Version emptyDoc() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    return r.version;
}

// A drawn shape in worldspawn "w", as the Box tool makes it (optionally after an ordinary brush `lead`).
Version drawn(ShapeId kind, ShapeParams p = {}, Turn turn = 0, bool lead = false) {
    p.turn = turn;
    const tools::ShapeOutcome r = tools::buildShape(kind, plane, rect, 3, "dev/wall", p, nextId, turn);
    REQUIRE(r.ok());
    doc::VersionEditor ed(emptyDoc());
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    REQUIRE(ed.addEntity(w));
    if (lead) REQUIRE(ed.addBrush("w", core::Brush::makeBox("x", {10, 0, 10}, {11, 1, 11})));
    REQUIRE(pm::addShape(ed, "w", r.brushes, kind, tools::frameOf(plane, rect, 3), p, turn, "g1"));
    const Version v = ed.finish();
    REQUIRE(v);
    return v;
}

const std::vector<std::shared_ptr<const doc::BrushNode>>& brushes(const Version& v, const std::string& entity = "w") {
    const int i = v->findEntity(entity);
    REQUIRE(i >= 0);
    return v->entities[static_cast<size_t>(i)]->brushes;
}
Version keep(const Version& prev, const Version& next) { return pm::reconcileShapes(prev, next, [] { return std::string("fresh"); }); }
Version ok(const pm::Regenerated& r) {
    INFO(r.error);
    REQUIRE(r.ok());
    return r.version;
}
bool hasShape(const doc::BrushNode& b) { return b.raw.contains("shape"); }
double maxY(const core::Brush& b) {
    double y = -1e9;
    for (const core::Vec3& p : b.vertices) y = std::max(y, p.y);
    return y;
}

std::vector<doc::SelRef> allParts(const Version& v) {
    std::vector<doc::SelRef> s;
    for (const auto& b : brushes(v)) s.push_back(doc::SelRef::brush("w", b->brush.id));
    return s;
}

} // namespace

TEST_CASE("parametric: every part of drawn stairs has the record; reconcile leaves a fresh shape alone; members come in part order") {
    ShapeParams p;
    p.steps = 5;
    const Version d = drawn(ShapeId::Stairs, p);
    REQUIRE(brushes(d).size() == 5);
    for (size_t i = 0; i < 5; ++i) {
        const auto s = pm::shapeOf(*brushes(d)[i]);
        REQUIRE(s);
        CHECK(s->group == "g1");
        CHECK(s->kind == ShapeId::Stairs);
        CHECK(s->parts == 5);
        CHECK(s->part == static_cast<int>(i));
    }
    CHECK(pm::shapeOf(*brushes(d)[0])->params == pm::Json{{"steps", 5}});
    CHECK(keep(nullptr, d) == d);
    CHECK(keep(d, d) == d);
    const auto members = pm::shapeMembers(*d, "w", brushes(d)[3]->brush.id);
    REQUIRE(members.size() == 5);
    CHECK(members[0] == brushes(d)[0]->brush.id);
    // an ordinary brush is its own member
    const Version withBox = drawn(ShapeId::Wedge, {}, 0, true);
    CHECK(pm::shapeMembers(*withBox, "w", "x") == std::vector<std::string>{"x"});
    CHECK(pm::shapeMembers(*withBox, "nobody", "x") == std::vector<std::string>{"x"});
}

TEST_CASE("parametric: fewer steps keep the first parts' ids, the rest go, every part is valid; reconcile keeps the result") {
    ShapeParams p;
    p.steps = 6;
    const Version d = drawn(ShapeId::Stairs, p);
    std::vector<std::string> ids;
    for (const auto& b : brushes(d)) ids.push_back(b->brush.id);
    pm::ShapePatch patch;
    patch.params = pm::Json{{"steps", 3}};
    const Version e = ok(pm::regenerateShape(d, "w", "g1", patch, nextId));
    REQUIRE(brushes(e).size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(brushes(e)[i]->brush.id == ids[i]);
        CHECK_FALSE(brushes(e)[i]->brush.checkConvexSolid());
        CHECK(pm::shapeOf(*brushes(e)[i])->parts == 3);
    }
    CHECK(keep(d, e) == e);
}

TEST_CASE("parametric: more steps give new parts new ids and the staircase still fills the same box") {
    ShapeParams p;
    p.steps = 2;
    const Version d = drawn(ShapeId::Stairs, p);
    int n = 0;
    pm::ShapePatch patch;
    patch.params = pm::Json{{"steps", 5}};
    const Version e = ok(pm::regenerateShape(d, "w", "g1", patch, [&n] { return "new" + std::to_string(n++); }));
    REQUIRE(brushes(e).size() == 5);
    int fresh = 0;
    double top = 0;
    for (const auto& b : brushes(e)) {
        if (b->brush.id.rfind("new", 0) == 0) ++fresh;
        top = std::max(top, maxY(b->brush));
    }
    CHECK(fresh == 3);
    CHECK(top == doctest::Approx(3));
    // the new parts take the look of the old ones
    CHECK(brushes(e)[4]->brush.faces[0].texture == "dev/wall");
}

TEST_CASE("parametric: faces keep their texture by face id; other brushes of the entity are untouched and stay in place") {
    const Version d = drawn(ShapeId::Wedge, {}, 0, true);
    const doc::BrushNode& wedge = *brushes(d)[1];
    REQUIRE(wedge.brush.faces.size() > 1);
    core::Brush changed = wedge.brush;
    uint32_t pick = changed.faces[1].id;
    changed.faces[1].texture = "dev/red";
    changed.faces[1].uvs.assign(changed.faces[1].loop.size(), core::Vec2{0, 0});
    doc::VersionEditor ed(d);
    REQUIRE(ed.replaceBrush("w", changed));
    const Version styled = ed.finish();
    REQUIRE(styled);
    pm::ShapePatch patch;
    patch.h = 5;
    const Version e = ok(pm::regenerateShape(styled, "w", "g1", patch, nextId));
    CHECK(brushes(e)[0] == brushes(styled)[0]);   // the same node
    const core::BrushFace* f = nullptr;
    for (const core::BrushFace& x : brushes(e)[1]->brush.faces)
        if (x.id == pick) f = &x;
    REQUIRE(f);
    CHECK(f->texture == "dev/red");
    bool zero = !f->uvs.empty();
    for (const core::Vec2& uv : f->uvs)
        if (uv.u != 0 || uv.v != 0) zero = false;
    CHECK_FALSE(zero);   // stale stored UVs are not kept
    CHECK(maxY(brushes(e)[1]->brush) == doctest::Approx(5));
    // the record follows the new size
    CHECK(pm::shapeOf(*brushes(e)[1])->frame.h == doctest::Approx(5));
}

TEST_CASE("parametric: a wedge turned 90 degrees rises along the other side of the box; every turn covers the same corners") {
    const auto top = [](const Version& v) {
        std::vector<core::Vec3> out;
        for (const core::Vec3& p : brushes(v)[0]->brush.vertices)
            if (p.y > 2.999) out.push_back(p);
        return out;
    };
    const Version d0 = drawn(ShapeId::Wedge);
    pm::ShapePatch patch;
    patch.turn = 1;
    const Version d1 = ok(pm::regenerateShape(d0, "w", "g1", patch, nextId));
    for (const core::Vec3& p : top(d0)) CHECK(std::fabs(p.z - 4) < 1e-9);   // turn 0: the high edge runs along v (x) at u = du (z = 4)
    std::set<long long> xs, zs;
    for (const core::Vec3& p : top(d1)) {
        xs.insert(std::llround(p.x * 1e6));
        zs.insert(std::llround(p.z * 1e6));
    }
    CHECK(xs.size() == 1);   // turn 1: it runs along u (z) at one end of v
    CHECK(zs.size() == 2);
    CHECK_FALSE(brushes(d1)[0]->brush.checkConvexSolid());
    CHECK(pm::shapeOf(*brushes(d1)[0])->turn == 1);
    const Frame f = tools::frameOf(plane, rect, 3);
    const auto corners = [&](Turn t) {
        const Frame g = tools::turned(f, t);
        std::set<std::string> out;
        for (const auto& ab : {std::pair<int, int>{0, 0}, {1, 0}, {0, 1}, {1, 1}}) {
            const Vec3 q = g.o + g.u * (g.du * ab.first) + g.v * (g.dv * ab.second);
            out.insert(std::to_string(std::llround(q.x * 1e6)) + "," + std::to_string(std::llround(q.y * 1e6)) + "," + std::to_string(std::llround(q.z * 1e6)));
        }
        return out;
    };
    for (Turn t : {1, 2, 3}) CHECK(corners(t) == corners(0));
}

TEST_CASE("parametric: a size of zero is refused and the old shape stays") {
    const Version d = drawn(ShapeId::Cylinder);
    pm::ShapePatch patch;
    patch.du = 0;
    const pm::Regenerated r = pm::regenerateShape(d, "w", "g1", patch, nextId);
    CHECK_FALSE(r.ok());
    CHECK_FALSE(r.error.empty());
    CHECK(pm::regenerateShape(d, "w", "nothing", {}, nextId).error == "That shape no longer exists");
}

TEST_CASE("parametric: rotating the whole shape keeps it parametric; the box turns with it and regenerating gives the rotated geometry back") {
    const Version d = drawn(ShapeId::Arch);
    const Version moved = tools::transformSelections(d, allParts(d), tools::rotateAbout({1, 0, 1}, {0, 1, 0}, 0.7), false, 40, true);
    REQUIRE(moved);
    const Version r = keep(d, moved);
    for (const auto& b : brushes(r)) {
        const auto s = pm::shapeOf(*b);
        REQUIRE(s);
        CHECK(s->group == "g1");
    }
    const Version again = ok(pm::regenerateShape(r, "w", "g1", {}, nextId));
    REQUIRE(brushes(again).size() == brushes(r).size());
    for (size_t i = 0; i < brushes(r).size(); ++i) {
        const core::Brush& a = brushes(again)[i]->brush;
        const core::Brush& b = brushes(r)[i]->brush;
        REQUIRE(a.vertices.size() == b.vertices.size());
        for (size_t k = 0; k < a.vertices.size(); ++k) {
            CHECK(a.vertices[k].x == doctest::Approx(b.vertices[k].x).epsilon(1e-9));
            CHECK(a.vertices[k].y == doctest::Approx(b.vertices[k].y).epsilon(1e-9));
            CHECK(a.vertices[k].z == doctest::Approx(b.vertices[k].z).epsilon(1e-9));
        }
    }
}

TEST_CASE("parametric: mapFrame of an affine map moves every point of the box like the map") {
    const Frame f = tools::frameOf(plane, rect, 3);
    const auto map = [](const Vec3& p) { return Vec3{2 * p.x + 1, p.y - 3, p.z + 0.5 * p.x}; };
    const Frame g = pm::mapFrame(f, map);
    const auto pt = [](const Frame& fr) { return fr.o + fr.u * (fr.du * 0.3) + fr.v * (fr.dv * 0.6) + fr.n * (fr.h * 0.9); };
    const Vec3 want = map(pt(f)), got = pt(g);
    CHECK(got.x == doctest::Approx(want.x).epsilon(1e-9));
    CHECK(got.y == doctest::Approx(want.y).epsilon(1e-9));
    CHECK(got.z == doctest::Approx(want.z).epsilon(1e-9));
    // a record round trip keeps what it says
    const auto spec = pm::shapeOf(*brushes(drawn(ShapeId::Wedge))[0]);
    REQUIRE(spec);
    const pm::Json mapped = pm::mapShapeRecord(pm::shapeRecord(*spec), map);
    const auto back = pm::shapeOf(pm::Json{{"shape", mapped}});
    REQUIRE(back);
    CHECK(back->frame.du == doctest::Approx(g.du));
    CHECK(back->frame.dv == doctest::Approx(g.dv));
    CHECK(back->group == "g1");
    // a damaged record is passed through as it is
    const pm::Json bad = pm::Json{{"group", "g"}};
    CHECK(pm::mapShapeRecord(bad, map) == bad);
}

TEST_CASE("parametric: a mesh edit of one part turns the whole shape into ordinary brushes") {
    ShapeParams p;
    p.steps = 3;
    const Version d = drawn(ShapeId::Stairs, p);
    const core::Brush& b0 = brushes(d)[0]->brush;
    const auto r = tools::mesh::transformVerts(b0, {0}, [](const core::Vec3& q, uint32_t) { return core::Vec3(q.x, q.y + 0.1, q.z); });
    REQUIRE(r.ok());
    doc::VersionEditor ed(d);
    REQUIRE(ed.replaceBrush("w", r.brush));
    const Version edited = ed.finish();
    REQUIRE(edited);
    const Version after = keep(d, edited);
    for (const auto& b : brushes(after)) CHECK_FALSE(hasShape(*b));
}

TEST_CASE("parametric: moving one part on its own turns the shape into brushes; moving all parts keeps it") {
    ShapeParams p;
    p.steps = 3;
    const Version d = drawn(ShapeId::Stairs, p);
    const Version one = tools::transformSelections(d, {doc::SelRef::brush("w", brushes(d)[1]->brush.id)}, tools::translateBy({1, 0, 0}), false, 0, true);
    REQUIRE(one);
    for (const auto& b : brushes(keep(d, one))) CHECK_FALSE(hasShape(*b));
    const Version all = tools::transformSelections(d, allParts(d), tools::translateBy({1, 0, 0}), false, 0, true);
    REQUIRE(all);
    for (const auto& b : brushes(keep(d, all))) {
        const auto s = pm::shapeOf(*b);
        REQUIRE(s);
        CHECK(s->group == "g1");
    }
}

TEST_CASE("parametric: a duplicate becomes its own shape and the original keeps its group; a part in another entity breaks both halves; a damaged record is dropped") {
    {
        const Version d = drawn(ShapeId::Wedge);
        core::Brush copy = brushes(d)[0]->brush;
        copy.id = "copy";
        doc::VersionEditor ed(d);
        REQUIRE(ed.addBrush("w", copy, brushes(d)[0]->raw));
        const Version dup = ed.finish();
        REQUIRE(dup);
        const Version r = keep(d, dup);
        CHECK(pm::shapeOf(*brushes(r)[0])->group == "g1");
        CHECK(pm::shapeOf(*brushes(r)[1])->group == "fresh");
    }
    {
        ShapeParams p;
        p.steps = 3;
        const Version d = drawn(ShapeId::Stairs, p);
        const auto c = brushes(d)[2];
        doc::VersionEditor ed(d);
        REQUIRE(ed.removeBrush("w", c->brush.id));
        core::Entity g;
        g.id = "g";
        g.classname = "func_group";
        g.brushes.push_back(c->brush);
        REQUIRE(ed.addEntity(g, doc::Json(), {c->raw}));
        const Version split = ed.finish();
        REQUIRE(split);
        const Version r = keep(d, split);
        for (const auto& b : brushes(r)) CHECK_FALSE(hasShape(*b));
        for (const auto& b : brushes(r, "g")) CHECK_FALSE(hasShape(*b));
    }
    {
        const Version d = drawn(ShapeId::Wedge);
        doc::VersionEditor ed(d);
        REQUIRE(ed.setBrushKey("w", brushes(d)[0]->brush.id, "shape", pm::Json{{"group", "g1"}, {"kind", "teapot"}}));
        const Version bad = ed.finish();
        REQUIRE(bad);
        CHECK_FALSE(pm::shapeOf(*brushes(bad)[0]));
        CHECK_FALSE(hasShape(*brushes(keep(d, bad))[0]));
    }
}

TEST_CASE("parametric: dissolving a shape drops the records and nothing else; the key is saved with the brush") {
    ShapeParams p;
    p.steps = 3;
    const Version d = drawn(ShapeId::Stairs, p);
    CHECK(pm::dissolveShape(d, "w", "nothing") == d);
    const Version o = pm::dissolveShape(d, "w", "g1");
    REQUIRE(o);
    CHECK(o != d);
    for (const auto& b : brushes(o)) CHECK_FALSE(hasShape(*b));
    CHECK(brushes(o)[0]->brush.vertices.size() == brushes(d)[0]->brush.vertices.size());
    // the record is in the saved JSON
    const doc::Json j = doc::toJson(*d);
    CHECK(j["entities"][0]["brushes"][0]["shape"]["kind"] == "stairs");
    CHECK(j["entities"][0]["brushes"][0]["shape"]["frame"]["du"].is_number());
}
