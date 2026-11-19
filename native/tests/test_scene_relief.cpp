// T3.3: relief meshes from the Core's texplan functions, per scene object. Converted from shell/tests/reliefBake.test.ts (the Shell's reliefFaces:
// displaced triangles per face, keyed like the Core's surface pieces; nothing for a texture without relief or a subtractor; the walls a subtractor
// cut are a back side keyed by the subtractor) plus the builder's relief wave (cache, neighbours, environment changes). The Core port of the
// relief functions itself is checked against the Shell's vectors in core_tests (texplan: ...). Headless.
#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>

#include "base/jobs.h"
#include "base/log.h"
#include "base/timing.h"
#include "doc/changeset.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "scene/builder.h"
#include "scene/relief.h"
#include "scene/render_mesh.h"

using namespace native;
using scene::ObjectKey;

namespace {

const char* kTex = "wall/relief";

// A recess in a 1 m repeat: a frame at the surface, sloped walls down to a floor 8 cm deep.
nlohmann::json recessProps() {
    using J = nlohmann::json;
    J vertices = J::array();
    for (auto v : {std::array<double, 3>{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {.12, .12, 0}, {.88, .12, 0}, {.88, .88, 0}, {.12, .88, 0},
                   {.2, .2, -.08}, {.8, .2, -.08}, {.8, .8, -.08}, {.2, .8, -.08}})
        vertices.push_back(v);
    J faces = J::array();
    for (auto f : {std::array<int, 4>{0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}, {4, 5, 9, 8}, {5, 6, 10, 9}, {6, 7, 11, 10}, {7, 4, 8, 11},
                   {8, 9, 10, 11}})
        faces.push_back(f);
    return J{{"relief", J{{"vertices", vertices}, {"faces", faces}}}};
}

struct Env {
    std::shared_ptr<std::map<std::string, core::texplan::TexInfo>> infos = std::make_shared<std::map<std::string, core::texplan::TexInfo>>();
    scene::ReliefEnv env;
    explicit Env(bool withRelief = true, uint64_t revision = 1) {
        core::texplan::TexInfo i;
        i.path = kTex;
        i.width = i.height = 64;
        i.ppm = 64;
        i.props = withRelief ? core::texplan::parseProps(recessProps(), i.path) : core::texplan::defaultProps();
        (*infos)[i.path] = i;
        auto map = infos;
        env.info = [map](const std::string& p) -> const core::texplan::TexInfo* {
            const auto it = map->find(p);
            return it == map->end() ? nullptr : &it->second;
        };
        env.groups = [](const std::string&) -> core::texplan::GroupPtr { return nullptr; };
        env.revision = revision;
    }
};

core::Brush box(const std::string& id, core::Vec3 mn, core::Vec3 mx, core::BrushOp op = core::BrushOp::Add, const std::string& tex = kTex) {
    core::Brush b = core::Brush::makeBox(id, mn, mx, tex);
    b.op = op;
    return b;
}

core::Document docOf(std::vector<core::Brush> brushes) {
    core::Document d;
    core::Entity e;
    e.id = "w";
    e.classname = "worldspawn";
    e.brushes = std::move(brushes);
    d.entities.push_back(std::move(e));
    return d;
}

scene::ParentFaceFn parentsOf(const core::Document& d) {
    return [&d](const std::string& inst, const std::string& brush, uint32_t face) {
        std::pair<const core::Brush*, const core::BrushFace*> r{nullptr, nullptr};
        if (!inst.empty()) return r;
        for (const auto& e : d.entities)
            for (const auto& b : e.brushes)
                if (b.id == brush) {
                    r.first = &b;
                    for (const auto& f : b.faces)
                        if (f.id == face) r.second = &f;
                }
        return r;
    };
}

std::vector<core::LightSurface> surfacesOf(const core::Document& d, const std::string& brushId) {
    return core::collectSurfacesFor(d, {}, [&](const std::string&, const std::string&, const std::string& b) { return b == brushId; });
}

std::shared_ptr<const doc::DocumentVersion> versionOf(std::vector<core::Brush> brushes) {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity e;
    e.id = "w";
    e.classname = "worldspawn";
    e.brushes = std::move(brushes);
    REQUIRE(ed.addEntity(e));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

std::shared_ptr<const doc::DocumentVersion> moveBrush(const std::shared_ptr<const doc::DocumentVersion>& v, const std::string& brush, core::Vec3 delta) {
    const int b = v->findBrush("w", brush);
    REQUIRE(b >= 0);
    core::Brush moved = v->entities[0]->brushes[static_cast<size_t>(b)]->brush;
    moved.translate(delta);
    doc::VersionEditor ed(v);
    REQUIRE(ed.replaceBrush("w", moved));
    const auto out = ed.finish();
    REQUIRE(out);
    return out;
}

ObjectKey key(const std::string& brush) { return ObjectKey{"", "w", brush}; }

const core::ReliefMesh* find(const scene::ReliefList& list, const std::string& brush, core::FaceSide side) {
    for (const auto& m : list)
        if (m->brushId == brush && m->side == side) return m.get();
    return nullptr;
}

size_t reliefMeshes(const scene::SceneSnapshot& s) {
    size_t n = 0;
    for (const auto& [k, p] : s.objects) n += p.relief ? p.relief->size() : 0;
    return n;
}

} // namespace

TEST_CASE("relief: a brush with a relief texture gets displaced triangles per face, keyed like its surface pieces") {
    const Env env;
    const core::Document d = docOf({box("a", {0, 0, 0}, {2, 2, 2})});
    const auto list = scene::buildRelief(surfacesOf(d, "a"), {}, parentsOf(d), env.env);
    REQUIRE(list->size() == 6);
    for (const auto& m : *list) {
        CHECK(m->side == core::FaceSide::Front);
        CHECK(m->entityId == "w");
        CHECK(m->brushId == "a");
        CHECK(m->instance.empty());
        CHECK(m->brushInstance.empty());
        CHECK(m->tris.size() % 9 == 0);
        CHECK(m->tris.size() > 2 * 9);   // more than the flat face's two triangles
        // no vertex above the plane (a recess only sinks), and some well below it
        bool deep = false;
        for (size_t i = 0; i + 2 < m->tris.size(); i += 3) {
            const core::Vec3 p{m->tris[i], m->tris[i + 1], m->tris[i + 2]};
            const double h = m->normal.dot(p) - m->offset;
            CHECK(h <= 1e-6);
            deep = deep || h < -1e-3;
        }
        CHECK(deep);
        CHECK(m->bvh);   // ReliefSet::make: the lighting can cast against it
    }
}

TEST_CASE("relief: nothing for a texture without a relief, nor for a subtractor itself") {
    const Env flat(false);
    const core::Document d = docOf({box("a", {0, 0, 0}, {2, 2, 2})});
    CHECK(scene::buildRelief(surfacesOf(d, "a"), {}, parentsOf(d), flat.env)->empty());

    const Env env;
    const core::Document sub = docOf({box("a", {0, 0, 0}, {2, 2, 2}, core::BrushOp::Subtract)});
    CHECK(surfacesOf(sub, "a").empty());   // a subtractor has no surfaces of its own
    CHECK(scene::buildRelief({}, {}, parentsOf(sub), env.env)->empty());

    CHECK(scene::buildRelief(surfacesOf(d, "a"), {}, parentsOf(d), scene::ReliefEnv{})->empty());   // no environment: nothing
}

TEST_CASE("relief: the walls a subtractor cut are a back side keyed by the subtractor") {
    const Env env;
    const core::Document d = docOf({box("wall", {0, 0, 0}, {4, 4, 1}), box("cut", {1, 1, -1}, {2, 2, 2}, core::BrushOp::Subtract)});
    const auto list = scene::buildRelief(surfacesOf(d, "wall"), {}, parentsOf(d), env.env);
    // the cut wall at x = 2 faces -x, into the opening; the authored face is the cutter's +x face
    const core::ReliefMesh* back = nullptr;
    for (const auto& m : *list)
        if (m->brushId == "cut" && m->side == core::FaceSide::Back && m->normal.x < -0.9) back = m.get();
    REQUIRE(back);
    CHECK(back->tris.size() >= 9);
    // the wall's own faces keep their keys
    CHECK(find(*list, "wall", core::FaceSide::Front));
}

TEST_CASE("relief: the builder builds it in a second wave and serves undo from the cache") {
    base::JobPool pool(4);
    scene::SceneBuilder b(pool);
    Env env;
    b.setReliefEnv(env.env);
    const auto v0 = versionOf({box("wall", {0, 0, 0}, {4, 4, 1}), box("hole", {1, 1, -1}, {2, 2, 2}, core::BrushOp::Subtract),
                               box("far", {100, 0, 100}, {102, 2, 102})});
    b.submit(v0, doc::allChanged(0, v0->version));
    REQUIRE(b.waitIdle());
    const auto first = b.snapshot();
    REQUIRE(first);
    CHECK(first->complete);
    REQUIRE(first->objects.at(key("wall")).relief);
    CHECK(first->objects.at(key("wall")).relief->size() > 6);   // the hole's walls too
    CHECK(first->objects.at(key("far")).relief->size() == 6);
    CHECK_FALSE(first->objects.at(key("hole")).relief);   // a subtractor has none (null)
    CHECK(b.stats().reliefComputed == 2);

    // moving the hole inside the wall: the wall's relief changes, the far brush's does not
    const auto v1 = moveBrush(v0, "hole", {1, 0, 0});
    b.submit(v1, doc::diff(*v0, *v1));
    REQUIRE(b.waitIdle());
    const auto second = b.snapshot();
    CHECK(b.stats().reliefComputed == 3);
    CHECK(second->objects.at(key("far")).relief == first->objects.at(key("far")).relief);
    CHECK(second->objects.at(key("wall")).relief != first->objects.at(key("wall")).relief);

    // undo: surfaces and relief from their caches
    const uint64_t computed = b.stats().computed;
    b.submit(v0, doc::diff(*v1, *v0));
    REQUIRE(b.waitIdle());
    CHECK(b.stats().computed == computed);
    CHECK(b.stats().reliefComputed == 3);
    CHECK(b.stats().reliefHits >= 1);
    CHECK(b.snapshot()->objects.at(key("wall")).relief == first->objects.at(key("wall")).relief);
}

TEST_CASE("relief: a neighbour that touches an object takes part in its outline, so it is rebuilt when the neighbour moves") {
    base::JobPool pool(4);
    scene::SceneBuilder b(pool);
    Env env;
    b.setReliefEnv(env.env);
    // two walls side by side (one wall built from two brushes), and a third far away
    const auto v0 = versionOf({box("left", {0, 0, 0}, {2, 2, 1}), box("right", {2, 0, 0}, {4, 2, 1}), box("far", {50, 0, 0}, {52, 2, 1})});
    b.submit(v0, doc::allChanged(0, v0->version));
    REQUIRE(b.waitIdle());
    const auto first = b.snapshot();
    const uint64_t computed = b.stats().reliefComputed;
    CHECK(computed == 3);

    // "right" gets taller: its own relief changes and "left", which touches it, is rebuilt (the shared wall edge of its front face moves); "far" is not
    doc::VersionEditor ed(v0);
    core::Brush taller = v0->entities[0]->brushes[1]->brush;
    for (auto& v : taller.vertices)
        if (v.y > 1) v.y = 3;
    REQUIRE(ed.replaceBrush("w", taller));
    const auto v1 = ed.finish();
    REQUIRE(v1);
    b.submit(v1, doc::diff(*v0, *v1));
    REQUIRE(b.waitIdle());
    const auto second = b.snapshot();
    CHECK(b.stats().reliefComputed == computed + 2);   // right, and left because it touches right
    CHECK(second->objects.at(key("far")).relief == first->objects.at(key("far")).relief);
    CHECK(second->objects.at(key("right")).relief != first->objects.at(key("right")).relief);
}

TEST_CASE("relief: setting or changing the environment rebuilds the relief of every object; no environment, no relief") {
    base::JobPool pool(4);
    scene::SceneBuilder b(pool);
    const auto v0 = versionOf({box("a", {0, 0, 0}, {2, 2, 2}), box("c", {10, 0, 0}, {12, 2, 2})});
    b.submit(v0, doc::allChanged(0, v0->version));
    REQUIRE(b.waitIdle());
    CHECK_FALSE(b.snapshot()->objects.at(key("a")).relief);   // no environment: no relief
    CHECK(b.snapshot()->objects.at(key("a")).render);         // but the object has its triangles
    const uint64_t built0 = b.stats().reliefComputed;
    CHECK(built0 == 2);

    Env env;
    b.setReliefEnv(env.env);
    REQUIRE(b.waitIdle());
    CHECK(b.snapshot()->objects.at(key("a")).relief->size() == 6);
    CHECK(b.stats().reliefComputed == built0 + 2);
    CHECK(reliefMeshes(*b.snapshot()) == 12);
    CHECK(b.stats().computed == 2);   // the surfaces were not recomputed
    // the triangles follow the relief now
    CHECK(b.snapshot()->objects.at(key("a")).render->indices.size() > 36);

    // the texture lost its relief (revision 2): everything is rebuilt, now flat
    Env flat(false, 2);
    b.setReliefEnv(flat.env);
    REQUIRE(b.waitIdle());
    CHECK(reliefMeshes(*b.snapshot()) == 0);
    CHECK(b.snapshot()->objects.at(key("a")).relief);   // an empty list, not null: the object was looked at
    CHECK(b.snapshot()->objects.at(key("a")).render->indices.size() == 36);

    // the same revision again: nothing happens
    const uint64_t jobs = b.stats().reliefJobs;
    b.setReliefEnv(flat.env);
    REQUIRE(b.waitIdle());
    CHECK(b.stats().reliefJobs == jobs);
}

TEST_CASE("relief: lvl2 with a recess on every texture builds without trouble on the pool") {
    namespace fs = std::filesystem;
    const fs::path lvl2 = "F:/Git/SdlWasmEngine/source/Games/ShooterGame/GameData/maps/lvl2.map.json";
    if (!fs::exists(lvl2)) {
        MESSAGE("lvl2 not found: skipped");
        return;
    }
    const doc::LoadResult r = doc::loadDocumentFile(lvl2, "maps/lvl2.map.json");
    REQUIRE(r.version);
    // every texture is the recess texture: the heaviest relief load the level can give
    auto infos = std::make_shared<std::map<std::string, core::texplan::TexInfo>>();
    auto mutex = std::make_shared<std::mutex>();
    scene::ReliefEnv env;
    env.info = [infos, mutex](const std::string& p) -> const core::texplan::TexInfo* {
        std::lock_guard<std::mutex> lock(*mutex);
        auto it = infos->find(p);
        if (it == infos->end()) {
            core::texplan::TexInfo i;
            i.path = p;
            i.width = i.height = 64;
            i.ppm = 64;
            i.props = core::texplan::parseProps(recessProps(), p);
            it = infos->emplace(p, std::move(i)).first;
        }
        return &it->second;
    };
    env.groups = [](const std::string&) -> core::texplan::GroupPtr { return nullptr; };
    env.revision = 1;
    base::JobPool pool(16);
    scene::SceneBuilder b(pool);
    b.setReliefEnv(env);
    base::Stopwatch sw;
    b.submit(r.version, doc::allChanged(0, r.version->version));
    REQUIRE(b.waitIdle(120000));
    const double ms = sw.elapsedMs();
    const auto snap = b.snapshot();
    REQUIRE(snap);
    size_t tris = 0;
    for (const auto& [k, p] : snap->objects)
        if (p.relief)
            for (const auto& m : *p.relief) tris += m->tris.size() / 9;
    MESSAGE("lvl2 with relief everywhere: " << ms << " ms, " << reliefMeshes(*snap) << " meshes, " << tris << " relief triangles");
    base::logInfo("scene: lvl2 relief build (every texture a recess) " + std::to_string(ms) + " ms, " + std::to_string(tris) + " triangles");
    CHECK(snap->complete);
    CHECK(reliefMeshes(*snap) > 1000);
}

TEST_CASE("render mesh: the raw brush, the plain pieces, and the sky flag") {
    // octahedral normals round-trip
    for (const core::Vec3 v : {core::Vec3{1, 0, 0}, {0, -1, 0}, {0, 0, 1}, {-0.577, 0.577, -0.577}, {0.2, -0.9, 0.3}}) {
        const core::Vec3 n = v.normalized();
        float out[3];
        scene::octDecode(scene::octEncode(static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)), out);
        CHECK(out[0] == doctest::Approx(n.x).epsilon(0.002));
        CHECK(out[1] == doctest::Approx(n.y).epsilon(0.002));
        CHECK(out[2] == doctest::Approx(n.z).epsilon(0.002));
    }
    CHECK(scene::isSkyTexture("env/mountaincube"));
    CHECK(scene::isSkyTexture("sky/clouds"));
    CHECK_FALSE(scene::isSkyTexture("wall/brick"));

    const core::Brush b = box("a", {0, 0, 0}, {2, 3, 4}, core::BrushOp::Add, "wall/brick");
    const auto raw = scene::buildRawMesh(b, false);
    CHECK(raw->kind == scene::RenderMesh::Kind::Solid);
    CHECK(raw->indices.size() == 36);   // 6 faces, 2 triangles
    CHECK(raw->textures == std::vector<std::string>{"wall/brick"});
    for (size_t i = 0; i + 2 < raw->indices.size(); i += 3) {   // wound counter-clockwise seen from outside: the normal of the triangle is the stored one
        const auto& a = raw->vertices[raw->indices[i]];
        const auto& bb = raw->vertices[raw->indices[i + 1]];
        const auto& c = raw->vertices[raw->indices[i + 2]];
        const core::Vec3 e1{bb.pos[0] - a.pos[0], bb.pos[1] - a.pos[1], bb.pos[2] - a.pos[2]}, e2{c.pos[0] - a.pos[0], c.pos[1] - a.pos[1], c.pos[2] - a.pos[2]};
        float n[3];
        scene::octDecode(a.normal, n);
        CHECK(e1.cross(e2).normalized().dot({n[0], n[1], n[2]}) > 0.99);
        CHECK(a.flags == 0);
    }
    const auto volume = scene::buildRawMesh(b, true);
    CHECK(volume->kind == scene::RenderMesh::Kind::Volume);
    CHECK((volume->vertices[0].flags & scene::kFlagVolume) != 0);
    const auto sky = scene::buildRawMesh(box("s", {0, 0, 0}, {1, 1, 1}, core::BrushOp::Add, "env/skycube"), false);
    CHECK((sky->vertices[0].flags & scene::kFlagSky) != 0);

    // the object's pieces without an environment: the same count, albedo uvs of the pieces
    const core::Document d = docOf({b});
    const auto surfaces = surfacesOf(d, "a");
    const auto geo = scene::buildGeometry(surfaces, {}, parentsOf(d), scene::ReliefEnv{});
    CHECK(geo.mesh->indices.size() == 36);
    CHECK(geo.relief->empty());
    const auto& v0 = geo.mesh->vertices[0];
    const core::Vec3 p0{v0.pos[0], v0.pos[1], v0.pos[2]};
    bool found = false;
    for (const auto& s : surfaces) {
        const core::Vec2 uv = core::albedoUvAt(s, p0);
        if (std::fabs(uv.u - v0.uv[0]) < 1e-4 && std::fabs(uv.v - v0.uv[1]) < 1e-4) found = true;
    }
    CHECK(found);

    // with a relief texture the face is replaced by the displaced triangles
    const Env env;
    const core::Document r = docOf({box("a", {0, 0, 0}, {2, 2, 2})});
    const auto relGeo = scene::buildGeometry(surfacesOf(r, "a"), {}, parentsOf(r), env.env);
    CHECK(relGeo.mesh->indices.size() > 36 * 4);
    CHECK(relGeo.relief->size() == 6);
}

TEST_CASE("render mesh: the builder gives every object its triangles; a subtractive brush is a translucent volume") {
    base::JobPool pool(4);
    scene::SceneBuilder b(pool);
    const auto v0 = versionOf({box("wall", {0, 0, 0}, {4, 4, 1}), box("hole", {1, 1, -1}, {2, 2, 2}, core::BrushOp::Subtract)});
    b.submit(v0, doc::allChanged(0, v0->version));
    REQUIRE(b.waitIdle());
    const auto snap = b.snapshot();
    REQUIRE(snap);
    const auto& wall = snap->objects.at(key("wall"));
    const auto& hole = snap->objects.at(key("hole"));
    REQUIRE(wall.render);
    CHECK(wall.render->kind == scene::RenderMesh::Kind::Solid);
    CHECK(wall.render->indices.size() > 36);   // the hole cuts into the wall: more pieces than 6 faces
    REQUIRE(hole.render);
    CHECK(hole.render->kind == scene::RenderMesh::Kind::Volume);
    CHECK(hole.render->indices.size() == 36);
}
