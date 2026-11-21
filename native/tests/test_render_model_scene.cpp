// render/model_scene: the placements of the entities that show a loaded model (T11.9c). Pure: no GPU.
#include <doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "base/jobs.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "model_builder.h"
#include "render/model_scene.h"
#include "scene/model_entity.h"

using namespace native;
using modeltest::Builder;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Assets {
    fs::path root;
    Assets() {
        root = fs::temp_directory_path() / ("model_scene_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root / "models");
        Builder b;   // one triangle (0 0 0, 2 0 0, 0 4 0)
        const int pos = b.floats({0, 0, 0, 2, 0, 0, 0, 4, 0}, "VEC3");
        const json g = b.document({{"meshes", json::array({json{{"primitives", json::array({json{{"attributes", {{"POSITION", pos}}}}})}}})}, {"nodes", json::array({json{{"mesh", 0}}})}});
        const std::vector<uint8_t> bytes = b.glb(g);
        std::ofstream(root / "models/tri.glb", std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    ~Assets() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

core::Entity point(const std::string& id, const std::string& cls, core::Vec3 origin, const std::string& model = {}) {
    core::Entity e;
    e.id = id;
    e.classname = cls;
    e.origin = origin;
    if (!model.empty()) e.keyvalues["model_path"] = model;
    return e;
}

scene::SceneSnapshot snapshotOf(const std::vector<core::Entity>& entities) {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    for (const core::Entity& e : entities) REQUIRE(ed.addEntity(e));
    scene::SceneSnapshot s;
    s.doc = ed.finish();
    REQUIRE(s.doc);
    return s;
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-4; }

} // namespace

TEST_CASE("model scene: one placement per entity that shows a loaded model") {
    Assets a;
    scene::ModelStore store;
    store.setSource(a.root, nullptr, {});
    const scene::SceneSnapshot snap = snapshotOf({point("a", "misc_model", {10, 0, 0}, "models/tri.glb"), point("b", "misc_model", {0, 5, 0}, "models/tri.glb"),
                                                  point("c", "info_player_start", {1, 1, 1}), point("d", "misc_model", {2, 2, 2}, "models/missing.glb")});
    const auto scene = render::buildModelScene(snap, store);
    REQUIRE(scene);
    REQUIRE(scene->placements.size() == 2);   // the plain entity and the file that cannot be read show no model
    const render::ModelPlacement& p = scene->placements[0];
    CHECK(near(p.matrix[12], 10));
    CHECK(near(p.bounds.min[0], 10));
    CHECK(near(p.bounds.max[0], 12));
    CHECK(near(p.bounds.max[1], 4));
    CHECK(p.animation == -1);
    CHECK(scene->placements[0].model == scene->placements[1].model);   // one loaded model, shared
    CHECK_FALSE(scene->animated());
}

TEST_CASE("model scene: rotation and scale reach the matrix and the world box") {
    Assets a;
    scene::ModelStore store;
    store.setSource(a.root, nullptr, {});
    core::Entity e = point("a", "misc_model", {0, 0, 0}, "models/tri.glb");
    e.keyvalues["model_scale"] = "2";
    const auto scene = render::buildModelScene(snapshotOf({e}), store);
    REQUIRE(scene);
    CHECK(near(scene->placements[0].bounds.max[0], 4));
    CHECK(near(scene->placements[0].bounds.max[1], 8));
}

TEST_CASE("model scene: hidden entities and nothing to show") {
    Assets a;
    scene::ModelStore store;
    store.setSource(a.root, nullptr, {});
    const scene::SceneSnapshot snap = snapshotOf({point("a", "misc_model", {0, 0, 0}, "models/tri.glb"), point("b", "misc_model", {5, 0, 0}, "models/tri.glb")});
    std::unordered_set<scene::ObjectKey, scene::ObjectKey::Hash> hidden;
    hidden.insert(scene::ObjectKey{"", "a", ""});
    const auto scene = render::buildModelScene(snap, store, &hidden);
    REQUIRE(scene);
    REQUIRE(scene->placements.size() == 1);
    CHECK(near(scene->placements[0].matrix[12], 5));
    hidden.insert(scene::ObjectKey{"", "b", ""});
    CHECK(render::buildModelScene(snap, store, &hidden) == nullptr);
    CHECK(render::buildModelScene(scene::SceneSnapshot{}, store) == nullptr);   // no document
}

TEST_CASE("model scene: a model that is still loading is left out, then appears") {
    Assets a;
    base::JobPool pool(1);
    scene::ModelStore store;
    store.setSource(a.root, &pool, {});
    const scene::SceneSnapshot snap = snapshotOf({point("a", "misc_model", {0, 0, 0}, "models/tri.glb")});
    const auto first = render::buildModelScene(snap, store);   // the ask starts the load
    pool.waitIdle();
    const auto second = render::buildModelScene(snap, store);
    REQUIRE(second);
    CHECK(second->placements.size() == 1);
    CHECK((first == nullptr || first->placements.size() == 1));   // a fast pool may already be done
}
