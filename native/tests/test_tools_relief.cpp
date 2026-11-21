// T11.5a: the editing state of the relief editor (tools/relief/relief_edit): edit space, checks, presets and the session with its history and modal tools. Pure.
#include <doctest.h>

#include "tools/relief/relief_edit.h"

using namespace native::tools::relief;
namespace mesh = native::tools::mesh;
using core::Vec3;

TEST_CASE("relief: edit space scales the tile to metres and back; the stored form is compact and clamped") {
    const Relief flat = flatRelief();
    const core::Brush b = reliefToBrush(flat, 2, 3);
    REQUIRE(b.vertices.size() == 4);
    CHECK(b.id == "relief");
    CHECK(b.vertices[2].x == doctest::Approx(2));
    CHECK(b.vertices[2].y == doctest::Approx(3));
    CHECK(b.faces.size() == 1);
    CHECK(b.faces[0].id == 0);
    CHECK(reliefEqual(&flat, &flat));
    const Relief back = brushToRelief(b, 2, 3);
    CHECK(reliefEqual(&flat, &back));
    CHECK(reliefEqual(nullptr, nullptr));
    CHECK_FALSE(reliefEqual(&flat, nullptr));
    // outside the tile and too deep: clamped; an unused vertex is dropped and the faces renumbered
    core::Brush w = b;
    w.vertices[0] = Vec3(-1, -1, 99);
    w.vertices.push_back(Vec3(1, 1, 1));   // used by no face
    const Relief r = brushToRelief(w, 2, 3);
    CHECK(r.vertices.size() == 4);
    CHECK(r.vertices[0][0] == doctest::Approx(0));
    CHECK(r.vertices[0][2] == doctest::Approx(kMaxDepth));
    const auto j = reliefToJson(r);
    CHECK(j["faces"].size() == 1);
    CHECK(j["vertices"].size() == 4);
}

TEST_CASE("relief: the confine step keeps the mesh inside its tile; a mesh without faces becomes the flat quad") {
    core::Brush b = reliefToBrush(flatRelief(), 2, 2);
    b.vertices[1] = Vec3(5, -2, 0);
    const core::Brush c = confineBrush(b, 2, 2);
    CHECK(c.vertices[1].x == doctest::Approx(2));
    CHECK(c.vertices[1].y == doctest::Approx(0));
    const core::Brush same = confineBrush(reliefToBrush(flatRelief(), 2, 2), 2, 2);
    CHECK(same.vertices.size() == 4);
    core::Brush empty = b;
    empty.faces.clear();
    CHECK(ensureBrush(empty, 2, 2).faces.size() == 1);
    CHECK(ensureBrush(b, 2, 2).faces.size() == 1);
}

TEST_CASE("relief: the report says what is wrong with the mesh and nothing about a clean one") {
    const core::Brush flat = reliefToBrush(flatRelief(), 2, 2);
    const ReliefReport ok = reliefReport(flat, 2, 2);
    CHECK(ok.warnings.empty());
    CHECK(ok.vertices == 4);
    CHECK(ok.faces == 1);
    CHECK(ok.triangles == 2);
    // a vertex raised on the border, and the mesh covers half of the tile
    core::Brush b = flat;
    b.vertices[0].z = 0.5;
    const ReliefReport raised = reliefReport(b, 2, 2);
    REQUIRE_FALSE(raised.warnings.empty());
    CHECK(raised.maxZ == doctest::Approx(0.5));
    b = reliefToBrush(Relief{{{0, 0, 0}, {0.5, 0, 0}, {0.5, 1, 0}, {0, 1, 0}}, {{0, 1, 2, 3}}}, 2, 2);
    const ReliefReport half = reliefReport(b, 2, 2);
    bool covers = false;
    for (const auto& w : half.warnings)
        if (w.find("covers 50%") != std::string::npos) covers = true;
    CHECK(covers);
    // a face that looks into the surface
    core::Brush back = flat;
    std::reverse(back.faces[0].loop.begin(), back.faces[0].loop.end());
    bool looks = false;
    for (const auto& w : reliefReport(back, 2, 2).warnings)
        if (w.find("look into the surface") != std::string::npos) looks = true;
    CHECK(looks);
}

TEST_CASE("relief: the viewer data: triangles carry position, normal and a v-down uv; edges are listed once") {
    const core::Brush flat = reliefToBrush(flatRelief(), 2, 2);
    const std::vector<float> t = reliefTriangles(flat, 2, 2);
    REQUIRE(t.size() == 2 * 3 * 8);
    // normal +z everywhere, the uv of a corner at the bottom (y = 0) has v = 1
    for (size_t i = 0; i < 6; ++i) CHECK(t[i * 8 + 5] == doctest::Approx(1));
    bool bottomV = false;
    for (size_t i = 0; i < 6; ++i)
        if (std::abs(t[i * 8 + 1]) < 1e-6 && std::abs(t[i * 8 + 7] - 1) < 1e-6) bottomV = true;
    CHECK(bottomV);
    CHECK(reliefEdgeLines(flat).size() == 4 * 6);
}

TEST_CASE("relief: the presets are clean, closed against the border and not flat") {
    REQUIRE(presets().size() == 4);
    for (const PresetInfo& p : presets()) {
        INFO(p.key);
        const Relief r = buildPreset(p.id, 2, 3);
        const core::Brush b = reliefToBrush(r, 2, 3);
        const ReliefReport rep = reliefReport(b, 2, 3);
        CHECK(rep.warnings.empty());
        if (p.id == Preset::Flat) {
            CHECK(core::texplan::isFlatRelief(r));
        } else {
            CHECK_FALSE(core::texplan::isFlatRelief(r));
            CHECK(r.faces.size() > 1);
        }
    }
    const Relief recess = buildPreset(Preset::Recess, 2, 3);
    double minZ = 0;
    for (const auto& v : recess.vertices) minZ = std::min(minZ, v[2]);
    CHECK(minZ < 0);
    double maxZ = 0;
    for (const auto& v : buildPreset(Preset::Raise, 2, 3).vertices) maxZ = std::max(maxZ, v[2]);
    CHECK(maxZ > 0);
}

TEST_CASE("session: a tool is one undo step, redo comes back, the selection is not part of the history") {
    ReliefSession s(nullptr, 2, 2);
    CHECK_FALSE(s.canUndo());
    s.selectAll();
    CHECK(s.hasSelection());
    const std::string err = s.apply([](const core::Brush& b, const mesh::MeshSel& sel) { return mesh::insetFaces(b, sel.faces, 0.2); });
    CHECK(err.empty());
    CHECK(s.brush().faces.size() > 1);
    CHECK(s.canUndo());
    const size_t faces = s.brush().faces.size();
    CHECK(s.undo());
    CHECK(s.brush().faces.size() == 1);
    CHECK(s.canRedo());
    CHECK(s.redo());
    CHECK(s.brush().faces.size() == faces);
    CHECK_FALSE(s.redo());
    // an error from a tool is reported, kept, and changes nothing
    const std::string bad = s.apply([](const core::Brush& b, const mesh::MeshSel&) {
        mesh::OpResult r;
        r.brush = b;
        r.error = "boom";
        return r;
    });
    CHECK(bad == "boom");
    CHECK(s.error() == "boom");
    CHECK(s.brush().faces.size() == faces);
}

TEST_CASE("session: a modal tool re-runs from the base; confirming is one undo step, cancelling restores") {
    ReliefSession s(nullptr, 2, 2);
    s.selectAll();
    s.apply([](const core::Brush& b, const mesh::MeshSel& sel) { return mesh::insetFaces(b, sel.faces, 0.3); });
    // the middle face of the inset is the selected one: extrude it in a modal
    const mesh::MeshSel middle = s.sel();
    REQUIRE_FALSE(middle.faces.empty());
    s.beginModal();
    CHECK(s.inModal());
    CHECK(s.apply([](const core::Brush& b, const mesh::MeshSel&) {
              mesh::OpResult r;
              r.brush = b;
              return r;
          }) == "A tool is active");
    const auto extrude = [](double d) { return [d](const core::Brush& b, const mesh::MeshSel& sel) { return mesh::extrudeFaces(b, sel.faces, d); }; };
    CHECK(s.modalUpdate(extrude(0.1)).empty());
    const size_t after = s.brush().faces.size();
    CHECK(s.modalUpdate(extrude(0.4)).empty());
    CHECK(s.brush().faces.size() == after);   // from the base each time, not stacked
    double maxZ = 0;
    for (const Vec3& v : s.brush().vertices) maxZ = std::max(maxZ, v.z);
    CHECK(maxZ == doctest::Approx(0.4));
    s.endModal(true);
    CHECK_FALSE(s.inModal());
    CHECK(s.undo());   // the whole drag
    maxZ = 0;
    for (const Vec3& v : s.brush().vertices) maxZ = std::max(maxZ, v.z);
    CHECK(maxZ == doctest::Approx(0));
    CHECK(s.undo());   // the inset
    CHECK(s.brush().faces.size() == 1);
    CHECK_FALSE(s.undo());
    // cancel
    s.selectAll();
    s.beginModal();
    s.modalUpdate(extrude(0.2));
    s.endModal(false);
    CHECK(s.brush().faces.size() == 1);
    CHECK(s.canRedo());   // the two undone steps are still there: cancelling touches no history
}

TEST_CASE("session: the mesh stays inside the tile and depth limit; deleting every face leaves the flat quad") {
    ReliefSession s(nullptr, 2, 2);
    s.setMode(mesh::SelMode::Vertex);
    s.setSel(mesh::selFromVerts(s.brush(), {0}));
    CHECK(s.apply([](const core::Brush& b, const mesh::MeshSel& sel) { return mesh::transformVerts(b, sel.verts, [](const Vec3& p, uint32_t) { return Vec3(p.x - 5, p.y, p.z + 40); }); }).empty());
    const Relief r = s.toRelief();
    for (const auto& v : r.vertices) {
        CHECK(v[0] >= 0);
        CHECK(v[2] <= kMaxDepth + 1e-9);
    }
    s.setMode(mesh::SelMode::Face);
    s.selectAll();
    CHECK_FALSE(s.deleteSelectedFaces().empty());   // every face: refused, as in mesh mode
    // a tool that leaves no faces gives the flat quad
    CHECK(s.apply([](const core::Brush& b, const mesh::MeshSel&) {
              mesh::OpResult r;
              r.brush = b;
              r.brush.faces.clear();
              return r;
          }).empty());
    CHECK_FALSE(s.brush().faces.empty());
    CHECK(core::texplan::isFlatRelief(s.toRelief()));
}

TEST_CASE("session: depth, reset, load, resize, mode changes and selection helpers") {
    ReliefSession s(nullptr, 2, 2);
    s.setMode(mesh::SelMode::Vertex);
    s.selectAll();
    CHECK(s.setDepth(0.5).empty());
    CHECK(s.toRelief().vertices[0][2] == doctest::Approx(0.5));
    CHECK(s.pivot().z == doctest::Approx(0.5));
    s.reset();
    CHECK(core::texplan::isFlatRelief(s.toRelief()));
    CHECK_FALSE(s.hasSelection());
    s.load(buildPreset(Preset::Raise, 2, 2));
    CHECK(s.brush().faces.size() > 1);
    const Relief before = s.toRelief();
    s.resize(4, 4);   // the same relief in a bigger tile; the history is gone
    CHECK(s.width() == doctest::Approx(4));
    const Relief resized = s.toRelief();
    CHECK(reliefEqual(&before, &resized));
    CHECK_FALSE(s.canUndo());
    CHECK(s.brush().vertices[1].x == doctest::Approx(4 * before.vertices[1][0]));
    // modes: faces to vertices keeps their corners
    s.setMode(mesh::SelMode::Face);
    s.setSel(mesh::selFromFaces(s.brush(), {s.brush().faces[0].id}));
    s.setMode(mesh::SelMode::Vertex);
    CHECK(s.sel().verts.size() == s.brush().faces[0].loop.size());
    s.invert();
    CHECK(s.sel().verts.size() == s.brush().vertices.size() - s.brush().faces[0].loop.size());
    s.selectNone();
    CHECK_FALSE(s.hasSelection());
    s.selectAll();
    s.shrink();
    CHECK(s.sel().verts.size() <= s.brush().vertices.size());
}
