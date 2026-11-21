// M11 item 1 (mesh mode), part 5: mesh mode on the document (tools/mesh/session): the id conversions, applying an operation as one edit, a concave result moving into
// func_detail, the instant tools and smooth marks. Converted from the store.applyMeshOp / viewport.runTool behaviour of the Shell. Headless, no window.
#include <doctest.h>

#include <set>

#include "doc/edit.h"
#include "doc/io.h"
#include "tools/mesh/session.h"

using namespace native;
using namespace native::tools::mesh;
using core::Brush;
using doc::SelRef;

namespace {

core::Entity worldWith(const std::string& classname = "worldspawn") {
    core::Entity w;
    w.id = "w";
    w.classname = classname;
    w.brushes.push_back(Brush::makeBox("b", {0, 0, 0}, {2, 2, 2}, "dev/grey"));
    return w;
}

std::shared_ptr<const doc::DocumentVersion> docOf(const std::vector<core::Entity>& entities) {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    for (const core::Entity& e : entities) REQUIRE(ed.addEntity(e));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

constexpr uint32_t TOP = 5;   // makeBox: face 5 is +Y

// a pit in the top face: inset it, then push the inner face into the brush (concave)
OpResult pit(const Brush& b, const MeshSel&) {
    OpResult in = insetFaces(b, {TOP}, 0.5);
    if (!in.ok()) return in;
    return extrudeFaces(in.brush, in.sel.faces, -0.5);
}

} // namespace

TEST_CASE("mesh session: selection ids convert both ways") {
    const Brush b = Brush::makeBox("b", {0, 0, 0}, {1, 1, 1}, "t");
    MeshSel s;
    s.verts = {1, 2};
    s.edges = {edgeKey(0, 1), edgeKey(4, 5)};
    s.faces = {3};
    const doc::MeshSel d = toDocSel(b, s);
    CHECK(d.vertices == std::vector<uint32_t>{1, 2});
    CHECK(d.edges.size() == 2);
    CHECK(d.faces == std::vector<uint32_t>{3});
    const MeshSel back = fromDocSel(b, d);
    CHECK(back.verts == s.verts);
    CHECK(back.edges == s.edges);
    CHECK(back.faces == s.faces);
    // an edge that is not in the mesh is dropped, an out of range index too
    MeshSel odd;
    odd.edges = {edgeKey(0, 6)};
    CHECK(toDocSel(b, odd).edges.empty());
    doc::MeshSel far;
    far.edges = {9999};
    CHECK(fromDocSel(b, far).edges.empty());
    CHECK(fromDocElement(toDocElement(SelMode::Edge)) == SelMode::Edge);
}

TEST_CASE("mesh session: the edited brush is the primary selection's brush") {
    const auto d = docOf({worldWith()});
    const auto t = meshTarget(*d, SelRef::brush("w", "b"));
    REQUIRE(t);
    CHECK(t->entityId == "w");
    CHECK(t->classname == "worldspawn");
    CHECK(t->brush().id == "b");
    CHECK_FALSE(meshTarget(*d, SelRef::brush("w", "nope")).has_value());
    CHECK_FALSE(meshTarget(*d, SelRef::entity("w")).has_value());   // worldspawn is never "an entity of one brush"
    core::Entity detail = worldWith("func_detail");
    detail.id = "d";
    const auto d2 = docOf({worldWith(), detail});
    const auto sole = meshTarget(*d2, SelRef::entity("d"));   // an entity of one brush is edited through that brush
    REQUIRE(sole);
    CHECK(sole->entityId == "d");
    CHECK(sole->brushId == "b");
    CHECK_FALSE(meshTarget(*d2, SelRef::instance("i")).has_value());
}

TEST_CASE("mesh session: an operation is one edit that replaces the brush") {
    const auto d = docOf({worldWith()});
    MeshSel sel;
    sel.faces = {TOP};
    const MeshApply r = applyMeshOp(d, SelRef::brush("w", "b"), "Extrude", [](const Brush& b, const MeshSel& s) { return extrudeFaces(b, s.faces, 1.0); }, sel);
    REQUIRE(r.ok());
    CHECK(r.error.empty());
    CHECK_FALSE(r.movedToDetail);
    CHECK(r.select == SelRef::brush("w", "b"));
    CHECK(r.version != d);
    const Brush& nb = r.version->entities[0]->brushes[0]->brush;
    CHECK(nb.signedVolume() == doctest::Approx(8 + 4));
    CHECK(r.sel.faces == std::vector<uint32_t>{TOP});
}

TEST_CASE("mesh session: an operation that changes nothing is not an edit; a refusal carries its reason") {
    const auto d = docOf({worldWith()});
    MeshSel sel;
    sel.faces = {TOP};
    const MeshApply same = applyMeshOp(d, SelRef::brush("w", "b"), "Extrude", [](const Brush& b, const MeshSel& s) { return extrudeFaces(b, s.faces, 0.0); }, sel);
    CHECK_FALSE(same.ok());
    CHECK(same.error.empty());
    // a drag that came back to where it began keeps the base version (and says which brush), so the interaction can end without an edit
    const MeshApply kept = applyMeshOp(d, SelRef::brush("w", "b"), "Extrude", [](const Brush& b, const MeshSel& s) { return extrudeFaces(b, s.faces, 0.0); }, sel, true, {}, true);
    CHECK(kept.ok());
    CHECK(kept.version == d);
    CHECK(kept.select == SelRef::brush("w", "b"));
    const MeshApply bad = applyMeshOp(d, SelRef::brush("w", "b"), "Dissolve", [](const Brush& b, const MeshSel& s) { return runMeshTool("dissolve", b, s, SelMode::Face); }, MeshSel{});
    CHECK_FALSE(bad.ok());
    const MeshApply nothing = applyMeshOp(d, SelRef::brush("w", "missing"), "X", pit, sel);
    CHECK_FALSE(nothing.ok());
    CHECK_FALSE(nothing.error.empty());
}

TEST_CASE("mesh session: a concave result moves into a func_detail entity, or is refused when auto-convert is off") {
    const auto d = docOf({worldWith()});
    MeshSel sel;
    sel.faces = {TOP};
    {
        const MeshApply r = applyMeshOp(d, SelRef::brush("w", "b"), "Pit", pit, sel, false);
        CHECK_FALSE(r.ok());
        CHECK(r.error.find("convex") != std::string::npos);
    }
    const MeshApply r = applyMeshOp(d, SelRef::brush("w", "b"), "Pit", pit, sel, true, "detail1");
    REQUIRE(r.ok());
    CHECK(r.movedToDetail);
    CHECK_FALSE(r.message.empty());
    CHECK(r.select == SelRef::brush("detail1", "b"));
    REQUIRE(r.version->entities.size() == 2);
    CHECK(r.version->entities[0]->brushes.empty());   // gone from worldspawn
    CHECK(r.version->entities[1]->typed.classname == "func_detail");
    REQUIRE(r.version->entities[1]->brushes.size() == 1);
    CHECK(r.version->entities[1]->brushes[0]->brush.checkConvexSolid().has_value());   // concave, and stored anyway
    // the next edit of that brush stays in func_detail
    const MeshApply again = applyMeshOp(r.version, r.select, "Move", [](const Brush& b, const MeshSel& s) {
        return transformVerts(b, s.verts, [](const core::Vec3& p, uint32_t) { return core::Vec3{p.x + 1, p.y, p.z}; });
    }, [&] { MeshSel s; s.verts = {0}; return s; }());
    // it is a valid solid, so it is accepted without another move (a vertex move may or may not keep it valid: both outcomes are fine, but never a second entity)
    if (again.ok()) CHECK(again.version->entities.size() == 2);
}

TEST_CASE("mesh session: the detail entity of a drag is reused") {
    const auto d = docOf({worldWith()});
    MeshSel sel;
    sel.faces = {TOP};
    const MeshApply first = applyMeshOp(d, SelRef::brush("w", "b"), "Pit", pit, sel, true, "dd");
    const MeshApply second = applyMeshOp(d, SelRef::brush("w", "b"), "Pit", pit, sel, true, "dd");   // the same start version again, as a drag does
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    CHECK(second.version->entities.size() == 2);
    CHECK(second.select.entityId == "dd");
}

TEST_CASE("mesh session: func_detail takes a concave brush in place") {
    core::Entity detail = worldWith("func_detail");
    detail.id = "d";
    const auto d = docOf({worldWith(), detail});
    MeshSel sel;
    sel.faces = {TOP};
    const MeshApply r = applyMeshOp(d, SelRef::brush("d", "b"), "Pit", pit, sel);
    REQUIRE(r.ok());
    CHECK_FALSE(r.movedToDetail);
    CHECK(r.version->entities.size() == 2);
    CHECK(r.select == SelRef::brush("d", "b"));
}

TEST_CASE("mesh session: the instant tools") {
    const Brush box = Brush::makeBox("b", {0, 0, 0}, {1, 1, 1}, "t");
    SUBCASE("delete removes the faces touched by the selection") {
        MeshSel s;
        s.faces = {TOP};
        const OpResult r = runMeshTool("delete", box, s, SelMode::Face);
        REQUIRE(r.ok());
        CHECK(r.brush.faces.size() == 5);
        CHECK_FALSE(isClosedManifold(r.brush));
        MeshSel v;
        v.verts = {0};
        const OpResult rv = runMeshTool("delete", box, v, SelMode::Vertex);
        REQUIRE(rv.ok());
        CHECK(rv.brush.faces.size() == 3);   // the three faces around a corner
    }
    SUBCASE("recalc and flip") {
        const OpResult r = runMeshTool("recalc", box, MeshSel{}, SelMode::Face);
        REQUIRE(r.ok());
        CHECK(r.brush.signedVolume() > 0);
        MeshSel s;
        s.faces = {TOP};
        const OpResult f = runMeshTool("flip", box, s, SelMode::Face);
        REQUIRE(f.ok());
    }
    SUBCASE("subdivide needs edge mode") {
        MeshSel s;
        s.edges = {edgeKey(0, 1)};
        CHECK_FALSE(runMeshTool("subdivide", box, s, SelMode::Vertex).ok());
        const OpResult r = runMeshTool("subdivide", box, s, SelMode::Edge, 2);
        REQUIRE(r.ok());
        CHECK(r.brush.vertices.size() == 10);
    }
    SUBCASE("unknown tool") { CHECK_FALSE(runMeshTool("nope", box, MeshSel{}, SelMode::Face).ok()); }
}

TEST_CASE("mesh session: smooth marks") {
    const Brush box = Brush::makeBox("b", {0, 0, 0}, {1, 1, 1}, "t");
    MeshSel s;
    s.edges = {edgeKey(0, 1)};
    const OpResult on = runMeshTool("smooth", box, s, SelMode::Edge);
    REQUIRE(on.ok());
    REQUIRE(on.brush.smoothEdges.size() == 1);
    CHECK(on.brush.smoothEdges[0].first < on.brush.smoothEdges[0].second);
    const OpResult again = runMeshTool("smooth", on.brush, s, SelMode::Edge);
    CHECK_FALSE(again.ok());
    CHECK(again.error.find("already") != std::string::npos);
    const OpResult off = runMeshTool("sharp", on.brush, s, SelMode::Edge);
    REQUIRE(off.ok());
    CHECK(off.brush.smoothEdges.empty());
    CHECK_FALSE(runMeshTool("sharp", box, s, SelMode::Edge).ok());
    // face mode: the edges between two selected faces; a lone face smooths all of its edges
    MeshSel two;
    two.faces = {2, 3};   // -X and +X do not touch: nothing in common, so every edge of both is marked
    const OpResult lone = runMeshTool("smooth", box, two, SelMode::Face);
    REQUIRE(lone.ok());
    CHECK(lone.brush.smoothEdges.size() >= 4);
    CHECK_FALSE(runMeshTool("smooth", box, MeshSel{}, SelMode::Edge).ok());
}

TEST_CASE("mesh session: the selection tools") {
    const Brush box = Brush::makeBox("b", {0, 0, 0}, {1, 1, 1}, "t");
    MeshSel s;
    s.verts = {0};
    CHECK(runSelectTool("selectall", box, s, SelMode::Vertex)->verts.size() == 8);
    CHECK(runSelectTool("selectnone", box, s, SelMode::Vertex)->empty());
    CHECK(runSelectTool("invert", box, s, SelMode::Vertex)->verts.size() == 7);
    CHECK(runSelectTool("grow", box, s, SelMode::Vertex)->verts.size() == 4);
    CHECK(runSelectTool("shrink", box, *runSelectTool("grow", box, s, SelMode::Vertex), SelMode::Vertex)->verts.size() == 1);
    CHECK_FALSE(runSelectTool("nope", box, s, SelMode::Vertex).has_value());
}
