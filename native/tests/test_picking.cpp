// T3.5: picking on the CPU: rays against generated boxes, the carved surface (a hole is clickable, a cut wall names the subtractor), subtractive
// brushes, point entities, box select, and the time a click takes on lvl2. Headless (scene snapshots only).
#include <doctest.h>

#include <cmath>
#include <filesystem>
#include <set>

#include "base/jobs.h"
#include "base/log.h"
#include "base/timing.h"
#include "doc/changeset.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "render/picking.h"
#include "scene/builder.h"

using namespace native;

namespace {

const std::filesystem::path kLvl2 = "F:/Git/SdlWasmEngine/source/Games/ShooterGame/GameData/maps/lvl2.map.json";

core::Brush box(const std::string& id, core::Vec3 mn, core::Vec3 mx, core::BrushOp op = core::BrushOp::Add) {
    core::Brush b = core::Brush::makeBox(id, mn, mx, "dev/grey");
    b.op = op;
    return b;
}

std::shared_ptr<const doc::DocumentVersion> versionOf(std::vector<core::Brush> brushes, std::vector<core::Entity> extra = {}) {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity e;
    e.id = "w";
    e.classname = "worldspawn";
    e.brushes = std::move(brushes);
    REQUIRE(ed.addEntity(e));
    for (auto& x : extra) REQUIRE(ed.addEntity(x));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

struct Built {
    base::JobPool pool{4};
    scene::SceneBuilder builder{pool};
    render::Picker picker;
    explicit Built(const std::shared_ptr<const doc::DocumentVersion>& v) {
        builder.submit(v, doc::allChanged(0, v->version));
        REQUIRE(builder.waitIdle(60000));
        picker.setSnapshot(builder.snapshot());
    }
};

render::Ray ray(double ox, double oy, double oz, double dx, double dy, double dz) { return {{ox, oy, oz}, render::normalize({dx, dy, dz})}; }

} // namespace

TEST_CASE("picking: a ray hits the nearest of generated boxes and names brush and face") {
    std::vector<core::Brush> brushes;
    for (int i = 0; i < 40; ++i) brushes.push_back(box("b" + std::to_string(i), {i * 3.0, 0, 0}, {i * 3.0 + 2, 2, 2}));
    Built b(versionOf(brushes));
    CHECK(b.picker.objectCount() == 40);
    for (int i = 0; i < 40; ++i) {
        // from the -x side, through the middle of the brush: the first face is its -x face
        const auto hit = b.picker.pick(ray(-10, 1, 1, 1, 0, 0));
        REQUIRE(hit);
        CHECK(hit->brushId == "b0");
        CHECK(hit->entityId == "w");
        REQUIRE(hit->faceId.has_value());
        CHECK(hit->t == doctest::Approx(10).epsilon(1e-6));
        // straight down onto brush i
        const auto down = b.picker.pick(ray(i * 3.0 + 1, 10, 1, 0, -1, 0));
        REQUIRE(down);
        CHECK(down->brushId == "b" + std::to_string(i));
        CHECK(down->point.y == doctest::Approx(2));
        CHECK(down->normal.y > 0.99);   // faces the viewer
    }
    CHECK_FALSE(b.picker.pick(ray(-10, 5, 1, 1, 0, 0)));   // above everything
    CHECK_FALSE(b.picker.pick(ray(1, 10, 1, 0, 1, 0)));    // looking away
    // the six faces of a box have six different ids (every makeBox numbers them the same; the +x one is seen on the last box of the row)
    std::set<uint32_t> faces;
    for (auto d : {ray(-5, 1, 1, 1, 0, 0), ray(200, 1, 1, -1, 0, 0), ray(1, -5, 1, 0, 1, 0), ray(1, 5, 1, 0, -1, 0), ray(1, 1, -5, 0, 0, 1), ray(1, 1, 5, 0, 0, -1)}) {
        const auto h = b.picker.pick(d);
        REQUIRE(h);
        REQUIRE(h->faceId.has_value());
        faces.insert(*h->faceId);
    }
    CHECK(faces.size() == 6);
}

TEST_CASE("picking: the hole a subtractor cuts is clickable and a cut wall names the subtractor") {
    // a 4 m thick wall with a 2 x 2 hole through it along z
    Built b(versionOf({box("wall", {0, 0, 0}, {10, 10, 4}), box("hole", {4, 4, -1}, {6, 6, 5}, core::BrushOp::Subtract)}));
    // through the hole: nothing of the wall, but the hole's own (subtractive) shape is hit
    const auto through = b.picker.pick(ray(5, 5, -10, 0, 0, 1), {.includeSubtractive = false});
    CHECK_FALSE(through);
    // beside the hole: the wall's front face
    const auto front = b.picker.pick(ray(2, 5, -10, 0, 0, 1));
    REQUIRE(front);
    CHECK(front->brushId == "wall");
    CHECK(front->t == doctest::Approx(10));
    // inside the hole, looking sideways at the cut wall: the subtractor's face
    const auto cut = b.picker.pick(ray(5, 5, 2, 1, 0, 0), {.includeSubtractive = false});
    REQUIRE(cut);
    CHECK(cut->brushId == "hole");
    CHECK(cut->t == doctest::Approx(1));
    // with the subtractive shape pickable the ray into the hole from outside hits it
    const auto shape = b.picker.pick(ray(5, 5, -10, 0, 0, 1));
    REQUIRE(shape);
    CHECK(shape->brushId == "hole");
    CHECK(shape->subtractive);
}

TEST_CASE("picking: point entities are hit by their box, selection refs follow the Radiant rules") {
    core::Entity lamp;
    lamp.id = "lamp";
    lamp.classname = "light";
    lamp.origin = {20, 3, 3};
    Built b(versionOf({box("floor", {0, 0, 0}, {10, 1, 10})}, {lamp}));
    const auto hit = b.picker.pick(ray(20, 3, -10, 0, 0, 1));
    REQUIRE(hit);
    CHECK(hit->pointEntity);
    CHECK(hit->entityId == "lamp");
    CHECK(hit->toSelRef(false) == doc::SelRef::entity("lamp"));
    const auto floor = b.picker.pick(ray(5, 10, 5, 0, -1, 0));
    REQUIRE(floor);
    CHECK(floor->toSelRef(false) == doc::SelRef::brush("w", "floor"));
    CHECK(floor->toSelRef(true).isFace());
}

TEST_CASE("picking: box select takes what lies completely inside the rectangle") {
    std::vector<core::Brush> brushes;
    for (int i = 0; i < 5; ++i) brushes.push_back(box("b" + std::to_string(i), {i * 4.0, 0, 0}, {i * 4.0 + 2, 2, 2}));
    Built b(versionOf(brushes));
    // an orthographic view along -z (x right, y up on screen), 20 m wide: the matrix maps x in [0, 20] to [-1, 1]
    render::Mat4 m = render::mat4Ortho(10, 10, -50, 50);   // looks along -z; x in [-10, 10]
    m = render::mat4Multiply(m, render::mat4LookAt({10, 5, 20}, {10, 5, 0}, {0, 1, 0}));
    // a rectangle around brushes 0 and 1 (x 0..6), above and below them
    const auto sel = b.picker.boxSelect(m, -1.0, -1.0, (6.5 - 10) / 10, 1.0);
    REQUIRE(sel.size() == 2);
    CHECK(sel[0] == doc::SelRef::brush("w", "b0"));
    CHECK(sel[1] == doc::SelRef::brush("w", "b1"));
    // a rectangle that cuts brush 2 in half takes neither half
    const auto cut = b.picker.boxSelect(m, -1.0, -1.0, (9.0 - 10) / 10, 1.0);
    CHECK(cut.size() == 2);
    // the whole view takes all of them
    CHECK(b.picker.boxSelect(m, -1, -1, 1, 1).size() == 5);
    // the ray the same matrix gives goes through the middle of the view
    const render::Ray r = render::rayThrough(m, 0.0, 0.0);
    CHECK(r.origin.x == doctest::Approx(10));
    CHECK(r.dir.z < -0.99);
}

TEST_CASE("picking: lvl2 answers a click in under 2 ms") {
    if (!std::filesystem::exists(kLvl2)) {
        MESSAGE("lvl2 not found at ", kLvl2.string(), ": skipped");
        return;
    }
    const doc::LoadResult r = doc::loadDocumentFile(kLvl2, "maps/lvl2.map.json");
    REQUIRE(r.version);
    Built b(r.version);
    base::Stopwatch swBuild;
    b.picker.setSnapshot(b.builder.snapshot());
    MESSAGE("lvl2 pick tree: ", swBuild.elapsedMs(), " ms for ", b.picker.objectCount(), " items");
    // rays from the middle of the level in many directions
    doc::Box all;
    for (const auto& [k, p] : b.builder.snapshot()->objects)
        if (p.additive) all.add(p.bounds);
    const render::Vec3 c{(all.mn.x + all.mx.x) / 2, (all.mn.y + all.mx.y) / 2, (all.mn.z + all.mx.z) / 2};
    int hits = 0;
    double worst = 0, total = 0;
    const int n = 400;
    for (int i = 0; i < n; ++i) {
        const double a = i * 0.7, e = std::sin(i * 0.37) * 1.2;
        const render::Ray ray{c, render::normalize({std::cos(a) * std::cos(e), std::sin(e), std::sin(a) * std::cos(e)})};
        base::Stopwatch sw;
        const auto hit = b.picker.pick(ray);
        const double ms = sw.elapsedMs();
        worst = std::max(worst, ms);
        total += ms;
        if (hit) ++hits;
    }
    MESSAGE("lvl2 pick: ", total / n, " ms average, ", worst, " ms worst, ", hits, " of ", n, " rays hit");
    CHECK(hits > n / 2);
    CHECK(total / n < 2.0);
}

TEST_CASE("picking: a turned point entity is picked where its box really is, a decal by its projector bounds") {
    const auto longBox = [](const std::string&) { return scene::Box{{-1, -0.1, -0.1}, {1, 0.1, 0.1}}; };   // 2 m along x, thin
    core::Entity turned;
    turned.id = "e1";
    turned.classname = "info_x";
    turned.angles = {0, 90, 0};
    {
        Built b(versionOf({}, {turned}));
        b.picker.setBoxOf(longBox);
        const auto side = b.picker.pick(ray(5, 0, 0.8, -1, 0, 0));   // z = 0.8 is inside the turned box (+-1 in z)
        REQUIRE(side);
        CHECK(side->t == doctest::Approx(4.9));
        CHECK(side->pointEntity);
        const auto along = b.picker.pick(ray(0, 0, 5, 0, 0, -1));
        REQUIRE(along);
        CHECK(along->t == doctest::Approx(4));
        CHECK(along->normal.z > 0.99);   // faces the viewer, in world space
        CHECK_FALSE(b.picker.pick(ray(5, 0, 1.5, -1, 0, 0)));
    }
    {
        core::Entity plain = turned;
        plain.angles = {0, 0, 0};
        Built b(versionOf({}, {plain}));
        b.picker.setBoxOf(longBox);
        CHECK_FALSE(b.picker.pick(ray(5, 0, 0.8, -1, 0, 0)));   // unturned: z = 0.8 is outside +-0.1
        const auto hit = b.picker.pick(ray(5, 0, 0, -1, 0, 0));
        REQUIRE(hit);
        CHECK(hit->t == doctest::Approx(4));
    }
    {
        core::Entity decal;
        decal.id = "d";
        decal.classname = "decal";
        decal.keyvalues = {{"width", "2"}, {"height", "1"}, {"depth", "0.5"}};
        Built b(versionOf({}, {decal}));
        const auto hit = b.picker.pick(ray(0, 5, 0.4, 0, -1, 0));
        REQUIRE(hit);
        CHECK(hit->t == doctest::Approx(4.75));
        CHECK_FALSE(b.picker.pick(ray(0, 5, 0.7, 0, -1, 0)));
        CHECK_FALSE(b.picker.pick(ray(1.5, 5, 0, 0, -1, 0)));
    }
}
