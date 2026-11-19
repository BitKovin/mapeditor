// T3.2: the scene builder. A moved subtractive brush rebuilds exactly the brushes it overlaps (old and new box), undo hits the cache,
// the builder's patched core::Document gives what a from-scratch collectSurfaces gives, rapid submits end on the last version, and lvl2 builds
// from scratch in under 5 s. Headless (job pool only).
#include <doctest.h>

#include <algorithm>
#include <filesystem>

#include "base/jobs.h"
#include "base/log.h"
#include "base/timing.h"
#include "doc/changeset.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "scene/builder.h"

using namespace native;
using scene::ObjectKey;

namespace {

namespace fs = std::filesystem;
const fs::path kLvl2 = "F:/Git/SdlWasmEngine/source/Games/ShooterGame/GameData/maps/lvl2.map.json";

core::Brush box(const std::string& id, core::Vec3 mn, core::Vec3 mx, core::BrushOp op = core::BrushOp::Add) {
    core::Brush b = core::Brush::makeBox(id, mn, mx);
    b.op = op;
    return b;
}

core::Entity entityWith(const std::string& id, const std::string& cls, std::vector<core::Brush> brushes) {
    core::Entity e;
    e.id = id;
    e.classname = cls;
    e.brushes = std::move(brushes);
    return e;
}

// worldspawn: wall (0..4)^3, hole (subtractive, through the wall), far (a floor 100 m away) | func_detail: lamp (also near the wall's far side)
std::shared_ptr<const doc::DocumentVersion> fixture() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    REQUIRE(ed.addEntity(entityWith("e_world", "worldspawn",
                                    {box("wall", {0, 0, 0}, {4, 4, 4}), box("hole", {1, 1, -1}, {2, 2, 5}, core::BrushOp::Subtract),
                                     box("far", {100, 0, 100}, {104, 1, 104})})));
    REQUIRE(ed.addEntity(entityWith("e_detail", "func_detail", {box("lamp", {10, 0, 0}, {11, 1, 1})})));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

std::shared_ptr<const doc::DocumentVersion> moveBrush(const std::shared_ptr<const doc::DocumentVersion>& v, const std::string& entity,
                                                      const std::string& brush, core::Vec3 delta) {
    const int e = v->findEntity(entity);
    REQUIRE(e >= 0);
    const int b = v->findBrush(entity, brush);
    REQUIRE(b >= 0);
    core::Brush moved = v->entities[static_cast<size_t>(e)]->brushes[static_cast<size_t>(b)]->brush;
    moved.translate(delta);
    doc::VersionEditor ed(v);
    REQUIRE(ed.replaceBrush(entity, moved));
    const auto out = ed.finish();
    REQUIRE(out);
    return out;
}

size_t pieceCount(const scene::SceneSnapshot& s) {
    size_t n = 0;
    for (const auto& [k, p] : s.objects) n += p.surfaces->size();
    return n;
}

size_t vertCount(const scene::SceneSnapshot& s) {
    size_t n = 0;
    for (const auto& [k, p] : s.objects)
        for (const auto& piece : *p.surfaces) n += piece.verts.size();
    return n;
}

ObjectKey key(const std::string& entity, const std::string& brush) { return ObjectKey{"", entity, brush}; }

} // namespace

TEST_CASE("scene builder: the first submit builds every brush") {
    base::JobPool pool(4);
    scene::SceneBuilder b(pool);
    const auto v = fixture();
    b.submit(v, doc::allChanged(0, v->version));
    REQUIRE(b.waitIdle());
    const auto snap = b.snapshot();
    REQUIRE(snap);
    CHECK(snap->complete);
    CHECK(snap->version == v->version);
    CHECK(snap->objects.size() == 4);
    CHECK(b.lastAffected().size() == 4);
    CHECK(snap->objects.at(key("e_world", "wall")).additive);
    CHECK_FALSE(snap->objects.at(key("e_world", "hole")).additive);
    CHECK(snap->objects.at(key("e_world", "hole")).surfaces->empty());
    CHECK(snap->objects.at(key("e_world", "wall")).surfaces->size() > 6);   // the hole's walls cut into it
    CHECK(snap->objects.at(key("e_world", "far")).surfaces->size() == 6);
    const doc::Box wallBox = snap->objects.at(key("e_world", "wall")).bounds;
    CHECK(wallBox.mn.x == 0);
    CHECK(wallBox.mx.x == 4);
    // what collectSurfaces makes for the whole document
    const std::vector<core::LightSurface> whole = core::collectSurfaces(doc::toCoreDocument(*v), {});
    CHECK(pieceCount(*snap) == whole.size());
    CHECK(b.stats().computed == 3);   // wall, far, lamp: the hole has no surfaces
}

TEST_CASE("scene builder: moving a subtractive brush rebuilds exactly the brushes it overlaps, old and new place") {
    base::JobPool pool(4);
    scene::SceneBuilder b(pool);
    const auto v0 = fixture();
    b.submit(v0, doc::allChanged(0, v0->version));
    REQUIRE(b.waitIdle());
    const auto first = b.snapshot();
    const uint64_t computed0 = b.stats().computed;

    // within the wall: the hole itself and the wall (old and new box both overlap the wall); the far floor and the lamp are not touched
    const auto v1 = moveBrush(v0, "e_world", "hole", {1, 0, 0});
    b.submit(v1, doc::diff(*v0, *v1));
    REQUIRE(b.waitIdle());
    std::vector<ObjectKey> affected = b.lastAffected();
    std::vector<ObjectKey> expected = {key("e_world", "hole"), key("e_world", "wall")};
    std::sort(expected.begin(), expected.end());
    CHECK(affected == expected);
    const auto second = b.snapshot();
    CHECK(second->version == v1->version);
    CHECK(b.stats().computed == computed0 + 1);   // the wall; the hole has nothing to compute
    // unrebuilt objects share their products with the previous snapshot
    CHECK(second->objects.at(key("e_world", "far")).surfaces == first->objects.at(key("e_world", "far")).surfaces);
    CHECK(second->objects.at(key("e_detail", "lamp")).surfaces == first->objects.at(key("e_detail", "lamp")).surfaces);
    CHECK(second->objects.at(key("e_world", "wall")).surfaces != first->objects.at(key("e_world", "wall")).surfaces);
    CHECK(pieceCount(*second) == core::collectSurfaces(doc::toCoreDocument(*v1), {}).size());

    // out of the wall to where only the lamp is: the old place's brushes (wall) and the new place's (lamp) are rebuilt, the far floor is not
    const auto v2 = moveBrush(v1, "e_world", "hole", {8.5, 0, 0});   // x 9.5 .. 10.5 over the lamp (10..11)
    b.submit(v2, doc::diff(*v1, *v2));
    REQUIRE(b.waitIdle());
    affected = b.lastAffected();
    expected = {key("e_world", "hole"), key("e_world", "wall"), key("e_detail", "lamp")};
    std::sort(expected.begin(), expected.end());
    CHECK(affected == expected);   // the lamp is in func_detail: a structural entity, carved by worldspawn's subtractor
    CHECK(pieceCount(*b.snapshot()) == core::collectSurfaces(doc::toCoreDocument(*v2), {}).size());
}

TEST_CASE("scene builder: undo and redo are cache hits") {
    base::JobPool pool(4);
    scene::SceneBuilder b(pool);
    const auto v0 = fixture();
    b.submit(v0, doc::allChanged(0, v0->version));
    REQUIRE(b.waitIdle());
    const auto original = b.snapshot();
    const auto v1 = moveBrush(v0, "e_world", "hole", {1, 0, 0});
    b.submit(v1, doc::diff(*v0, *v1));
    REQUIRE(b.waitIdle());
    const auto moved = b.snapshot();
    const uint64_t computed = b.stats().computed;
    const uint64_t hits = b.stats().cacheHits;

    b.submit(v0, doc::diff(*v1, *v0));   // undo
    REQUIRE(b.waitIdle());
    CHECK(b.stats().computed == computed);   // nothing recomputed
    CHECK(b.stats().cacheHits == hits + 1);  // the wall came from the cache
    const auto undone = b.snapshot();
    CHECK(undone->objects.at(key("e_world", "wall")).surfaces == original->objects.at(key("e_world", "wall")).surfaces);

    b.submit(v1, doc::diff(*v0, *v1));   // redo
    REQUIRE(b.waitIdle());
    CHECK(b.stats().computed == computed);
    CHECK(b.snapshot()->objects.at(key("e_world", "wall")).surfaces == moved->objects.at(key("e_world", "wall")).surfaces);
}

TEST_CASE("scene builder: adding and removing entities and brushes") {
    base::JobPool pool(4);
    scene::SceneBuilder b(pool);
    const auto v0 = fixture();
    b.submit(v0, doc::allChanged(0, v0->version));
    REQUIRE(b.waitIdle());

    doc::VersionEditor ed(v0);
    REQUIRE(ed.addEntity(entityWith("e_new", "func_detail", {box("n1", {50, 0, 50}, {51, 1, 51}), box("n2", {52, 0, 50}, {53, 1, 51})})));
    REQUIRE(ed.addBrush("e_world", box("extra", {3, 3, 3}, {6, 6, 6})));
    const auto v1 = ed.finish();
    REQUIRE(v1);
    b.submit(v1, doc::diff(*v0, *v1));
    REQUIRE(b.waitIdle());
    CHECK(b.snapshot()->objects.size() == 7);
    CHECK(pieceCount(*b.snapshot()) == core::collectSurfaces(doc::toCoreDocument(*v1), {}).size());

    doc::VersionEditor ed2(v1);
    REQUIRE(ed2.removeEntity("e_new"));
    REQUIRE(ed2.removeBrush("e_world", "wall"));
    const auto v2 = ed2.finish();
    REQUIRE(v2);
    b.submit(v2, doc::diff(*v1, *v2));
    REQUIRE(b.waitIdle());
    const auto snap = b.snapshot();
    CHECK(snap->objects.size() == 4);
    CHECK(snap->objects.count(key("e_new", "n1")) == 0);
    CHECK(snap->objects.count(key("e_world", "wall")) == 0);
    CHECK(pieceCount(*snap) == core::collectSurfaces(doc::toCoreDocument(*v2), {}).size());
    CHECK(vertCount(*snap) == [&] {
        size_t n = 0;
        for (const auto& s : core::collectSurfaces(doc::toCoreDocument(*v2), {})) n += s.verts.size();
        return n;
    }());
}

TEST_CASE("scene builder: changing an entity's class rebuilds its brushes and what they carve") {
    base::JobPool pool(4);
    scene::SceneBuilder b(pool);
    const auto v0 = fixture();
    b.submit(v0, doc::allChanged(0, v0->version));
    REQUIRE(b.waitIdle());

    // the lamp's entity becomes a non-structural one: worldspawn's hole no longer carves it (it does not reach it anyway), but the
    // class is part of the products, so the lamp is rebuilt
    doc::VersionEditor ed(v0);
    core::Entity e = v0->entities[static_cast<size_t>(v0->findEntity("e_detail"))]->typed;
    e.classname = "func_door";
    REQUIRE(ed.replaceEntity(e));
    const auto v1 = ed.finish();
    REQUIRE(v1);
    b.submit(v1, doc::diff(*v0, *v1));
    REQUIRE(b.waitIdle());
    const std::vector<ObjectKey> affected = b.lastAffected();
    CHECK(std::find(affected.begin(), affected.end(), key("e_detail", "lamp")) != affected.end());
    CHECK(std::find(affected.begin(), affected.end(), key("e_world", "far")) == affected.end());
    CHECK(pieceCount(*b.snapshot()) == core::collectSurfaces(doc::toCoreDocument(*v1), {}).size());
}

TEST_CASE("scene builder: rapid submits end on the last version") {
    base::JobPool pool(4);
    scene::SceneBuilder b(pool);
    std::atomic<int> publishes{0};
    std::atomic<bool> lastComplete{false};
    b.setListener([&](const std::shared_ptr<const scene::SceneSnapshot>& s) {
        ++publishes;
        lastComplete = s->complete;
    });
    auto v = fixture();
    b.submit(v, doc::allChanged(0, v->version));
    for (int i = 0; i < 40; ++i) {
        const auto next = moveBrush(v, "e_world", "hole", {(i % 2 ? -0.1 : 0.1), 0, 0});
        b.submit(next, doc::diff(*v, *next));
        v = next;
    }
    REQUIRE(b.waitIdle());
    const auto snap = b.snapshot();
    CHECK(snap->version == v->version);
    CHECK(snap->complete);
    CHECK(lastComplete.load());
    CHECK(publishes.load() >= 1);
    CHECK(pieceCount(*snap) == core::collectSurfaces(doc::toCoreDocument(*v), {}).size());
    MESSAGE("builds " << b.stats().builds << " of " << b.stats().submits << " submits, abandoned jobs " << b.stats().abandoned);
}

TEST_CASE("scene builder: destroying it while jobs run is clean") {
    base::JobPool pool(2);
    {
        scene::SceneBuilder b(pool);
        const auto v = fixture();
        b.submit(v, doc::allChanged(0, v->version));
    }   // the destructor supersedes the jobs and waits for them
    pool.waitIdle();
    CHECK(true);
}

TEST_CASE("scene builder: the derived cache keeps several versions of one brush and drops brushes that left") {
    scene::DerivedCache<int, std::vector<int>> c(1000);
    c.put(1, 10, std::make_shared<std::vector<int>>(3), 100);
    c.put(1, 11, std::make_shared<std::vector<int>>(4), 100);
    CHECK(c.get(1, 10));
    CHECK(c.get(1, 11));
    CHECK_FALSE(c.get(1, 12));
    CHECK(c.hits() == 2);
    CHECK(c.misses() == 1);
    c.put(2, 20, std::make_shared<std::vector<int>>(), 900);   // over budget: the least recently used (1, 10) goes first
    CHECK_FALSE(c.get(1, 10));
    c.retain({2});
    CHECK(c.entries() == 1);
    CHECK(c.bytes() == 900);
}

TEST_CASE("scene builder: lvl2 builds from scratch in under 5 seconds") {
    if (!fs::exists(kLvl2)) {
        MESSAGE("lvl2 not found at ", kLvl2.string(), ": skipped");
        return;
    }
    const doc::LoadResult r = doc::loadDocumentFile(kLvl2, "maps/lvl2.map.json");
    REQUIRE(r.version);
    base::JobPool pool(16);
    scene::SceneBuilder b(pool);
    base::Stopwatch sw;
    b.submit(r.version, doc::allChanged(0, r.version->version));
    REQUIRE(b.waitIdle());
    const double ms = sw.elapsedMs();
    const auto snap = b.snapshot();
    REQUIRE(snap);
    MESSAGE("lvl2 scene build: " << ms << " ms, " << snap->objects.size() << " brushes, " << pieceCount(*snap) << " pieces");
    base::logInfo("scene: lvl2 built from scratch in " + std::to_string(ms) + " ms (" + std::to_string(snap->objects.size()) + " brushes, " +
                  std::to_string(pieceCount(*snap)) + " pieces)");
    CHECK(ms < 5000);
    CHECK(snap->complete);
    CHECK(pieceCount(*snap) == core::collectSurfaces(doc::toCoreDocument(*r.version), {}).size());

    // one moved brush afterwards: only what it touches
    const auto& world = *r.version->entities[0];
    REQUIRE_FALSE(world.brushes.empty());
    const auto moved = moveBrush(r.version, world.typed.id, world.brushes[0]->brush.id, {0.25, 0, 0});
    sw.restart();
    b.submit(moved, doc::diff(*r.version, *moved));
    REQUIRE(b.waitIdle());
    MESSAGE("lvl2 after one moved brush: " << sw.elapsedMs() << " ms, rebuilt " << b.lastAffected().size() << " brushes");
    CHECK(b.lastAffected().size() < 200);
}
